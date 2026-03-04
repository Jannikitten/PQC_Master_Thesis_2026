#include "ClientLayer.h"
#include "ServerPacket.h"
#include "ApplicationGUI.h"
#include "UI.h"
#include "misc/cpp/imgui_stdlib.h"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <ranges>
#include <format>

#include <spdlog/spdlog.h>
#include <stb_image.h>

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

// Draw text truncated with "..." if it exceeds maxW pixels.
void SidebarDrawTextTruncated(ImFont* f, ImVec2 pos, ImU32 col,
                              const char* text, float maxW) {
    if (f) ImGui::PushFont(f);

    ImVec2 fullSz = ImGui::CalcTextSize(text);
    if (fullSz.x <= maxW) {
        // Fits — draw normally
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    } else {
        // Truncate: find how many chars fit, then append "..."
        const char* ellipsis = "...";
        float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
        float availW = maxW - ellipsisW;
        if (availW < 0.0f) availW = 0.0f;

        // Binary-ish search for the longest substring that fits
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

        // Build truncated string
        std::string trunc(text, best);
        trunc += ellipsis;
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, trunc.c_str());
    }

    if (f) ImGui::PopFont();
}

} // anon namespace

static void DrawIconShape(ImDrawList* draw, ImVec2 center, float radius, uint8_t idx) {
    const uint32_t col = Safira::Icons::kColors[idx % Safira::Icons::kCount];
    draw->AddCircleFilled(center, radius, col);
    draw->AddCircle(center, radius, IM_COL32(80, 80, 80, 200), 0, 1.5f);
}

// =========================================================================
// Layer lifecycle
// =========================================================================

void ClientLayer::OnAttach() {
    m_ScratchBuffer.resize(1024);

    m_Client = std::make_unique<Safira::Client>();

    m_Client->OnServerConnected   ([this]()                       { OnConnected();        });
    m_Client->OnServerDisconnected([this]()                       { OnDisconnected();     });
    m_Client->OnDataReceived      ([this](Safira::ByteSpan data)  { OnDataReceived(data); });

    m_Console.SetMessageSendCallback([this](std::string_view msg) { SendChatMessage(msg); });

    // Set logout callback for titlebar button
    auto& app = Safira::ApplicationGUI::Get();
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

void ClientLayer::DrawUserIcon(uint8_t iconIndex, float size, bool /*clickable*/) {
    const ImVec2 pos    = ImGui::GetCursorScreenPos();
    const float  radius = size * 0.45f;
    const ImVec2 center = { pos.x + size * 0.5f, pos.y + size * 0.5f };
    DrawIconShape(ImGui::GetWindowDrawList(), center, radius, iconIndex);
    ImGui::Dummy({ size, size });
}

// =========================================================================
// AddLobbyMessage -- TIMESTAMP BUG FIX: set time at creation
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
        .Time      = Safira::ChatPanel::NowTimestamp(),   // ← FIX: set NOW
        .AvatarTex = {},
    });

    // Set avatar texture for the user's own messages
    if (who == m_Username && m_AvatarTexture) {
        m_LobbyMessages.back().AvatarTex = m_AvatarTexture;
    }

    if (m_ActiveConvoIdx == 0)
        m_ChatPanel.RequestScrollToBottom();
}

// =========================================================================
// RebuildConversationList
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
        m_ConversationList.push_back({
            .Title     = peer,
            .Preview   = entries->empty()
                             ? "..."
                             : entries->back().Text.substr(0, 36),
            .TimeLabel = session->IsConnected() ? "online" : "",
            .Messages  = entries,
            .HasUnread = false,
            .AvatarTex = {},
        });
    }
}

// =========================================================================
// LoadAvatarImage -- loads file via stb_image → Safira::Image → ImTextureID
// =========================================================================

void ClientLayer::LoadAvatarImage(const std::string& filepath) {
    if (filepath.empty()) return;

    int w, h, channels;
    unsigned char* pixels = stbi_load(filepath.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        spdlog::warn("Failed to load avatar image: {}", filepath);
        return;
    }

    m_AvatarImage = std::make_shared<Safira::Image>(
        static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        Safira::ImageFormat::RGBA, pixels);
    stbi_image_free(pixels);

    m_AvatarTexture = (ImTextureID)m_AvatarImage->GetDescriptorSet();
    m_AvatarImagePath = filepath;
    spdlog::info("Avatar image loaded: {}x{} from {}", w, h, filepath);
}

// =========================================================================
// Logout -- disconnect, clear state, re-open connection modal
// =========================================================================

void ClientLayer::Logout() {
    // Close all private chats
    for (auto& [name, session] : m_PrivateChats)
        session->Close();
    m_PrivateChats.clear();

    // Disconnect
    m_Client->Disconnect();

    // Clear state
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

    // Clear titlebar state
    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarUserName.clear();
    app.m_TitlebarConnected  = false;
    app.m_TitlebarAvatarTex   = ImTextureID{};
    app.m_UserManualAway      = false;

    // Connection modal will re-open automatically because
    // status != Connected triggers ImGui::OpenPopup in UI_ConnectionModal
}

// =========================================================================
// LeavePrivateChat -- close specific private session, switch to lobby
// =========================================================================

void ClientLayer::LeavePrivateChat(const std::string& peerUsername) {
    auto it = m_PrivateChats.find(peerUsername);
    if (it != m_PrivateChats.end()) {
        it->second->Close();
        m_PrivateChats.erase(it);
    }
    m_PendingOutgoingInvites.erase(peerUsername);
    m_ActiveConvoIdx = 0;  // switch to lobby

    AddLobbyMessage("System",
        std::format("Left private chat with {}.", peerUsername),
        0xFF888888, Safira::MessageRole::System);
}

// =========================================================================
// UI_ConnectionModal -- REDESIGNED: dark theme, no color picker, image avatar
// =========================================================================

void ClientLayer::UI_ConnectionModal() {
    if (!m_ConnectionModalOpen &&
        m_Client->GetConnectionStatus() != Safira::ConnectionStatus::Connected)
        ImGui::OpenPopup("Connect to Safira");

    // Dark theme styling for the modal
    ImGui::PushStyleColor(ImGuiCol_PopupBg,      IM_COL32(28, 28, 28, 245));
    ImGui::PushStyleColor(ImGuiCol_Border,        IM_COL32(60, 60, 60, 200));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       IM_COL32(36, 36, 36, 255));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(36, 36, 36, 255));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(210, 210, 210, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,       IM_COL32(48, 48, 48, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(58, 58, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(68, 68, 68, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

    m_ConnectionModalOpen = ImGui::BeginPopupModal(
        "Connect to Safira", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    if (!m_ConnectionModalOpen) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(8);
        return;
    }

    // ── Title header ──────────────────────────────────────────────
    ImFont* bold = SidebarBoldFont();
    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored({ 0.85f, 0.73f, 0.42f, 1.0f }, "Safira");
    if (bold) ImGui::PopFont();
    ImGui::TextColored({ 0.55f, 0.55f, 0.55f, 1.0f }, "Post-Quantum Secure Messaging");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Username ──────────────────────────────────────────────────
    ImGui::TextColored({ 0.65f, 0.65f, 0.65f, 1.0f }, "Username");
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputText("##username", &m_Username);
    ImGui::Spacing();

    // ── Avatar image (replaces color picker) ─────────────────────
    ImGui::TextColored({ 0.65f, 0.65f, 0.65f, 1.0f }, "Profile Image (optional)");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##avatarpath", &m_AvatarImagePath);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        // NOTE: For a full native file dialog, integrate NFD or tinyfiledialogs.
        // This button serves as a placeholder -- user types the path manually.
        spdlog::info("Browse for avatar image (type path manually for now)");
    }

    // Preview of avatar if loaded
    if (m_AvatarTexture) {
        ImGui::SameLine();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float r = 14.0f;
        ImGui::GetWindowDrawList()->AddImageRounded(
            m_AvatarTexture,
            { pos.x, pos.y }, { pos.x + r * 2, pos.y + r * 2 },
            { 0, 0 }, { 1, 1 }, IM_COL32(255,255,255,255), r);
        ImGui::Dummy({ r * 2, r * 2 });
    }

    ImGui::Spacing();

    // ── Pick an icon (still available as fallback) ───────────────
    ImGui::TextColored({ 0.65f, 0.65f, 0.65f, 1.0f }, "Fallback Icon");
    constexpr float kIconSize = 28.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < Safira::Icons::kCount; ++i) {
        if (i > 0) ImGui::SameLine();

        const ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float  radius = kIconSize * 0.45f;
        const ImVec2 center = { pos.x + kIconSize * 0.5f, pos.y + kIconSize * 0.5f };

        if (m_IconIndex == static_cast<uint8_t>(i))
            dl->AddRect({ pos.x - 2, pos.y - 2 },
                        { pos.x + kIconSize + 2, pos.y + kIconSize + 2 },
                        IM_COL32(218, 185, 107, 255), 3.0f, 0, 2.0f);

        DrawIconShape(dl, center, radius, static_cast<uint8_t>(i));
        ImGui::Dummy({ kIconSize, kIconSize });

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_IconIndex = static_cast<uint8_t>(i);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Safira::Icons::kLabels[i]);
    }

    ImGui::Spacing();

    // ── Server address ───────────────────────────────────────────
    ImGui::TextColored({ 0.65f, 0.65f, 0.65f, 1.0f }, "Server Address");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##address", &m_ServerIP);
    ImGui::SameLine();

    // Connect button -- gold accent
    ImGui::PushStyleColor(ImGuiCol_Button,       IM_COL32(218, 185, 107, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(240, 206, 125, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200, 170, 90, 255));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(18, 18, 18, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    if (ImGui::Button("Connect", { 80, 0 })) {
        // Load avatar image if path changed
        if (!m_AvatarImagePath.empty() && !m_AvatarTexture) {
            LoadAvatarImage(m_AvatarImagePath);
        }

        m_Color = IM_COL32(210, 210, 210, 255);  // default neutral color

        std::string addr = m_ServerIP;
        if (addr.rfind(':') == std::string::npos)
            addr += ":8192";
        m_Client->ConnectToServer(addr);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    ImGui::Spacing();

    // Quit button -- subtle
    ImGui::PushStyleColor(ImGuiCol_Button,       IM_COL32(60, 60, 60, 200));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 50, 50, 220));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(100, 50, 50, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    if (Safira::UI::ButtonCentered("Quit"))
        Safira::ApplicationGUI::Get().Close();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    // ── Connection status feedback ───────────────────────────────
    if (const auto status = m_Client->GetConnectionStatus();
        status == Safira::ConnectionStatus::Connected) {
        Safira::BufferWriter writer(m_ScratchBuffer);
        Safira::SerializePacket(writer, Safira::ConnectionRequestPacket{
            m_Color, m_IconIndex, m_Username });
        m_Client->Send(writer.Written());
        SaveConnectionDetails(m_ConnectionDetailsFilePath);

        // Set titlebar user info
        auto& app = Safira::ApplicationGUI::Get();
        app.m_TitlebarUserName    = m_Username;
        app.m_TitlebarConnected  = true;
        app.m_TitlebarAvatarTex   = m_AvatarTexture;

        ImGui::CloseCurrentPopup();
    } else if (status == Safira::ConnectionStatus::FailedToConnect) {
        ImGui::TextColored({ 0.9f, 0.2f, 0.1f, 1.0f }, "Connection failed.");
        const auto& msg = m_Client->GetConnectionDebugMessage();
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
// UI_UserListSection -- RIGHT-CLICK context menu (replaces left-click invite)
// =========================================================================

void ClientLayer::UI_UserListSection(float) {
    ImFont* bold = SidebarBoldFont();

    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored({ 0.82f, 0.82f, 0.82f, 1.0f }, "Online (%d)",
                       static_cast<int>(m_ConnectedClients.size()));
    if (bold) ImGui::PopFont();

    ImGui::Spacing();

    constexpr float kIconSize = 20.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (const auto& [username, info] : m_ConnectedClients) {
        if (username.empty()) continue;

        const bool isOurs = (username == m_Username);
        constexpr float itemPad = 14.0f;   // same as conversation list pad
        ImGui::SetCursorPosX(itemPad);
        const ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float  radius = kIconSize * 0.45f;
        const ImVec2 center = { pos.x + kIconSize * 0.5f, pos.y + kIconSize * 0.5f };

        DrawIconShape(dl, center, radius, info.IconIndex);
        ImGui::Dummy({ kIconSize, kIconSize });

        // Right-click context menu on the icon (not left-click)
        if (!isOurs) {
            const std::string popupId = "##UserCtx_" + username;
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                m_ContextMenuTarget = username;
                ImGui::OpenPopup(popupId.c_str());
            }

            // Dark-themed context menu
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(38, 38, 38, 245));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(60, 60, 60, 255));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(210, 210, 210, 255));
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);

            if (ImGui::BeginPopup(popupId.c_str())) {
                // Option 1: Invite to private chat
                const bool alreadyInChat = m_PrivateChats.contains(username);
                const bool alreadyInvited = m_PendingOutgoingInvites.contains(username);

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
                            0xFF888888, Safira::MessageRole::System);
                    }
                }

                ImGui::Separator();

                // Option 2: Report user
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
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(info.Color).Value);
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
// UI_ReportModal -- modal dialog for reporting a user with a reason
// =========================================================================

void ClientLayer::UI_ReportModal() {
    if (m_ReportModalOpen) {
        ImGui::OpenPopup("Report User##ReportModal");
        m_ReportModalOpen = false;
    }

    // Dark theme styling
    ImGui::PushStyleColor(ImGuiCol_PopupBg,      IM_COL32(32, 32, 32, 245));
    ImGui::PushStyleColor(ImGuiCol_Border,        IM_COL32(60, 60, 60, 200));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(210, 210, 210, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,       IM_COL32(48, 48, 48, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal("Report User##ReportModal", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Report %s", m_ReportTarget.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored({ 0.65f, 0.65f, 0.65f, 1.0f }, "Reason:");
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextMultiline("##ReportReason", m_ReportReasonBuf,
                                   sizeof(m_ReportReasonBuf),
                                   { 300, 80 });

        ImGui::Spacing();

        // Submit button (gold)
        ImGui::PushStyleColor(ImGuiCol_Button,       IM_COL32(218, 185, 107, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(240, 206, 125, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(200, 170, 90, 255));
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(18, 18, 18, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Submit Report", { 130, 0 })) {
            std::string reason(m_ReportReasonBuf);
            if (!reason.empty()) {
                // Send report as a special message to server
                std::string reportMsg = std::format(
                    "/report {} {}", m_ReportTarget, reason);
                Safira::BufferWriter writer(m_ScratchBuffer);
                Safira::SerializePacket(writer, Safira::MessagePacket{ reportMsg });
                m_Client->Send(writer.Written());

                AddLobbyMessage("System",
                    std::format("Reported {} to server.", m_ReportTarget),
                    0xFF888888, Safira::MessageRole::System);
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        // Cancel button
        ImGui::PushStyleColor(ImGuiCol_Button,       IM_COL32(60, 60, 60, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 60, 60, 220));
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
// UI_IncomingInvites (unchanged)
// =========================================================================

void ClientLayer::UI_IncomingInvites() {
    if (m_IncomingInvites.empty()) return;

    auto& invite = m_IncomingInvites.front();
    const std::string popupId = "Private Chat Request##" + invite.FromUsername;

    ImGui::OpenPopup(popupId.c_str());

    // Dark theme for invite popup
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(32, 32, 32, 245));
    ImGui::PushStyleColor(ImGuiCol_Text,    IM_COL32(210, 210, 210, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal(popupId.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s wants to chat with you privately.", invite.FromUsername.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        // Center the two buttons
        constexpr float btnW = 120.0f;
        constexpr float btnGap = 8.0f;
        const float totalW = btnW * 2.0f + btnGap;
        const float availW = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - totalW) * 0.5f);

        // Accept (gold)
        ImGui::PushStyleColor(ImGuiCol_Button,       IM_COL32(218, 185, 107, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(240, 206, 125, 255));
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(18, 18, 18, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Accept", { btnW, 0 })) {
            StartPrivateChatAsResponder(invite.FromUsername);
            m_IncomingInvites.erase(m_IncomingInvites.begin());
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, btnGap);

        // Decline
        ImGui::PushStyleColor(ImGuiCol_Button,       IM_COL32(60, 60, 60, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 50, 50, 220));
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
// UI_UnifiedChatWindow -- with private mode, leave callback, peer header
// =========================================================================

void ClientLayer::UI_UnifiedChatWindow() {
    if (!Safira::ApplicationGUI::Get().IsChatPanelVisible())
        return;

    const ImVec2 outerAvail = ImGui::GetContentRegionAvail();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{ 0.14f, 0.14f, 0.14f, 1.0f });
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
                          ImVec4{ 0.14f, 0.14f, 0.14f, 1.0f });
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
                        IM_COL32(48, 48, 48, 255), 1.0f);
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
            if (i == m_ActiveConvoIdx) bg = IM_COL32(28, 28, 28, 255);
            else if (hovered)          bg = IM_COL32(32, 32, 32, 255);

            if (bg)
                ImGui::GetWindowDrawList()->AddRectFilled(
                    cursor,
                    { cursor.x + itemSz.x, cursor.y + itemSz.y },
                    bg, 6.0f);

            if (ImGui::InvisibleButton("##c", itemSz))
                m_ActiveConvoIdx = i;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            constexpr float kR = 14.0f;
            const float tx = cursor.x + pad;
            const float ax = tx + kR;
            const float ay = cursor.y + itemSz.y * 0.5f;
            char letter = c.Title.empty()
                ? '?'
                : static_cast<char>(toupper(c.Title[0]));
            char buf[2] = { letter, '\0' };

            ImU32 avatarCol = IM_COL32(218, 185, 107, 255);
            if (i == 0)
                avatarCol = IM_COL32(80, 120, 170, 255);

            // Use conversation avatar texture if available
            if (c.AvatarTex) {
                dl->AddImageRounded(c.AvatarTex,
                    { ax - kR, ay - kR }, { ax + kR, ay + kR },
                    { 0, 0 }, { 1, 1 },
                    IM_COL32(255, 255, 255, 255), kR);
            } else {
                dl->AddCircleFilled({ ax, ay }, kR, avatarCol, 24);
                ImVec2 lsz = SidebarMeasureText(bold, buf);
                if (bold) ImGui::PushFont(bold);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                            { ax - lsz.x * 0.5f, ay - lsz.y * 0.5f },
                            IM_COL32(255, 255, 255, 255), buf);
                if (bold) ImGui::PopFont();
            }

            const float textX = tx + kR * 2.0f + 10.0f;
            const float rightEdge = cursor.x + sideW - pad - 4.0f;

            // Title — truncate accounting for time label on the right
            float titleMaxW = rightEdge - textX;
            if (!c.TimeLabel.empty()) {
                ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
                titleMaxW -= (tSz.x + 8.0f);  // gap before time label
            }
            SidebarDrawTextTruncated(bold, { textX, cursor.y + 8.0f },
                              IM_COL32(210, 210, 210, 255),
                              c.Title.c_str(), titleMaxW);

            // Preview — truncate to available width
            const float previewMaxW = rightEdge - textX;
            SidebarDrawTextTruncated(body, { textX, cursor.y + 28.0f },
                              IM_COL32(130, 130, 130, 255),
                              c.Preview.c_str(), previewMaxW);

            if (!c.TimeLabel.empty()) {
                ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { cursor.x + sideW - pad - tSz.x - 4.0f,
                      cursor.y + 10.0f },
                    IM_COL32(110, 110, 110, 255),
                    c.TimeLabel.c_str());
            }

            ImGui::PopID();
        }

        if (body) ImGui::PopFont();
        ImGui::EndChild(); // ConvoList
    }
    ImGui::EndChild(); // Sidebar
    ImGui::PopStyleColor();

    // -- Right chat area -----------------------------------------------------
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4{ 0.14f, 0.14f, 0.14f, 1.0f });
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

        // ── Configure ChatPanel for this conversation ──────────────
        m_ChatPanel.StatusProtocol = (m_ActiveConvoIdx == 0)
            ? "DTLS 1.3 | ML-KEM-512"
            : "TLS 1.3 | X25519/ML-KEM-768";

        // Private mode: hide author names, show peer avatar + leave button
        m_ChatPanel.SetPrivateChatMode(isPrivate);

        if (isPrivate) {
            m_ChatPanel.SetOwnAvatar(m_AvatarTexture);
            m_ChatPanel.SetPeerAvatar(ImTextureID{}); // peer avatar not available locally

            // Set leave callback for this specific peer
            m_ChatPanel.SetOnLeaveCallback([this, peerName]() {
                LeavePrivateChat(peerName);
            });
        } else {
            m_ChatPanel.SetOwnAvatar(ImTextureID{});
            m_ChatPanel.SetPeerAvatar(ImTextureID{});
            m_ChatPanel.SetOnLeaveCallback(nullptr);
        }

        // Match left/top padding with sidebar "Online" header
        ImGui::SetCursorPos({ 14.0f, 8.0f });

        m_ChatPanel.RenderChatArea(convo, m_Username, title,
                                   connected, handshaking);

        // Handle outbound messages
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
        // Empty state
        const char* sub = "Select a conversation or start a new one.";
        ImVec2 sSz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPos({ (chatW - sSz.x) * 0.5f, avail.y * 0.45f });
        ImGui::TextColored({ 0.45f, 0.45f, 0.45f, 1.0f }, "%s", sub);
    }

    ImGui::EndChild(); // ChatArea
    ImGui::PopStyleColor();

    // ── Separator lines ─────────────────────────────────────────────────
    // Use the foreground draw list so lines render ON TOP of child windows.
    // Skip when any modal popup is open (connection, invite, report) so
    // lines don't bleed through them.
    const bool anyModalOpen = !IsConnected()
        || !m_IncomingInvites.empty()
        || ImGui::IsPopupOpen("Report User##ReportModal");
    if (!anyModalOpen) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 origin = ImGui::GetWindowPos();
        const ImU32 lineCol = IM_COL32(48, 48, 48, 255);

        // Horizontal line at top (separates titlebar from panels)
        dl->AddLine(origin, { origin.x + avail.x, origin.y },
                    lineCol, 1.0f);

        // Vertical line between sidebar and chat area
        dl->AddLine({ origin.x + sideW, origin.y },
                    { origin.x + sideW, origin.y + avail.y },
                    lineCol, 1.0f);
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

    // Update titlebar status
    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarConnected = true;
}

void ClientLayer::OnDisconnected() {
    m_Console.AddItalicMessageWithColor(0xFF8A8A8A, "Lost connection to server!");
    AddLobbyMessage("System", "Lost connection to server!",
                    0xFF888888, Safira::MessageRole::System);
    m_IncomingInvites.clear();
    m_PendingOutgoingInvites.clear();

    // Update titlebar status
    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarConnected = false;
}

// =========================================================================
// OnDataReceived -- TIMESTAMP FIX: all messages get time at creation
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

            Safira::MessageRole role = (pkt.From == "SERVER")
                    ? Safira::MessageRole::System
                    : Safira::MessageRole::Peer;
            AddLobbyMessage(pkt.From, pkt.Message, col, role);
        },
        [&](const Safira::ConnectionResponsePacket& pkt) {
            if (pkt.Accepted) {
                m_ShowSuccessfulConnectionMessage = true;
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
            for (const auto& u : pkt.Clients)
                m_ConnectedClients[u.Username] = u;
        },
        [&](const Safira::ClientConnectPacket& pkt) {
            m_Console.AddItalicMessageWithColor(pkt.Client.Color,
                                                "Welcome {}!", pkt.Client.Username);
            m_ConnectedClients[pkt.Client.Username] = pkt.Client;
            AddLobbyMessage("System",
                std::format("Welcome {}!", pkt.Client.Username),
                0xFF888888, Safira::MessageRole::System);
        },
        [&](const Safira::ClientDisconnectPacket& pkt) {
            m_ConnectedClients.erase(pkt.Client.Username);
            m_PrivateChats.erase(pkt.Client.Username);
            m_PendingOutgoingInvites.erase(pkt.Client.Username);
            m_Console.AddItalicMessageWithColor(pkt.Client.Color,
                                                "Goodbye {}!", pkt.Client.Username);
            AddLobbyMessage("System",
                std::format("Goodbye {}!", pkt.Client.Username),
                0xFF888888, Safira::MessageRole::System);
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
                // Note: history messages get current time since server
                // doesn't transmit original timestamps
            }
            if (m_ShowSuccessfulConnectionMessage) {
                m_ShowSuccessfulConnectionMessage = false;
                m_Console.AddItalicMessageWithColor(
                    0xFF8A8A8A,
                    "Successfully connected to {} with username {}",
                    m_ServerIP, m_Username);
                AddLobbyMessage("System",
                    std::format("Connected to {} as {}", m_ServerIP, m_Username),
                    0xFF888888, Safira::MessageRole::System);
            }
        },
        [&](const Safira::ServerShutdownPacket&) {
            m_Console.AddItalicMessage("Server is shutting down... goodbye!");
            AddLobbyMessage("System", "Server is shutting down...",
                            0xFF888888, Safira::MessageRole::System);
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
                0xFF888888, Safira::MessageRole::System);
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
    auto session = std::make_unique<Safira::PrivateChatSession>(peerUsername);
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
    auto session = std::make_unique<Safira::PrivateChatSession>(peerUsername);
    session->StartAsInitiator(peerAddress);
    m_PrivateChats[peerUsername] = std::move(session);
    m_Console.AddItalicMessage("Connecting to {} for private chat...", peerUsername);
}

// =========================================================================
// Chat / persistence -- COLOR REMOVED, AVATAR PATH ADDED
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
            0xFF888888, Safira::MessageRole::System);

        if (!app.m_UserManualAway)
            app.m_LastActivityTime = std::chrono::steady_clock::now();
        return;  // don't send /afk to server
    }

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::MessagePacket{ msg });
    m_Client->Send(writer.Written());

    m_Console.AddTaggedMessageWithColor(m_Color | 0xFF000000, m_Username, msg);
    AddLobbyMessage(m_Username, msg, m_Color | 0xFF000000,
                    Safira::MessageRole::Own);
}

void ClientLayer::SaveConnectionDetails(const std::filesystem::path& filepath) {
    YAML::Emitter out;
    out << YAML::BeginMap
        << YAML::Key << "ConnectionDetails" << YAML::Value << YAML::BeginMap
            << YAML::Key << "Username"        << YAML::Value << m_Username
            << YAML::Key << "IconIndex"       << YAML::Value << static_cast<int>(m_IconIndex)
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

    m_Username  = root["Username"].as<std::string>("");
    m_ServerIP  = root["ServerIP"].as<std::string>("127.0.0.1");
    m_IconIndex = static_cast<uint8_t>(root["IconIndex"].as<int>(0));
    m_Color     = IM_COL32(210, 210, 210, 255);  // default neutral

    // Load avatar image path and attempt to load image
    m_AvatarImagePath = root["AvatarImagePath"].as<std::string>("");
    if (!m_AvatarImagePath.empty()) {
        LoadAvatarImage(m_AvatarImagePath);
    }

    return true;
}