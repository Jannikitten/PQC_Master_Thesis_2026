#include "ClientLayer.h"
#include "ServerPacket.h"
#include "ApplicationGUI.h"
#include "UI.h"
#include "misc/cpp/imgui_stdlib.h"

// ═════════════════════════════════════════════════════════════════════════════
// ClientLayer.cpp
//
// §5.3  Serialization – BufferWriter + SerializePacket (concept-based)
// C++23               – ranges::any_of, erase_if, string_view, std::visit
// ═════════════════════════════════════════════════════════════════════════════

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <fstream>
#include <ranges>

#include <spdlog/spdlog.h>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static void DrawIconShape(ImDrawList* draw, ImVec2 center, float radius, uint8_t idx) {
    const uint32_t col = Safira::Icons::kColors[idx % Safira::Icons::kCount];
    draw->AddCircleFilled(center, radius, col);
    draw->AddCircle(center, radius, 0xFF000000, 0, 1.5f);
}

// ═════════════════════════════════════════════════════════════════════════════
// Layer lifecycle
// ═════════════════════════════════════════════════════════════════════════════

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
    m_Console.OnUIRender();
    UI_ClientList();
    UI_IncomingInvites();
    UI_PrivateChatWindows();
}

bool ClientLayer::IsConnected() const {
    return m_Client->GetConnectionStatus() == Safira::ConnectionStatus::Connected;
}

void ClientLayer::OnDisconnectButton() { m_Client->Disconnect(); }

// ═════════════════════════════════════════════════════════════════════════════
// DrawUserIcon
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::DrawUserIcon(uint8_t iconIndex, float size, bool /*clickable*/) {
    const ImVec2 pos    = ImGui::GetCursorScreenPos();
    const float  radius = size * 0.45f;
    const ImVec2 center = { pos.x + size * 0.5f, pos.y + size * 0.5f };
    DrawIconShape(ImGui::GetWindowDrawList(), center, radius, iconIndex);
    ImGui::Dummy({ size, size });
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_ConnectionModal
// ═════════════════════════════════════════════════════════════════════════════

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

    // ── Icon picker ─────────────────────────────────────────────────────────
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

    if (const auto status = m_Client->GetConnectionStatus(); status == Safira::ConnectionStatus::Connected) {
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

// ═════════════════════════════════════════════════════════════════════════════
// UI_ClientList
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_ClientList() {
    ImGui::Begin("Users Online");
    ImGui::Text("Online: %d", static_cast<int>(m_ConnectedClients.size()));
    ImGui::Separator();

    constexpr float kIconSize = 22.0f;
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
            ImGui::SetTooltip("Click to start a private chat with %s", username.c_str());

        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(info.Color).Value);
        ImGui::TextUnformatted(username.c_str());
        ImGui::PopStyleColor();

        if (m_PrivateChats.contains(username)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(private)");
        } else if (m_PendingOutgoingInvites.contains(username)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(invited...)");
        }

        if (iconClicked
            && !m_PrivateChats.contains(username)
            && !m_PendingOutgoingInvites.contains(username)) {
            SendPrivateChatInvite(username);
            m_PendingOutgoingInvites.insert(username);
            m_Console.AddItalicMessage("Invited {} to a private chat.", username);
        }
    }

    ImGui::End();
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_IncomingInvites
// ═════════════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════════════
// UI_PrivateChatWindows
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_PrivateChatWindows() {
    std::erase_if(m_PrivateChats, [this](auto& pair) {
        auto& [peer, session] = pair;
        const bool alive = session->OnUIRender(m_Username, m_Color);
        return !alive || session->IsClosed();
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// Server callbacks
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::OnConnected() { m_Console.ClearLog(); }

void ClientLayer::OnDisconnected() {
    m_Console.AddItalicMessageWithColor(0xFF8A8A8A, "Lost connection to server!");
    m_IncomingInvites.clear();
    m_PendingOutgoingInvites.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// OnDataReceived — variant dispatch via std::visit
//
// §3.4  Typestate  – compile-time exhaustive via Overloaded visitor
// §5.3  Serialization – DeserializeClientPacket returns expected<variant>
// ═════════════════════════════════════════════════════════════════════════════

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
        },
        [&](const Safira::ConnectionResponsePacket& pkt) {
            if (pkt.Accepted) {
                m_ShowSuccessfulConnectionMessage = true;
            } else {
                m_Console.AddItalicMessageWithColor(
                    0xFFFA4A4A,
                    "Server rejected connection with username {}", m_Username);
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
        },
        [&](const Safira::ClientDisconnectPacket& pkt) {
            m_ConnectedClients.erase(pkt.Client.Username);
            m_PrivateChats.erase(pkt.Client.Username);
            m_PendingOutgoingInvites.erase(pkt.Client.Username);
            m_Console.AddItalicMessageWithColor(pkt.Client.Color,
                                                "Goodbye {}!", pkt.Client.Username);
        },
        [&](const Safira::MessageHistoryPacket& pkt) {
            for (const auto& m : pkt.Messages) {
                uint32_t col = m_ConnectedClients.contains(m.Username)
                               ? m_ConnectedClients.at(m.Username).Color
                               : 0xFFFFFFFF;
                m_Console.AddTaggedMessageWithColor(col, m.Username, m.Message);
            }
            if (m_ShowSuccessfulConnectionMessage) {
                m_ShowSuccessfulConnectionMessage = false;
                m_Console.AddItalicMessageWithColor(
                    0xFF8A8A8A,
                    "Successfully connected to {} with username {}", m_ServerIP, m_Username);
            }
        },
        [&](const Safira::ServerShutdownPacket&) {
            m_Console.AddItalicMessage("Server is shutting down... goodbye!");
            m_Client->RequestDisconnect();
        },
        [&](const Safira::ClientKickPacket& pkt) {
            m_Console.AddItalicMessage("You have been kicked by server!");
            if (!pkt.Reason.empty())
                m_Console.AddItalicMessage("Reason: {}", pkt.Reason);
            m_Client->RequestDisconnect();
        },
        [&](const Safira::PrivateChatInvitePacket& pkt) {
            const bool alreadyHave = m_PrivateChats.contains(pkt.Username)
                || std::ranges::any_of(m_IncomingInvites,
                       [&](const IncomingInvite& i) { return i.FromUsername == pkt.Username; });

            if (!alreadyHave)
                m_IncomingInvites.push_back({ pkt.Username });
        },
        [&](const Safira::PrivateChatConnectToPacket& pkt) {
            m_PendingOutgoingInvites.erase(pkt.PeerUsername);
            StartPrivateChatAsInitiator(pkt.PeerUsername, pkt.Address);
        },
        [&](const Safira::PrivateChatDeclinedPacket& pkt) {
            m_PendingOutgoingInvites.erase(pkt.PeerUsername);
            m_Console.AddItalicMessage("{} declined your private chat request.", pkt.PeerUsername);
        },
    }, *packet);
}

// ═════════════════════════════════════════════════════════════════════════════
// P2P helpers — BufferWriter + SerializePacket
// ═════════════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════════════
// Chat / persistence
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::SendChatMessage(std::string_view message) {
    std::string msg(message);
    if (!Safira::IsValidMessage(msg)) return;

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::MessagePacket{ msg });
    m_Client->Send(writer.Written());

    m_Console.AddTaggedMessageWithColor(m_Color | 0xFF000000, m_Username, msg);
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
