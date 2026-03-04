#include "ServerPacket.h"

// ═════════════════════════════════════════════════════════════════════════════
// ServerPacket.cpp — packet deserialization implementations
//
// §3.2  Result types  – every field read is fallible, errors propagate
//                       via std::expected without intermediate checks
// C++23              – std::expected, monadic composition
// ═════════════════════════════════════════════════════════════════════════════

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Server-side: deserialise a packet received from a client.
// ─────────────────────────────────────────────────────────────────────────────
std::expected<ServerIncomingPacket, ParseError>
DeserializeServerPacket(BufferReader& r) {
    auto type = Deserialize<PacketType>(r);
    if (!type) return std::unexpected(type.error());

    switch (*type) {

    case PacketType::Message: {
        auto msg = Deserialize<std::string>(r);
        if (!msg) return std::unexpected(msg.error());
        return MessagePacket{ std::move(*msg) };
    }

    case PacketType::ClientConnectionRequest: {
        auto color = Deserialize<uint32_t>(r);
        if (!color) return std::unexpected(color.error());
        auto name = Deserialize<std::string>(r);
        if (!name) return std::unexpected(name.error());
        auto avatar = DeserializeVector<uint8_t>(r);
        if (!avatar) return std::unexpected(avatar.error());
        return ConnectionRequestPacket{ *color, std::move(*name), std::move(*avatar) };
    }

    case PacketType::PrivateChatInvite: {
        auto name = Deserialize<std::string>(r);
        if (!name) return std::unexpected(name.error());
        return PrivateChatInvitePacket{ std::move(*name) };
    }

    case PacketType::PrivateChatResponse: {
        auto name = Deserialize<std::string>(r);
        if (!name) return std::unexpected(name.error());
        auto accepted = Deserialize<bool>(r);
        if (!accepted) return std::unexpected(accepted.error());
        auto port = Deserialize<uint16_t>(r);
        if (!port) return std::unexpected(port.error());
        return PrivateChatResponsePacket{ std::move(*name), *accepted, *port };
    }

    default:
        return std::unexpected(ParseError::InvalidPacketType);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Client-side: deserialise a packet received from the server.
// ─────────────────────────────────────────────────────────────────────────────
std::expected<ClientIncomingPacket, ParseError>
DeserializeClientPacket(BufferReader& r) {
    auto type = Deserialize<PacketType>(r);
    if (!type) return std::unexpected(type.error());

    switch (*type) {

    case PacketType::Message: {
        auto from = Deserialize<std::string>(r);
        if (!from) return std::unexpected(from.error());
        auto msg = Deserialize<std::string>(r);
        if (!msg) return std::unexpected(msg.error());
        return ServerMessagePacket{ std::move(*from), std::move(*msg) };
    }

    case PacketType::ClientConnectionRequest: {
        auto accepted = Deserialize<bool>(r);
        if (!accepted) return std::unexpected(accepted.error());
        return ConnectionResponsePacket{ *accepted };
    }

    case PacketType::ClientList: {
        auto clients = DeserializeVector<UserInfo>(r);
        if (!clients) return std::unexpected(clients.error());
        return ClientListPacket{ std::move(*clients) };
    }

    case PacketType::ClientConnect: {
        auto client = Deserialize<UserInfo>(r);
        if (!client) return std::unexpected(client.error());
        return ClientConnectPacket{ std::move(*client) };
    }

    case PacketType::ClientDisconnect: {
        auto client = Deserialize<UserInfo>(r);
        if (!client) return std::unexpected(client.error());
        return ClientDisconnectPacket{ std::move(*client) };
    }

    case PacketType::MessageHistory: {
        auto messages = DeserializeVector<ChatMessage>(r);
        if (!messages) return std::unexpected(messages.error());
        return MessageHistoryPacket{ std::move(*messages) };
    }

    case PacketType::ServerShutdown:
        return ServerShutdownPacket{};

    case PacketType::ClientKick: {
        auto reason = Deserialize<std::string>(r);
        if (!reason) return std::unexpected(reason.error());
        return ClientKickPacket{ std::move(*reason) };
    }

    case PacketType::PrivateChatInvite: {
        auto name = Deserialize<std::string>(r);
        if (!name) return std::unexpected(name.error());
        return PrivateChatInvitePacket{ std::move(*name) };
    }

    case PacketType::PrivateChatConnectTo: {
        auto peer = Deserialize<std::string>(r);
        if (!peer) return std::unexpected(peer.error());
        auto addr = Deserialize<std::string>(r);
        if (!addr) return std::unexpected(addr.error());
        return PrivateChatConnectToPacket{ std::move(*peer), std::move(*addr) };
    }

    case PacketType::PrivateChatDeclined: {
        auto peer = Deserialize<std::string>(r);
        if (!peer) return std::unexpected(peer.error());
        return PrivateChatDeclinedPacket{ std::move(*peer) };
    }

    default:
        return std::unexpected(ParseError::InvalidPacketType);
    }
}

} // namespace Safira