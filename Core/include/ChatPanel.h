#ifndef PQC_MASTER_THESIS_2026_CHATPANEL_H
#define PQC_MASTER_THESIS_2026_CHATPANEL_H

// =============================================================================
// ChatPanel.h -- Claude-style chat rendering panel
//
// Designed to plug into the existing ApplicationGUI layer stack.
// Uses fonts from ApplicationGUI::GetFont() and the existing Safira theme.
//
// Usage inside a Layer::OnUIRender():
//
//     // Once, in constructor or OnAttach:
//     m_ChatPanel = std::make_unique<ChatPanel>();
//
//     // Every frame, inside your ImGui window:
//     m_ChatPanel->RenderChatArea(messages, ownUser, peerUser, connected, handshaking);
//     if (auto msg = m_ChatPanel->ConsumePendingMessage()) {
//         session.Send(*msg);
//         session.AppendMessage(ownUsername, *msg, myColor);
//     }
//
// Or use the full-window overload that includes the sidebar:
//     m_ChatPanel->RenderFullLayout(conversations, activeIdx, ownUsername);
// =============================================================================

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace Safira {

// -----------------------------------------------------------------------------
// Data model -- mirrors PrivateChatSession::LogEntry but adds role semantics
// -----------------------------------------------------------------------------

enum class MessageRole : uint8_t { Own, Peer, System };

struct ChatEntry {
    std::string  Who;
    std::string  Text;
    uint32_t     Color = 0xFFFFFFFF;
    MessageRole  Role  = MessageRole::Peer;
    std::string  Time;           // "HH:MM", filled by ChatPanel if empty
};

struct ConversationInfo {
    std::string              Title;
    std::string              Preview;    // last-message snippet
    std::string              TimeLabel;  // "now", "2m", "Yesterday"
    std::vector<ChatEntry>*  Messages = nullptr;   // non-owning
    bool                     HasUnread = false;
};

// -----------------------------------------------------------------------------
// Chat-specific palette -- extends the existing Safira theme
// -----------------------------------------------------------------------------

struct ChatColors {
    // Surfaces — dark, matching Safira's titlebar/background palette
    ImU32 BgMain          = IM_COL32( 36,  36,  36, 255);
    ImU32 BgSidebar       = IM_COL32( 28,  28,  28, 255);
    ImU32 BgSidebarHover  = IM_COL32( 46,  46,  46, 255);
    ImU32 BgSidebarActive = IM_COL32( 54,  54,  54, 255);

    // Bubbles
    ImU32 BgOwnBubble     = IM_COL32( 50,  64,  82, 255);  // muted steel-blue
    ImU32 BgPeerBubble    = IM_COL32( 50,  50,  50, 255);
    ImU32 BgSystemBubble  = IM_COL32( 36,  36,  36, 255);

    // Accent — warm gold matching Safira tab highlights (255,225,135)
    ImU32 Accent          = IM_COL32(218, 185, 107, 255);
    ImU32 AccentHover     = IM_COL32(240, 206, 125, 255);
    ImU32 AccentText      = IM_COL32( 18,  18,  18, 255);

    // Text — light on dark
    ImU32 TextPrimary     = IM_COL32(210, 210, 210, 255);
    ImU32 TextSecondary   = IM_COL32(140, 140, 140, 255);
    ImU32 TextMuted       = IM_COL32(100, 100, 100, 255);
    ImU32 TextSystem      = IM_COL32(115, 115, 115, 255);

    // Misc
    ImU32 Divider         = IM_COL32( 56,  56,  56, 255);
    ImU32 InputBg         = IM_COL32( 48,  48,  48, 255);
    ImU32 InputBorder     = IM_COL32( 68,  68,  68, 255);
    ImU32 InputShadow     = IM_COL32(  0,   0,   0,  40);
    ImU32 UnreadDot       = IM_COL32(218, 185, 107, 255);
    ImU32 StatusOnline    = IM_COL32( 76, 200,  76, 255);
    ImU32 StatusPending   = IM_COL32(218, 185, 107, 255);
    ImU32 StatusOffline   = IM_COL32(180,  65,  65, 255);
};

// -----------------------------------------------------------------------------
// ChatPanel
// -----------------------------------------------------------------------------

class ChatPanel {
public:
    ChatPanel();

    // -- Full-window mode (sidebar + chat area) ------------------------------
    // Call this inside an ImGui::Begin/End block that spans the full panel.
    // Returns the index of the newly active conversation if the user
    // clicked a different one, or std::nullopt if unchanged.
    std::optional<int> RenderFullLayout(
        std::vector<ConversationInfo>& conversations,
        int                            activeIdx,
        const std::string&             ownUsername);

    // -- Standalone chat area (no sidebar) -----------------------------------
    // Drop-in replacement for PrivateChatSession::OnUIRender() internals.
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

    // -- New-conversation callback -------------------------------------------
    using NewConvoCallback = std::function<void()>;
    void SetNewConversationCallback(NewConvoCallback fn) {
        m_OnNewConversation = std::move(fn);
    }

    ChatColors  Colors;
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
                    char letter, ImU32 bgCol, ImU32 textCol);

    // Utility
    static std::string NowTimestamp();

    // State
    static constexpr std::size_t kInputBufSize = 4096;
    char                      m_InputBuf[kInputBufSize] {};
    bool                      m_FocusInput    = false;
    bool                      m_ScrollToBottom = false;
    std::optional<std::string> m_PendingOut;
    NewConvoCallback          m_OnNewConversation;

    // Layout constants
    static constexpr float kSidebarWidth   = 260.0f;
    static constexpr float kBubbleRounding = 16.0f;
    static constexpr float kBubbleMaxFrac  = 0.72f;   // of chat width
    static constexpr float kBubblePadX     = 14.0f;
    static constexpr float kBubblePadY     = 8.0f;
    static constexpr float kAvatarRadius   = 15.0f;
    static constexpr float kInputBarHeight = 64.0f;
    static constexpr float kInputRounding  = 20.0f;
};

} // namespace Safira
#endif // PQC_MASTER_THESIS_2026_CHATPANEL_H