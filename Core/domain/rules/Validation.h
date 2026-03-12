#ifndef SAFIRA_DOMAIN_RULES_VALIDATION_H
#define SAFIRA_DOMAIN_RULES_VALIDATION_H

// ═════════════════════════════════════════════════════════════════════════════
// Validation.h — pure business-rule functions for input validation
//
// Every function here is pure: same inputs always produce the same output,
// with no side effects.  This makes them trivially testable.
// ═════════════════════════════════════════════════════════════════════════════

#include "Message.h"
#include "User.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace Safira::Rules {

// ─────────────────────────────────────────────────────────────────────────────
// Username policy constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t kMinUsernameLength = 2;
static constexpr size_t kMaxUsernameLength = 24;

static constexpr std::array kReservedNames = {
    "SERVER", "SYSTEM", "Admin", "admin", "server", "system",
};

// ─────────────────────────────────────────────────────────────────────────────
// ValidateUsername — pure function
//
// Returns std::nullopt if valid, or a human-readable rejection reason.
// The caller passes existing usernames so the function stays pure (no
// access to mutable state).
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline std::optional<std::string>
ValidateUsername(std::string_view username,
                 std::ranges::range auto const& existingUsernames) {
    if (username.size() < kMinUsernameLength)
        return std::format("too short (min {} chars)", kMinUsernameLength);
    if (username.size() > kMaxUsernameLength)
        return std::format("too long (max {} chars)", kMaxUsernameLength);

    auto isAllowedChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c))
            || c == '_' || c == '-' || c == '.';
    };
    if (!std::ranges::all_of(username, isAllowedChar))
        return "contains invalid characters (allowed: a-z, 0-9, _ - .)";

    if (std::ranges::find(kReservedNames, username) != kReservedNames.end())
        return "reserved name";

    if (std::ranges::any_of(existingUsernames,
            [&](const auto& existing) { return existing == username; }))
        return "already taken";

    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// ValidateMessage — pure function
//
// Checks a message body against business rules.  Returns a sanitised
// (truncated) copy on success, or std::nullopt if invalid.
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline std::optional<std::string>
ValidateMessage(std::string_view message) {
    if (message.empty()) return std::nullopt;
    if (message.find_first_not_of(" \t\n\v\f\r") == std::string_view::npos)
        return std::nullopt;

    // Truncate to wire-format cap
    if (message.size() > static_cast<size_t>(MaxMessageLength))
        return std::string(message.substr(0, MaxMessageLength));

    return std::string(message);
}

// ─────────────────────────────────────────────────────────────────────────────
// ValidateAvatar — pure function
//
// Returns true if avatar data is within the allowed size.
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline bool
ValidateAvatar(const std::vector<uint8_t>& avatarData) {
    return avatarData.size() <= kMaxAvatarBytes;
}

} // namespace Safira::Rules

#endif // SAFIRA_DOMAIN_RULES_VALIDATION_H
