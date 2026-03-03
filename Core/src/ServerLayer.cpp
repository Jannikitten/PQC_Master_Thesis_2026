#include "ServerLayer.h"
#include "ServerPacket.h"
#include "SafiraAssert.h"
#include "StringUtils.h"

// ═════════════════════════════════════════════════════════════════════════════
// ServerLayer.cpp
//
// §3.2   Result types     – std::optional for FindClientID, no bare sentinels
// §3.4   Strong types     – ClientID is a struct, printed via .Value
// §5.3   Serialization    – BufferWriter + SerializePacket (concept-based)
// C++23                   – ranges, string_view, structured bindings, std::visit
// ═════════════════════════════════════════════════════════════════════════════

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <fstream>
#include <print>
#include <ranges>

#include <spdlog/spdlog.h>

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::OnAttach() {
    constexpr uint16_t Port = 8192;
    m_ScratchBuffer.resize(8192);

    m_Server = std::make_unique<Safira::Server>(Port);

    m_Server->OnClientConnected(
        [this](Safira::ClientInfo& c) { OnClientConnected(c); });
    m_Server->OnClientDisconnected(
        [this](Safira::ClientInfo& c) { OnClientDisconnected(c); });
    m_Server->OnDataReceived(
        [this](Safira::ClientInfo& c, Safira::ByteSpan d) { OnDataReceived(c, d); });
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
}

void ServerLayer::OnUpdate(float ts) {
    m_ClientListTimer -= ts;
    if (m_ClientListTimer < 0) {
        m_ClientListTimer = kClientListInterval;
        SendClientListToAllClients();
        SaveMessageHistoryToFile(m_MessageHistoryFilePath);
    }
}

void ServerLayer::OnUIRender() { m_Console.OnUIRender(); }

// ═════════════════════════════════════════════════════════════════════════════
// Server event callbacks
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::OnClientConnected(Safira::ClientInfo&) {
    // Full registration deferred to ConnectionRequestPacket.
}

void ServerLayer::OnClientDisconnected(Safira::ClientInfo& clientInfo) {
    if (!m_ConnectedClients.contains(clientInfo.ID)) {
        spdlog::warn("OnClientDisconnected — unknown client ID={} addr={}",
                     clientInfo.ID.Value, clientInfo.AddressStr);
        return;
    }

    const std::string username = m_ConnectedClients.at(clientInfo.ID).Username;

    CleanupPendingInvites(clientInfo.ID, username);

    SendClientDisconnect(clientInfo);
    m_Console.AddItalicMessage("Client {} disconnected", username);
    m_ConnectedClients.erase(clientInfo.ID);
}

void ServerLayer::CleanupPendingInvites(Safira::ClientID disconnectedID,
                                        const std::string& disconnectedUsername) {
    if (auto it = m_PendingPrivateChatInvites.find(disconnectedUsername);
        it != m_PendingPrivateChatInvites.end()) {
        ForwardPrivateChatDeclined(it->second, disconnectedUsername);
        m_PendingPrivateChatInvites.erase(it);
    }

    std::erase_if(m_PendingPrivateChatInvites,
        [&](const auto& pair) { return pair.second == disconnectedID; });
}

// ═════════════════════════════════════════════════════════════════════════════
// OnDataReceived — variant dispatch via std::visit
//
// §3.4  Typestate  – compile-time exhaustive via Overloaded visitor
// §5.3  Serialization – DeserializeServerPacket returns expected<variant>
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::OnDataReceived(Safira::ClientInfo& clientInfo,
                                 Safira::ByteSpan data) {
    Safira::BufferReader reader(data);
    auto packet = Safira::DeserializeServerPacket(reader);
    if (!packet) {
        spdlog::warn("[server] packet parse error: {}",
                     Safira::Describe(packet.error()));
        return;
    }

    std::visit(Safira::Overloaded{
        [&](const Safira::MessagePacket& pkt) {
            if (!m_ConnectedClients.contains(clientInfo.ID)) {
                m_Console.AddMessage("Rejected data from unregistered client ID={} addr={}",
                                    clientInfo.ID.Value, clientInfo.AddressStr);
                return;
            }
            std::string message = pkt.Message;
            if (Safira::IsValidMessage(message)) {
                const auto& client = m_ConnectedClients.at(clientInfo.ID);
                m_MessageHistory.emplace_back(client.Username, message);
                m_Console.AddTaggedMessageWithColor(
                    client.Color | 0xFF000000, client.Username, message);
                SendMessageToAllClients(clientInfo, message);
            }
        },
        [&](const Safira::ConnectionRequestPacket& pkt) {
            const bool isValid = IsValidUsername(pkt.Username);
            SendClientConnectionRequestResponse(clientInfo, isValid);

            if (isValid) {
                m_Console.AddMessage("Welcome {} (icon={}) from {}",
                                    pkt.Username, pkt.IconIndex, clientInfo.AddressStr);
                auto& client     = m_ConnectedClients[clientInfo.ID];
                client.Username  = pkt.Username;
                client.Color     = pkt.Color;
                client.IconIndex = pkt.IconIndex;

                SendClientConnect(clientInfo);
                SendClientList(clientInfo);
                SendMessageHistory(clientInfo);
            } else {
                m_Console.AddMessage("Connection rejected: username='{}' addr={}",
                                    pkt.Username, clientInfo.AddressStr);
            }
        },
        [&](const Safira::PrivateChatInvitePacket& pkt) {
            if (!m_ConnectedClients.contains(clientInfo.ID)) return;
            const auto& fromUsername = m_ConnectedClients.at(clientInfo.ID).Username;

            auto targetID = FindClientID(pkt.Username);
            if (!targetID) {
                m_Console.AddMessage("PrivateChatInvite: target '{}' not found", pkt.Username);
                return;
            }

            m_PendingPrivateChatInvites[pkt.Username] = clientInfo.ID;
            ForwardPrivateChatInvite(*targetID, fromUsername);
            m_Console.AddMessage("{} invited {} to a private chat",
                                fromUsername, pkt.Username);
        },
        [&](const Safira::PrivateChatResponsePacket& pkt) {
            if (!m_ConnectedClients.contains(clientInfo.ID)) return;
            const auto& responderUsername = m_ConnectedClients.at(clientInfo.ID).Username;

            auto initiatorID = FindClientID(pkt.Username);
            m_PendingPrivateChatInvites.erase(responderUsername);

            if (!pkt.Accepted || !initiatorID) {
                if (initiatorID)
                    ForwardPrivateChatDeclined(*initiatorID, responderUsername);
                m_Console.AddMessage("{} declined private chat with {}",
                                    responderUsername, pkt.Username);
                return;
            }

            const auto& addrStr = clientInfo.AddressStr;
            const auto  colonPos = addrStr.rfind(':');
            std::string p2pAddress = std::string(addrStr.substr(0, colonPos))
                                   + ":" + std::to_string(pkt.ListenPort);

            ForwardPrivateChatConnectTo(*initiatorID, responderUsername, p2pAddress);
            m_Console.AddMessage("{} accepted private chat with {} on {}",
                                responderUsername, pkt.Username, p2pAddress);
        },
    }, *packet);
}

// ═════════════════════════════════════════════════════════════════════════════
// Private chat forwarding — BufferWriter + SerializePacket
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::ForwardPrivateChatInvite(Safira::ClientID targetID,
                                           const std::string& fromUsername) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::PrivateChatInvitePacket{ fromUsername });
    m_Server->SendToClient(targetID, writer.Written());
}

void ServerLayer::ForwardPrivateChatConnectTo(Safira::ClientID initiatorID,
                                              const std::string& responderUsername,
                                              const std::string& responderIPAndPort) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::PrivateChatConnectToPacket{
        responderUsername, responderIPAndPort });
    m_Server->SendToClient(initiatorID, writer.Written());
}

void ServerLayer::ForwardPrivateChatDeclined(Safira::ClientID initiatorID,
                                             const std::string& responderUsername) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::PrivateChatDeclinedPacket{ responderUsername });
    m_Server->SendToClient(initiatorID, writer.Written());
}

// ═════════════════════════════════════════════════════════════════════════════
// Send helpers — BufferWriter + SerializePacket
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::SendClientList(const Safira::ClientInfo& clientInfo) {
    auto values = m_ConnectedClients | std::views::values;
    std::vector<Safira::UserInfo> list(values.begin(), values.end());

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ClientListPacket{ std::move(list) });
    m_Server->SendToClient(clientInfo.ID, writer.Written());
}

void ServerLayer::SendClientListToAllClients() {
    auto values = m_ConnectedClients | std::views::values;
    std::vector<Safira::UserInfo> list(values.begin(), values.end());

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ClientListPacket{ std::move(list) });
    m_Server->SendToAllClients(writer.Written());
}

void ServerLayer::SendClientConnect(const Safira::ClientInfo& newClient) {
    WL_VERIFY(m_ConnectedClients.contains(newClient.ID));

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ClientConnectPacket{
        m_ConnectedClients.at(newClient.ID) });
    m_Server->SendToAllClients(writer.Written(), newClient.ID);
}

void ServerLayer::SendClientDisconnect(const Safira::ClientInfo& clientInfo) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ClientDisconnectPacket{
        m_ConnectedClients.at(clientInfo.ID) });
    m_Server->SendToAllClients(writer.Written(), clientInfo.ID);
}

void ServerLayer::SendClientConnectionRequestResponse(const Safira::ClientInfo& clientInfo,
                                                      bool response) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ConnectionResponsePacket{ response });
    m_Server->SendToClient(clientInfo.ID, writer.Written());
}

void ServerLayer::SendMessageToAllClients(const Safira::ClientInfo& from,
                                          std::string_view message) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ServerMessagePacket{
        GetClientUsername(from.ID), std::string(message) });
    m_Server->SendToAllClients(writer.Written(), from.ID);
}

void ServerLayer::SendMessageHistory(const Safira::ClientInfo& clientInfo) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::MessageHistoryPacket{ m_MessageHistory });
    m_Server->SendToClient(clientInfo.ID, writer.Written());
}

void ServerLayer::SendServerShutdownToAllClients() {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ServerShutdownPacket{});
    m_Server->SendToAllClients(writer.Written());
}

void ServerLayer::SendClientKick(const Safira::ClientInfo& clientInfo,
                                 std::string_view reason) {
    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ClientKickPacket{ std::string(reason) });
    m_Server->SendToClient(clientInfo.ID, writer.Written());
}

// ═════════════════════════════════════════════════════════════════════════════
// Commands
// ═════════════════════════════════════════════════════════════════════════════

bool ServerLayer::KickUser(std::string_view username, std::string_view reason) {
    auto found = FindClientID(std::string(username));
    if (!found) return false;

    Safira::ClientInfo clientInfo;
    clientInfo.ID = *found;
    SendClientKick(clientInfo, reason);
    m_Server->KickClient(*found);
    return true;
}

void ServerLayer::Quit() {
    SendServerShutdownToAllClients();
    m_Server->Stop();
}

void ServerLayer::SendChatMessage(std::string_view message) {
    if (!message.empty() && message[0] == '/') {
        OnCommand(message);
        return;
    }

    Safira::BufferWriter writer(m_ScratchBuffer);
    Safira::SerializePacket(writer, Safira::ServerMessagePacket{
        "SERVER", std::string(message) });
    m_Server->SendToAllClients(writer.Written());

    m_Console.AddTaggedMessage("SERVER", message);
    m_MessageHistory.emplace_back("SERVER", std::string(message));
}

void ServerLayer::OnCommand(std::string_view command) {
    if (command.size() < 2 || command[0] != '/') return;

    auto commandStr = command.substr(1);
    auto tokens = Safira::Utils::SplitString(commandStr, ' ');

    if (tokens[0] == "kick") {
        if (tokens.size() == 2 || tokens.size() == 3) {
            std::string reason = tokens.size() == 3 ? std::string(tokens[2]) : "";
            if (KickUser(tokens[1], reason)) {
                m_Console.AddItalicMessage("User {} has been kicked.", tokens[1]);
                if (!reason.empty())
                    m_Console.AddItalicMessage("  Reason: {}", reason);
            } else {
                m_Console.AddItalicMessage("Could not kick user {}; not found.", tokens[1]);
            }
        } else {
            m_Console.AddItalicMessage("Usage: /kick <username> [reason]");
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Helpers
// ═════════════════════════════════════════════════════════════════════════════

bool ServerLayer::IsValidUsername(const std::string& username) const {
    return std::ranges::none_of(
        m_ConnectedClients | std::views::values,
        [&](const Safira::UserInfo& info) { return info.Username == username; });
}

const std::string& ServerLayer::GetClientUsername(Safira::ClientID id) const {
    WL_VERIFY(m_ConnectedClients.contains(id));
    return m_ConnectedClients.at(id).Username;
}

uint32_t ServerLayer::GetClientColor(Safira::ClientID id) const {
    WL_VERIFY(m_ConnectedClients.contains(id));
    return m_ConnectedClients.at(id).Color;
}

std::optional<Safira::ClientID> ServerLayer::FindClientID(const std::string& username) const {
    auto it = std::ranges::find_if(
        m_ConnectedClients,
        [&](const auto& pair) { return pair.second.Username == username; });

    if (it != m_ConnectedClients.end())
        return it->first;

    return std::nullopt;
}

// ═════════════════════════════════════════════════════════════════════════════
// Persistence
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::SaveMessageHistoryToFile(const std::filesystem::path& filepath) {
    YAML::Emitter out;
    out << YAML::BeginMap
        << YAML::Key << "MessageHistory"
        << YAML::Value << YAML::BeginSeq;

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
    try {
        data = YAML::LoadFile(filepath.string());
    } catch (const YAML::ParserException& e) {
        spdlog::error("failed to parse {}: {}", filepath.string(), e.what());
        return false;
    }

    auto root = data["MessageHistory"];
    if (!root) return false;

    m_MessageHistory.reserve(root.size());
    for (const auto& node : root) {
        m_MessageHistory.emplace_back(
            node["User"].as<std::string>(),
            node["Message"].as<std::string>());
    }
    return true;
}
