#ifndef PQC_MASTER_THESIS_2026_USERINFO_H
#define PQC_MASTER_THESIS_2026_USERINFO_H

// ═════════════════════════════════════════════════════════════════════════════
// UserInfo.h — chat user metadata and message types
//
// §5.3  Serialization  – free-function Serialize / Deserialize overloads
//                        constrained by concepts (Serialization.h)
// C++23               – std::expected, concepts
// ═════════════════════════════════════════════════════════════════════════════

#include "Serialization.h"

#include <cstdint>
#include <string>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Icons — 8 predefined coloured circles drawn via ImDrawList.
// The index is stored in UserInfo and transmitted with every client list update.
// ─────────────────────────────────────────────────────────────────────────────
namespace Icons {
    static constexpr int kCount = 8;

    // ABGR colours (ImGui IM_COL32 byte order)
    static constexpr uint32_t kColors[kCount] = {
        0xFF4444EE, // Red
        0xFF44CC44, // Green
        0xFFEE8844, // Orange
        0xFF44BBEE, // Sky blue
        0xFFEEEE44, // Yellow
        0xFFCC44CC, // Purple
        0xFF44EECC, // Teal
        0xFFEEEEEE, // White
    };

    static constexpr const char* kLabels[kCount] = {
        "Red", "Green", "Orange", "Blue",
        "Yellow", "Purple", "Teal", "White",
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// UserInfo
// ─────────────────────────────────────────────────────────────────────────────
struct UserInfo {
    uint32_t    Color     = 0xFFFFFFFF;
    std::string Username;
    uint8_t     IconIndex = 0;          // index into Icons::kColors[]
};

// §5.3 — Free-function serialization (concept-based, replaces old static methods)
[[nodiscard]] inline bool Serialize(BufferWriter& w, const UserInfo& u) {
    return Serialize(w, u.Color)
        && Serialize(w, u.Username)
        && Serialize(w, u.IconIndex);
}

template <>
[[nodiscard]] inline std::expected<UserInfo, ParseError>
Deserialize<UserInfo>(BufferReader& r) {
    auto color = Deserialize<uint32_t>(r);
    if (!color) return std::unexpected(color.error());
    auto username = Deserialize<std::string>(r);
    if (!username) return std::unexpected(username.error());
    auto icon = Deserialize<uint8_t>(r);
    if (!icon) return std::unexpected(icon.error());
    return UserInfo{ *color, std::move(*username), *icon };
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
