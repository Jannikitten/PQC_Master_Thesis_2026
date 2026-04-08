#ifndef SAFIRA_APPLICATION_SERVER_SERVERREDUCER_H
#define SAFIRA_APPLICATION_SERVER_SERVERREDUCER_H

// ═════════════════════════════════════════════════════════════════════════════
// The reducer is a pure function: (State, Action) -> (State, Effects)
// It never performs I/O, never mutates its arguments, and always
// produces the same output for the same input.
//
// Side effects are described via typed Effect values returned alongside
// state changes.  The effect middleware serializes protocol effects
// and executes infrastructure effects.
// ═════════════════════════════════════════════════════════════════════════════

#include "ServerState.h"
#include "ServerAction.h"
#include "Effect.h"
#include "Validation.h"
#include "RateLimit.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <random>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // ReducerResult — new state + batch of effects to execute
    // ─────────────────────────────────────────────────────────────────────────────
    struct ServerReducerResult {
        ServerState                              State;
        Effect::EffectBatch<Effect::ServerEffect> Effects;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // ServerReduce — the pure reducer function
    //
    // Takes the current state and an action, returns a new state plus
    // any effects that need to be executed.  Protocol effects describe
    // WHAT to send (typed data); the effect middleware serializes them.
    // ─────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] inline ServerReducerResult
    ServerReduce(const ServerState& state, const ServerAction& action) {
        using namespace Action;
        namespace FX = Effect;

        ServerState next = state;
        FX::EffectBatch<FX::ServerEffect> effects;

        std::visit(Overloaded{
            // ── Client connected (handshake only, registration deferred) ────
            [&](const ClientConnected& a) {
                next.ClientAddresses[a.ID] = a.Address;
            },

            // ── Client disconnected ──
            [&](const ClientDisconnected& a) {
                if (!next.ConnectedClients.contains(a.ID)) return;

                const auto clientInfo = next.ConnectedClients.at(a.ID); // copy before erase
                const auto& username = clientInfo.Username;

                // Clean up pending invites — forward decline if this client
                // was a pending responder
                if (auto it = next.PendingInvites.find(a.ID);
                    it != next.PendingInvites.end()) {
                    effects.push_back(FX::ForwardPrivateChatDeclined{
                        it->second, username });
                    next.PendingInvites.erase(it);
                }
                // Remove invites where disconnected client was the initiator
                std::erase_if(next.PendingInvites,
                    [&](const auto& p) { return p.second == a.ID; });

                next.RateLimits.erase(a.ID);
                next.MutedUsers.erase(username);
                next.ConnectedClients.erase(a.ID);
                next.ClientAddresses.erase(a.ID);

                // Broadcast disconnect to remaining clients
                effects.push_back(FX::BroadcastClientDisconnect{
                    clientInfo, a.ID });

                effects.push_back(FX::LogMessage{
                    std::format("Client {} disconnected", username),
                    FX::LogMessage::Level::Italic });
            },

            // ── Raw data received (deserialized by middleware) ──
            [&](const DataReceived&) {
                // The deserialize middleware converts DataReceived into
                // typed actions (MessageReceived, ConnectionRequested, etc.)
            },

            // ── Chat message received ──
            [&](const MessageReceived& a) {
                if (!next.ConnectedClients.contains(a.From)) return;

                const auto& client = next.ConnectedClients.at(a.From);

                // Mute check
                if (next.MutedUsers.contains(client.Username)) return;

                // Rate limit check (pure)
                auto rlResult = Rules::CheckRateLimit(
                    next.RateLimits[a.From],
                    std::chrono::steady_clock::now());
                next.RateLimits[a.From] = rlResult.NewState;

                if (rlResult.Decision == Rules::RateLimitDecision::Kick) {
                    effects.push_back(FX::SendKickNotification{
                        a.From, "Flood protection: message rate exceeded" });
                    effects.push_back(FX::KickClient{ a.From });
                    effects.push_back(FX::LogMessage{
                        std::format("Auto-kicking {} for flooding", client.Username),
                        FX::LogMessage::Level::Italic });
                    return;
                }
                if (rlResult.Decision == Rules::RateLimitDecision::Throttled) return;

                // Message validation (pure)
                auto validated = Rules::ValidateMessage(a.Message);
                if (!validated) return;

                // Add to history
                next.MessageHistory.emplace_back(client.Username, *validated);
                if (next.MessageHistory.size() > next.MaxHistory) {
                    next.MessageHistory.erase(
                        next.MessageHistory.begin(),
                        next.MessageHistory.begin() +
                            static_cast<std::ptrdiff_t>(
                                next.MessageHistory.size() - next.MaxHistory));
                }

                // Broadcast message to all (excluding sender)
                effects.push_back(FX::BroadcastServerMessage{
                    client.Username, *validated, a.From });

                effects.push_back(FX::LogMessage{
                    *validated, FX::LogMessage::Level::Tagged,
                    client.Username, client.Color | 0xFF000000 });
            },

            // ── Connection request ──
            [&](const ConnectionRequested& a) {
                auto existingNames = next.ConnectedClients
                    | std::views::values
                    | std::views::transform(&UserInfo::Username);
                std::vector<std::string> names(existingNames.begin(), existingNames.end());

                auto validation = Rules::ValidateUsername(a.Packet.Username, names);
                const bool valid = !validation.has_value();

                // Always send connection response
                effects.push_back(FX::SendConnectionResponse{ a.ID, valid });

                if (valid) {
                    // Assign random colour
                    static std::mt19937 rng(std::random_device{}());
                    const uint32_t color = AvatarColors::kPalette[
                        rng() % AvatarColors::kCount];

                    auto& client = next.ConnectedClients[a.ID];
                    client.Username = a.Packet.Username;
                    client.Color    = color;
                    if (Rules::ValidateAvatar(a.Packet.AvatarData))
                        client.AvatarData = a.Packet.AvatarData;

                    // Protocol effects for successful connection
                    effects.push_back(FX::BroadcastClientConnect{ a.ID });
                    effects.push_back(FX::SendClientListTo{ a.ID });
                    effects.push_back(FX::SendMessageHistoryTo{ a.ID });

                    if (!next.Motd.empty())
                        effects.push_back(FX::SendMotdTo{ a.ID });

                    effects.push_back(FX::LogMessage{
                        std::format("Welcome {} from {}", a.Packet.Username, a.Address),
                        FX::LogMessage::Level::Info });
                } else {
                    effects.push_back(FX::LogMessage{
                        std::format("Connection rejected: '{}' — {} (addr={})",
                            a.Packet.Username, *validation, a.Address),
                        FX::LogMessage::Level::Info });
                }
            },

            // ── Private chat invite ──
            [&](const PrivateChatInviteReceived& a) {
                if (!next.ConnectedClients.contains(a.From)) return;
                const auto& fromUsername = next.ConnectedClients.at(a.From).Username;

                // Find target by username
                auto it = std::ranges::find_if(next.ConnectedClients,
                    [&](const auto& p) { return p.second.Username == a.TargetUsername; });
                if (it == next.ConnectedClients.end()) {
                    effects.push_back(FX::LogMessage{
                        std::format("PrivateChatInvite: target '{}' not found", a.TargetUsername),
                        FX::LogMessage::Level::Info });
                    return;
                }

                next.PendingInvites[it->first] = a.From;

                effects.push_back(FX::ForwardPrivateChatInvite{
                    it->first, fromUsername });
                effects.push_back(FX::LogMessage{
                    std::format("{} invited {} to a private chat",
                        fromUsername, a.TargetUsername),
                    FX::LogMessage::Level::Info });
            },

            // ── Private chat response ──
            [&](const PrivateChatResponseReceived& a) {
                if (!next.ConnectedClients.contains(a.From)) return;
                const auto& responderUsername = next.ConnectedClients.at(a.From).Username;

                auto pendingIt = next.PendingInvites.find(a.From);
                if (pendingIt == next.PendingInvites.end()) {
                    effects.push_back(FX::LogMessage{
                        std::format("Ignoring unsolicited private-chat response from {}",
                            responderUsername),
                        FX::LogMessage::Level::Info });
                    return;
                }

                const auto initiatorID = pendingIt->second;
                if (!next.ConnectedClients.contains(initiatorID)) {
                    next.PendingInvites.erase(pendingIt);
                    return;
                }

                const auto& expectedInitiator =
                    next.ConnectedClients.at(initiatorID).Username;
                if (a.Packet.Username != expectedInitiator) {
                    effects.push_back(FX::LogMessage{
                        std::format("Ignoring mismatched private-chat response from {} "
                                    "(expected {}, got {})",
                            responderUsername, expectedInitiator, a.Packet.Username),
                        FX::LogMessage::Level::Info });
                    return;
                }

                next.PendingInvites.erase(pendingIt);

                if (!a.Packet.Accepted || a.Packet.ListenPort == 0) {
                    effects.push_back(FX::ForwardPrivateChatDeclined{
                        initiatorID, responderUsername });
                    effects.push_back(FX::LogMessage{
                        std::format("{} declined private chat with {}",
                            responderUsername, a.Packet.Username),
                        FX::LogMessage::Level::Info });
                    return;
                }

                // Build P2P address from responder's transport IP + listen port
                const auto& addrStr = next.ClientAddresses.at(a.From);
                const auto colonPos = addrStr.rfind(':');
                std::string p2pAddress =
                    std::string(addrStr.substr(0, colonPos))
                    + ":" + std::to_string(a.Packet.ListenPort);

                effects.push_back(FX::ForwardPrivateChatConnectTo{
                    initiatorID, responderUsername, p2pAddress });
                effects.push_back(FX::LogMessage{
                    std::format("{} accepted private chat with {} on {}",
                        responderUsername, a.Packet.Username, p2pAddress),
                    FX::LogMessage::Level::Info });
            },

            // ── Admin: kick ──
            [&](const KickCommand& a) {
                auto it = std::ranges::find_if(next.ConnectedClients,
                    [&](const auto& p) { return p.second.Username == a.Username; });
                if (it != next.ConnectedClients.end()) {
                    effects.push_back(FX::SendKickNotification{
                        it->first, a.Reason });
                    effects.push_back(FX::KickClient{ it->first });
                    effects.push_back(FX::LogMessage{
                        std::format("User {} has been kicked.", a.Username),
                        FX::LogMessage::Level::Italic });
                    if (!a.Reason.empty())
                        effects.push_back(FX::LogMessage{
                            std::format("  Reason: {}", a.Reason),
                            FX::LogMessage::Level::Italic });
                } else {
                    effects.push_back(FX::LogMessage{
                        std::format("Could not kick user {}; not found.", a.Username),
                        FX::LogMessage::Level::Italic });
                }
            },

            // ── Admin: mute ──
            [&](const MuteCommand& a) {
                auto it = std::ranges::find_if(next.ConnectedClients,
                    [&](const auto& p) { return p.second.Username == a.Username; });
                if (it != next.ConnectedClients.end()) {
                    next.MutedUsers.insert(a.Username);
                    // Broadcast mute notification to all clients
                    effects.push_back(FX::BroadcastServerMessage{
                        "SERVER",
                        std::format("{} has been muted by the server.", a.Username) });
                    effects.push_back(FX::LogMessage{
                        std::format("User {} is now muted.", a.Username),
                        FX::LogMessage::Level::Italic });
                } else {
                    effects.push_back(FX::LogMessage{
                        std::format("User {} not found.", a.Username),
                        FX::LogMessage::Level::Italic });
                }
            },

            // ── Admin: unmute ──
            [&](const UnmuteCommand& a) {
                if (next.MutedUsers.erase(a.Username)) {
                    // Broadcast unmute notification to all clients
                    effects.push_back(FX::BroadcastServerMessage{
                        "SERVER",
                        std::format("{} has been unmuted.", a.Username) });
                    effects.push_back(FX::LogMessage{
                        std::format("User {} is now unmuted.", a.Username),
                        FX::LogMessage::Level::Italic });
                } else {
                    effects.push_back(FX::LogMessage{
                        std::format("User {} was not muted.", a.Username),
                        FX::LogMessage::Level::Italic });
                }
            },

            // ── Admin: broadcast ──
            [&](const BroadcastCommand& a) {
                effects.push_back(FX::BroadcastServerMessage{ "SERVER", a.Message });
                effects.push_back(FX::LogMessage{
                    a.Message, FX::LogMessage::Level::Tagged, "BROADCAST" });
            },

            // ── Admin: set MOTD ──
            [&](const SetMotdCommand& a) {
                next.Motd = a.Message;
                if (a.Message.empty())
                    effects.push_back(FX::LogMessage{
                        "MOTD cleared.", FX::LogMessage::Level::Italic });
                else
                    effects.push_back(FX::LogMessage{
                        std::format("MOTD set: {}", a.Message),
                        FX::LogMessage::Level::Italic });
            },

            // ── Server chat message ──
            [&](const SendChatMessage& a) {
                if (a.Message.empty() ||
                    a.Message.find_first_not_of(" \t\n\v\f\r") == std::string::npos)
                    return;

                next.MessageHistory.emplace_back("SERVER", a.Message);
                if (next.MessageHistory.size() > next.MaxHistory) {
                    next.MessageHistory.erase(
                        next.MessageHistory.begin(),
                        next.MessageHistory.begin() +
                            static_cast<std::ptrdiff_t>(
                                next.MessageHistory.size() - next.MaxHistory));
                }
                effects.push_back(FX::BroadcastServerMessage{ "SERVER", a.Message });
                effects.push_back(FX::LogMessage{
                    a.Message, FX::LogMessage::Level::Tagged, "SERVER" });
            },

            // ── Tick (periodic update) ──
            [&](const Tick& a) {
                next.ClientListTimer -= a.DeltaTime;
                if (next.ClientListTimer < 0) {
                    next.ClientListTimer = ServerState::kClientListInterval;
                    effects.push_back(FX::SendClientListToAll{});
                    effects.push_back(FX::SaveHistory{});
                }
            },

            // ── Shutdown ──
            [&](const Shutdown&) {
                effects.push_back(FX::SendServerShutdownToAll{});
                effects.push_back(FX::SaveHistory{});
            },

            // ── History loaded ──
            [&](const HistoryLoaded& a) {
                next.MessageHistory = a.Messages;
            },
        }, action);

        return { std::move(next), std::move(effects) };
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Store-compatible reducer (wraps ServerReduce, discarding effects).
    // Effects are computed and executed by the effect middleware.
    // ─────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] inline ServerState
    ServerReducerFn(const ServerState& state, const ServerAction& action) {
        return ServerReduce(state, action).State;
    }

} // namespace Safira

#endif // SAFIRA_APPLICATION_SERVER_SERVERREDUCER_H
