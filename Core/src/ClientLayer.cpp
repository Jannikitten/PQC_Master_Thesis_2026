#include "ClientLayer.h"
#include "ServerPacket.h"
#include "ApplicationGUI.h"
#include "UI.h"
#include "BufferStream.h"
#include "misc/cpp/imgui_stdlib.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static void DrawIconShape(ImDrawList* draw, ImVec2 center, float radius, uint8_t idx) {
    uint32_t col = Safira::Icons::kColors[idx % Safira::Icons::kCount];
    draw->AddCircleFilled(center, radius, col);
    draw->AddCircle(center, radius, 0xFF000000, 0, 1.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Layer lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::OnAttach() {
    m_ScratchBuffer.Allocate(1024);

    m_Client = std::make_unique<Safira::Client>();
    m_Client->SetServerConnectedCallback([this]()                       { OnConnected();        });
    m_Client->SetServerDisconnectedCallback([this]()                    { OnDisconnected();     });
    m_Client->SetDataReceivedCallback([this](const Safira::Buffer data) { OnDataReceived(data); });

    m_Console.SetMessageSendCallback([this](std::string_view msg) { SendChatMessage(msg); });

    LoadConnectionDetails(m_ConnectionDetailsFilePath);
}

void ClientLayer::OnDetach() {
    for (auto& [name, session] : m_PrivateChats) session->Close();
    m_PrivateChats.clear();
    m_Client->Disconnect();
    m_ScratchBuffer.Release();
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

// ─────────────────────────────────────────────────────────────────────────────
// DrawUserIcon
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::DrawUserIcon(uint8_t iconIndex, float size, bool /*clickable*/) {
    ImVec2      pos    = ImGui::GetCursorScreenPos();
    float       radius = size * 0.45f;
    ImVec2      center = {pos.x + size * 0.5f, pos.y + size * 0.5f};
    ImDrawList* dl     = ImGui::GetWindowDrawList();
    DrawIconShape(dl, center, radius, iconIndex);
    ImGui::Dummy({size, size});
}

// ─────────────────────────────────────────────────────────────────────────────
// UI_ConnectionModal
// ─────────────────────────────────────────────────────────────────────────────
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

    // ── Icon picker ──────────────────────────────────────────────────────────
    ImGui::Text("Pick an icon");
    constexpr float kIconSize = 28.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < Safira::Icons::kCount; ++i) {
        if (i > 0) ImGui::SameLine();

        ImVec2 pos    = ImGui::GetCursorScreenPos();
        float  radius = kIconSize * 0.45f;
        ImVec2 center = {pos.x + kIconSize * 0.5f, pos.y + kIconSize * 0.5f};

        if (m_IconIndex == static_cast<uint8_t>(i))
            dl->AddRect({pos.x - 2, pos.y - 2},
                        {pos.x + kIconSize + 2, pos.y + kIconSize + 2},
                        0xFFFFFFFF, 3.0f, 0, 2.0f);

        DrawIconShape(dl, center, radius, static_cast<uint8_t>(i));
        ImGui::Dummy({kIconSize, kIconSize});

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_IconIndex = static_cast<uint8_t>(i);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Safira::Icons::kLabels[i]);
    }

    ImGui::Text("Server Address");
    ImGui::InputText("##address", &m_ServerIP);
    ImGui::SameLine();
    if (ImGui::Button("Connect")) {
        m_Color = IM_COL32(m_ColorBuffer[0] * 255.0f, m_ColorBuffer[1] * 255.0f,
                           m_ColorBuffer[2] * 255.0f, m_ColorBuffer[3] * 255.0f);
        std::string addr = m_ServerIP;
        if (addr.rfind(':') == std::string::npos) addr += ":8192";
        m_Client->ConnectToServer(addr);
    }

    if (Safira::UI::ButtonCentered("Quit")) Safira::ApplicationGUI::Get().Close();

    const auto status = m_Client->GetConnectionStatus();
    if (status == Safira::ConnectionStatus::Connected) {
        Safira::BufferStreamWriter stream(m_ScratchBuffer);
        stream.WriteRaw<PacketType>(PacketType::ClientConnectionRequest);
        stream.WriteRaw<uint32_t>(m_Color);
        stream.WriteRaw<uint8_t>(m_IconIndex);
        stream.WriteString(m_Username);
        m_Client->SendBuffer(stream.GetBuffer());
        SaveConnectionDetails(m_ConnectionDetailsFilePath);
        ImGui::CloseCurrentPopup();
    } else if (status == Safira::ConnectionStatus::FailedToConnect) {
        ImGui::TextColored({0.9f, 0.2f, 0.1f, 1.0f}, "Connection failed.");
        const auto& msg = m_Client->GetConnectionDebugMessage();
        if (!msg.empty()) ImGui::TextColored({0.9f, 0.2f, 0.1f, 1.0f}, "%s", msg.c_str());
    } else if (status == Safira::ConnectionStatus::Connecting) {
        ImGui::TextColored({0.8f, 0.8f, 0.8f, 1.0f}, "Connecting...");
    }

    ImGui::EndPopup();
}

// ─────────────────────────────────────────────────────────────────────────────
// UI_ClientList
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::UI_ClientList() {
    ImGui::Begin("Users Online");
    ImGui::Text("Online: %d", static_cast<int>(m_ConnectedClients.size()));
    ImGui::Separator();

    constexpr float kIconSize = 22.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (const auto& [username, info] : m_ConnectedClients) {
        if (username.empty()) continue;

        bool isOurs = (username == m_Username);

        ImVec2 pos    = ImGui::GetCursorScreenPos();
        float  radius = kIconSize * 0.45f;
        ImVec2 center = {pos.x + kIconSize * 0.5f, pos.y + kIconSize * 0.5f};

        DrawIconShape(dl, center, radius, info.IconIndex);
        ImGui::Dummy({kIconSize, kIconSize});

        bool iconClicked = !isOurs
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

        if (iconClicked &&
            !m_PrivateChats.contains(username) &&
            !m_PendingOutgoingInvites.contains(username)) {
            SendPrivateChatInvite(username);
            m_PendingOutgoingInvites.insert(username);
            m_Console.AddItalicMessage("Invited {} to a private chat.", username);
        }
    }

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// UI_IncomingInvites
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::UI_IncomingInvites() {
    if (m_IncomingInvites.empty()) return;

    auto& invite = m_IncomingInvites.front();
    std::string popupId = "Private Chat Request##" + invite.FromUsername;

    ImGui::OpenPopup(popupId.c_str());
    bool open = true;
    if (ImGui::BeginPopupModal(popupId.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s wants to chat with you privately.", invite.FromUsername.c_str());
        ImGui::Separator();

        if (ImGui::Button("Accept", {120, 0})) {
            StartPrivateChatAsResponder(invite.FromUsername);
            m_IncomingInvites.erase(m_IncomingInvites.begin());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", {120, 0})) {
            SendPrivateChatResponse(invite.FromUsername, false);
            m_IncomingInvites.erase(m_IncomingInvites.begin());
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    if (!open) {
        // User closed the window via X — treat as a decline so the inviter
        // is unblocked and the server clears the pending entry.
        SendPrivateChatResponse(invite.FromUsername, false);
        m_IncomingInvites.erase(m_IncomingInvites.begin());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UI_PrivateChatWindows
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::UI_PrivateChatWindows() {
    std::vector<std::string> toRemove;
    for (auto& [peer, session] : m_PrivateChats) {
        bool alive = session->OnUIRender(m_Username, m_Color);
        if (!alive || session->IsClosed())
            toRemove.push_back(peer);
    }
    for (const auto& p : toRemove) m_PrivateChats.erase(p);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server callbacks
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::OnConnected() { m_Console.ClearLog(); }

void ClientLayer::OnDisconnected() {
    m_Console.AddItalicMessageWithColor(0xFF8A8A8A, "Lost connection to server!");
    m_IncomingInvites.clear();
    m_PendingOutgoingInvites.clear();
}

void ClientLayer::OnDataReceived(const Safira::Buffer buffer) {
    Safira::BufferStreamReader stream(buffer);
    PacketType type;
    stream.ReadRaw<PacketType>(type);

    switch (type) {
    case PacketType::Message: {
        std::string from, message;
        stream.ReadString(from);
        stream.ReadString(message);
        uint32_t col = m_ConnectedClients.contains(from)
                       ? m_ConnectedClients.at(from).Color : 0xFFFFFFFF;
        if (from == "SERVER") col = 0xFFFFFFFF;
        m_Console.AddTaggedMessageWithColor(col, from, message);
        break;
    }
    case PacketType::ClientConnectionRequest: {
        bool ok; stream.ReadRaw<bool>(ok);
        if (ok) m_ShowSuccessfulConnectionMessage = true;
        else    m_Console.AddItalicMessageWithColor(0xFFFA4A4A,
                    "Server rejected connection with username {}", m_Username);
        break;
    }
    case PacketType::ConnectionStatus: break;
    case PacketType::ClientList: {
        std::vector<Safira::UserInfo> list; stream.ReadArray(list);
        m_ConnectedClients.clear();
        for (auto& u : list) m_ConnectedClients[u.Username] = u;
        break;
    }
    case PacketType::ClientConnect: {
        Safira::UserInfo u; stream.ReadObject(u);
        m_ConnectedClients[u.Username] = u;
        m_Console.AddItalicMessageWithColor(u.Color, "Welcome {}!", u.Username);
        break;
    }
    case PacketType::ClientUpdate: break;
    case PacketType::ClientDisconnect: {
        Safira::UserInfo u; stream.ReadObject(u);
        m_ConnectedClients.erase(u.Username);
        m_PrivateChats.erase(u.Username);
        m_PendingOutgoingInvites.erase(u.Username);
        m_Console.AddItalicMessageWithColor(u.Color, "Goodbye {}!", u.Username);
        break;
    }
    case PacketType::ClientUpdateResponse: break;
    case PacketType::MessageHistory: {
        std::vector<Safira::ChatMessage> hist; stream.ReadArray(hist);
        for (auto& m : hist) {
            uint32_t col = m_ConnectedClients.contains(m.Username)
                           ? m_ConnectedClients.at(m.Username).Color : 0xFFFFFFFF;
            m_Console.AddTaggedMessageWithColor(col, m.Username, m.Message);
        }
        if (m_ShowSuccessfulConnectionMessage) {
            m_ShowSuccessfulConnectionMessage = false;
            m_Console.AddItalicMessageWithColor(0xFF8A8A8A,
                "Successfully connected to {} with username {}", m_ServerIP, m_Username);
        }
        break;
    }
    case PacketType::ServerShutdown:
        m_Console.AddItalicMessage("Server is shutting down... goodbye!");
        m_Client->RequestDisconnect();
        break;
    case PacketType::ClientKick: {
        m_Console.AddItalicMessage("You have been kicked by server!");
        std::string r; stream.ReadString(r);
        if (!r.empty()) m_Console.AddItalicMessage("Reason: {}", r);
        m_Client->RequestDisconnect();
        break;
    }

    // ── Private chat ─────────────────────────────────────────────────────────
    case PacketType::PrivateChatInvite: {
        std::string from; stream.ReadString(from);
        bool alreadyHave = m_PrivateChats.contains(from) ||
            std::any_of(m_IncomingInvites.begin(), m_IncomingInvites.end(),
                [&](const IncomingInvite& i){ return i.FromUsername == from; });
        if (!alreadyHave) m_IncomingInvites.push_back({from});
        break;
    }
    case PacketType::PrivateChatConnectTo: {
        std::string peer, address;
        stream.ReadString(peer);
        stream.ReadString(address);
        m_PendingOutgoingInvites.erase(peer);
        StartPrivateChatAsInitiator(peer, address);
        break;
    }
    case PacketType::PrivateChatDeclined: {
        std::string peer; stream.ReadString(peer);
        m_PendingOutgoingInvites.erase(peer);
        m_Console.AddItalicMessage("{} declined your private chat request.", peer);
        break;
    }

    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// P2P helpers
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::SendPrivateChatInvite(const std::string& targetUsername) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::PrivateChatInvite);
    stream.WriteString(targetUsername);
    m_Client->SendBuffer(stream.GetBuffer());
}

void ClientLayer::SendPrivateChatResponse(const std::string& toUsername, bool accepted) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::PrivateChatResponse);
    stream.WriteString(toUsername);
    stream.WriteRaw<bool>(accepted);
    stream.WriteRaw<uint16_t>(0);
    m_Client->SendBuffer(stream.GetBuffer());
}

void ClientLayer::StartPrivateChatAsResponder(const std::string& peerUsername) {
    auto session = std::make_unique<Safira::PrivateChatSession>(peerUsername);
    uint16_t port = session->StartAsResponder("server.pem", "server-key.pem");
    if (port == 0) {
        m_Console.AddItalicMessage("Failed to start P2P listener for {}", peerUsername);
        return;
    }
    m_PrivateChats[peerUsername] = std::move(session);

    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::PrivateChatResponse);
    stream.WriteString(peerUsername);
    stream.WriteRaw<bool>(true);
    stream.WriteRaw<uint16_t>(port);
    m_Client->SendBuffer(stream.GetBuffer());
    m_Console.AddItalicMessage("Accepted private chat with {}. Waiting for connection...", peerUsername);
}

void ClientLayer::StartPrivateChatAsInitiator(const std::string& peerUsername,
                                              const std::string& peerAddress) {
    auto session = std::make_unique<Safira::PrivateChatSession>(peerUsername);
    session->StartAsInitiator(peerAddress);
    m_PrivateChats[peerUsername] = std::move(session);
    m_Console.AddItalicMessage("Connecting to {} for private chat...", peerUsername);
}

// ─────────────────────────────────────────────────────────────────────────────
// Chat / persistence
// ─────────────────────────────────────────────────────────────────────────────
void ClientLayer::SendChatMessage(std::string_view message) {
    std::string msg(message);
    if (IsValidMessage(msg)) {
        Safira::BufferStreamWriter stream(m_ScratchBuffer);
        stream.WriteRaw<PacketType>(PacketType::Message);
        stream.WriteString(msg);
        m_Client->SendBuffer(stream.GetBuffer());
        m_Console.AddTaggedMessageWithColor(m_Color | 0xFF000000, m_Username, msg);
    }
}

void ClientLayer::SaveConnectionDetails(const std::filesystem::path& filepath) {
    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "ConnectionDetails" << YAML::Value << YAML::BeginMap
        << YAML::Key << "Username"  << YAML::Value << m_Username
        << YAML::Key << "Color"     << YAML::Value << m_Color
        << YAML::Key << "IconIndex" << YAML::Value << static_cast<int>(m_IconIndex)
        << YAML::Key << "ServerIP"  << YAML::Value << m_ServerIP
        << YAML::EndMap << YAML::EndMap;
    std::ofstream fout(filepath);
    fout << out.c_str();
}

bool ClientLayer::LoadConnectionDetails(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) return false;
    YAML::Node data;
    try { data = YAML::LoadFile(filepath.string()); }
    catch (YAML::ParserException& e) {
        std::cout << "[ERROR] " << e.what() << "\n"; return false;
    }
    auto root = data["ConnectionDetails"];
    if (!root) return false;

    m_Username  = root["Username"].as<std::string>();
    m_Color     = root["Color"].as<uint32_t>();
    m_ServerIP  = root["ServerIP"].as<std::string>();
    m_IconIndex = static_cast<uint8_t>(root["IconIndex"].as<int>(0));

    ImVec4 c = ImColor(m_Color).Value;
    m_ColorBuffer[0] = c.x; m_ColorBuffer[1] = c.y;
    m_ColorBuffer[2] = c.z; m_ColorBuffer[3] = c.w;
    return true;
}