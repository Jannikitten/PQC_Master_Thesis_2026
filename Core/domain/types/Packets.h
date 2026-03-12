#ifndef SAFIRA_DOMAIN_TYPES_PACKETS_H
#define SAFIRA_DOMAIN_TYPES_PACKETS_H

// ═════════════════════════════════════════════════════════════════════════════
// Packets.h — strongly-typed packet definitions with variant dispatch
//
// Pure domain types only — no serialization logic.  Wire-format encoding
// is handled by infrastructure/serialization/PacketCodec.
// ═════════════════════════════════════════════════════════════════════════════

#include "User.h"
#include "Message.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Wire-format packet type tag
// ─────────────────────────────────────────────────────────────────────────────
enum class PacketType : uint32_t {
    None                  = 0,
    Message               = 1,
    ClientConnectionRequest = 2,
    ConnectionStatus      = 3,
    ClientList            = 4,
    ClientConnect         = 5,
    ClientUpdate          = 6,
    ClientDisconnect      = 7,
    ClientUpdateResponse  = 8,
    MessageHistory        = 9,
    ServerShutdown        = 10,
    ClientKick            = 11,

    // P2P signalling (relayed through main server)
    PrivateChatInvite     = 20,
    PrivateChatResponse   = 21,
    PrivateChatConnectTo  = 22,
    PrivateChatDeclined   = 23,
};

// ═════════════════════════════════════════════════════════════════════════════
// Packet structs
// ═════════════════════════════════════════════════════════════════════════════

// ── Client → Server packets ─────────────────────────────────────────────────

struct MessagePacket {
    static constexpr auto kType = PacketType::Message;
    std::string Message;
};

struct ConnectionRequestPacket {
    static constexpr auto kType = PacketType::ClientConnectionRequest;
    uint32_t              Color;
    std::string           Username;
    std::vector<uint8_t>  AvatarData;
};

struct PrivateChatInvitePacket {
    static constexpr auto kType = PacketType::PrivateChatInvite;
    std::string Username;
};

struct PrivateChatResponsePacket {
    static constexpr auto kType = PacketType::PrivateChatResponse;
    std::string Username;
    bool        Accepted;
    uint16_t    ListenPort;
};

// ── Server → Client packets ─────────────────────────────────────────────────

struct ServerMessagePacket {
    static constexpr auto kType = PacketType::Message;
    std::string From;
    std::string Message;
};

struct ConnectionResponsePacket {
    static constexpr auto kType = PacketType::ClientConnectionRequest;
    bool Accepted;
};

struct ClientListPacket {
    static constexpr auto kType = PacketType::ClientList;
    std::vector<UserInfo> Clients;
};

struct ClientConnectPacket {
    static constexpr auto kType = PacketType::ClientConnect;
    UserInfo Client;
};

struct ClientDisconnectPacket {
    static constexpr auto kType = PacketType::ClientDisconnect;
    UserInfo Client;
};

struct MessageHistoryPacket {
    static constexpr auto kType = PacketType::MessageHistory;
    std::vector<ChatMessage> Messages;
};

struct ServerShutdownPacket {
    static constexpr auto kType = PacketType::ServerShutdown;
};

struct ClientKickPacket {
    static constexpr auto kType = PacketType::ClientKick;
    std::string Reason;
};

struct PrivateChatConnectToPacket {
    static constexpr auto kType = PacketType::PrivateChatConnectTo;
    std::string PeerUsername;
    std::string Address;
};

struct PrivateChatDeclinedPacket {
    static constexpr auto kType = PacketType::PrivateChatDeclined;
    std::string PeerUsername;
};

// ═════════════════════════════════════════════════════════════════════════════
// Variant types for type-safe dispatch via std::visit
// ═════════════════════════════════════════════════════════════════════════════

using ServerIncomingPacket = std::variant<
    MessagePacket,
    ConnectionRequestPacket,
    PrivateChatInvitePacket,
    PrivateChatResponsePacket
>;

using ClientIncomingPacket = std::variant<
    ServerMessagePacket,
    ConnectionResponsePacket,
    ClientListPacket,
    ClientConnectPacket,
    ClientDisconnectPacket,
    MessageHistoryPacket,
    ServerShutdownPacket,
    ClientKickPacket,
    PrivateChatInvitePacket,
    PrivateChatConnectToPacket,
    PrivateChatDeclinedPacket
>;

} // namespace Safira

#endif // SAFIRA_DOMAIN_TYPES_PACKETS_H
