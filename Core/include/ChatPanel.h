#ifndef PQC_MASTER_THESIS_2026_CHATPANEL_H
#define PQC_MASTER_THESIS_2026_CHATPANEL_H

// =============================================================================
// ChatPanel.h -- Claude-style chat rendering panel
//
// Features:
//   - Private mode (hides author names in bubbles, shows peer header)
//   - Image avatar support via ImTextureID (circular rendering)
//   - Leave-chat callback for private sessions
//   - Timestamp set at creation, NOT during render
// =============================================================================

#include <imgui.h>
#include "Theme.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace Safira {

// -----------------------------------------------------------------------------
// Data model
// -----------------------------------------------------------------------------

enum class MessageRole : uint8_t { Own, Peer, System };

struct ChatEntry {
    std::string  Who;
    std::string  Text;
    uint32_t     Color     = 0xFFFFFFFF;
    MessageRole  Role      = MessageRole::Peer;
    std::string  Time;              // "HH:MM" -- must be set at creation!
    ImTextureID  AvatarTex = {};        // per-message avatar image (or 0/null)
};

struct ConversationInfo {
    std::string              Title;
    std::string              Preview;
    std::string              TimeLabel;
    std::vector<ChatEntry>*  Messages   = nullptr;
    bool                     HasUnread  = false;
    ImTextureID              AvatarTex  = {};     // sidebar avatar for this convo
};

// -----------------------------------------------------------------------------
// Chat-specific palette is now in Theme.h (Safira::ThemeData)
// Access via: const auto& t = Safira::Theme::Get();
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// ChatPanel
// -----------------------------------------------------------------------------

class ChatPanel {
public:
    ChatPanel();

    // -- Full-window mode (sidebar + chat area) ------------------------------
    std::optional<int> RenderFullLayout(
        std::vector<ConversationInfo>& conversations,
        int                            activeIdx,
        const std::string&             ownUsername);

    // -- Standalone chat area (no sidebar) -----------------------------------
    void RenderChatArea(
        std::vector<ChatEntry>& messages,
        const std::string&      ownUsername,
        const std::string&      peerUsername,
        bool                    connected,
        bool                    handshaking);

    // -- Outbound message retrieval ------------------------------------------
    [[nodiscard]] std::optional<std::string> ConsumePendingMessage();

    // -- Scroll control -------------------------------------------------------
    void RequestScrollToBottom() { m_ScrollToBottom = true; }

    // -- Callbacks -----------------------------------------------------------
    using Callback = std::function<void()>;
    void SetNewConversationCallback(Callback fn) { m_OnNewConversation = std::move(fn); }
    void SetOnLeaveCallback(Callback fn)         { m_OnLeave = std::move(fn); }

    // -- Private chat mode ---------------------------------------------------
    void SetPrivateChatMode(bool enabled) { m_PrivateMode = enabled; }
    bool IsPrivateChatMode() const        { return m_PrivateMode; }

    // -- Avatar textures (for header and own messages) -----------------------
    void SetPeerAvatar(ImTextureID tex)   { m_PeerAvatarTex = tex; }
    void SetOwnAvatar(ImTextureID tex)    { m_OwnAvatarTex = tex; }

    // -- Timestamp utility (call at message creation, NOT at render) ----------
    static std::string NowTimestamp();

    // -- Public config -------------------------------------------------------
    std::string StatusProtocol = "DTLS 1.3 | ML-KEM-512";

private:
    // Sub-panels
    int  RenderSidebar(std::vector<ConversationInfo>& convos, int activeIdx,
                       float width, float height);
    void RenderMessages(std::vector<ChatEntry>& messages, float width,
                        float height, const std::string& ownUsername);
    void RenderInputBar(float areaWidth, const std::string& ownUsername);
    void RenderStatusIndicator(bool connected, bool handshaking,
                               const std::string& peer);

    // Drawing helpers
    void DrawBubble(ImDrawList* dl, const ChatEntry& msg,
                    float regionWidth, const std::string& ownUsername);
    void DrawAvatar(ImDrawList* dl, float cx, float cy, float radius,
                    char letter, ImU32 bgCol, ImU32 textCol,
                    ImTextureID tex = {});

    // State
    static constexpr std::size_t kInputBufSize = 4096;
    char                       m_InputBuf[kInputBufSize] {};
    bool                       m_FocusInput     = false;
    bool                       m_ScrollToBottom  = false;
    bool                       m_PrivateMode     = false;
    std::optional<std::string> m_PendingOut;

    ImTextureID m_PeerAvatarTex = {};
    ImTextureID m_OwnAvatarTex  = {};

    Callback m_OnNewConversation;
    Callback m_OnLeave;

    // Layout constants
    static constexpr float kSidebarWidth   = 260.0f;
    static constexpr float kBubbleRounding = 16.0f;
    static constexpr float kBubbleMaxFrac  = 0.72f;
    static constexpr float kBubblePadX     = 14.0f;
    static constexpr float kBubblePadY     = 8.0f;
    static constexpr float kAvatarRadius   = 15.0f;
    static constexpr float kInputBarHeight = 64.0f;
    static constexpr float kInputRounding  = 20.0f;
};

} // namespace Safira
#endif // PQC_MASTER_THESIS_2026_CHATPANEL_H