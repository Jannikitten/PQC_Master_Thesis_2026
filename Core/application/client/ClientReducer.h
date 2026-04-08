#ifndef SAFIRA_APPLICATION_CLIENT_CLIENTREDUCER_H
#define SAFIRA_APPLICATION_CLIENT_CLIENTREDUCER_H

// =============================================================================
// Same contract as the server reducer:
//   - Pure function: (State, Action) -> (State, Effects)
//   - No I/O, no mutation, deterministic
//   - Side effects described as typed ClientEffect values
// =============================================================================

#include "ClientState.h"
#include "ClientAction.h"
#include "Effect.h"
#include "Validation.h"
#include "Types.h"

#include <algorithm>
#include <format>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // ClientReducerResult — new state + batch of effects
    // ─────────────────────────────────────────────────────────────────────────────
    struct ClientReducerResult {
        ClientState                                 State;
        Effect::EffectBatch<Effect::ClientEffect>   Effects;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // ClientReduce — the pure client reducer
    // ─────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] inline ClientReducerResult
    ClientReduce(const ClientState& state, const ClientActionVariant& action) {
        namespace CA = ClientAction;
        namespace FX = Effect;

        ClientState next = state;
        FX::EffectBatch<FX::ClientEffect> effects;

        std::visit(Overloaded{
            // ─────────────────────────────────────────────────────────────────────────────
            // User-initiated commands
            // ─────────────────────────────────────────────────────────────────────────────

            // ── Connect requested ──
            [&](const CA::ConnectRequested& a) {
                next.Status        = ConnectionStatus::Connecting;
                next.ServerAddress = a.Address;
                next.Username      = a.Username;
                next.AvatarBytes   = a.AvatarData;

                effects.push_back(FX::ConnectToServer{ a.Address });
            },

            // ── Disconnect requested ──
            [&](const CA::DisconnectRequested&) {
                next.Status = ConnectionStatus::Disconnected;
                next.ConnectedClients.clear();
                next.LobbyMessages.clear();
                next.IncomingInvites.clear();
                next.PendingOutgoingInvites.clear();
                next.ActivePrivateChats.clear();
                next.MessageHistory.clear();
                next.KickReason.clear();

                effects.push_back(FX::CloseAllP2PSessions{});
                effects.push_back(FX::Disconnect{});
            },

            // ── Send chat message ──
            [&](const CA::SendMessage& a) {
                auto validated = Rules::ValidateMessage(a.Message);
                if (!validated) return;

                next.LobbyMessages.push_back(
                    ChatMessage{ next.Username, *validated });

                effects.push_back(FX::SendChatMessageToServer{ *validated });
                effects.push_back(FX::LogMessage{
                    *validated, FX::LogMessage::Level::Tagged,
                    next.Username, next.Color | 0xFF000000 });
            },

            // ── Send private chat invite ──
            [&](const CA::SendPrivateChatInvite& a) {
                next.PendingOutgoingInvites.insert(a.TargetUsername);
                effects.push_back(FX::SendPrivateChatInviteToServer{
                    a.TargetUsername });
            },

            // ── Respond to private chat invite ──
            [&](const CA::RespondToPrivateChatInvite& a) {
                std::erase_if(next.IncomingInvites,
                    [&](const IncomingInvite& inv) {
                        return inv.FromUsername == a.FromUsername;
                    });

                if (a.Accepted) {
                    // Effect handler starts P2P session and sends response
                    effects.push_back(FX::StartP2PAsResponder{
                        a.FromUsername });
                } else {
                    effects.push_back(FX::SendPrivateChatResponseToServer{
                        a.FromUsername, false, 0 });
                }
            },

            // ── Leave private chat ──
            [&](const CA::LeavePrivateChat& a) {
                next.ActivePrivateChats.erase(a.PeerUsername);
                effects.push_back(FX::CloseP2PSession{ a.PeerUsername });
            },

            // ─────────────────────────────────────────────────────────────────────────────
            // Raw data from server (deserialized by middleware)
            // ─────────────────────────────────────────────────────────────────────────────

            [&](const CA::DataReceived&) {
                // The deserialize middleware converts this into typed actions
            },

            // ─────────────────────────────────────────────────────────────────────────────
            // Network events
            // ─────────────────────────────────────────────────────────────────────────────

            // ── TLS handshake complete → send connection request ──
            [&](const CA::TlsHandshakeComplete&) {
                effects.push_back(FX::SendConnectionRequestToServer{
                    next.Username, 0, next.AvatarBytes });
                effects.push_back(FX::SaveConnectionDetails{});
            },

            // ── Connected (server accepted our registration) ──
            [&](const CA::Connected& a) {
                next.Status = ConnectionStatus::Connected;
                next.Color  = a.AssignedColor;
            },

            // ── Disconnected ──
            [&](const CA::Disconnected&) {
                next.Status = ConnectionStatus::Disconnected;
                next.ConnectedClients.clear();
                next.IncomingInvites.clear();
                next.PendingOutgoingInvites.clear();
                next.ActivePrivateChats.clear();

                effects.push_back(FX::CloseAllP2PSessions{});
            },

            // ── Connection failed ──
            [&](const CA::ConnectionFailed& a) {
                next.Status = ConnectionStatus::FailedToConnect;
                effects.push_back(FX::LogMessage{
                    std::format("Connection failed: {}", a.Reason),
                    FX::LogMessage::Level::Info });
            },

            // ─────────────────────────────────────────────────────────────────────────────
            // Server-pushed events
            // ─────────────────────────────────────────────────────────────────────────────

            // ── Chat message received ──
            [&](const CA::MessageReceived& a) {
                next.LobbyMessages.push_back(
                    ChatMessage{ a.From, a.Message });
            },

            // ── Full client list received ──
            [&](const CA::ClientListReceived& a) {
                next.ConnectedClients.clear();
                for (const auto& client : a.Clients) {
                    next.ConnectedClients[client.Username] = client;
                    // Sync own color from server-assigned value
                    if (client.Username == next.Username) {
                        next.Color = client.Color;
                    }
                }
            },

            // ── Single client connected ──
            [&](const CA::ClientConnectedEvent& a) {
                next.ConnectedClients[a.Client.Username] = a.Client;
                // Sync own color if this is our connect event
                if (a.Client.Username == next.Username) {
                    next.Color = a.Client.Color;
                }
                next.LobbyMessages.push_back(
                    ChatMessage{ "", std::format("{} joined.", a.Client.Username) });
            },

            // ── Single client disconnected ──
            [&](const CA::ClientDisconnectedEvent& a) {
                next.ConnectedClients.erase(a.Client.Username);
                next.PendingOutgoingInvites.erase(a.Client.Username);
                std::erase_if(next.IncomingInvites,
                    [&](const IncomingInvite& inv) {
                        return inv.FromUsername == a.Client.Username;
                    });
                next.ActivePrivateChats.erase(a.Client.Username);

                next.LobbyMessages.push_back(
                    ChatMessage{ "", std::format("{} left.", a.Client.Username) });

                effects.push_back(FX::CloseP2PSession{ a.Client.Username });
            },

            // ── Message history received ──
            [&](const CA::MessageHistoryReceived& a) {
                next.MessageHistory = a.Messages;
                for (const auto& msg : a.Messages) {
                    next.LobbyMessages.push_back(msg);
                }
            },

            // ── Server shutdown ──
            [&](const CA::ServerShutdownReceived&) {
                next.Status = ConnectionStatus::Disconnected;
                next.ConnectedClients.clear();
                next.LobbyMessages.push_back(
                    ChatMessage{ "", "Server is shutting down." });

                effects.push_back(FX::CloseAllP2PSessions{});
                effects.push_back(FX::Disconnect{});
            },

            // ── Kicked ──
            [&](const CA::Kicked& a) {
                next.Status     = ConnectionStatus::Disconnected;
                next.KickReason = a.Reason;
                next.ConnectedClients.clear();

                effects.push_back(FX::CloseAllP2PSessions{});
                effects.push_back(FX::Disconnect{});
                effects.push_back(FX::LogMessage{
                    std::format("Kicked: {}", a.Reason),
                    FX::LogMessage::Level::Info });
            },

            // ─────────────────────────────────────────────────────────────────────────────
            // Private chat events
            // ─────────────────────────────────────────────────────────────────────────────

            // ── Private chat invite received ──
            [&](const CA::PrivateChatInviteReceived& a) {
                next.IncomingInvites.push_back(IncomingInvite{ a.FromUsername });
            },

            // ── Private chat established (initiator side) ──
            [&](const CA::PrivateChatEstablished& a) {
                next.PendingOutgoingInvites.erase(a.PeerUsername);
                next.ActivePrivateChats.insert(a.PeerUsername);

                effects.push_back(FX::StartP2PAsInitiator{
                    a.PeerUsername, a.PeerAddress });
            },

            // ── Private chat declined ──
            [&](const CA::PrivateChatDeclined& a) {
                next.PendingOutgoingInvites.erase(a.PeerUsername);
            },
        }, action);

        return { std::move(next), std::move(effects) };
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Store-compatible wrapper (effects handled by middleware)
    // ─────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] inline ClientState
    ClientReducerFn(const ClientState& state, const ClientActionVariant& action) {
        return ClientReduce(state, action).State;
    }

} // namespace Safira

#endif // SAFIRA_APPLICATION_CLIENT_CLIENTREDUCER_H
