#ifndef SAFIRA_APPLICATION_STORE_EFFECT_H
#define SAFIRA_APPLICATION_STORE_EFFECT_H

// ═════════════════════════════════════════════════════════════════════════════
// Effects are value types that describe what should happen, not how.
// The effect middleware interprets them and calls into infrastructure.
// This keeps reducers pure — they return effects alongside new state.
//
// Server protocol effects describe WHAT to send at a high level;
// the effect middleware serializes and sends them.
// ═════════════════════════════════════════════════════════════════════════════

#include "Types.h"
#include "ClientID.h"
#include "User.h"

#include <string>
#include <variant>
#include <vector>

namespace Safira::Effect {

    // ── No-op ──

    struct None {};

    // ── Infrastructure effects (shared) ──

    struct KickClient {
        ClientID    Target;
    };

    struct Disconnect {};

    struct SaveHistory {};

    struct LoadHistory {};

    struct LogMessage {
        std::string Message;
        enum class Level { Info, Italic, Tagged } Type = Level::Info;
        std::string Tag;
        uint32_t    Color = 0;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Server protocol effects — typed descriptions of outbound packets
    //
    // The reducer produces these to describe which packets to send.
    // The effect middleware serializes them using SerializePacket and
    // calls into the network adapter.
    // ─────────────────────────────────────────────────────────────────────────────

    struct SendConnectionResponse {
        ClientID Target;
        bool     Accepted;
    };

    struct BroadcastClientConnect {
        ClientID NewClient;        // send this client's info to all
    };

    struct SendClientListTo {
        ClientID Target;
    };

    struct SendClientListToAll {};

    struct SendMessageHistoryTo {
        ClientID Target;
    };

    struct BroadcastServerMessage {
        std::string From;
        std::string Message;
        ClientID    Exclude {};    // optional: skip this client
    };

    struct BroadcastClientDisconnect {
        UserInfo Client;
        ClientID Exclude {};       // skip the disconnected client
    };

    struct SendServerShutdownToAll {};

    struct SendKickNotification {
        ClientID    Target;
        std::string Reason;
    };

    struct ForwardPrivateChatInvite {
        ClientID    Target;
        std::string FromUsername;
    };

    struct ForwardPrivateChatConnectTo {
        ClientID    Target;
        std::string PeerUsername;
        std::string Address;
    };

    struct ForwardPrivateChatDeclined {
        ClientID    Target;
        std::string PeerUsername;
    };

    struct SendMotdTo {
        ClientID Target;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Client protocol effects — typed descriptions of outbound packets
    //
    // Same pattern as server: the reducer describes WHAT to send;
    // the effect middleware serializes and sends.
    // ─────────────────────────────────────────────────────────────────────────────

    struct SendChatMessageToServer {
        std::string Message;
    };

    struct SendConnectionRequestToServer {
        std::string              Username;
        uint32_t                 Color;
        std::vector<uint8_t>     AvatarData;
    };

    struct SendPrivateChatInviteToServer {
        std::string TargetUsername;
    };

    struct SendPrivateChatResponseToServer {
        std::string PeerUsername;
        bool        Accepted;
        uint16_t    ListenPort;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Client infrastructure effects
    // ─────────────────────────────────────────────────────────────────────────────

    struct ConnectToServer {
        std::string Address;
    };

    struct SaveConnectionDetails {};

    struct StartP2PAsResponder {
        std::string PeerUsername;
    };

    struct StartP2PAsInitiator {
        std::string PeerUsername;
        std::string PeerAddress;
    };

    struct CloseP2PSession {
        std::string PeerUsername;
    };

    struct CloseAllP2PSessions {};

    // ─────────────────────────────────────────────────────────────────────────────
    // ServerEffect / ClientEffect — variant of all possible effects
    // ─────────────────────────────────────────────────────────────────────────────

    using ServerEffect = std::variant<
        None,
        // Protocol effects (describe what to send — middleware serializes)
        SendConnectionResponse,
        BroadcastClientConnect,
        SendClientListTo,
        SendClientListToAll,
        SendMessageHistoryTo,
        BroadcastServerMessage,
        BroadcastClientDisconnect,
        SendServerShutdownToAll,
        SendKickNotification,
        ForwardPrivateChatInvite,
        ForwardPrivateChatConnectTo,
        ForwardPrivateChatDeclined,
        SendMotdTo,
        // Infrastructure effects
        KickClient,
        SaveHistory,
        LoadHistory,
        LogMessage
    >;

    using ClientEffect = std::variant<
        None,
        // Protocol effects (describe what to send — middleware serializes)
        SendChatMessageToServer,
        SendConnectionRequestToServer,
        SendPrivateChatInviteToServer,
        SendPrivateChatResponseToServer,
        // Infrastructure effects
        ConnectToServer,
        Disconnect,
        SaveConnectionDetails,
        StartP2PAsResponder,
        StartP2PAsInitiator,
        CloseP2PSession,
        CloseAllP2PSessions,
        LogMessage
    >;

    // ── Batch helper ──

    template <typename EffectVariant>
    using EffectBatch = std::vector<EffectVariant>;

} // namespace Safira::Effect

#endif // SAFIRA_APPLICATION_STORE_EFFECT_H
