#include "ClientLayer.h"
#include "ServerPacket.h"
#include "ApplicationGUI.h"
#include "UI.h"
#include "misc/cpp/imgui_stdlib.h"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <fstream>
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
// AddLobbyMessage
// =========================================================================

void ClientLayer::AddLobbyMessage(const std::string& who,
                                  const std::string& text,
                                  uint32_t color,
                                  Safira::MessageRole role) {
    std::lock_guard<std::mutex> lock(m_LobbyMutex);
    m_LobbyMessages.push_back({
        .Who   = who,
        .Text  = text,
        .Color = color,
        .Role  = role,
        .Time  = {},
    });
    // Auto-scroll when lobby is the active conversation
    if (m_ActiveConvoIdx == 0)
        m_ChatPanel.RequestScrollToBottom();
}

// =========================================================================
// RebuildConversationList
// =========================================================================

void ClientLayer::RebuildConversationList() {
    m_ConversationList.clear();

    // Snapshot lobby messages under lock so the UI thread
    // never iterates a vector the network thread is mutating.
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
        });
    }
}

// =========================================================================
// UI_ConnectionModal (unchanged)
// =========================================================================

void ClientLayer::UI_ConnectionModal() {
    if (!m_ConnectionModalOpen &&
        m_Client->GetConnectionStatus() != Safira::ConnectionStatus::Connected)
        ImGui::OpenPopup("Connect to server");

    m_ConnectionModalOpen = ImGui::BeginPopupModal(
        "Connect to server", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (!m_ConnectionModalOpen) return;

    ImGui::Text("Your Name");
    ImGui::InputText("##username", &m_Username);

    ImGui::Text("Pick a color");
    ImGui::SameLine();
    ImGui::ColorEdit4("##color", m_ColorBuffer);

    ImGui::Text("Pick an icon");
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
                        0xFFFFFFFF, 3.0f, 0, 2.0f);

        DrawIconShape(dl, center, radius, static_cast<uint8_t>(i));
        ImGui::Dummy({ kIconSize, kIconSize });

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_IconIndex = static_cast<uint8_t>(i);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Safira::Icons::kLabels[i]);
    }

    ImGui::Text("Server Address");
    ImGui::InputText("##address", &m_ServerIP);
    ImGui::SameLine();
    if (ImGui::Button("Connect")) {
        m_Color = IM_COL32(
            static_cast<int>(m_ColorBuffer[0] * 255.0f),
            static_cast<int>(m_ColorBuffer[1] * 255.0f),
            static_cast<int>(m_ColorBuffer[2] * 255.0f),
            static_cast<int>(m_ColorBuffer[3] * 255.0f));

        std::string addr = m_ServerIP;
        if (addr.rfind(':') == std::string::npos)
            addr += ":8192";
        m_Client->ConnectToServer(addr);
    }

    if (Safira::UI::ButtonCentered("Quit"))
        Safira::ApplicationGUI::Get().Close();

    if (const auto status = m_Client->GetConnectionStatus();
        status == Safira::ConnectionStatus::Connected) {
        Safira::BufferWriter writer(m_ScratchBuffer);
        Safira::SerializePacket(writer, Safira::ConnectionRequestPacket{
            m_Color, m_IconIndex, m_Username });
        m_Client->Send(writer.Written());
        SaveConnectionDetails(m_ConnectionDetailsFilePath);
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
}

// =========================================================================
// UI_UserListSection -- compact user list inside the sidebar
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
        const ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float  radius = kIconSize * 0.45f;
        const ImVec2 center = { pos.x + kIconSize * 0.5f, pos.y + kIconSize * 0.5f };

        DrawIconShape(dl, center, radius, info.IconIndex);
        ImGui::Dummy({ kIconSize, kIconSize });

        const bool iconClicked = !isOurs
            && ImGui::IsItemHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        if (ImGui::IsItemHovered() && !isOurs)
            ImGui::SetTooltip("Click to start a private chat with %s",
                              username.c_str());

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

        if (iconClicked
            && !m_PrivateChats.contains(username)
            && !m_PendingOutgoingInvites.contains(username)) {
            SendPrivateChatInvite(username);
            m_PendingOutgoingInvites.insert(username);
            AddLobbyMessage("System",
                std::format("Invited {} to a private chat.", username),
                0xFF888888, Safira::MessageRole::System);
        }
    }
}

// =========================================================================
// UI_IncomingInvites (unchanged)
// =========================================================================

void ClientLayer::UI_IncomingInvites() {
    if (m_IncomingInvites.empty()) return;

    auto& invite = m_IncomingInvites.front();
    const std::string popupId = "Private Chat Request##" + invite.FromUsername;

    ImGui::OpenPopup(popupId.c_str());
    bool open = true;
    if (ImGui::BeginPopupModal(popupId.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s wants to chat with you privately.", invite.FromUsername.c_str());
        ImGui::Separator();

        if (ImGui::Button("Accept", { 120, 0 })) {
            StartPrivateChatAsResponder(invite.FromUsername);
            m_IncomingInvites.erase(m_IncomingInvites.begin());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", { 120, 0 })) {
            SendPrivateChatResponse(invite.FromUsername, false);
            m_IncomingInvites.erase(m_IncomingInvites.begin());
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (!open) {
        SendPrivateChatResponse(invite.FromUsername, false);
        m_IncomingInvites.erase(m_IncomingInvites.begin());
    }
}

// =========================================================================
// UI_UnifiedChatWindow
// =========================================================================

void ClientLayer::UI_UnifiedChatWindow() {
    // Skip rendering when the toggle hides the panel — popups
    // (connection modal, invites) still work from OnUIRender().
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4{ 0.11f, 0.11f, 0.11f, 1.0f });
    ImGui::BeginChild("##Sidebar", { sideW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);
    {
        const float pad = 14.0f;
        ImGui::SetCursorPos({ pad, pad });

        ImFont* bold = SidebarBoldFont();
        if (bold) ImGui::PushFont(bold);
        ImGui::TextColored({ 0.85f, 0.73f, 0.42f, 1.0f }, "Safira");
        if (bold) ImGui::PopFont();

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);

        const float userListH = std::min(
            static_cast<float>(m_ConnectedClients.size()) * 26.0f + 40.0f,
            avail.y * 0.35f);

        ImGui::BeginChild("##UserSection", { sideW - 2, userListH }, false);
        ImGui::SetCursorPos({ pad, 4.0f });
        UI_UserListSection(sideW);
        ImGui::EndChild();

        // Divider
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine({ p.x + pad, p.y },
                        { p.x + sideW - pad, p.y },
                        IM_COL32(56, 56, 56, 255), 1.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        }

        // Conversation list
        ImGui::BeginChild("##ConvoList", { 0, 0 }, false);

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
            if (i == m_ActiveConvoIdx) bg = IM_COL32(54, 54, 54, 255);
            else if (hovered)          bg = IM_COL32(46, 46, 46, 255);

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
            dl->AddCircleFilled({ ax, ay }, kR, avatarCol, 24);

            // Avatar letter
            {
                ImVec2 lsz = SidebarMeasureText(bold, buf);
                if (bold) ImGui::PushFont(bold);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                            { ax - lsz.x * 0.5f, ay - lsz.y * 0.5f },
                            IM_COL32(255, 255, 255, 255), buf);
                if (bold) ImGui::PopFont();
            }

            const float textX = tx + kR * 2.0f + 10.0f;

            SidebarDrawTextAt(bold, { textX, cursor.y + 8.0f },
                              IM_COL32(210, 210, 210, 255), c.Title.c_str());

            std::string preview = c.Preview.substr(0, 30);
            if (c.Preview.size() > 30) preview += "...";
            SidebarDrawTextAt(body, { textX, cursor.y + 28.0f },
                              IM_COL32(130, 130, 130, 255),
                              preview.c_str());

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

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4{ 0.14f, 0.14f, 0.14f, 1.0f });
    ImGui::BeginChild("##ChatArea", { chatW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);

    if (m_ActiveConvoIdx >= 0
        && m_ActiveConvoIdx < static_cast<int>(m_ConversationList.size())
        && m_ConversationList[m_ActiveConvoIdx].Messages)
    {
        auto& convo = *m_ConversationList[m_ActiveConvoIdx].Messages;
        const std::string& title =
            m_ConversationList[m_ActiveConvoIdx].Title;

        bool connected   = true;
        bool handshaking = false;

        if (m_ActiveConvoIdx == 0) {
            connected = IsConnected();
        } else {
            int sessionIdx = 0;
            for (auto& [peer, session] : m_PrivateChats) {
                if (sessionIdx == m_ActiveConvoIdx - 1) {
                    connected   = session->IsConnected();
                    handshaking = session->IsRunning() && !connected;
                    break;
                }
                sessionIdx++;
            }
        }

        m_ChatPanel.StatusProtocol = (m_ActiveConvoIdx == 0)
            ? "DTLS 1.3 | ML-KEM-512"
            : "TLS 1.3 | X25519/ML-KEM-768";

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
        ImFont* bold = SidebarBoldFont();
        if (bold) ImGui::PushFont(bold);
        const char* appTitle = "Safira";
        ImVec2 tSz = ImGui::CalcTextSize(appTitle);
        ImGui::SetCursorPos({ (chatW - tSz.x) * 0.5f, avail.y * 0.38f });
        ImGui::TextColored({ 0.85f, 0.73f, 0.42f, 1.0f }, "%s", appTitle);
        if (bold) ImGui::PopFont();

        const char* sub = "Post-Quantum Secure Messaging";
        ImVec2 sSz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPosX((chatW - sSz.x) * 0.5f);
        ImGui::TextColored({ 0.45f, 0.45f, 0.45f, 1.0f }, "%s", sub);
    }

    ImGui::EndChild(); // ChatArea
    ImGui::PopStyleColor();

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
}

void ClientLayer::OnDisconnected() {
    m_Console.AddItalicMessageWithColor(0xFF8A8A8A, "Lost connection to server!");
    AddLobbyMessage("System", "Lost connection to server!",
                    0xFF888888, Safira::MessageRole::System);
    m_IncomingInvites.clear();
    m_PendingOutgoingInvites.clear();
}

// =========================================================================
// OnDataReceived
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

            // Skip lobby add for own messages — SendChatMessage() already
            // added them locally for instant feedback.
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
// Chat / persistence
// =========================================================================

void ClientLayer::SendChatMessage(std::string_view message) {
    std::string msg(message);
    if (!Safira::IsValidMessage(msg)) return;

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
            << YAML::Key << "Username"  << YAML::Value << m_Username
            << YAML::Key << "Color"     << YAML::Value << m_Color
            << YAML::Key << "IconIndex" << YAML::Value << static_cast<int>(m_IconIndex)
            << YAML::Key << "ServerIP"  << YAML::Value << m_ServerIP
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

    m_Username  = root["Username"].as<std::string>();
    m_Color     = root["Color"].as<uint32_t>();
    m_ServerIP  = root["ServerIP"].as<std::string>();
    m_IconIndex = static_cast<uint8_t>(root["IconIndex"].as<int>(0));

    const ImVec4 c = ImColor(m_Color).Value;
    m_ColorBuffer[0] = c.x;
    m_ColorBuffer[1] = c.y;
    m_ColorBuffer[2] = c.z;
    m_ColorBuffer[3] = c.w;
    return true;
}