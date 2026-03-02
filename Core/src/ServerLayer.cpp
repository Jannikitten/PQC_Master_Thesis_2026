#include "ServerLayer.h"
#include "ServerPacket.h"
#include "SafiraAssert.h"
#include "BufferStream.h"
#include "StringUtils.h"

#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::OnAttach() {
    const int Port = 8192;
    m_ScratchBuffer.Allocate(8192);

    m_Server = std::make_unique<Safira::Server>(Port);
    m_Server->SetClientConnectedCallback(
        [this](const Safira::ClientInfo& c) { OnClientConnected(c); });
    m_Server->SetClientDisconnectedCallback(
        [this](const Safira::ClientInfo& c) { OnClientDisconnected(c); });
    m_Server->SetDataReceivedCallback(
        [this](const Safira::ClientInfo& c, const Safira::Buffer d) { OnDataReceived(c, d); });
    m_Server->Start();

    m_MessageHistoryFilePath = "MessageHistory.yaml";
    m_Console.AddTaggedMessage("Info", "Loading message history...");
    LoadMessageHistoryFromFile(m_MessageHistoryFilePath);
    for (const auto& msg : m_MessageHistory)
        m_Console.AddTaggedMessage(msg.Username, msg.Message);
    m_Console.AddTaggedMessage("Info", "Started server on port {}", Port);

    m_Console.SetMessageSendCallback([this](std::string_view msg) { SendChatMessage(msg); });
}

void ServerLayer::OnDetach() {
    m_Server->Stop();
    m_ScratchBuffer.Release();
}

void ServerLayer::OnUpdate(float ts) {
    m_ClientListTimer -= ts;
    if (m_ClientListTimer < 0) {
        m_ClientListTimer = m_ClientListInterval;
        SendClientListToAllClients();
        SaveMessageHistoryToFile(m_MessageHistoryFilePath);
    }
}

void ServerLayer::OnUIRender() { m_Console.OnUIRender(); }

// ─────────────────────────────────────────────────────────────────────────────
// Server event callbacks
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::OnClientConnected(const Safira::ClientInfo&) {
    // Full registration deferred to ClientConnectionRequest packet
}

void ServerLayer::OnClientDisconnected(const Safira::ClientInfo& clientInfo) {
    if (!m_ConnectedClients.contains(clientInfo.ID)) {
        std::cout << "[WARN] OnClientDisconnected - unknown client ID="
                  << clientInfo.ID << " addr=" << clientInfo.AddressStr << "\n";
        return;
    }

    const std::string username = m_ConnectedClients.at(clientInfo.ID).Username;

    // Cancel any pending private chat invite where this user was the inviter or
    // the invited party, and notify the other side.
    // Case 1: this user was the RESPONDER (their username is a key in the map)
    if (m_PendingPrivateChatInvites.contains(username)) {
        Safira::ClientID initiatorID = m_PendingPrivateChatInvites.at(username);
        ForwardPrivateChatDeclined(initiatorID, username);
        m_PendingPrivateChatInvites.erase(username);
    }
    // Case 2: this user was the INITIATOR (their ClientID appears as a value)
    for (auto it = m_PendingPrivateChatInvites.begin();
         it != m_PendingPrivateChatInvites.end(); ) {
        if (it->second == clientInfo.ID)
            it = m_PendingPrivateChatInvites.erase(it);
        else
            ++it;
    }

    SendClientDisconnect(clientInfo);
    m_Console.AddItalicMessage("Client {} disconnected", username);
    m_ConnectedClients.erase(clientInfo.ID);
}

// ─────────────────────────────────────────────────────────────────────────────
// OnDataReceived
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::OnDataReceived(const Safira::ClientInfo& clientInfo,
                                 const Safira::Buffer buffer) {
    Safira::BufferStreamReader stream(buffer);
    PacketType type;
    bool ok = stream.ReadRaw<PacketType>(type);
    WL_CORE_VERIFY(ok);
    if (!ok) return;

    switch (type) {
    // ── Existing packets ─────────────────────────────────────────────────────
    case PacketType::Message: {
        if (!m_ConnectedClients.contains(clientInfo.ID)) {
            m_Console.AddMessage("Rejected data from unregistered client ID={} addr={}",
                clientInfo.ID, clientInfo.AddressStr);
            return;
        }
        std::string message;
        if (stream.ReadString(message) && IsValidMessage(message)) {
            const auto& client = m_ConnectedClients.at(clientInfo.ID);
            m_MessageHistory.emplace_back(client.Username, message);
            m_Console.AddTaggedMessageWithColor(client.Color | 0xFF000000, client.Username, message);
            SendMessageToAllClients(clientInfo, message);
        }
        break;
    }
    case PacketType::ClientConnectionRequest: {
        uint32_t    requestedColor;
        uint8_t     requestedIcon = 0;
        std::string requestedUsername;

        stream.ReadRaw<uint32_t>(requestedColor);
        stream.ReadRaw<uint8_t>(requestedIcon);          // ← new: read icon index
        if (!stream.ReadString(requestedUsername)) break;

        bool isValid = IsValidUsername(requestedUsername);
        SendClientConnectionRequestResponse(clientInfo, isValid);

        if (isValid) {
            m_Console.AddMessage("Welcome {} (icon={}) from {}",
                requestedUsername, requestedIcon, clientInfo.AddressStr);
            auto& client     = m_ConnectedClients[clientInfo.ID];
            client.Username  = requestedUsername;
            client.Color     = requestedColor;
            client.IconIndex = requestedIcon;            // ← store icon

            SendClientConnect(clientInfo);
            SendClientList(clientInfo);
            SendMessageHistory(clientInfo);
        } else {
            m_Console.AddMessage("Connection rejected: username='{}' addr={}",
                requestedUsername, clientInfo.AddressStr);
        }
        break;
    }

    // ── Private chat signalling ───────────────────────────────────────────────
    case PacketType::PrivateChatInvite: {
        // Client A invites Client B by username
        if (!m_ConnectedClients.contains(clientInfo.ID)) break;
        const std::string& fromUsername = m_ConnectedClients.at(clientInfo.ID).Username;

        std::string targetUsername;
        if (!stream.ReadString(targetUsername)) break;

        Safira::ClientID targetID = FindClientID(targetUsername);
        if (targetID == 0) {
            m_Console.AddMessage("PrivateChatInvite: target '{}' not found", targetUsername);
            break;
        }

        // Record: responder(targetUsername) → initiator(clientInfo.ID)
        m_PendingPrivateChatInvites[targetUsername] = clientInfo.ID;

        ForwardPrivateChatInvite(targetID, fromUsername);
        m_Console.AddMessage("{} invited {} to a private chat", fromUsername, targetUsername);
        break;
    }
    case PacketType::PrivateChatResponse: {
        // Client B responds (accept + port, or decline)
        if (!m_ConnectedClients.contains(clientInfo.ID)) break;
        const std::string& responderUsername = m_ConnectedClients.at(clientInfo.ID).Username;

        std::string inviterUsername;
        bool        accepted = false;
        uint16_t    listenPort = 0;

        if (!stream.ReadString(inviterUsername)) break;
        stream.ReadRaw<bool>(accepted);
        stream.ReadRaw<uint16_t>(listenPort);

        // Look up the initiator's ClientID
        Safira::ClientID initiatorID = FindClientID(inviterUsername);
        m_PendingPrivateChatInvites.erase(responderUsername);

        if (!accepted || initiatorID == 0) {
            if (initiatorID != 0)
                ForwardPrivateChatDeclined(initiatorID, responderUsername);
            m_Console.AddMessage("{} declined private chat with {}", responderUsername, inviterUsername);
            break;
        }

        // Extract B's IP from their UDP connection address ("a.b.c.d:port")
        // then combine with the P2P listen port they told us
        const std::string& addrStr = clientInfo.AddressStr; // "a.b.c.d:mainport"
        std::string ip = addrStr.substr(0, addrStr.rfind(':'));
        std::string p2pAddress = ip + ":" + std::to_string(listenPort);

        ForwardPrivateChatConnectTo(initiatorID, responderUsername, p2pAddress);
        m_Console.AddMessage("{} accepted private chat with {} on {}",
            responderUsername, inviterUsername, p2pAddress);
        break;
    }

    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stubs for unimplemented handlers
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::OnMessageReceived(const Safira::ClientInfo&, std::string_view) {}
void ServerLayer::OnClientConnectionRequest(const Safira::ClientInfo&, uint32_t, std::string_view) {}
void ServerLayer::OnClientUpdate(const Safira::ClientInfo&, uint32_t, std::string_view) {}
void ServerLayer::SendClientUpdateResponse(const Safira::ClientInfo&) {}

// ─────────────────────────────────────────────────────────────────────────────
// Private chat forwarding helpers
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::ForwardPrivateChatInvite(Safira::ClientID targetID,
                                           const std::string& fromUsername) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::PrivateChatInvite);
    stream.WriteString(fromUsername);
    m_Server->SendBufferToClient(targetID,
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::ForwardPrivateChatConnectTo(Safira::ClientID initiatorID,
                                              const std::string& responderUsername,
                                              const std::string& responderIPAndPort) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::PrivateChatConnectTo);
    stream.WriteString(responderUsername);
    stream.WriteString(responderIPAndPort);
    m_Server->SendBufferToClient(initiatorID,
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::ForwardPrivateChatDeclined(Safira::ClientID initiatorID,
                                             const std::string& responderUsername) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::PrivateChatDeclined);
    stream.WriteString(responderUsername);
    m_Server->SendBufferToClient(initiatorID,
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Existing send helpers
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::SendClientList(const Safira::ClientInfo& clientInfo) {
    std::vector<Safira::UserInfo> list;
    list.reserve(m_ConnectedClients.size());
    for (const auto& [id, info] : m_ConnectedClients) list.push_back(info);

    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::ClientList);
    stream.WriteArray(list);
    m_Server->SendBufferToClient(clientInfo.ID,
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendClientListToAllClients() {
    std::vector<Safira::UserInfo> list;
    list.reserve(m_ConnectedClients.size());
    for (const auto& [id, info] : m_ConnectedClients) list.push_back(info);

    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::ClientList);
    stream.WriteArray(list);
    m_Server->SendBufferToAllClients(
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendClientConnect(const Safira::ClientInfo& newClient) {
    WL_VERIFY(m_ConnectedClients.contains(newClient.ID));
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::ClientConnect);
    stream.WriteObject(m_ConnectedClients.at(newClient.ID));
    m_Server->SendBufferToAllClients(
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()), newClient.ID);
}

void ServerLayer::SendClientDisconnect(const Safira::ClientInfo& clientInfo) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::ClientDisconnect);
    stream.WriteObject(m_ConnectedClients.at(clientInfo.ID));
    m_Server->SendBufferToAllClients(
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()), clientInfo.ID);
}

void ServerLayer::SendClientConnectionRequestResponse(const Safira::ClientInfo& clientInfo,
                                                      bool response) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::ClientConnectionRequest);
    stream.WriteRaw<bool>(response);
    m_Server->SendBufferToClient(clientInfo.ID,
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendMessageToAllClients(const Safira::ClientInfo& from,
                                          std::string_view message) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::Message);
    stream.WriteString(GetClientUsername(from.ID));
    stream.WriteString(message);
    m_Server->SendBufferToAllClients(
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()), from.ID);
}

void ServerLayer::SendMessageHistory(const Safira::ClientInfo& clientInfo) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::MessageHistory);
    stream.WriteArray(m_MessageHistory);
    m_Server->SendBufferToClient(clientInfo.ID,
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendServerShutdownToAllClients() {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::ServerShutdown);
    m_Server->SendBufferToAllClients(
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendClientKick(const Safira::ClientInfo& clientInfo,
                                 std::string_view reason) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::ClientKick);
    stream.WriteString(std::string(reason));
    m_Server->SendBufferToClient(clientInfo.ID,
        Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Commands
// ─────────────────────────────────────────────────────────────────────────────
bool ServerLayer::KickUser(std::string_view username, std::string_view reason) {
    for (const auto& [clientID, userInfo] : m_ConnectedClients) {
        if (userInfo.Username == username) {
            Safira::ClientInfo clientInfo;
            clientInfo.ID = clientID;
            SendClientKick(clientInfo, reason);
            m_Server->KickClient(clientID);
            return true;
        }
    }
    return false;
}

void ServerLayer::Quit() {
    SendServerShutdownToAllClients();
    m_Server->Stop();
}

void ServerLayer::SendChatMessage(std::string_view message) {
    if (message[0] == '/') { OnCommand(message); return; }

    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    stream.WriteRaw<PacketType>(PacketType::Message);
    stream.WriteString(std::string_view("SERVER"));
    stream.WriteString(message);
    m_Server->SendBufferToAllClients(stream.GetBuffer());

    m_Console.AddTaggedMessage("SERVER", message);
    m_MessageHistory.emplace_back("SERVER", std::string(message));
}

void ServerLayer::OnCommand(std::string_view command) {
    if (command.size() < 2 || command[0] != '/') return;
    std::string_view commandStr(&command[1], command.size() - 1);
    auto tokens = Safira::Utils::SplitString(commandStr, ' ');

    if (tokens[0] == "kick") {
        if (tokens.size() == 2 || tokens.size() == 3) {
            std::string_view reason = tokens.size() == 3 ? tokens[2] : "";
            if (KickUser(tokens[1], reason)) {
                m_Console.AddItalicMessage("User {} has been kicked.", tokens[1]);
                if (!reason.empty()) m_Console.AddItalicMessage("  Reason: {}", reason);
            } else {
                m_Console.AddItalicMessage("Could not kick user {}; not found.", tokens[1]);
            }
        } else {
            m_Console.AddItalicMessage("Usage: /kick <username> [reason]");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
bool ServerLayer::IsValidUsername(const std::string& username) const {
    for (const auto& [id, client] : m_ConnectedClients)
        if (client.Username == username) return false;
    return true;
}

const std::string& ServerLayer::GetClientUsername(Safira::ClientID id) const {
    WL_VERIFY(m_ConnectedClients.contains(id));
    return m_ConnectedClients.at(id).Username;
}

uint32_t ServerLayer::GetClientColor(Safira::ClientID id) const {
    WL_VERIFY(m_ConnectedClients.contains(id));
    return m_ConnectedClients.at(id).Color;
}

Safira::ClientID ServerLayer::FindClientID(const std::string& username) const {
    for (const auto& [id, info] : m_ConnectedClients)
        if (info.Username == username) return id;
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::SaveMessageHistoryToFile(const std::filesystem::path& filepath) {
    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "MessageHistory" << YAML::Value << YAML::BeginSeq;
    for (const auto& msg : m_MessageHistory) {
        out << YAML::BeginMap
            << YAML::Key << "User"    << YAML::Value << msg.Username
            << YAML::Key << "Message" << YAML::Value << msg.Message
            << YAML::EndMap;
    }
    out << YAML::EndSeq << YAML::EndMap;
    std::ofstream fout(filepath);
    fout << out.c_str();
}

bool ServerLayer::LoadMessageHistoryFromFile(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) return false;
    m_MessageHistory.clear();
    YAML::Node data;
    try { data = YAML::LoadFile(filepath.string()); }
    catch (YAML::ParserException& e) {
        std::cout << "[ERROR] " << e.what() << "\n"; return false;
    }
    auto root = data["MessageHistory"];
    if (!root) return false;
    m_MessageHistory.reserve(root.size());
    for (const auto& node : root)
        m_MessageHistory.emplace_back(node["User"].as<std::string>(),
                                      node["Message"].as<std::string>());
    return true;
}