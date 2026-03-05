#include "ClientLayer.h"
#include "ServerPacket.h"
#include "ApplicationGUI.h"
#include "Theme.h"
#include "UI.h"
#include "misc/cpp/imgui_stdlib.h"

using Safira::Theme;
using Safira::U32ToVec4;

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <numeric>
#include <ranges>
#include <format>

#include <spdlog/spdlog.h>

// Font-safe helpers -- ZERO direct ImFont member access (no ->FontSize).
namespace {

ImFont* SidebarBodyFont() {
    ImFont* f = Safira::ApplicationGUI::GetFont("Default");
    return f ? f : ImGui::GetFont();
}

ImFont* SidebarBoldFont() {
    ImFont* f = Safira::ApplicationGUI::GetFont("Bold");
    return f ? f : SidebarBodyFont();
}

ImVec2 SidebarMeasureText(ImFont* f, const char* text, float wrapWidth = 0.0f) {
    if (f) ImGui::PushFont(f);
    ImVec2 sz = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
    if (f) ImGui::PopFont();
    return sz;
}

void SidebarDrawTextAt(ImFont* f, ImVec2 pos, ImU32 col,
                       const char* text, float wrapWidth = 0.0f) {
    ImGui::SetCursorScreenPos(pos);
    if (f) ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    if (wrapWidth > 0.0f) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextUnformatted(text);
    }
    ImGui::PopStyleColor();
    if (f) ImGui::PopFont();
}

void SidebarDrawTextTruncated(ImFont* f, ImVec2 pos, ImU32 col,
                              const char* text, float maxW) {
    if (f) ImGui::PushFont(f);

    ImVec2 fullSz = ImGui::CalcTextSize(text);
    if (fullSz.x <= maxW) {
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    } else {
        const char* ellipsis = "...";
        float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
        float availW = maxW - ellipsisW;
        if (availW < 0.0f) availW = 0.0f;

        int len = (int)strlen(text);
        int lo = 0, hi = len, best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            ImVec2 sz = ImGui::CalcTextSize(text, text + mid);
            if (sz.x <= availW) {
                best = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }

        std::string trunc(text, best);
        trunc += ellipsis;
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, trunc.c_str());
    }

    if (f) ImGui::PopFont();
}

// Simple hash for detecting avatar data changes (avoids re-uploading
// unchanged textures every frame).
size_t HashAvatarData(const std::vector<uint8_t>& data) {
    // FNV-1a-ish — fast, good enough for change detection
    size_t h = 14695981039346656037ULL;
    for (auto b : data) {
        h ^= static_cast<size_t>(b);
        h *= 1099511628211ULL;
    }
    return h;
}

} // anon namespace

// =========================================================================
// DrawAvatarCircle — image avatar or coloured letter fallback
// =========================================================================

void ClientLayer::DrawAvatarCircle(ImDrawList* dl, ImVec2 center, float radius,
                                   uint32_t color, const std::string& username,
                                   ImTextureID tex) {
    if (tex) {
        // Round-clipped avatar image
        dl->AddImageRounded(tex,
            { center.x - radius, center.y - radius },
            { center.x + radius, center.y + radius },
            { 0, 0 }, { 1, 1 },
            Theme::Get().AvatarImageTint, radius);
    } else {
        // Coloured circle + first letter
        dl->AddCircleFilled(center, radius, color, 24);
        dl->AddCircle(center, radius, Theme::Get().IconOutline, 0, 1.5f);

        char letter = username.empty()
            ? '?'
            : static_cast<char>(toupper(username[0]));
        char buf[2] = { letter, '\0' };

        ImFont* bold = SidebarBoldFont();
        ImVec2 lsz = SidebarMeasureText(bold, buf);
        if (bold) ImGui::PushFont(bold);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { center.x - lsz.x * 0.5f, center.y - lsz.y * 0.5f },
                    Theme::Get().AvatarLetterCol, buf);
        if (bold) ImGui::PopFont();
    }
}

// =========================================================================
// Layer lifecycle
// =========================================================================

void ClientLayer::OnAttach() {
    m_ScratchBuffer.resize(1024);

    m_Client = std::make_unique<Safira::Client>();
    auto& app = Safira::ApplicationGUI::Get();

    m_Client->OnServerConnected([this, &app]() {
        app.QueueEvent([this] { OnConnected(); });
    });

    m_Client->OnServerDisconnected([this, &app]() {
        app.QueueEvent([this] { OnDisconnected(); });
    });

    m_Client->OnDataReceived([this, &app](Safira::ByteSpan data) {
        std::vector<uint8_t> copied(data.begin(), data.end());
        app.QueueEvent([this, payload = std::move(copied)]() {
            OnDataReceived(Safira::ByteSpan(payload.data(), payload.size()));
        });
    });

    m_Console.SetMessageSendCallback([this](std::string_view msg) { SendChatMessage(msg); });
    app.m_OnLogout = [this]() { Logout(); };

    LoadConnectionDetails(m_ConnectionDetailsFilePath);
}

void ClientLayer::OnDetach() {
    for (auto& [name, session] : m_PrivateChats)
        session->Close();
    m_PrivateChats.clear();
    m_Client->Disconnect();
}

void ClientLayer::OnUIRender() {
    UI_ConnectionModal();
    UI_CropModal();
    UI_IncomingInvites();
    UI_ReportModal();
    UI_UnifiedChatWindow();

    std::erase_if(m_PrivateChats, [](auto& pair) {
        return pair.second->IsClosed();
    });
}

bool ClientLayer::IsConnected() const {
    return m_Client->GetConnectionStatus() == Safira::ConnectionStatus::Connected;
}

void ClientLayer::OnDisconnectButton() { m_Client->Disconnect(); }

// =========================================================================
// AddLobbyMessage — attaches avatar texture for rendering
// =========================================================================

void ClientLayer::AddLobbyMessage(const std::string& who,
                                  const std::string& text,
                                  uint32_t color,
                                  Safira::MessageRole role) {
    std::lock_guard<std::mutex> lock(m_LobbyMutex);
    m_LobbyMessages.push_back({
        .Who       = who,
        .Text      = text,
        .Color     = color,
        .Role      = role,
        .Time      = Safira::ChatPanel::NowTimestamp(),
        .AvatarTex = {},
    });

    // Attach avatar texture: own or peer
    if (who == m_Username && m_AvatarTexture) {
        m_LobbyMessages.back().AvatarTex = m_AvatarTexture;
    } else if (auto it = m_PeerAvatars.find(who); it != m_PeerAvatars.end()) {
        m_LobbyMessages.back().AvatarTex = it->second.Tex;
    }

    if (m_ActiveConvoIdx == 0)
        m_ChatPanel.RequestScrollToBottom();
}

// =========================================================================
// RebuildConversationList — populates peer avatars from cache
// =========================================================================

void ClientLayer::RebuildConversationList() {
    m_ConversationList.clear();

    {
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        m_LobbySnapshot = m_LobbyMessages;
    }

    m_ConversationList.push_back({
        .Title     = "Lobby",
        .Preview   = m_LobbySnapshot.empty()
                         ? "Send a message..."
                         : m_LobbySnapshot.back().Text.substr(0, 36),
        .TimeLabel = "",
        .Messages  = &m_LobbySnapshot,
        .HasUnread = false,
        .AvatarTex = {},   // Lobby uses default icon
    });

    for (auto& [peer, session] : m_PrivateChats) {
        auto* entries = session->RefreshAndGetChatEntries(m_Username);

        // Resolve peer avatar from cache
        ImTextureID peerTex = {};
        if (auto it = m_PeerAvatars.find(peer); it != m_PeerAvatars.end())
            peerTex = it->second.Tex;

        m_ConversationList.push_back({
            .Title     = peer,
            .Preview   = entries->empty()
                             ? "..."
                             : entries->back().Text.substr(0, 36),
            .TimeLabel = session->IsConnected() ? "online" : "",
            .Messages  = entries,
            .HasUnread = false,
            .AvatarTex = peerTex,
        });
    }
}

// =========================================================================
// LoadAvatarImage — loads file → GPU texture + processes raw RGBA bytes for wire
// =========================================================================

void ClientLayer::LoadAvatarImage(const std::string& filepath) {
    if (filepath.empty()) return;

    // Load raw image for dimensions + crop check
    auto raw = Safira::LoadImageFromFile(filepath);
    if (!raw.Valid()) {
        spdlog::warn("Failed to load avatar image: {}", filepath);
        return;
    }

    m_AvatarImagePath = filepath;

    if (raw.NeedsCrop()) {
        // Non-square: open crop modal.  Upload full image for preview.
        m_CropSrcWidth  = raw.Width;
        m_CropSrcHeight = raw.Height;
        m_CropRect      = Safira::DefaultCenterCrop(raw);

        m_CropPreviewImage = std::make_shared<Safira::Image>(
            static_cast<uint32_t>(raw.Width),
            static_cast<uint32_t>(raw.Height),
            Safira::ImageFormat::RGBA,
            raw.Pixels.get());
        m_CropPreviewTex = (ImTextureID)m_CropPreviewImage->GetDescriptorSet();

        m_ShowCropModal = true;
        spdlog::info("Avatar {}x{} is not square — opening crop UI", raw.Width, raw.Height);
    } else {
        // Square: process immediately
        auto crop = Safira::DefaultCenterCrop(raw);
        auto rgba = Safira::ProcessAvatarImage(filepath, crop);
        if (!rgba) {
            spdlog::warn("Avatar processing failed for {}", filepath);
            return;
        }
        m_AvatarBytes = std::move(*rgba);

        // Upload display texture (full resolution for local display)
        m_AvatarImage = std::make_shared<Safira::Image>(
            static_cast<uint32_t>(raw.Width),
            static_cast<uint32_t>(raw.Height),
            Safira::ImageFormat::RGBA,
            raw.Pixels.get());
        m_AvatarTexture = (ImTextureID)m_AvatarImage->GetDescriptorSet();
        spdlog::info("Avatar loaded: {}x{} from {} ({} bytes raw RGBA)",
                     raw.Width, raw.Height, filepath, m_AvatarBytes.size());
    }
}

// =========================================================================
// UploadPeerAvatarTexture — raw RGBA → GPU texture, cached by hash
// =========================================================================

void ClientLayer::UploadPeerAvatarTexture(const std::string& username,
                                          const std::vector<uint8_t>& avatarData) {
    if (avatarData.empty()) {
        m_PeerAvatars.erase(username);
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        for (auto& msg : m_LobbyMessages) {
            if (msg.Who == username)
                msg.AvatarTex = {};
        }
        return;
    }

    const size_t hash = HashAvatarData(avatarData);

    // Skip if unchanged
    if (auto it = m_PeerAvatars.find(username); it != m_PeerAvatars.end()) {
        if (it->second.DataHash == hash)
            return;
    }

    // Avatar data is raw RGBA at kAvatarPixelSize × kAvatarPixelSize
    const uint32_t side = Safira::kAvatarPixelSize;
    const size_t expectedSize = side * side * 4;
    if (avatarData.size() != expectedSize) {
        spdlog::warn("Avatar for {} has unexpected size {} (expected {})",
                     username, avatarData.size(), expectedSize);
        m_PeerAvatars.erase(username);
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        for (auto& msg : m_LobbyMessages) {
            if (msg.Who == username)
                msg.AvatarTex = {};
        }
        return;
    }

    auto img = std::make_shared<Safira::Image>(
        side, side, Safira::ImageFormat::RGBA, avatarData.data());

    PeerAvatarCache cache;
    cache.Image    = std::move(img);
    cache.Tex      = (ImTextureID)cache.Image->GetDescriptorSet();
    cache.DataHash = hash;
    m_PeerAvatars[username] = std::move(cache);

    // Rebind historical lobby messages to the latest descriptor handle.
    std::lock_guard<std::mutex> lock(m_LobbyMutex);
    for (auto& msg : m_LobbyMessages) {
        if (msg.Who == username)
            msg.AvatarTex = m_PeerAvatars[username].Tex;
    }
}

// =========================================================================
// Logout
// =========================================================================

void ClientLayer::Logout() {
    for (auto& [name, session] : m_PrivateChats)
        session->Close();
    m_PrivateChats.clear();

    m_Client->Disconnect();

    m_ConnectedClients.clear();
    m_IncomingInvites.clear();
    m_PendingOutgoingInvites.clear();
    m_ActiveConvoIdx = 0;
    {
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        m_LobbyMessages.clear();
    }
    m_LobbySnapshot.clear();
    m_ConversationList.clear();

    m_Console.ClearLog();

    // Clear peer avatar cache
    m_PeerAvatars.clear();

    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarUserName.clear();
    app.m_TitlebarConnected  = false;
    app.m_TitlebarAvatarTex   = ImTextureID{};
    app.m_UserManualAway      = false;
}

// =========================================================================
// LeavePrivateChat
// =========================================================================

void ClientLayer::LeavePrivateChat(const std::string& peerUsername) {
    auto it = m_PrivateChats.find(peerUsername);
    if (it != m_PrivateChats.end()) {
        it->second->Close();
        m_PrivateChats.erase(it);
    }
    m_PendingOutgoingInvites.erase(peerUsername);
    m_ActiveConvoIdx = 0;

    AddLobbyMessage("System",
        std::format("Left private chat with {}.", peerUsername),
        Theme::Get().TextSystem, Safira::MessageRole::System);
}

// =========================================================================
// UI_ConnectionModal — REDESIGNED: no colour/icon picker, image avatar only
// =========================================================================

void ClientLayer::UI_ConnectionModal() {
    if (!m_ConnectionModalOpen &&
        m_Client->GetConnectionStatus() != Safira::ConnectionStatus::Connected &&
        !m_ShowCropModal &&
        !ImGui::IsPopupOpen("Crop Avatar##CropModal"))
        ImGui::OpenPopup("Connection");

    const auto& t = Theme::Get();
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       t.BgPopup);
    ImGui::PushStyleColor(ImGuiCol_Border,         t.ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        t.BgPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  t.BgPanel);
    ImGui::PushStyleColor(ImGuiCol_Text,           t.TextPrimary);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        t.BgFrame);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, t.BgFrameHovered);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  t.BgFrameActive);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

    const ImVec2 vpSize = ImGui::GetMainViewport()->WorkSize;
    const float modalW = std::clamp(vpSize.x * 0.52f, 500.0f, 640.0f);
    const float modalH = std::clamp(vpSize.y * 0.46f, 310.0f, 380.0f);
    ImGui::SetNextWindowSize({ modalW, modalH }, ImGuiCond_Always);
    m_ConnectionModalOpen = ImGui::BeginPopupModal(
        "Connection", nullptr, ImGuiWindowFlags_NoResize);

    if (!m_ConnectionModalOpen) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(8);
        return;
    }

    const auto statusPreview = m_Client->GetConnectionStatus();
    const std::string debugMessage = m_Client->GetConnectionDebugMessage();
    float statusReserve = 0.0f;
    if (statusPreview == Safira::ConnectionStatus::FailedToConnect) {
        statusReserve = debugMessage.empty() ? 24.0f : 42.0f;
    } else if (statusPreview == Safira::ConnectionStatus::Connecting) {
        statusReserve = 24.0f;
    }
    float bodyH = ImGui::GetContentRegionAvail().y - statusReserve - 6.0f;
    if (bodyH < 210.0f) bodyH = 210.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.BgPopupAlt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("##ConnectBody", { 0.0f, bodyH }, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float bodyW = ImGui::GetContentRegionAvail().x;
        const float leftW = std::clamp(bodyW * 0.34f, 160.0f, 220.0f);
        const float panelGap = 12.0f;
        const float panelH = ImGui::GetContentRegionAvail().y;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.BgFrame);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);

        // Left panel: true circular avatar + actions (no rectangular image card)
        ImGui::BeginChild("##ConnectAvatarPanel", { leftW, panelH }, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImDrawList* panelDl = ImGui::GetWindowDrawList();
            const float avatarDiameter = 106.0f;
            const float avatarRadius = avatarDiameter * 0.5f;
            const float availW = ImGui::GetContentRegionAvail().x;
            const float avatarIndent = std::max(0.0f, (availW - avatarDiameter) * 0.5f);

            ImGui::Dummy({ 0.0f, 6.0f });
            ImGui::Indent(avatarIndent);
            ImGui::InvisibleButton("##AvatarPreview", { avatarDiameter, avatarDiameter });
            ImGui::Unindent(avatarIndent);

            const ImVec2 avatarMin = ImGui::GetItemRectMin();
            const ImVec2 avatarMax = ImGui::GetItemRectMax();
            const ImVec2 center = {
                (avatarMin.x + avatarMax.x) * 0.5f,
                (avatarMin.y + avatarMax.y) * 0.5f
            };

            panelDl->AddCircleFilled(center, avatarRadius + 8.0f, IM_COL32(44, 46, 62, 255), 48);
            panelDl->AddCircle(center, avatarRadius + 8.0f, t.InputBorder, 48, 1.2f);

            if (m_AvatarTexture) {
                panelDl->AddImageRounded(m_AvatarTexture,
                    { center.x - avatarRadius, center.y - avatarRadius },
                    { center.x + avatarRadius, center.y + avatarRadius },
                    { 0, 0 }, { 1, 1 },
                    t.AvatarImageTint, avatarRadius);
            } else {
                panelDl->AddCircleFilled(center, avatarRadius, IM_COL32(84, 84, 96, 255), 48);
                char letter = m_Username.empty() ? '?' : static_cast<char>(toupper(m_Username[0]));
                char buf[2] = { letter, '\0' };
                ImVec2 lsz = ImGui::CalcTextSize(buf);
                panelDl->AddText({ center.x - lsz.x * 0.5f, center.y - lsz.y * 0.5f },
                                 IM_COL32(236, 236, 242, 255), buf);
            }

            const float actionH = 36.0f;
            const float footerY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - actionH;
            if (ImGui::GetCursorPosY() < footerY)
                ImGui::SetCursorPosY(footerY);

            float browseW = ImGui::GetContentRegionAvail().x;
            if (m_AvatarTexture)
                browseW = std::max(96.0f, browseW - 66.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        t.SendBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.SendBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  t.SendBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          t.SendBtnText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 10.0f, 7.0f });
            if (ImGui::Button("Browse Image", { browseW, actionH })) {
                auto path = Safira::FileDialog::OpenImage();
                if (path) {
                    LoadAvatarImage(*path);
                    if (m_ShowCropModal)
                        ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            if (m_AvatarTexture) {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,         IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.LogoutBtnHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   t.LogoutBtnActive);
                ImGui::PushStyleColor(ImGuiCol_Text,           t.LogoutIcon);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 8.0f, 7.0f });
                if (ImGui::Button("Clear", { 60.0f, actionH })) {
                    m_AvatarTexture   = {};
                    m_AvatarImage     = nullptr;
                    m_AvatarBytes.clear();
                    m_AvatarImagePath.clear();
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(4);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelGap);

        // Right panel: compact connection form + actions
        ImGui::BeginChild("##ConnectFormPanel", { 0.0f, panelH }, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImGui::TextColored(U32ToVec4(t.TextSecondary), "Username");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##username", &m_Username);

            ImGui::Dummy({ 0.0f, 10.0f });
            ImGui::TextColored(U32ToVec4(t.TextSecondary), "Server Address");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##address", &m_ServerIP);

            const float actionH = 36.0f;
            const float footerY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - actionH;
            if (ImGui::GetCursorPosY() < footerY)
                ImGui::SetCursorPosY(footerY);

            const float actionW = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;

            ImGui::PushStyleColor(ImGuiCol_Button,        t.SendBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.SendBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  t.SendBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          t.SendBtnText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 12.0f, 8.0f });
            if (ImGui::Button("Connect", { actionW, actionH })) {
                std::string addr = m_ServerIP;
                if (addr.rfind(':') == std::string::npos)
                    addr += ":8192";
                m_Client->ConnectToServer(addr);
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            ImGui::SameLine(0.0f, 12.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,         IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.LogoutBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   t.LogoutBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,           t.LogoutIcon);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 12.0f, 8.0f });
            if (ImGui::Button("Quit", { actionW, actionH }))
                Safira::ApplicationGUI::Get().Close();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(1);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(1);

    // ── Connection status feedback / send packet on success ──────
    if (const auto status = m_Client->GetConnectionStatus();
        status == Safira::ConnectionStatus::Connected) {

        // Send connection request with avatar data (no IconIndex)
        Safira::BufferWriter writer(m_ScratchBuffer);
        Safira::SerializePacket(writer, Safira::ConnectionRequestPacket{
            .Color      = 0,                // server will override with random
            .Username   = m_Username,
            .AvatarData = m_AvatarBytes,
        });
        m_Client->Send(writer.Written());
        SaveConnectionDetails(m_ConnectionDetailsFilePath);

        // Set titlebar user info
        auto& app = Safira::ApplicationGUI::Get();
        app.m_TitlebarUserName   = m_Username;
        app.m_TitlebarConnected  = true;
        app.m_TitlebarAvatarTex  = m_AvatarTexture;

        ImGui::CloseCurrentPopup();
    } else if (status == Safira::ConnectionStatus::FailedToConnect) {
        ImGui::TextColored({ 0.9f, 0.2f, 0.1f, 1.0f }, "Connection failed.");
        const auto msg = m_Client->GetConnectionDebugMessage();
        if (!msg.empty())
            ImGui::TextColored({ 0.9f, 0.2f, 0.1f, 1.0f }, "%s", msg.c_str());
    } else if (status == Safira::ConnectionStatus::Connecting) {
        ImGui::TextColored({ 0.8f, 0.8f, 0.8f, 1.0f }, "Connecting...");
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(8);
}

// =========================================================================
// UI_CropModal — square crop for non-square avatar images
// =========================================================================

void ClientLayer::UI_CropModal() {
    if (m_ShowCropModal) {
        ImGui::OpenPopup("Crop Avatar##CropModal");
        m_ShowCropModal = false;
    }

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Get().BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Border,   Theme::Get().ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,     Theme::Get().TextPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal("Crop Avatar##CropModal", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {

        ImFont* bold = SidebarBoldFont();
        if (bold) ImGui::PushFont(bold);
        ImGui::Text("Crop to Square");
        if (bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        // Draw interactive crop widget
        m_CropRect = Safira::DrawCropWidget(
            m_CropPreviewTex, m_CropSrcWidth, m_CropSrcHeight,
            m_CropRect, 300.0f);

        ImGui::Spacing();

        // Apply button
        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Get().AccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Get().AccentText);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Apply Crop", { 120, 0 })) {
            auto rgba = Safira::ProcessAvatarImage(m_AvatarImagePath, m_CropRect);
            if (rgba) {
                m_AvatarBytes = std::move(*rgba);

                // Create display texture from cropped + resized pixels
                auto raw = Safira::LoadImageFromFile(m_AvatarImagePath);
                if (raw.Valid()) {
                    auto cropped = Safira::CropSquare(raw, m_CropRect);
                    auto resized = Safira::ResizeSquare(cropped.data(), m_CropRect.Size,
                                                        Safira::kAvatarPixelSize);
                    m_AvatarImage = std::make_shared<Safira::Image>(
                        Safira::kAvatarPixelSize, Safira::kAvatarPixelSize,
                        Safira::ImageFormat::RGBA, resized.data());
                    m_AvatarTexture = (ImTextureID)m_AvatarImage->GetDescriptorSet();
                }
                spdlog::info("Avatar cropped and processed ({} bytes raw RGBA)",
                             m_AvatarBytes.size());
            } else {
                spdlog::warn("Avatar crop/compress failed");
            }

            // Clean up crop preview
            m_CropPreviewImage = nullptr;
            m_CropPreviewTex   = {};
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        // Cancel
        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Cancel", { 80, 0 })) {
            m_CropPreviewImage = nullptr;
            m_CropPreviewTex   = {};
            m_AvatarImagePath.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    if (!open) {
        // User closed via X
        m_CropPreviewImage = nullptr;
        m_CropPreviewTex   = {};
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

// =========================================================================
// UI_UserListSection — avatar images or coloured-letter circles
// =========================================================================

void ClientLayer::UI_UserListSection(float) {
    ImFont* bold = SidebarBoldFont();

    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored(U32ToVec4(Theme::Get().TextPrimary), "Online (%d)",
                       static_cast<int>(m_ConnectedClients.size()));
    if (bold) ImGui::PopFont();

    ImGui::Spacing();

    constexpr float kIconSize = 20.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (const auto& [username, info] : m_ConnectedClients) {
        if (username.empty()) continue;

        const bool isOurs = (username == m_Username);
        constexpr float itemPad = 14.0f;
        ImGui::SetCursorPosX(itemPad);
        const ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float  radius = kIconSize * 0.45f;
        const ImVec2 center = { pos.x + kIconSize * 0.5f, pos.y + kIconSize * 0.5f };

        // Resolve avatar texture: own or peer
        ImTextureID tex = {};
        if (isOurs && m_AvatarTexture) {
            tex = m_AvatarTexture;
        } else if (auto it = m_PeerAvatars.find(username); it != m_PeerAvatars.end()) {
            tex = it->second.Tex;
        }

        DrawAvatarCircle(dl, center, radius, info.Color, username, tex);
        ImGui::Dummy({ kIconSize, kIconSize });

        // Right-click context menu on the icon (not left-click)
        if (!isOurs) {
            const std::string popupId = "##UserCtx_" + username;
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                m_ContextMenuTarget = username;
                ImGui::OpenPopup(popupId.c_str());
            }

            ImGui::PushStyleColor(ImGuiCol_PopupBg,      Theme::Get().BgPopupAlt);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Get().BgFrameHovered);
            ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Get().TextPrimary);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);

            if (ImGui::BeginPopup(popupId.c_str())) {
                const bool alreadyInChat   = m_PrivateChats.contains(username);
                const bool alreadyInvited  = m_PendingOutgoingInvites.contains(username);

                if (alreadyInChat) {
                    ImGui::TextDisabled("Already in private chat");
                } else if (alreadyInvited) {
                    ImGui::TextDisabled("Invite pending...");
                } else {
                    if (ImGui::Selectable("Invite to private chat")) {
                        SendPrivateChatInvite(username);
                        m_PendingOutgoingInvites.insert(username);
                        AddLobbyMessage("System",
                            std::format("Invited {} to a private chat.", username),
                            Theme::Get().TextSystem, Safira::MessageRole::System);
                    }
                }

                ImGui::Separator();

                if (ImGui::Selectable("Report user to server")) {
                    m_ReportTarget = username;
                    m_ReportReasonBuf[0] = '\0';
                    m_ReportModalOpen = true;
                }

                ImGui::EndPopup();
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(Theme::Get().TextPrimary).Value);
        ImGui::TextUnformatted(username.c_str());
        ImGui::PopStyleColor();

        if (m_PrivateChats.contains(username)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(private)");
        } else if (m_PendingOutgoingInvites.contains(username)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(invited)");
        }
    }
}

// =========================================================================
// UI_ReportModal
// =========================================================================

void ClientLayer::UI_ReportModal() {
    if (m_ReportModalOpen) {
        ImGui::OpenPopup("Report User##ReportModal");
        m_ReportModalOpen = false;
    }

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Get().BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Border,   Theme::Get().ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,     Theme::Get().TextPrimary);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,  Theme::Get().BgFrame);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal("Report User##ReportModal", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Report %s", m_ReportTarget.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(U32ToVec4(Theme::Get().TextSecondary), "Reason:");
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextMultiline("##ReportReason", m_ReportReasonBuf,
                                   sizeof(m_ReportReasonBuf),
                                   { 300, 80 });

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Get().AccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Get().AccentText);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Submit Report", { 130, 0 })) {
            std::string reason(m_ReportReasonBuf);
            if (!reason.empty()) {
                std::string reportMsg = std::format(
                    "/report {} {}", m_ReportTarget, reason);
                Safira::BufferWriter writer(m_ScratchBuffer);
                Safira::SerializePacket(writer, Safira::MessagePacket{ reportMsg });
                m_Client->Send(writer.Written());

                AddLobbyMessage("System",
                    std::format("Reported {} to server.", m_ReportTarget),
                    Theme::Get().TextSystem, Safira::MessageRole::System);
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Cancel", { 80, 0 })) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

// =========================================================================
// UI_IncomingInvites
// =========================================================================

void ClientLayer::UI_IncomingInvites() {
    if (m_IncomingInvites.empty()) return;

    auto& invite = m_IncomingInvites.front();
    const std::string popupId = "Private Chat Request##" + invite.FromUsername;

    ImGui::OpenPopup(popupId.c_str());

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Get().BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Text,    Theme::Get().TextPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal(popupId.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s wants to chat with you privately.", invite.FromUsername.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        constexpr float btnW = 120.0f;
        constexpr float btnGap = 8.0f;
        const float totalW = btnW * 2.0f + btnGap;
        const float availW = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - totalW) * 0.5f);

        ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Get().SendBtn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().SendBtnHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Get().SendBtnActive);
    ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Get().SendBtnText);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Accept", { btnW, 0 })) {
            StartPrivateChatAsResponder(invite.FromUsername);
            m_IncomingInvites.erase(m_IncomingInvites.begin());
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine(0, btnGap);

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Decline", { btnW, 0 })) {
            SendPrivateChatResponse(invite.FromUsername, false);
            m_IncomingInvites.erase(m_IncomingInvites.begin());
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (!open) {
        SendPrivateChatResponse(invite.FromUsername, false);
        m_IncomingInvites.erase(m_IncomingInvites.begin());
    }
}

// =========================================================================
// UI_UnifiedChatWindow
// =========================================================================

void ClientLayer::UI_UnifiedChatWindow() {
    if (!Safira::ApplicationGUI::Get().IsChatPanelVisible())
        return;

    const ImVec2 outerAvail = ImGui::GetContentRegionAvail();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Get().PanelBgVec4());
    ImGui::BeginChild("##ChatPanel", outerAvail, false,
                       ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    RebuildConversationList();

    if (m_ActiveConvoIdx >= static_cast<int>(m_ConversationList.size()))
        m_ActiveConvoIdx = std::max(0,
            static_cast<int>(m_ConversationList.size()) - 1);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sideW = 260.0f;
    const float chatW = avail.x - sideW;

    // -- Left sidebar --------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          Theme::Get().PanelBgVec4());
    ImGui::BeginChild("##Sidebar", { sideW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    {
        const float pad = 14.0f;

        const float userListH = std::min(
            static_cast<float>(m_ConnectedClients.size()) * 26.0f + 36.0f,
            avail.y * 0.35f);

        ImGui::SetCursorPos({ 0, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::BeginChild("##UserSection", { sideW, userListH }, false);
        ImGui::PopStyleVar();
        ImGui::SetCursorPos({ pad, 8.0f });
        UI_UserListSection(sideW);
        ImGui::EndChild();

        // Divider
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine({ p.x + pad, p.y },
                        { p.x + sideW - pad, p.y },
                        Theme::Get().Divider, 1.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        }

        // Conversation list
        ImGui::BeginChild("##ConvoList", { 0, 0 }, false);

        ImFont* bold = SidebarBoldFont();
        ImFont* body = SidebarBodyFont();
        if (body) ImGui::PushFont(body);

        for (int i = 0; i < static_cast<int>(m_ConversationList.size()); ++i) {
            const auto& c = m_ConversationList[i];
            ImGui::PushID(i);

            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImVec2 itemSz = { sideW - 4.0f, 54.0f };
            const bool hovered  = ImGui::IsMouseHoveringRect(
                cursor, { cursor.x + itemSz.x, cursor.y + itemSz.y });

            ImU32 bg = 0;
            if (i == m_ActiveConvoIdx) bg = Theme::Get().BgItemSelected;
            else if (hovered)          bg = Theme::Get().BgItemHovered;

            if (bg)
                ImGui::GetWindowDrawList()->AddRectFilled(
                    cursor,
                    { cursor.x + itemSz.x, cursor.y + itemSz.y },
                    bg, 6.0f);

            if (ImGui::InvisibleButton("##c", itemSz))
                m_ActiveConvoIdx = i;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            constexpr float kR   = 14.0f;
            constexpr float kPad = 14.0f;
            const float tx = cursor.x + kPad;
            const float ax = tx + kR;
            const float ay = cursor.y + itemSz.y * 0.5f;

            // Resolve avatar colour for fallback circle
            uint32_t avatarCol = Theme::Get().Accent;
            if (i == 0) {
                avatarCol = Theme::Get().LobbyAvatar;
            } else {
                // Private chat peer — use their server-assigned colour
                const auto& title = c.Title;
                if (auto it = m_ConnectedClients.find(title);
                    it != m_ConnectedClients.end())
                    avatarCol = it->second.Color;
            }

            DrawAvatarCircle(dl, { ax, ay }, kR, avatarCol, c.Title, c.AvatarTex);

            const float textX = tx + kR * 2.0f + 10.0f;
            const float rightEdge = cursor.x + sideW - kPad - 4.0f;

            float titleMaxW = rightEdge - textX;
            if (!c.TimeLabel.empty()) {
                ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
                titleMaxW -= (tSz.x + 8.0f);
            }
            SidebarDrawTextTruncated(bold, { textX, cursor.y + 8.0f },
                              Theme::Get().ConvoTitleCol,
                              c.Title.c_str(), titleMaxW);

            const float previewMaxW = rightEdge - textX;
            SidebarDrawTextTruncated(body, { textX, cursor.y + 28.0f },
                              Theme::Get().ConvoPreviewCol,
                              c.Preview.c_str(), previewMaxW);

            if (!c.TimeLabel.empty()) {
                ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { cursor.x + sideW - kPad - tSz.x - 4.0f,
                      cursor.y + 10.0f },
                    Theme::Get().ConvoTimeCol,
                    c.TimeLabel.c_str());
            }

            ImGui::PopID();
        }

        if (body) ImGui::PopFont();
        ImGui::EndChild(); // ConvoList
    }
    ImGui::EndChild(); // Sidebar

    // Vertical separator
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 sidebarEnd = ImGui::GetCursorScreenPos();
        float panelTop = sidebarEnd.y - avail.y;
        dl->AddLine({ sidebarEnd.x, panelTop },
                    { sidebarEnd.x, panelTop + avail.y },
                    Theme::Get().Separator, 1.0f);
    }

    ImGui::PopStyleColor();

    // -- Right chat area -----------------------------------------------------
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          Theme::Get().PanelBgVec4());
    ImGui::BeginChild("##ChatArea", { chatW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    if (m_ActiveConvoIdx >= 0
        && m_ActiveConvoIdx < static_cast<int>(m_ConversationList.size())
        && m_ConversationList[m_ActiveConvoIdx].Messages)
    {
        auto& convo = *m_ConversationList[m_ActiveConvoIdx].Messages;
        const std::string& title =
            m_ConversationList[m_ActiveConvoIdx].Title;

        bool connected   = true;
        bool handshaking = false;
        const bool isPrivate = (m_ActiveConvoIdx > 0);

        std::string peerName;

        if (m_ActiveConvoIdx == 0) {
            connected = IsConnected();
        } else {
            int sessionIdx = 0;
            for (auto& [peer, session] : m_PrivateChats) {
                if (sessionIdx == m_ActiveConvoIdx - 1) {
                    connected   = session->IsConnected();
                    handshaking = session->IsRunning() && !connected;
                    peerName    = peer;
                    break;
                }
                sessionIdx++;
            }
        }

        m_ChatPanel.StatusProtocol = (m_ActiveConvoIdx == 0)
            ? "DTLS 1.3 | ML-KEM-512"
            : "TLS 1.3 | X25519/ML-KEM-768";

        m_ChatPanel.SetPrivateChatMode(isPrivate);

        if (isPrivate) {
            m_ChatPanel.SetOwnAvatar(m_AvatarTexture);

            // Set peer avatar from cache
            ImTextureID peerTex = {};
            if (auto it = m_PeerAvatars.find(peerName); it != m_PeerAvatars.end())
                peerTex = it->second.Tex;
            m_ChatPanel.SetPeerAvatar(peerTex);

            m_ChatPanel.SetOnLeaveCallback([this, peerName]() {
                LeavePrivateChat(peerName);
            });
        } else {
            m_ChatPanel.SetOwnAvatar(ImTextureID{});
            m_ChatPanel.SetPeerAvatar(ImTextureID{});
            m_ChatPanel.SetOnLeaveCallback(nullptr);
        }

        ImGui::SetCursorPos({ 14.0f, 8.0f });

        m_ChatPanel.RenderChatArea(convo, m_Username, title,
                                   connected, handshaking);

        if (auto msg = m_ChatPanel.ConsumePendingMessage()) {
            if (m_ActiveConvoIdx == 0) {
                SendChatMessage(*msg);
            } else {
                int sessionIdx = 0;
                for (auto& [peer, session] : m_PrivateChats) {
                    if (sessionIdx == m_ActiveConvoIdx - 1) {
                        session->Send(*msg);
                        session->AppendMessage(m_Username, *msg, 0xFFFFFFFF);
                        break;
                    }
                    sessionIdx++;
                }
            }
        }
    } else {
        const char* sub = "Select a conversation or start a new one.";
        ImVec2 sSz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPos({ (chatW - sSz.x) * 0.5f, avail.y * 0.45f });
        ImGui::TextColored({ 0.45f, 0.45f, 0.45f, 1.0f }, "%s", sub);
    }

    ImGui::EndChild(); // ChatArea
    ImGui::PopStyleColor();

    // ── Separator overlay ───────────────────────────────────────────────
    const bool anyModalOpen = !IsConnected()
        || !m_IncomingInvites.empty()
        || ImGui::IsPopupOpen("Report User##ReportModal");
    if (!anyModalOpen) {
        ImVec2 origin = ImGui::GetWindowPos();
        const ImU32 lineCol = Theme::Get().Separator;

        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize({ avail.x, avail.y });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

        ImGuiWindowFlags lineFlags =
              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoDocking    | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("##SeparatorOverlay", nullptr, lineFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(origin, { origin.x + avail.x, origin.y },
                        lineCol, 1.0f);
            dl->AddLine({ origin.x + sideW, origin.y },
                        { origin.x + sideW, origin.y + avail.y },
                        lineCol, 1.0f);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    ImGui::EndChild(); // ##ChatPanel
}

// =========================================================================
// Server callbacks
// =========================================================================

void ClientLayer::OnConnected() {
    m_Console.ClearLog();
    {
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        m_LobbyMessages.clear();
    }

    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarConnected = true;
}

void ClientLayer::OnDisconnected() {
    m_Console.AddItalicMessageWithColor(0xFF8A8A8A, "Lost connection to server!");
    AddLobbyMessage("System", "Lost connection to server!",
                    Theme::Get().TextSystem, Safira::MessageRole::System);
    m_IncomingInvites.clear();
    m_PendingOutgoingInvites.clear();

    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarConnected = false;
}

// =========================================================================
// OnDataReceived — caches peer avatar textures from ClientListPacket
// =========================================================================

void ClientLayer::OnDataReceived(Safira::ByteSpan data) {
    Safira::BufferReader reader(data);
    auto packet = Safira::DeserializeClientPacket(reader);
    if (!packet) {
        spdlog::warn("[client] packet parse error: {}",
                     Safira::Describe(packet.error()));
        return;
    }

    std::visit(Safira::Overloaded{
        [&](const Safira::ServerMessagePacket& pkt) {
            uint32_t col = m_ConnectedClients.contains(pkt.From)
                           ? m_ConnectedClients.at(pkt.From).Color
                           : 0xFFFFFFFF;
            if (pkt.From == "SERVER") col = 0xFFFFFFFF;
            m_Console.AddTaggedMessageWithColor(col, pkt.From, pkt.Message);

            if (pkt.From == m_Username) return;

            Safira::MessageRole role = Safira::MessageRole::Peer;
            AddLobbyMessage(pkt.From, pkt.Message, col, role);
        },
        [&](const Safira::ConnectionResponsePacket& pkt) {
            if (pkt.Accepted) {
                m_ShowSuccessfulConnectionMessage = true;
                m_Console.AddItalicMessageWithColor(
                    0xFF8A8A8A, "Welcome {}!", m_Username);
                AddLobbyMessage("System",
                    std::format("Welcome {}!", m_Username),
                    Theme::Get().TextSystem, Safira::MessageRole::System);
            } else {
                m_Console.AddItalicMessageWithColor(
                    0xFFFA4A4A,
                    "Server rejected connection with username {}", m_Username);
                AddLobbyMessage("System",
                    std::format("Server rejected username {}", m_Username),
                    0xFFFA4A4A, Safira::MessageRole::System);
            }
        },
        [&](const Safira::ClientListPacket& pkt) {
            m_ConnectedClients.clear();
            for (const auto& u : pkt.Clients) {
                m_ConnectedClients[u.Username] = u;

                // Update own colour from server assignment
                if (u.Username == m_Username)
                    m_Color = u.Color;

                // Cache peer avatar textures (skip self — we have our own)
                if (u.Username != m_Username)
                    UploadPeerAvatarTexture(u.Username, u.AvatarData);
            }

            // Remove stale peer avatars for users no longer in the list
            std::vector<std::string> removedUsers;
            std::erase_if(m_PeerAvatars, [&](const auto& pair) {
                const bool remove = !m_ConnectedClients.contains(pair.first);
                if (remove) removedUsers.push_back(pair.first);
                return remove;
            });
            if (!removedUsers.empty()) {
                std::lock_guard<std::mutex> lock(m_LobbyMutex);
                for (auto& msg : m_LobbyMessages) {
                    if (std::ranges::find(removedUsers, msg.Who) != removedUsers.end())
                        msg.AvatarTex = {};
                }
            }
        },
        [&](const Safira::ClientConnectPacket& pkt) {
            if (pkt.Client.Username != m_Username) {
                m_Console.AddItalicMessageWithColor(pkt.Client.Color,
                                                    "Welcome {}!", pkt.Client.Username);
            }
            m_ConnectedClients[pkt.Client.Username] = pkt.Client;

            // Cache their avatar
            if (pkt.Client.Username != m_Username)
                UploadPeerAvatarTexture(pkt.Client.Username, pkt.Client.AvatarData);
            if (pkt.Client.Username != m_Username) {
                AddLobbyMessage("System",
                    std::format("{} joined the lobby.", pkt.Client.Username),
                    Theme::Get().TextSystem, Safira::MessageRole::System);
            }
        },
        [&](const Safira::ClientDisconnectPacket& pkt) {
            // Scrub stale descriptor handles from historical messages before
            // removing avatar cache entries and freeing textures.
            {
                std::lock_guard<std::mutex> lock(m_LobbyMutex);
                for (auto& msg : m_LobbyMessages) {
                    if (msg.Who == pkt.Client.Username)
                        msg.AvatarTex = {};
                }
            }
            m_ConnectedClients.erase(pkt.Client.Username);
            m_PeerAvatars.erase(pkt.Client.Username);
            m_PrivateChats.erase(pkt.Client.Username);
            m_PendingOutgoingInvites.erase(pkt.Client.Username);
            m_Console.AddItalicMessageWithColor(pkt.Client.Color,
                                                "Goodbye {}!", pkt.Client.Username);
            AddLobbyMessage("System",
                std::format("Goodbye {}!", pkt.Client.Username),
                Theme::Get().TextSystem, Safira::MessageRole::System);
        },
        [&](const Safira::MessageHistoryPacket& pkt) {
            for (const auto& m : pkt.Messages) {
                uint32_t col = m_ConnectedClients.contains(m.Username)
                               ? m_ConnectedClients.at(m.Username).Color
                               : 0xFFFFFFFF;
                m_Console.AddTaggedMessageWithColor(col, m.Username, m.Message);

                Safira::MessageRole role = (m.Username == m_Username)
                    ? Safira::MessageRole::Own
                    : Safira::MessageRole::Peer;
                AddLobbyMessage(m.Username, m.Message, col, role);
            }
            if (m_ShowSuccessfulConnectionMessage) {
                m_ShowSuccessfulConnectionMessage = false;
                m_Console.AddItalicMessageWithColor(
                    0xFF8A8A8A,
                    "Successfully connected to {} with username {}",
                    m_ServerIP, m_Username);
                AddLobbyMessage("System",
                    std::format("Connected to {} as {}", m_ServerIP, m_Username),
                    Theme::Get().TextSystem, Safira::MessageRole::System);
            }
        },
        [&](const Safira::ServerShutdownPacket&) {
            m_Console.AddItalicMessage("Server is shutting down... goodbye!");
            AddLobbyMessage("System", "Server is shutting down...",
                            Theme::Get().TextSystem, Safira::MessageRole::System);
            m_Client->RequestDisconnect();
        },
        [&](const Safira::ClientKickPacket& pkt) {
            m_Console.AddItalicMessage("You have been kicked by server!");
            AddLobbyMessage("System", "You have been kicked!",
                            0xFFFA4A4A, Safira::MessageRole::System);
            if (!pkt.Reason.empty()) {
                m_Console.AddItalicMessage("Reason: {}", pkt.Reason);
                AddLobbyMessage("System",
                    std::format("Reason: {}", pkt.Reason),
                    0xFFFA4A4A, Safira::MessageRole::System);
            }
            m_Client->RequestDisconnect();
        },
        [&](const Safira::PrivateChatInvitePacket& pkt) {
            const bool alreadyHave = m_PrivateChats.contains(pkt.Username)
                || std::ranges::any_of(m_IncomingInvites,
                       [&](const IncomingInvite& i) {
                           return i.FromUsername == pkt.Username;
                       });
            if (!alreadyHave)
                m_IncomingInvites.push_back({ pkt.Username });
        },
        [&](const Safira::PrivateChatConnectToPacket& pkt) {
            m_PendingOutgoingInvites.erase(pkt.PeerUsername);
            StartPrivateChatAsInitiator(pkt.PeerUsername, pkt.Address);
        },
        [&](const Safira::PrivateChatDeclinedPacket& pkt) {
            m_PendingOutgoingInvites.erase(pkt.PeerUsername);
            m_Console.AddItalicMessage(
                "{} declined your private chat request.", pkt.PeerUsername);
            AddLobbyMessage("System",
                std::format("{} declined your chat request.", pkt.PeerUsername),
                Theme::Get().TextSystem, Safira::MessageRole::System);
        },
    }, *packet);
}

// =========================================================================
// P2P helpers
// =========================================================================

void ClientLayer::SendPrivateChatInvite(const std::string& targetUsername) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::PrivateChatInvitePacket{ targetUsername });
    m_Client->Send(writer.Written());
}

void ClientLayer::SendPrivateChatResponse(const std::string& toUsername, bool accepted) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::PrivateChatResponsePacket{
        toUsername, accepted, 0 });
    m_Client->Send(writer.Written());
}

void ClientLayer::StartPrivateChatAsResponder(const std::string& peerUsername) {
    auto session = std::make_unique<Safira::PrivateChatSession>(m_Username, peerUsername);
    const uint16_t port = session->StartAsResponder(Safira::P2PKeyType::RSA_PSS);
    if (port == 0) {
        m_Console.AddItalicMessage("Failed to start P2P listener for {}", peerUsername);
        return;
    }
    m_PrivateChats[peerUsername] = std::move(session);

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::PrivateChatResponsePacket{
        peerUsername, true, port });
    m_Client->Send(writer.Written());

    m_Console.AddItalicMessage(
        "Accepted private chat with {}. Waiting for connection...", peerUsername);
}

void ClientLayer::StartPrivateChatAsInitiator(const std::string& peerUsername,
                                              const std::string& peerAddress) {
    auto session = std::make_unique<Safira::PrivateChatSession>(m_Username, peerUsername);
    session->StartAsInitiator(peerAddress);
    m_PrivateChats[peerUsername] = std::move(session);
    m_Console.AddItalicMessage("Connecting to {} for private chat...", peerUsername);
}

// =========================================================================
// Chat send
// =========================================================================

void ClientLayer::SendChatMessage(std::string_view message) {
    std::string msg(message);
    if (!Safira::IsValidMessage(msg)) return;

    // ── Handle /afk command locally ─────────────────────────────────────
    if (msg == "/afk" || msg == "/away") {
        auto& app = Safira::ApplicationGUI::Get();
        app.m_UserManualAway = !app.m_UserManualAway;

        const char* status = app.m_UserManualAway ? "Away" : "Online";
        AddLobbyMessage("System",
            std::format("Status changed to {}.", status),
            Theme::Get().TextSystem, Safira::MessageRole::System);

        if (!app.m_UserManualAway)
            app.m_LastActivityTime = std::chrono::steady_clock::now();
        return;
    }

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::MessagePacket{ msg });
    m_Client->Send(writer.Written());

    m_Console.AddTaggedMessageWithColor(m_Color | 0xFF000000, m_Username, msg);
    AddLobbyMessage(m_Username, msg, m_Color | 0xFF000000,
                    Safira::MessageRole::Own);
}

// =========================================================================
// Persistence — IconIndex removed, AvatarImagePath kept
// =========================================================================

void ClientLayer::SaveConnectionDetails(const std::filesystem::path& filepath) {
    YAML::Emitter out;
    out << YAML::BeginMap
        << YAML::Key << "ConnectionDetails" << YAML::Value << YAML::BeginMap
            << YAML::Key << "Username"        << YAML::Value << m_Username
            << YAML::Key << "ServerIP"        << YAML::Value << m_ServerIP
            << YAML::Key << "AvatarImagePath" << YAML::Value << m_AvatarImagePath
        << YAML::EndMap
        << YAML::EndMap;

    std::ofstream fout(filepath);
    fout << out.c_str();
}

bool ClientLayer::LoadConnectionDetails(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) return false;

    YAML::Node data;
    try {
        data = YAML::LoadFile(filepath.string());
    } catch (const YAML::ParserException& e) {
        spdlog::error("failed to parse {}: {}", filepath.string(), e.what());
        return false;
    }

    auto root = data["ConnectionDetails"];
    if (!root) return false;

    m_Username = root["Username"].as<std::string>("");
    m_ServerIP = root["ServerIP"].as<std::string>("127.0.0.1");

    // Load avatar image path and attempt to load + process image
    m_AvatarImagePath = root["AvatarImagePath"].as<std::string>("");
    if (!m_AvatarImagePath.empty()) {
        LoadAvatarImage(m_AvatarImagePath);
    }

    return true;
}
