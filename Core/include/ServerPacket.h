#ifndef PQC_MASTER_THESIS_2026_SERVERPACKET_H
#define PQC_MASTER_THESIS_2026_SERVERPACKET_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerPacket.h — strongly-typed packet definitions with variant dispatch
//
// §3.2  Result types  – deserialize returns std::expected<Packet, ParseError>
// §3.4  Strong types  – each packet is a distinct struct, not a bare enum
// C++23              – std::variant, std::visit, concepts
//
// The enum is KEPT for wire-format tagging (first uint32_t of every packet).
// Dispatch is via std::variant + std::visit, not switch statements.
// ═════════════════════════════════════════════════════════════════════════════

#include "Serialization.h"
#include "UserInfo.h"

#include <string>
#include <variant>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Wire-format packet type tag (unchanged from original)
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

// ─────────────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline bool IsValidMessage(std::string& msg) {
    if (msg.empty()) return false;
    if (msg.find_first_not_of(" \t\n\v\f\r") == std::string::npos) return false;
    if (msg.size() > MaxMessageLength) {
        msg = msg.substr(0, MaxMessageLength);
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Packet structs — each carries a static PacketType tag for serialization
// ═════════════════════════════════════════════════════════════════════════════

// ── Client → Server packets ─────────────────────────────────────────────────

struct MessagePacket {
    static constexpr auto kType = PacketType::Message;
    std::string Message;
};

struct ConnectionRequestPacket {
    static constexpr auto kType = PacketType::ClientConnectionRequest;
    uint32_t    Color;
    uint8_t     IconIndex;
    std::string Username;
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

// ═════════════════════════════════════════════════════════════════════════════
// Serialize — free-function overloads for each packet struct
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] inline bool Serialize(BufferWriter& w, const MessagePacket& p) {
    return Serialize(w, p.Message);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ConnectionRequestPacket& p) {
    return Serialize(w, p.Color)
        && Serialize(w, p.IconIndex)
        && Serialize(w, p.Username);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const PrivateChatInvitePacket& p) {
    return Serialize(w, p.Username);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const PrivateChatResponsePacket& p) {
    return Serialize(w, p.Username)
        && Serialize(w, p.Accepted)
        && Serialize(w, p.ListenPort);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ServerMessagePacket& p) {
    return Serialize(w, p.From)
        && Serialize(w, p.Message);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ConnectionResponsePacket& p) {
    return Serialize(w, p.Accepted);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ClientListPacket& p) {
    return Serialize(w, p.Clients);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ClientConnectPacket& p) {
    return Serialize(w, p.Client);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ClientDisconnectPacket& p) {
    return Serialize(w, p.Client);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const MessageHistoryPacket& p) {
    return Serialize(w, p.Messages);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ServerShutdownPacket&) {
    return true;
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ClientKickPacket& p) {
    return Serialize(w, p.Reason);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const PrivateChatConnectToPacket& p) {
    return Serialize(w, p.PeerUsername)
        && Serialize(w, p.Address);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const PrivateChatDeclinedPacket& p) {
    return Serialize(w, p.PeerUsername);
}

// ═════════════════════════════════════════════════════════════════════════════
// SerializePacket — writes the PacketType tag + packet payload
// ═════════════════════════════════════════════════════════════════════════════

template <typename P>
bool SerializePacket(BufferWriter& w, const P& pkt) {
    return Serialize(w, P::kType) && Serialize(w, pkt);
}

// ═════════════════════════════════════════════════════════════════════════════
// Deserialize — packet deserialization (implemented in ServerPacket.cpp)
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] std::expected<ServerIncomingPacket, ParseError>
DeserializeServerPacket(BufferReader& r);

[[nodiscard]] std::expected<ClientIncomingPacket, ParseError>
DeserializeClientPacket(BufferReader& r);

} // namespace Safira

#endif //PQC_MASTER_THESIS_2026_SERVERPACKET_H
