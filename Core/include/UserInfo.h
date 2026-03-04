#ifndef PQC_MASTER_THESIS_2026_USERINFO_H
#define PQC_MASTER_THESIS_2026_USERINFO_H

// ═════════════════════════════════════════════════════════════════════════════
// UserInfo.h — chat user metadata and message types
//
// §5.3  Serialization  – free-function Serialize / Deserialize overloads
//                        constrained by concepts (Serialization.h)
// C++23               – std::expected, concepts
//
// v2: AvatarData added — raw RGBA pixels (64×64, ≤ 16 KB).  Transmitted with
//     every client-list update.  Empty = letter-circle fallback.
//     IconIndex removed — replaced by avatar image or random colour.
// ═════════════════════════════════════════════════════════════════════════════

#include "Serialization.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Avatar constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int    kAvatarPixelSize = 64;           // target square size
static constexpr size_t kMaxAvatarBytes  = kAvatarPixelSize * kAvatarPixelSize * 4;  // 16 KB raw RGBA

// ─────────────────────────────────────────────────────────────────────────────
// Random avatar colours — used when no image is set.
// Server assigns one at registration time.  8 visually distinct hues.
// ─────────────────────────────────────────────────────────────────────────────
namespace AvatarColors {
    static constexpr int kCount = 8;

    // ABGR colours (ImGui IM_COL32 byte order)
    static constexpr uint32_t kPalette[kCount] = {
        0xFF4444EE, // Red
        0xFF44CC44, // Green
        0xFFEE8844, // Orange
        0xFF44BBEE, // Sky blue
        0xFFEEEE44, // Yellow
        0xFFCC44CC, // Purple
        0xFF44EECC, // Teal
        0xFFEEEEEE, // White
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// UserInfo
//
// Color is now server-assigned (random from AvatarColors).
// AvatarData carries raw RGBA pixels of the user's chosen image
// (kAvatarPixelSize × kAvatarPixelSize × 4 channels).
// When empty the client falls back to a coloured-circle + letter.
// ─────────────────────────────────────────────────────────────────────────────
struct UserInfo {
    uint32_t              Color      = 0xFFFFFFFF;
    std::string           Username;
    std::vector<uint8_t>  AvatarData;    // raw RGBA, ≤ kMaxAvatarBytes
};

// §5.3 — Free-function serialization

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

// ─────────────────────────────────────────────────────────────────────────────
// ChatMessage
// ─────────────────────────────────────────────────────────────────────────────
struct ChatMessage {
    std::string Username;
    std::string Message;

    ChatMessage() = default;
    ChatMessage(std::string username, std::string message)
        : Username(std::move(username)), Message(std::move(message)) {}
};

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

// ─────────────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────────────
constexpr int MaxMessageLength = 4096;

} // namespace Safira

#endif //PQC_MASTER_THESIS_2026_USERINFO_H