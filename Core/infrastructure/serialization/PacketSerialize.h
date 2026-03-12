#ifndef SAFIRA_INFRASTRUCTURE_SERIALIZATION_PACKETSERIALIZE_H
#define SAFIRA_INFRASTRUCTURE_SERIALIZATION_PACKETSERIALIZE_H

// ═════════════════════════════════════════════════════════════════════════════
// PacketSerialize.h — Serialize/Deserialize overloads for each packet struct
//
// Provides free-function Serialize() overloads for every packet type defined
// in domain/types/Packets.h, plus the top-level DeserializeServerPacket /
// DeserializeClientPacket entry points.
// ═════════════════════════════════════════════════════════════════════════════

// Domain types (canonical definitions for packets, users, messages)
#include "Packets.h"

// Serialization primitives + UserInfo/ChatMessage serialization
#include "BufferIO.h"
#include "UserInfoSerialize.h"

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Validation (legacy location — pure version in domain/rules/Validation.h)
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
// Serialize — free-function overloads for each packet struct
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] inline bool Serialize(BufferWriter& w, const MessagePacket& p) {
    return Serialize(w, p.Message);
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ConnectionRequestPacket& p) {
    return Serialize(w, p.Color)
        && Serialize(w, p.Username)
        && Serialize(w, p.AvatarData);
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

#endif // SAFIRA_INFRASTRUCTURE_SERIALIZATION_PACKETSERIALIZE_H
