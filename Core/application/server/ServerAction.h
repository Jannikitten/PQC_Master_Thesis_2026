#ifndef SAFIRA_APPLICATION_SERVER_SERVERACTION_H
#define SAFIRA_APPLICATION_SERVER_SERVERACTION_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerAction.h — All possible server actions as a closed variant
//
// Actions are pure data — they describe what happened, not what to do.
// The reducer interprets them to produce new state + effects.
// ═════════════════════════════════════════════════════════════════════════════

#include "ClientID.h"
#include "Packets.h"
#include "Types.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Safira::Action {

    // ── Network events ──

    struct ClientConnected {
        ClientID    ID;
        std::string Address;
    };

    struct ClientDisconnected {
        ClientID    ID;
    };

    struct DataReceived {
        ClientID           ID;
        std::vector<uint8_t> Payload;
    };

    // ── Parsed packet actions (from DataReceived after deserialization) ──

    struct MessageReceived {
        ClientID    From;
        std::string Message;
    };

    struct ConnectionRequested {
        ClientID                ID;
        std::string             Address;
        ConnectionRequestPacket Packet;
    };

    struct PrivateChatInviteReceived {
        ClientID    From;
        std::string TargetUsername;
    };

    struct PrivateChatResponseReceived {
        ClientID                  From;
        PrivateChatResponsePacket Packet;
    };

    // ── Admin commands ──

    struct KickCommand {
        std::string Username;
        std::string Reason;
    };

    struct MuteCommand {
        std::string Username;
    };

    struct UnmuteCommand {
        std::string Username;
    };

    struct BroadcastCommand {
        std::string Message;
    };

    struct SetMotdCommand {
        std::string Message;  // empty = clear
    };

    struct SendChatMessage {
        std::string Message;
    };

    // ── Lifecycle ──

    struct Tick {
        float DeltaTime;
    };

    struct Shutdown {};

    struct HistoryLoaded {
        std::vector<Safira::ChatMessage> Messages;
    };

} // namespace Safira::Action

namespace Safira {

    using ServerAction = std::variant<
        Action::ClientConnected,
        Action::ClientDisconnected,
        Action::DataReceived,
        Action::MessageReceived,
        Action::ConnectionRequested,
        Action::PrivateChatInviteReceived,
        Action::PrivateChatResponseReceived,
        Action::KickCommand,
        Action::MuteCommand,
        Action::UnmuteCommand,
        Action::BroadcastCommand,
        Action::SetMotdCommand,
        Action::SendChatMessage,
        Action::Tick,
        Action::Shutdown,
        Action::HistoryLoaded
    >;

} // namespace Safira

#endif // SAFIRA_APPLICATION_SERVER_SERVERACTION_H
