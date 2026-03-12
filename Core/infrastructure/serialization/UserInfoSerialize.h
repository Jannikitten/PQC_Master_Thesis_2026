#ifndef SAFIRA_INFRASTRUCTURE_SERIALIZATION_USERINFOSERIALIZE_H
#define SAFIRA_INFRASTRUCTURE_SERIALIZATION_USERINFOSERIALIZE_H

// ═════════════════════════════════════════════════════════════════════════════
// UserInfoSerialize.h — Serialize/Deserialize overloads for UserInfo and
//                       ChatMessage domain types
// ═════════════════════════════════════════════════════════════════════════════

// Domain types (canonical definitions)
#include "User.h"
#include "Message.h"

// Serialization primitives
#include "BufferIO.h"

namespace Safira {

// ── Serialization for UserInfo ──────────────────────────────────────────────

[[nodiscard]] inline bool Serialize(BufferWriter& w, const UserInfo& u) {
    return Serialize(w, u.Color)
        && Serialize(w, u.Username)
        && Serialize(w, u.AvatarData);
}

template <>
[[nodiscard]] inline std::expected<UserInfo, ParseError>
Deserialize<UserInfo>(BufferReader& r) {
    auto color = Deserialize<uint32_t>(r);
    if (!color) return std::unexpected(color.error());
    auto username = Deserialize<std::string>(r);
    if (!username) return std::unexpected(username.error());
    auto avatar = DeserializeVector<uint8_t>(r);
    if (!avatar) return std::unexpected(avatar.error());
    return UserInfo{ *color, std::move(*username), std::move(*avatar) };
}

// ── Serialization for ChatMessage ───────────────────────────────────────────

[[nodiscard]] inline bool Serialize(BufferWriter& w, const ChatMessage& m) {
    return Serialize(w, m.Username)
        && Serialize(w, m.Message);
}

template <>
[[nodiscard]] inline std::expected<ChatMessage, ParseError>
Deserialize<ChatMessage>(BufferReader& r) {
    auto username = Deserialize<std::string>(r);
    if (!username) return std::unexpected(username.error());
    auto message = Deserialize<std::string>(r);
    if (!message) return std::unexpected(message.error());
    return ChatMessage{ std::move(*username), std::move(*message) };
}

} // namespace Safira

#endif // SAFIRA_INFRASTRUCTURE_SERIALIZATION_USERINFOSERIALIZE_H
