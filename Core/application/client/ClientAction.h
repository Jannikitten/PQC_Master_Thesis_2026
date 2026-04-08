#ifndef SAFIRA_APPLICATION_CLIENT_CLIENTACTION_H
#define SAFIRA_APPLICATION_CLIENT_CLIENTACTION_H

// ═════════════════════════════════════════════════════════════════════════════
// ClientAction.h — All possible client actions as a closed variant
//
// Actions describe what happened (events) or what the user/system wants
// to do (commands).  The reducer interprets them purely.
// ═════════════════════════════════════════════════════════════════════════════

#include "User.h"
#include "Message.h"
#include "Connection.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Safira::ClientAction {

    // ── User-initiated commands ──

    struct ConnectRequested {
        std::string Address;
        std::string Username;
        std::vector<uint8_t> AvatarData;
    };

    struct DisconnectRequested {};

    struct SendMessage {
        std::string Message;
    };

    struct SendPrivateChatInvite {
        std::string TargetUsername;
    };

    struct RespondToPrivateChatInvite {
        std::string FromUsername;
        bool        Accepted;
    };

    struct LeavePrivateChat {
        std::string PeerUsername;
    };

    // ── Raw data from server ──

    struct DataReceived {
        std::vector<uint8_t> Payload;
    };

    // ── Network events ──

    struct TlsHandshakeComplete {};

    struct Connected {
        uint32_t AssignedColor;
    };

    struct Disconnected {};

    struct ConnectionFailed {
        std::string Reason;
    };

    struct MessageReceived {
        std::string From;
        std::string Message;
    };

    struct ClientListReceived {
        std::vector<UserInfo> Clients;
    };

    struct ClientConnectedEvent {
        UserInfo Client;
    };

    struct ClientDisconnectedEvent {
        UserInfo Client;
    };

    struct MessageHistoryReceived {
        std::vector<ChatMessage> Messages;
    };

    struct ServerShutdownReceived {};

    struct Kicked {
        std::string Reason;
    };

    // ── Private chat events ──

    struct PrivateChatInviteReceived {
        std::string FromUsername;
    };

    struct PrivateChatEstablished {
        std::string PeerUsername;
        std::string PeerAddress;
    };

    struct PrivateChatDeclined {
        std::string PeerUsername;
    };

} // namespace Safira::ClientAction

namespace Safira {

    using ClientActionVariant = std::variant<
        ClientAction::ConnectRequested,
        ClientAction::DisconnectRequested,
        ClientAction::SendMessage,
        ClientAction::SendPrivateChatInvite,
        ClientAction::RespondToPrivateChatInvite,
        ClientAction::LeavePrivateChat,

        ClientAction::DataReceived,
        ClientAction::TlsHandshakeComplete,
        ClientAction::Connected,
        ClientAction::Disconnected,
        ClientAction::ConnectionFailed,
        ClientAction::MessageReceived,
        ClientAction::ClientListReceived,
        ClientAction::ClientConnectedEvent,
        ClientAction::ClientDisconnectedEvent,
        ClientAction::MessageHistoryReceived,
        ClientAction::ServerShutdownReceived,
        ClientAction::Kicked,

        ClientAction::PrivateChatInviteReceived,
        ClientAction::PrivateChatEstablished,
        ClientAction::PrivateChatDeclined
    >;

} // namespace Safira

#endif // SAFIRA_APPLICATION_CLIENT_CLIENTACTION_H
