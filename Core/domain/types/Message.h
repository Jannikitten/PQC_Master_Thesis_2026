#ifndef SAFIRA_DOMAIN_TYPES_MESSAGE_H
#define SAFIRA_DOMAIN_TYPES_MESSAGE_H

// ═════════════════════════════════════════════════════════════════════════════
// Message.h — pure domain types for chat messages
// ═════════════════════════════════════════════════════════════════════════════

#include <string>
#include <utility>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// ChatMessage — a username + message pair (used for history, wire format, etc.)
// ─────────────────────────────────────────────────────────────────────────────
struct ChatMessage {
    std::string Username;
    std::string Message;

    ChatMessage() = default;
    ChatMessage(std::string username, std::string message)
        : Username(std::move(username)), Message(std::move(message)) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Wire-format hard cap for message body
// ─────────────────────────────────────────────────────────────────────────────
constexpr int MaxMessageLength = 4096;

} // namespace Safira

#endif // SAFIRA_DOMAIN_TYPES_MESSAGE_H
