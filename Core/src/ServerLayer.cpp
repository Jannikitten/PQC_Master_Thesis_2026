#include "ServerLayer.h"
#include "SafiraAssert.h"
#include "StringUtils.h"

// ═════════════════════════════════════════════════════════════════════════════
// ServerLayer.cpp
//
// §3.2   Result types     – std::optional for FindClientID, no bare sentinels
// §3.4   Strong types     – ClientID is a struct, printed via .Value
// §5.3   Serialization    – BufferWriter + SerializePacket (concept-based)
// C++23                   – ranges, string_view, structured bindings, std::visit
//
// ── New in v2 ────────────────────────────────────────────────────────────────
//  - Rate limiting         – per-client message burst tracking, auto-kick
//  - Mute system           – /mute, /unmute commands
//  - Expanded commands     – /list, /stats, /broadcast, /motd, /mute, /unmute
//  - Username validation   – length, character, and reserved-name checks
//  - Flood protection      – auto-kick after sustained spam
//  - MOTD                  – sent to each new client on connect
//
// Types from the Safira library:
//  ChatMessage             (UserInfo.h)    — username + message pair
//  MaxMessageLength        (UserInfo.h)    — wire-format message cap (4096)
//  IsValidMessage()        (ServerPacket.h)— whitespace / length check
// ═════════════════════════════════════════════════════════════════════════════

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <print>
#include <random>
#include <ranges>

#include <spdlog/spdlog.h>

// ─────────────────────────────────────────────────────────────────────────────
// Application-level constants
//
// MaxMessageLength (4096) from UserInfo.h is the wire-format hard cap —
// IsValidMessage() already truncates to it.  The constants below are
// server-policy tunables layered on top.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t kMinUsernameLength   = 2;
static constexpr size_t kMaxUsernameLength   = 24;
static constexpr int    kRateLimitMessages   = 10;     // messages per window
static constexpr float  kRateLimitWindowSec  = 5.0f;   // sliding window
static constexpr int    kFloodKickThreshold  = 3;      // violations before auto-kick

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::OnAttach() {
    m_ScratchBuffer.resize(8192);

    Safira::ServerConfig config;
    config.Port                    = 8192;
    config.MaxClients              = 64;
    config.HandshakeTimeoutSeconds = 10.0f;

    m_Server = std::make_unique<Safira::Server>(config);

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

    m_Console.AddTaggedMessage("Info", "Started server on port {} (max {} clients)",
                               config.Port, config.MaxClients);

    // Display available commands on startup
    m_Console.AddItalicMessage("");
    m_Console.AddItalicMessage("Available server commands:");
    m_Console.AddItalicMessage("  /kick <user> [reason]  — disconnect a user");
    m_Console.AddItalicMessage("  /mute <user>           — silence a user (broadcast to all)");
    m_Console.AddItalicMessage("  /unmute <user>         — restore a user's voice");
    m_Console.AddItalicMessage("  /list                  — show connected clients with uptime");
    m_Console.AddItalicMessage("  /stats                 — server-wide statistics");
    m_Console.AddItalicMessage("  /broadcast <msg>       — send server announcement");
    m_Console.AddItalicMessage("  /motd [msg]            — set/clear message of the day");
    m_Console.AddItalicMessage("  /help                  — show this list again");
    m_Console.AddItalicMessage("");
    m_Console.SetMessageSendCallback([this](std::string_view msg) { SendChatMessage(msg); });
}

void ServerLayer::OnDetach() {
    SendServerShutdownToAllClients();
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

    // Clean up per-client state
    m_RateLimitState.erase(clientInfo.ID);
    m_MutedUsers.erase(username);

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
// Rate limiting
//
// Sliding-window approach: track timestamps of recent messages per client.
// If a client exceeds kRateLimitMessages within kRateLimitWindowSec, the
// message is silently dropped and a violation counter increments.  After
// kFloodKickThreshold violations the client is auto-kicked.
// ═════════════════════════════════════════════════════════════════════════════

bool ServerLayer::IsRateLimited(Safira::ClientID id) {
    auto& state = m_RateLimitState[id];
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::duration<float>(kRateLimitWindowSec);

    // Purge timestamps outside the window
    std::erase_if(state.Timestamps, [&](auto& tp) { return (now - tp) > window; });

    if (static_cast<int>(state.Timestamps.size()) >= kRateLimitMessages) {
        state.Violations++;

        if (state.Violations >= kFloodKickThreshold) {
            const auto& username = GetClientUsername(id);
            m_Console.AddItalicMessage("Auto-kicking {} for flooding ({} violations)",
                                       username, state.Violations);

            Safira::ClientInfo info;
            info.ID = id;
            SendClientKick(info, "Flood protection: message rate exceeded");
            m_Server->KickClient(id);
        }
        return true;
    }

    state.Timestamps.push_back(now);
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// OnDataReceived — variant dispatch via std::visit
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

            const auto& client = m_ConnectedClients.at(clientInfo.ID);

            // ── Mute check ──────────────────────────────────────────────
            if (m_MutedUsers.contains(client.Username)) {
                spdlog::debug("dropped message from muted user {}", client.Username);
                return;
            }

            // ── Rate-limit check ────────────────────────────────────────
            if (IsRateLimited(clientInfo.ID)) {
                spdlog::debug("rate-limited message from {}", client.Username);
                return;
            }

            // ── Message validation ──────────────────────────────────────
            // IsValidMessage (ServerPacket.h) checks whitespace-only and
            // truncates to MaxMessageLength (UserInfo.h, 4096).
            std::string message = pkt.Message;
            if (!Safira::IsValidMessage(message))
                return;

            m_MessageHistory.emplace_back(client.Username, message);
            m_Console.AddTaggedMessageWithColor(
                client.Color | 0xFF000000, client.Username, message);
            SendMessageToAllClients(clientInfo, message);
        },

        [&](const Safira::ConnectionRequestPacket& pkt) {
            const auto validation = ValidateUsername(pkt.Username);
            const bool isValid = !validation.has_value();

            SendClientConnectionRequestResponse(clientInfo, isValid);

            if (isValid) {
                // Assign a random colour from the palette
                static std::mt19937 rng(std::random_device{}());
                const uint32_t color = Safira::AvatarColors::kPalette[
                    rng() % Safira::AvatarColors::kCount];

                m_Console.AddMessage("Welcome {} from {}",
                                    pkt.Username, clientInfo.AddressStr);
                auto& client     = m_ConnectedClients[clientInfo.ID];
                client.Username  = pkt.Username;
                client.Color     = color;

                // Store avatar if provided and within size limit
                if (pkt.AvatarData.size() <= Safira::kMaxAvatarBytes)
                    client.AvatarData = pkt.AvatarData;

                SendClientConnect(clientInfo);
                SendClientList(clientInfo);
                SendMessageHistory(clientInfo);

                // Send MOTD if set
                if (!m_Motd.empty()) {
                    Safira::BufferWriter writer(m_ScratchBuffer);
                    Safira::SerializePacket(writer, Safira::ServerMessagePacket{
                        "SERVER", m_Motd });
                    m_Server->SendToClient(clientInfo.ID, writer.Written());
                }
            } else {
                m_Console.AddMessage("Connection rejected: '{}' — {} (addr={})",
                                    pkt.Username, *validation, clientInfo.AddressStr);
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
// Private chat forwarding
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
// Send helpers
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
    if (tokens.empty()) return;

    const auto& cmd = tokens[0];

    // ── /kick <username> [reason] ───────────────────────────────────────
    if (cmd == "kick") {
        if (tokens.size() >= 2) {
            std::string reason;
            for (size_t i = 2; i < tokens.size(); ++i) {
                if (!reason.empty()) reason += ' ';
                reason += std::string(tokens[i]);
            }
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
    // ── /list — show connected users ────────────────────────────────────
    else if (cmd == "list") {
        m_Console.AddItalicMessage("Connected clients ({}/{}):",
                                   m_ConnectedClients.size(),
                                   m_Server->GetConfig().MaxClients);
        for (const auto& [id, info] : m_ConnectedClients) {
            auto metrics = m_Server->GetClientMetrics(id);
            if (metrics) {
                m_Console.AddItalicMessage("  {} — uptime {:.0f}s, {} msgs",
                                           info.Username,
                                           metrics->UptimeSeconds(),
                                           metrics->MessagesRecv);
            } else {
                m_Console.AddItalicMessage("  {}", info.Username);
            }
        }
    }
    // ── /stats — server-wide statistics ─────────────────────────────────
    else if (cmd == "stats") {
        uint64_t totalMsgs = 0;
        uint64_t totalBytesIn = 0;
        uint64_t totalBytesOut = 0;

        for (const auto& [id, _] : m_ConnectedClients) {
            if (auto m = m_Server->GetClientMetrics(id)) {
                totalMsgs     += m->MessagesRecv;
                totalBytesIn  += m->BytesRecv;
                totalBytesOut += m->BytesSent;
            }
        }

        m_Console.AddItalicMessage("Server stats:");
        m_Console.AddItalicMessage("  Clients:  {}/{}",
                                   m_ConnectedClients.size(),
                                   m_Server->GetConfig().MaxClients);
        m_Console.AddItalicMessage("  Messages: {} total ({} in history)",
                                   totalMsgs, m_MessageHistory.size());
        m_Console.AddItalicMessage("  Traffic:  {} KB in, {} KB out",
                                   totalBytesIn / 1024, totalBytesOut / 1024);
        m_Console.AddItalicMessage("  Muted:    {}", m_MutedUsers.size());
    }
    // ── /broadcast <message> — server announcement ──────────────────────
    else if (cmd == "broadcast") {
        if (tokens.size() >= 2) {
            std::string msg;
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (!msg.empty()) msg += ' ';
                msg += std::string(tokens[i]);
            }

            Safira::BufferWriter writer(m_ScratchBuffer);
            Safira::SerializePacket(writer, Safira::ServerMessagePacket{
                "SERVER", msg });
            m_Server->SendToAllClients(writer.Written());
            m_Console.AddTaggedMessage("BROADCAST", msg);
        } else {
            m_Console.AddItalicMessage("Usage: /broadcast <message>");
        }
    }
    // ── /motd [message] — set or clear the message of the day ───────────
    else if (cmd == "motd") {
        if (tokens.size() >= 2) {
            m_Motd.clear();
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (!m_Motd.empty()) m_Motd += ' ';
                m_Motd += std::string(tokens[i]);
            }
            m_Console.AddItalicMessage("MOTD set: {}", m_Motd);
        } else {
            m_Motd.clear();
            m_Console.AddItalicMessage("MOTD cleared.");
        }
    }
    // ── /mute <username> ────────────────────────────────────────────────
    else if (cmd == "mute") {
        if (tokens.size() == 2) {
            auto target = std::string(tokens[1]);
            if (FindClientID(target)) {
                m_MutedUsers.insert(target);
                m_Console.AddItalicMessage("User {} is now muted.", target);

                // Broadcast mute notification to all clients
                Safira::BufferWriter writer(m_ScratchBuffer);
                Safira::SerializePacket(writer, Safira::ServerMessagePacket{
                    "SERVER", std::format("{} has been muted by the server.", target) });
                m_Server->SendToAllClients(writer.Written());
            } else {
                m_Console.AddItalicMessage("User {} not found.", target);
            }
        } else {
            m_Console.AddItalicMessage("Usage: /mute <username>");
        }
    }
    // ── /unmute <username> ──────────────────────────────────────────────
    else if (cmd == "unmute") {
        if (tokens.size() == 2) {
            auto target = std::string(tokens[1]);
            if (m_MutedUsers.erase(target)) {
                m_Console.AddItalicMessage("User {} is now unmuted.", target);

                // Broadcast unmute notification to all clients
                Safira::BufferWriter writer(m_ScratchBuffer);
                Safira::SerializePacket(writer, Safira::ServerMessagePacket{
                    "SERVER", std::format("{} has been unmuted.", target) });
                m_Server->SendToAllClients(writer.Written());
            } else {
                m_Console.AddItalicMessage("User {} was not muted.", target);
            }
        } else {
            m_Console.AddItalicMessage("Usage: /unmute <username>");
        }
    }
    // ── /help ───────────────────────────────────────────────────────────
    else if (cmd == "help") {
        m_Console.AddItalicMessage("Available commands:");
        m_Console.AddItalicMessage("  /kick <user> [reason]  — disconnect a user");
        m_Console.AddItalicMessage("  /mute <user>           — silence a user");
        m_Console.AddItalicMessage("  /unmute <user>         — restore a user's voice");
        m_Console.AddItalicMessage("  /list                  — show connected clients");
        m_Console.AddItalicMessage("  /stats                 — server statistics");
        m_Console.AddItalicMessage("  /broadcast <msg>       — send server announcement");
        m_Console.AddItalicMessage("  /motd [msg]            — set/clear message of the day");
        m_Console.AddItalicMessage("  /help                  — this message");
    }
    // ── Unknown ─────────────────────────────────────────────────────────
    else {
        m_Console.AddItalicMessage("Unknown command: /{}. Type /help for a list.", cmd);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Username validation
//
// Returns std::nullopt if valid, or a human-readable rejection reason.
// Replaces the old bool-only IsValidUsername with richer feedback.
// ═════════════════════════════════════════════════════════════════════════════

std::optional<std::string> ServerLayer::ValidateUsername(const std::string& username) const {
    if (username.size() < kMinUsernameLength)
        return std::format("too short (min {} chars)", kMinUsernameLength);
    if (username.size() > kMaxUsernameLength)
        return std::format("too long (max {} chars)", kMaxUsernameLength);

    // Character whitelist: alphanumeric, underscore, hyphen, period
    auto isAllowedChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c))
            || c == '_' || c == '-' || c == '.';
    };
    if (!std::ranges::all_of(username, isAllowedChar))
        return "contains invalid characters (allowed: a-z, 0-9, _ - .)";

    // Reserved names
    static constexpr std::array kReservedNames = {
        "SERVER", "SYSTEM", "Admin", "admin", "server", "system",
    };
    if (std::ranges::find(kReservedNames, username) != kReservedNames.end())
        return "reserved name";

    // Uniqueness
    const bool taken = std::ranges::any_of(
        m_ConnectedClients | std::views::values,
        [&](const Safira::UserInfo& info) { return info.Username == username; });
    if (taken)
        return "already taken";

    return std::nullopt;
}

bool ServerLayer::IsValidUsername(const std::string& username) const {
    return !ValidateUsername(username).has_value();
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