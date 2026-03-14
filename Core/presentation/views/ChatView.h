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
#include "ChatTypes.h"
#include "UserListView.h"
#include "ConversationListView.h"

#include <functional>
#include <optional>

struct ImDrawList;
struct ImFont;

namespace Safira {

// -----------------------------------------------------------------------------
// Chat-specific palette is now in Theme.h (Safira::ThemeData)
// Access via: const auto& t = Safira::Theme::Get();
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Full-layout input / output structs
// -----------------------------------------------------------------------------

struct FullLayoutInput {
    // Sidebar — user list
    std::vector<UserListEntry>           Users;
    // Sidebar — conversations
    const std::vector<ConversationInfo>* Conversations = nullptr;
    int                                  ActiveConvoIdx = 0;
    // Chat area
    bool              IsConnected    = false;
    bool              IsHandshaking  = false;
    bool              IsPrivateChat  = false;
    std::string       ConvoTitle;
    std::string       PeerName;
    std::string       StatusProtocol;
    ImTextureID       OwnAvatar      = {};
    ImTextureID       PeerAvatar     = {};
    std::string       Username;
    std::vector<ChatEntry>* Messages = nullptr;
    // Overlay
    bool              SuppressOverlay = false;
};

struct FullLayoutOutput {
    std::optional<int>         NewActiveIdx;
    std::optional<std::string> PendingMessage;
    bool                       LeaveRequested = false;
};

// -----------------------------------------------------------------------------
// ChatPanel
// -----------------------------------------------------------------------------

class ChatPanel {
public:
    ChatPanel();

    // -- Full-window mode (users + conversations sidebar + chat area) ---------
    FullLayoutOutput RenderFullLayout(const FullLayoutInput& input);

    // -- Standalone chat area (no sidebar) -----------------------------------
    void RenderChatArea(
        std::vector<ChatEntry>& messages,
        const std::string&      ownUsername,
        const std::string&      peerUsername,
        bool                    connected,
        bool                    handshaking);

    // -- Scroll control -------------------------------------------------------
    void RequestScrollToBottom() { m_ScrollToBottom = true; }

    // -- Sub-view access (for callback wiring) --------------------------------
    UserListView& GetUserListView() { return m_UserListView; }

    // -- Outbound message retrieval ------------------------------------------
    [[nodiscard]] std::optional<std::string> ConsumePendingMessage();

    // -- Timestamp utility (call at message creation, NOT at render) ----------
    static std::string NowTimestamp();

private:
    // Layout sub-sections
    void RenderSidebar(const FullLayoutInput& input, float sideW, float height,
                       FullLayoutOutput& out);
    void RenderChatSection(const FullLayoutInput& input, float chatW,
                           float height, FullLayoutOutput& out);
    void RenderSeparatorOverlay(ImVec2 origin, float width, float height,
                                float sideW, bool visible);

    // Chat area internals
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


    // Sub-views
    UserListView         m_UserListView;
    ConversationListView m_ConversationListView;

    // State
    static constexpr std::size_t kInputBufSize = 4096;
    char                       m_InputBuf[kInputBufSize] {};
    bool                       m_FocusInput     = false;
    bool                       m_ScrollToBottom  = false;
    bool                       m_PrivateMode     = false;
    std::optional<std::string> m_PendingOut;

    ImTextureID m_PeerAvatarTex = {};
    ImTextureID m_OwnAvatarTex  = {};

    bool m_LeaveRequested = false;

    using Callback = std::function<void()>;
    Callback m_OnLeave;

    std::string StatusProtocol;

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