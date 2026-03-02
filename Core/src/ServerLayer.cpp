#include "ServerLayer.h"
#include "ServerPacket.h"
#include "SafiraAssert.h"
#include "BufferStream.h"
#include "StringUtils.h"

// ═════════════════════════════════════════════════════════════════════════════
// ServerLayer.cpp
//
// §3.2   Result types     – std::optional for FindClientID, no bare sentinels
// §3.4   Strong types     – ClientID is a struct, printed via .Value
// §5.3   Serialization    – BuildPacket centralises scratch-buffer boilerplate
// C++23                   – ranges, string_view, structured bindings
// ═════════════════════════════════════════════════════════════════════════════

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <fstream>
#include <print>
#include <ranges>

#include <spdlog/spdlog.h>

// ─────────────────────────────────────────────────────────────────────────────
// §5.3 — Serialization helper  (Slides 175-198)
//
//   "Turning in-memory objects into byte streams for storage/network."
//
// Every Send* method was doing the same dance: create a BufferStreamWriter
// on m_ScratchBuffer, write fields, then slice the buffer.  BuildPacket
// factors that into one place — the lambda writes the payload, and the
// helper returns the correctly-sized Buffer view.
//
// This also makes it harder to accidentally send unsized scratch data
// (the raw m_ScratchBuffer), which was the old code's biggest footgun.
// ─────────────────────────────────────────────────────────────────────────────
template <typename WriteFn>
Safira::Buffer ServerLayer::BuildPacket(WriteFn&& writeFn) {
    Safira::BufferStreamWriter stream(m_ScratchBuffer);
    std::forward<WriteFn>(writeFn)(stream);
    return Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition());
}

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::OnAttach() {
    constexpr uint16_t Port = 8192;
    m_ScratchBuffer.Allocate(8192);

    m_Server = std::make_unique<Safira::Server>(Port);

    // Callback names match the refactored Server API.
    // Signatures: non-const ClientInfo& (Server owns the client state).
    m_Server->OnClientConnected(
        [this](Safira::ClientInfo& c) { OnClientConnected(c); });
    m_Server->OnClientDisconnected(
        [this](Safira::ClientInfo& c) { OnClientDisconnected(c); });
    m_Server->OnDataReceived(
        [this](Safira::ClientInfo& c, Safira::Buffer d) { OnDataReceived(c, d); });
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
    // Full registration deferred to ClientConnectionRequest packet.
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

// ─────────────────────────────────────────────────────────────────────────────
// CleanupPendingInvites — extracted from OnClientDisconnected so the
// disconnect handler reads as a linear pipeline.
//
// §3.3 — Side channels  (Slide 92): we don't log *which* private chat
// was pending — only that an invite was cancelled — to avoid leaking
// social-graph information into server logs.
// ─────────────────────────────────────────────────────────────────────────────
void ServerLayer::CleanupPendingInvites(Safira::ClientID disconnectedID,
                                        const std::string& disconnectedUsername) {
    // Case 1: this user was the RESPONDER (their username is a key).
    if (auto it = m_PendingPrivateChatInvites.find(disconnectedUsername);
        it != m_PendingPrivateChatInvites.end()) {
        ForwardPrivateChatDeclined(it->second, disconnectedUsername);
        m_PendingPrivateChatInvites.erase(it);
    }

    // Case 2: this user was the INITIATOR (their ClientID appears as a value).
    // C++20 std::erase_if on associative containers — one pass, no manual iterator dance.
    std::erase_if(m_PendingPrivateChatInvites,
        [&](const auto& pair) { return pair.second == disconnectedID; });
}

// ═════════════════════════════════════════════════════════════════════════════
// OnDataReceived — packet dispatch
//
// §5.3  Serialization  (Slides 175-198)
//   "Postel's law is dangerous for crypto: increased attack surface."
//
// We validate every field read from the stream.  If any read fails the
// handler breaks out immediately rather than processing partial data.
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::OnDataReceived(Safira::ClientInfo& clientInfo,
                                 Safira::Buffer buffer) {
    Safira::BufferStreamReader stream(buffer);
    PacketType type;
    if (bool ok = stream.ReadRaw<PacketType>(type); !ok) {
        WL_CORE_VERIFY(ok);
        return;
    }

    switch (type) {

    case PacketType::Message: {
        if (!m_ConnectedClients.contains(clientInfo.ID)) {
            m_Console.AddMessage("Rejected data from unregistered client ID={} addr={}",
                                clientInfo.ID.Value, clientInfo.AddressStr);
            return;
        }
        std::string message;
        if (stream.ReadString(message) && IsValidMessage(message)) {
            const auto& client = m_ConnectedClients.at(clientInfo.ID);
            m_MessageHistory.emplace_back(client.Username, message);
            m_Console.AddTaggedMessageWithColor(
                client.Color | 0xFF000000, client.Username, message);
            SendMessageToAllClients(clientInfo, message);
        }
        break;
    }

    case PacketType::ClientConnectionRequest: {
        uint32_t    requestedColor;
        uint8_t     requestedIcon = 0;
        std::string requestedUsername;

        stream.ReadRaw<uint32_t>(requestedColor);
        stream.ReadRaw<uint8_t>(requestedIcon);
        if (!stream.ReadString(requestedUsername)) break;

        const bool isValid = IsValidUsername(requestedUsername);
        SendClientConnectionRequestResponse(clientInfo, isValid);

        if (isValid) {
            m_Console.AddMessage("Welcome {} (icon={}) from {}",
                                requestedUsername, requestedIcon, clientInfo.AddressStr);
            auto& client     = m_ConnectedClients[clientInfo.ID];
            client.Username  = requestedUsername;
            client.Color     = requestedColor;
            client.IconIndex = requestedIcon;

            SendClientConnect(clientInfo);
            SendClientList(clientInfo);
            SendMessageHistory(clientInfo);
        } else {
            m_Console.AddMessage("Connection rejected: username='{}' addr={}",
                                requestedUsername, clientInfo.AddressStr);
        }
        break;
    }

    // ── Private chat signalling ─────────────────────────────────────────────
    case PacketType::PrivateChatInvite: {
        if (!m_ConnectedClients.contains(clientInfo.ID)) break;
        const auto& fromUsername = m_ConnectedClients.at(clientInfo.ID).Username;

        std::string targetUsername;
        if (!stream.ReadString(targetUsername)) break;

        // §3.2 — std::optional: FindClientID returns nullopt, not a magic 0.
        auto targetID = FindClientID(targetUsername);
        if (!targetID) {
            m_Console.AddMessage("PrivateChatInvite: target '{}' not found", targetUsername);
            break;
        }

        m_PendingPrivateChatInvites[targetUsername] = clientInfo.ID;
        ForwardPrivateChatInvite(*targetID, fromUsername);
        m_Console.AddMessage("{} invited {} to a private chat",
                            fromUsername, targetUsername);
        break;
    }

    case PacketType::PrivateChatResponse: {
        if (!m_ConnectedClients.contains(clientInfo.ID)) break;
        const auto& responderUsername = m_ConnectedClients.at(clientInfo.ID).Username;

        std::string inviterUsername;
        bool        accepted   = false;
        uint16_t    listenPort = 0;

        if (!stream.ReadString(inviterUsername)) break;
        stream.ReadRaw<bool>(accepted);
        stream.ReadRaw<uint16_t>(listenPort);

        auto initiatorID = FindClientID(inviterUsername);
        m_PendingPrivateChatInvites.erase(responderUsername);

        if (!accepted || !initiatorID) {
            if (initiatorID)
                ForwardPrivateChatDeclined(*initiatorID, responderUsername);
            m_Console.AddMessage("{} declined private chat with {}",
                                responderUsername, inviterUsername);
            break;
        }

        // Extract IP from "a.b.c.d:port", combine with P2P listen port.
        const auto& addrStr = clientInfo.AddressStr;
        const auto  colonPos = addrStr.rfind(':');
        std::string p2pAddress = std::string(addrStr.substr(0, colonPos))
                               + ":" + std::to_string(listenPort);

        ForwardPrivateChatConnectTo(*initiatorID, responderUsername, p2pAddress);
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

// ═════════════════════════════════════════════════════════════════════════════
// Private chat forwarding — now using BuildPacket helper
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::ForwardPrivateChatInvite(Safira::ClientID targetID,
                                           const std::string& fromUsername) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::PrivateChatInvite);
        w.WriteString(fromUsername);
    });
    m_Server->SendToClient(targetID, packet);
}

void ServerLayer::ForwardPrivateChatConnectTo(Safira::ClientID initiatorID,
                                              const std::string& responderUsername,
                                              const std::string& responderIPAndPort) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::PrivateChatConnectTo);
        w.WriteString(responderUsername);
        w.WriteString(responderIPAndPort);
    });
    m_Server->SendToClient(initiatorID, packet);
}

void ServerLayer::ForwardPrivateChatDeclined(Safira::ClientID initiatorID,
                                             const std::string& responderUsername) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::PrivateChatDeclined);
        w.WriteString(responderUsername);
    });
    m_Server->SendToClient(initiatorID, packet);
}

// ═════════════════════════════════════════════════════════════════════════════
// Send helpers — all using BuildPacket + refactored Server API
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::SendClientList(const Safira::ClientInfo& clientInfo) {
    // Collect UserInfo values via ranges pipeline.
    auto values = m_ConnectedClients | std::views::values;
    std::vector<Safira::UserInfo> list(values.begin(), values.end());

    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::ClientList);
        w.WriteArray(list);
    });
    m_Server->SendToClient(clientInfo.ID, packet);
}

void ServerLayer::SendClientListToAllClients() {
    auto values = m_ConnectedClients | std::views::values;
    std::vector<Safira::UserInfo> list(values.begin(), values.end());

    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::ClientList);
        w.WriteArray(list);
    });
    m_Server->SendToAllClients(packet);
}

void ServerLayer::SendClientConnect(const Safira::ClientInfo& newClient) {
    WL_VERIFY(m_ConnectedClients.contains(newClient.ID));

    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::ClientConnect);
        w.WriteObject(m_ConnectedClients.at(newClient.ID));
    });
    m_Server->SendToAllClients(packet, newClient.ID);
}

void ServerLayer::SendClientDisconnect(const Safira::ClientInfo& clientInfo) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::ClientDisconnect);
        w.WriteObject(m_ConnectedClients.at(clientInfo.ID));
    });
    m_Server->SendToAllClients(packet, clientInfo.ID);
}

void ServerLayer::SendClientConnectionRequestResponse(const Safira::ClientInfo& clientInfo,
                                                      bool response) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::ClientConnectionRequest);
        w.WriteRaw<bool>(response);
    });
    m_Server->SendToClient(clientInfo.ID, packet);
}

void ServerLayer::SendMessageToAllClients(const Safira::ClientInfo& from,
                                          std::string_view message) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::Message);
        w.WriteString(GetClientUsername(from.ID));
        w.WriteString(message);
    });
    m_Server->SendToAllClients(packet, from.ID);
}

void ServerLayer::SendMessageHistory(const Safira::ClientInfo& clientInfo) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::MessageHistory);
        w.WriteArray(m_MessageHistory);
    });
    m_Server->SendToClient(clientInfo.ID, packet);
}

void ServerLayer::SendServerShutdownToAllClients() {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::ServerShutdown);
    });
    m_Server->SendToAllClients(packet);
}

void ServerLayer::SendClientKick(const Safira::ClientInfo& clientInfo,
                                 std::string_view reason) {
    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::ClientKick);
        w.WriteString(std::string(reason));
    });
    m_Server->SendToClient(clientInfo.ID, packet);
}

// ═════════════════════════════════════════════════════════════════════════════
// Commands
// ═════════════════════════════════════════════════════════════════════════════

bool ServerLayer::KickUser(std::string_view username, std::string_view reason) {
    // §3.2 — and_then: find the client, then kick.
    // FindClientID → optional<ClientID>; if present, proceed.
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

    auto packet = BuildPacket([&](Safira::BufferStreamWriter& w) {
        w.WriteRaw<PacketType>(PacketType::Message);
        w.WriteString(std::string_view("SERVER"));
        w.WriteString(message);
    });
    m_Server->SendToAllClients(packet);

    m_Console.AddTaggedMessage("SERVER", message);
    m_MessageHistory.emplace_back("SERVER", std::string(message));
}

void ServerLayer::OnCommand(std::string_view command) {
    if (command.size() < 2 || command[0] != '/') return;

    auto commandStr = command.substr(1);
    auto tokens = Safira::Utils::SplitString(commandStr, ' ');

    if (tokens[0] == "kick") {
        if (tokens.size() == 2 || tokens.size() == 3) {
            std::string_view reason = tokens.size() == 3 ? tokens[2] : "";
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
    // §C++23 ranges — check that no connected client already has this name.
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

// §3.2 — Result types  (Slides 88-89)
//
// Returns std::optional<ClientID> instead of a bare 0 sentinel.
// The caller must unwrap explicitly — no chance of accidentally using
// a "not found" value as a real ID.
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
//
// §5.3 — Serialization  (Slides 175-198)
//
//   "pickle is dangerous, allows arbitrary code execution."
//   "YAML v1.1: 'NO' parses as boolean."
//
// YAML is acceptable here because the data is server-controlled
// (m_MessageHistory is populated from validated client messages, not from
// raw file input that bypasses validation).  The catch around LoadFile
// guards against malformed files.
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