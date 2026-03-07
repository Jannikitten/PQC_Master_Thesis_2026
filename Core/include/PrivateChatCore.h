#ifndef PQC_MASTER_THESIS_2026_PRIVATE_CHAT_CORE_H
#define PQC_MASTER_THESIS_2026_PRIVATE_CHAT_CORE_H

#include "ChatPanel.h"
#include <string>
#include <string_view>
#include <vector>
#include <ranges>

namespace Safira::Crypto {

struct LogEntry {
    std::string Who;
    std::string Text;
    uint32_t    Color;
};

struct ChatState {
    std::vector<LogEntry>    Log;
    std::vector<std::string> PendingOutbound;
    bool                     IsConnected = false;
    bool                     IsRunning   = false;
    bool                     ScrollToBottom = false;
};

struct EventMessageReceived { std::string Who; std::string Text; uint32_t Color = 0xFFFFFFFF; };
struct EventMessageSent     { std::string Text; };
struct EventConnectionState { bool IsConnected; bool IsRunning; };
struct EventMessagesFlushed {};

[[nodiscard]] constexpr ChatState Reduce(ChatState state, EventMessageReceived event) {
    state.Log.push_back({ std::move(event.Who), std::move(event.Text), event.Color });
    state.ScrollToBottom = true;
    return state;
}

[[nodiscard]] constexpr ChatState Reduce(ChatState state, EventMessageSent event) {
    state.PendingOutbound.push_back(std::move(event.Text));
    return state;
}

[[nodiscard]] constexpr ChatState Reduce(ChatState state, EventConnectionState event) {
    state.IsConnected = event.IsConnected;
    state.IsRunning   = event.IsRunning;
    return state;
}

[[nodiscard]] constexpr ChatState Reduce(ChatState state, EventMessagesFlushed) {
    state.PendingOutbound.clear();
    return state;
}

[[nodiscard]] inline std::vector<ChatEntry> TransformLogToEntries(
    std::span<const LogEntry> log,
    std::string_view ownUsername)
{
    auto to_chat_entry = [ownUsername](const LogEntry& e) -> ChatEntry {
        MessageRole role = (e.Who == "System") ? MessageRole::System :
                           (e.Who == ownUsername) ? MessageRole::Own :
                           MessageRole::Peer;
        return { e.Who, e.Text, e.Color, role, {} };
    };

    auto view = log | std::views::transform(to_chat_entry);
    return { view.begin(), view.end() };
}

}

#endif // PQC_MASTER_THESIS_2026_PRIVATE_CHAT_CORE_H