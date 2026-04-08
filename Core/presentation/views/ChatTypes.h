#ifndef PQC_MASTER_THESIS_2026_CHATTYPES_H
#define PQC_MASTER_THESIS_2026_CHATTYPES_H

// ─────────────────────────────────────────────────────────────────────────────
// Defines ChatEntry, ConversationInfo, and MessageRole used across multiple
// view components (ChatPanel, ConversationListView, etc.).
// ─────────────────────────────────────────────────────────────────────────────

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Safira {

    enum class MessageRole : uint8_t { Own, Peer, System };

    struct ChatEntry {
        std::string  Who;
        std::string  Text;
        uint32_t     Color     = 0xFFFFFFFF;
        MessageRole  Role      = MessageRole::Peer;
        std::string  Time;              // "HH:MM" -- must be set at creation!
        ImTextureID  AvatarTex = {};    // per-message avatar image (or 0/null)
    };

    struct ConversationInfo {
        std::string              Title;
        std::string              Preview;
        std::string              TimeLabel;
        std::vector<ChatEntry>*  Messages   = nullptr;
        bool                     HasUnread  = false;
        ImTextureID              AvatarTex  = {};     // sidebar avatar for this convo
    };

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_CHATTYPES_H
