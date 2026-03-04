#include "ChatPanel.h"
#include "ApplicationGUI.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>

namespace Safira {

// ===========================================================================
// Font helpers -- ZERO direct ImFont member access.
// ===========================================================================

static ImFont* GetBodyFont() {
    ImFont* f = ApplicationGUI::GetFont("Default");
    return f ? f : ImGui::GetFont();
}

static ImFont* GetBoldFont() {
    ImFont* f = ApplicationGUI::GetFont("Bold");
    return f ? f : GetBodyFont();
}

static float FontHeight(ImFont* f) {
    if (!f) return ImGui::GetFontSize();
    ImGui::PushFont(f);
    float h = ImGui::GetFontSize();
    ImGui::PopFont();
    return h;
}

static ImVec2 MeasureText(ImFont* f, const char* text, float wrapWidth = 0.0f) {
    if (f) ImGui::PushFont(f);
    ImVec2 sz = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
    if (f) ImGui::PopFont();
    return sz;
}

static void DrawTextAt(ImFont* f, ImVec2 pos, ImU32 col,
                       const char* text, float wrapWidth = 0.0f) {
    ImGui::SetCursorScreenPos(pos);
    if (f) ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    if (wrapWidth > 0.0f) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextUnformatted(text);
    }
    ImGui::PopStyleColor();
    if (f) ImGui::PopFont();
}


// ---------------------------------------------------------------------------
ChatPanel::ChatPanel() = default;

// ===========================================================================
// RenderFullLayout
// ===========================================================================

std::optional<int> ChatPanel::RenderFullLayout(
    std::vector<ConversationInfo>& conversations,
    int activeIdx, const std::string& ownUsername)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sideW = kSidebarWidth;
    const float chatW = avail.x - sideW;

    int newIdx = RenderSidebar(conversations, activeIdx, sideW, avail.y);
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, U32ToVec4(Theme::Get().BgPanel));
    ImGui::BeginChild("##ChatArea", { chatW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);

    if (activeIdx >= 0 && activeIdx < (int)conversations.size()
        && conversations[activeIdx].Messages)
    {
        auto& convo = conversations[activeIdx];
        ImGui::SetCursorPos({ 14.0f, 8.0f });
        RenderStatusIndicator(true, false, convo.Title);
        float msgH = ImGui::GetContentRegionAvail().y - kInputBarHeight - 8.0f;
        RenderMessages(*convo.Messages, chatW, msgH, ownUsername);
        RenderInputBar(chatW, ownUsername);
    } else {
        const char* sub = "Select a conversation or start a new one.";
        ImFont* body = GetBodyFont();
        ImVec2 sSz = MeasureText(body, sub);
        ImGui::SetCursorPos({ (chatW - sSz.x) * 0.5f, avail.y * 0.45f });
        ImGui::TextColored(U32ToVec4(Theme::Get().TextMuted), "%s", sub);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (newIdx != activeIdx) return newIdx;
    return std::nullopt;
}

// ===========================================================================
// RenderChatArea -- standalone chat area (used inside UI_UnifiedChatWindow)
// ===========================================================================

void ChatPanel::RenderChatArea(
    std::vector<ChatEntry>& messages, const std::string& ownUsername,
    const std::string& peerUsername, bool connected, bool handshaking)
{
    RenderStatusIndicator(connected, handshaking, peerUsername);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, { p.x + w, p.y }, Theme::Get().Divider, 1.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

    float remaining = ImGui::GetContentRegionAvail().y - kInputBarHeight - 8.0f;
    RenderMessages(messages, w, remaining, ownUsername);
    RenderInputBar(w, ownUsername);
}

// ===========================================================================
// Sidebar
// ===========================================================================

int ChatPanel::RenderSidebar(std::vector<ConversationInfo>& convos,
                             int activeIdx, float width, float height) {
    int result = activeIdx;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, U32ToVec4(Theme::Get().BgPanel));
    ImGui::BeginChild("##ChatSidebar", { width, height }, false,
                      ImGuiWindowFlags_NoScrollbar);

    const float pad = 14.0f;
    ImGui::SetCursorPos({ pad, pad });

    ImFont* bold = GetBoldFont();
    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored(U32ToVec4(Theme::Get().TextPrimary), "Conversations");
    if (bold) ImGui::PopFont();

    ImGui::SameLine(width - pad - 26.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,       U32ToVec4(Theme::Get().Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, U32ToVec4(Theme::Get().AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  U32ToVec4(Theme::Get().AccentHover));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 13.0f);
    if (ImGui::Button("+", { 26.0f, 26.0f })) {
        if (m_OnNewConversation) m_OnNewConversation();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine({ p.x + pad, p.y }, { p.x + width - pad, p.y },
                    Theme::Get().Divider, 1.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
    }

    ImGui::BeginChild("##ConvoList", { 0, 0 }, false);
    ImFont* body = GetBodyFont();

    for (int i = 0; i < (int)convos.size(); ++i) {
        const auto& c = convos[i];
        const bool selected = (i == activeIdx);
        ImGui::PushID(i);

        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImVec2 itemSz = { width - 4.0f, 58.0f };

        bool hovered = ImGui::IsMouseHoveringRect(
            cursor, { cursor.x + itemSz.x, cursor.y + itemSz.y });

        ImU32 bg = 0;
        if (selected)     bg = Theme::Get().BgItemSelected;
        else if (hovered) bg = Theme::Get().BgItemHovered;
        if (bg)
            ImGui::GetWindowDrawList()->AddRectFilled(
                cursor, { cursor.x + itemSz.x, cursor.y + itemSz.y }, bg, 6.0f);

        if (ImGui::InvisibleButton("##c", itemSz))
            result = i;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float tx = cursor.x + pad;

        // Avatar -- use conversation texture if available
        float ax = tx + kAvatarRadius;
        float ay = cursor.y + itemSz.y * 0.5f;
        char letter = c.Title.empty() ? '?' : (char)toupper(c.Title[0]);

        ImU32 avatarBg = (i == 0)
            ? Theme::Get().LobbyAvatar
            : Theme::Get().Accent;

        DrawAvatar(dl, ax, ay, kAvatarRadius, letter,
                   avatarBg, Theme::Get().AccentText, c.AvatarTex);

        float textX = tx + kAvatarRadius * 2.0f + 10.0f;

        DrawTextAt(bold, { textX, cursor.y + 10.0f },
                   Theme::Get().TextPrimary, c.Title.c_str());

        std::string preview = c.Preview.substr(0, 32);
        if (c.Preview.size() > 32) preview += "...";
        DrawTextAt(body, { textX, cursor.y + 30.0f },
                   Theme::Get().TextSecondary, preview.c_str());

        if (!c.TimeLabel.empty()) {
            ImVec2 tSz = MeasureText(body, c.TimeLabel.c_str());
            DrawTextAt(body,
                { cursor.x + width - pad - tSz.x - 4.0f, cursor.y + 12.0f },
                Theme::Get().TextMuted, c.TimeLabel.c_str());
        }

        if (c.HasUnread)
            dl->AddCircleFilled(
                { cursor.x + width - pad - 4.0f, cursor.y + itemSz.y * 0.5f },
                4.0f, Theme::Get().UnreadDot, 12);

        ImGui::SetCursorScreenPos({ cursor.x, cursor.y + itemSz.y });
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return result;
}

// ===========================================================================
// Messages
//
// BUG FIX: Removed lazy timestamp fill (msg.Time = NowTimestamp()) that was
// called during rendering. Timestamps must be set at message creation time
// in ClientLayer::AddLobbyMessage() and packet handlers.
// ===========================================================================

void ChatPanel::RenderMessages(std::vector<ChatEntry>& messages,
                               float width, float height,
                               const std::string& ownUsername) {
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          Theme::Get().ScrollBg);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,         Theme::Get().ScrollGrab);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,  Theme::Get().ScrollGrabHover);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,   Theme::Get().ScrollGrabActive);

    ImGui::BeginChild("##MsgScroll", { width, height }, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (messages.empty()) {
        const char* hint = "No messages yet.";
        ImVec2 sz = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPos({ (width - sz.x) * 0.5f, (height - sz.y) * 0.5f });
        ImGui::TextColored(U32ToVec4(Theme::Get().TextMuted), "%s", hint);
    } else {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        for (auto& msg : messages) {
            // NO lazy timestamp fill here -- timestamps set at creation time.
            // If somehow empty, just leave it blank rather than overwriting
            // every frame with the current time.
            DrawBubble(dl, msg, width, ownUsername);
        }
    }

    if (m_ScrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        m_ScrollToBottom = false;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}

// ===========================================================================
// Bubble
//
// In private mode (m_PrivateMode == true):
//   - Author name is hidden
//   - Bubble height is reduced (no author line)
// ===========================================================================

void ChatPanel::DrawBubble(ImDrawList* dl, const ChatEntry& msg,
                           float regionWidth, const std::string& ownUsername) {
    const bool isOwn    = (msg.Role == MessageRole::Own || msg.Who == ownUsername);
    const bool isSystem = (msg.Role == MessageRole::System || msg.Who == "System");

    if (isSystem) {
        ImVec2 sz = ImGui::CalcTextSize(msg.Text.c_str(), nullptr, false,
                                        regionWidth * 0.8f);
        ImGui::SetCursorPosX((regionWidth - sz.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, U32ToVec4(Theme::Get().TextSystem));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + regionWidth * 0.8f);
        ImGui::TextUnformatted(msg.Text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy({ 0, 6.0f });
        return;
    }

    ImFont* body = GetBodyFont();
    ImFont* bold = GetBoldFont();

    float maxBubbleW = regionWidth * kBubbleMaxFrac;
    float avatarSpace = kAvatarRadius * 2.0f + 10.0f;
    float textWrapW   = maxBubbleW - kBubblePadX * 2.0f;

    // In private mode, skip the author name line
    const bool showAuthor = !m_PrivateMode;

    float boldH   = FontHeight(bold);
    float authorH = showAuthor ? (boldH + 2.0f) : 0.0f;

    ImVec2 textSz   = MeasureText(body, msg.Text.c_str(), textWrapW);
    ImVec2 authorSz = showAuthor ? MeasureText(bold, msg.Who.c_str()) : ImVec2{0, 0};

    float bubbleW = std::max(textSz.x, authorSz.x) + kBubblePadX * 2.0f;
    float bubbleH = textSz.y + kBubblePadY * 2.0f + authorH;

    float marginX = 16.0f;
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    // In private mode, no avatar shown next to bubbles
    float avatarOffset = (m_PrivateMode || isOwn) ? 0.0f : avatarSpace;

    float bubbleX = isOwn
        ? cursor.x + regionWidth - bubbleW - marginX
        : cursor.x + marginX + avatarOffset;
    float bubbleY = cursor.y;

    // Avatar (peer only, NOT in private mode)
    if (!isOwn && !m_PrivateMode) {
        float ax = cursor.x + marginX + kAvatarRadius;
        float ay = bubbleY + kAvatarRadius + 2.0f;
        char letter = msg.Who.empty() ? '?' : (char)toupper(msg.Who[0]);
        DrawAvatar(dl, ax, ay, kAvatarRadius, letter,
                   Theme::Get().Accent, Theme::Get().AccentText, msg.AvatarTex);
    }

    // Bubble background
    ImU32 bubbleCol = isOwn ? Theme::Get().BgOwnBubble : Theme::Get().BgPeerBubble;
    dl->AddRectFilled({ bubbleX, bubbleY },
                      { bubbleX + bubbleW, bubbleY + bubbleH },
                      bubbleCol, kBubbleRounding, ImDrawFlags_RoundCornersAll);

    // Tail corner
    if (isOwn) {
        dl->AddRectFilled(
            { bubbleX + bubbleW - kBubbleRounding, bubbleY },
            { bubbleX + bubbleW, bubbleY + kBubbleRounding },
            bubbleCol, 3.0f, ImDrawFlags_RoundCornersTopRight);
    } else {
        dl->AddRectFilled(
            { bubbleX, bubbleY },
            { bubbleX + kBubbleRounding, bubbleY + kBubbleRounding },
            bubbleCol, 3.0f, ImDrawFlags_RoundCornersTopLeft);
    }

    // Border on peer bubbles
    if (!isOwn)
        dl->AddRect({ bubbleX, bubbleY },
                    { bubbleX + bubbleW, bubbleY + bubbleH },
                    Theme::Get().Divider, kBubbleRounding, 0, 1.0f);

    // Author name (skipped in private mode)
    float textY = bubbleY + kBubblePadY;
    if (showAuthor) {
        ImU32 nameCol = isOwn ? Theme::Get().TextPrimary : Theme::Get().Accent;
        DrawTextAt(bold, { bubbleX + kBubblePadX, textY }, nameCol, msg.Who.c_str());
        textY += authorH;
    }

    // Body text
    DrawTextAt(body, { bubbleX + kBubblePadX, textY },
               Theme::Get().TextPrimary, msg.Text.c_str(), textWrapW);

    // Timestamp
    if (!msg.Time.empty()) {
        ImVec2 tsSz = MeasureText(nullptr, msg.Time.c_str());
        DrawTextAt(nullptr,
            { bubbleX + bubbleW - tsSz.x - kBubblePadX,
              bubbleY + bubbleH + 2.0f },
            Theme::Get().TextMuted, msg.Time.c_str());
    }

    // Advance cursor
    ImGui::SetCursorScreenPos({ cursor.x, bubbleY + bubbleH + 14.0f });
    ImGui::Dummy({ 0, 0 });
}

// ===========================================================================
// Avatar -- supports optional ImTextureID for image avatars
//
// When tex != 0: renders a circular image using AddImageRounded.
// When tex == 0: falls back to colored circle + centered letter.
// ===========================================================================

void ChatPanel::DrawAvatar(ImDrawList* dl, float cx, float cy, float radius,
                           char letter, ImU32 bgCol, ImU32 textCol,
                           ImTextureID tex) {
    if (tex) {
        // Circular image avatar
        dl->AddImageRounded(
            tex,
            { cx - radius, cy - radius },
            { cx + radius, cy + radius },
            { 0.0f, 0.0f }, { 1.0f, 1.0f },
            Theme::Get().AvatarImageTint,
            radius);
    } else {
        // Colored circle with letter fallback
        dl->AddCircleFilled({ cx, cy }, radius, bgCol, 24);

        char buf[2] = { letter, '\0' };
        ImFont* bold = GetBoldFont();
        ImVec2 sz = MeasureText(bold, buf);

        if (bold) ImGui::PushFont(bold);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { cx - sz.x * 0.5f, cy - sz.y * 0.5f }, textCol, buf);
        if (bold) ImGui::PopFont();
    }
}

// ===========================================================================
// Status indicator
//
// In private mode: shows peer avatar (if set) + name + leave button
// ===========================================================================

void ChatPanel::RenderStatusIndicator(bool connected, bool handshaking,
                                      const std::string& peer) {
    ImU32 dotCol;
    std::string label;

    if (connected) {
        dotCol = Theme::Get().StatusOnline;
        label  = "Connected  (" + StatusProtocol + ")";
    } else if (handshaking) {
        dotCol = Theme::Get().StatusPending;
        label  = "Handshaking...";
    } else {
        dotCol = Theme::Get().StatusOffline;
        label  = "Disconnected";
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* bold = GetBoldFont();
    ImFont* body = GetBodyFont();

    // Use cursor position set by caller (no additional left offset)
    const float startX  = ImGui::GetCursorPosX();
    const float regionW = ImGui::GetContentRegionAvail().x + startX; // total width
    const float rightPad = startX; // match left padding on right side

    // ── In private mode: [name] [dot] ......................... [Leave] ────
    // Layout matches lobby mode height: one line of bold text + 4px padding.
    // No avatar — it's already visible in the sidebar.
    if (m_PrivateMode) {
        // Measure text height to size the Leave button
        float lineH = FontHeight(bold);

        // Measure Leave button — compact, fits within the text line
        constexpr float btnPadX = 8.0f;
        constexpr float btnPadY = 0.0f;
        ImVec2 leaveSz = MeasureText(body, "Leave");
        const float btnW = leaveSz.x + btnPadX * 2.0f;

        // Right edge for name truncation (leave room for button + gap)
        const float leaveX = regionW - rightPad - btnW;
        const float nameRightEdge = leaveX - 20.0f; // gap before button

        // Peer name (truncated to fit before Leave button)
        {
            const float maxNameW = nameRightEdge - startX;

            if (bold) ImGui::PushFont(bold);
            ImVec2 nameSz = MeasureText(bold, peer.c_str());
            if (nameSz.x <= maxNameW || maxNameW <= 0) {
                ImGui::TextColored(U32ToVec4(Theme::Get().TextPrimary), "%s", peer.c_str());
            } else {
                // Truncate with ellipsis
                std::string truncated = peer;
                ImVec2 dotsSz = MeasureText(bold, "...");
                while (!truncated.empty()) {
                    truncated.pop_back();
                    ImVec2 sz = MeasureText(bold, truncated.c_str());
                    if (sz.x + dotsSz.x <= maxNameW) {
                        truncated += "...";
                        break;
                    }
                }
                ImGui::TextColored(U32ToVec4(Theme::Get().TextPrimary), "%s", truncated.c_str());
            }
            if (bold) ImGui::PopFont();
        }

        ImGui::SameLine(0.0f, 8.0f);

        // Status dot (compact, no label — same as lobby dot)
        ImVec2 dotPos = ImGui::GetCursorScreenPos();
        float textH = ImGui::GetFontSize();
        dl->AddCircleFilled({ dotPos.x + 4.0f, dotPos.y + textH * 0.5f },
                            4.0f, dotCol, 12);
        ImGui::Dummy({ 12.0f, textH }); // advance past dot

        // Leave button pinned to right, vertically centred within the line
        if (m_OnLeave) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(leaveX);

            ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Get().DangerBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  Theme::Get().DangerBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   Theme::Get().DangerBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,           Theme::Get().AvatarLetterCol);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { btnPadX, btnPadY });

            if (ImGui::Button("Leave##pvt")) {
                m_OnLeave();
            }

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);
        }

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        return;
    }

    // ── Non-private (Lobby) mode: [name] [dot] [Connected (...)] ────────

    // Peer name
    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored(U32ToVec4(Theme::Get().TextPrimary), "%s", peer.c_str());
    if (bold) ImGui::PopFont();

    ImGui::SameLine(0.0f, 12.0f);

    // Status dot
    ImVec2 dotPos = ImGui::GetCursorScreenPos();
    float textH = ImGui::GetFontSize();
    dl->AddCircleFilled({ dotPos.x + 4.0f, dotPos.y + textH * 0.5f },
                        4.0f, dotCol, 12);

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
    ImGui::TextColored(U32ToVec4(Theme::Get().TextSecondary), "%s", label.c_str());

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
}

// ===========================================================================
// Input bar
// ===========================================================================

void ChatPanel::RenderInputBar(float areaWidth, const std::string&) {
    float pad = 16.0f;
    float btnW = 36.0f;
    float inputW = areaWidth - pad * 2.0f - btnW - 8.0f;
    float inputH = kInputBarHeight - 12.0f;

    ImGui::SetCursorPos({ pad, ImGui::GetCursorPosY() + 4.0f });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 inputPos = ImGui::GetCursorScreenPos();

    dl->AddRectFilled(
        { inputPos.x - 1, inputPos.y - 1 },
        { inputPos.x + inputW + 1, inputPos.y + inputH + 2 },
        Theme::Get().InputShadow, kInputRounding + 1);
    dl->AddRectFilled(inputPos,
        { inputPos.x + inputW, inputPos.y + inputH },
        Theme::Get().BgInput, kInputRounding);
    dl->AddRect(inputPos,
        { inputPos.x + inputW, inputPos.y + inputH },
        Theme::Get().InputBorder, kInputRounding, 0, 1.2f);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        { 14.0f, (inputH - ImGui::GetFontSize()) * 0.5f });
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kInputRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,       IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Text, U32ToVec4(Theme::Get().TextPrimary));

    ImGui::PushItemWidth(inputW);
    ImFont* body = GetBodyFont();
    if (body) ImGui::PushFont(body);

    if (m_FocusInput) { ImGui::SetKeyboardFocusHere(); m_FocusInput = false; }

    bool submitted = ImGui::InputTextWithHint(
        "##ChatPanelInput", "Message...",
        m_InputBuf, kInputBufSize, ImGuiInputTextFlags_EnterReturnsTrue);

    if (body) ImGui::PopFont();
    ImGui::PopItemWidth();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);

    ImGui::SameLine(0.0f, 6.0f);
    float btnY = ImGui::GetCursorPosY() + (inputH - btnW) * 0.5f;
    ImGui::SetCursorPosY(btnY);

    ImGui::PushStyleColor(ImGuiCol_Button,       U32ToVec4(Theme::Get().Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, U32ToVec4(Theme::Get().AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  U32ToVec4(Theme::Get().AccentHover));
    ImGui::PushStyleColor(ImGuiCol_Text,          U32ToVec4(Theme::Get().AccentText));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, btnW * 0.5f);

    bool clicked = ImGui::Button(">", { btnW, btnW });

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    if ((submitted || clicked) && m_InputBuf[0] != '\0') {
        std::string text(m_InputBuf);
        m_InputBuf[0] = '\0';
        m_FocusInput = true;
        m_ScrollToBottom = true;
        m_PendingOut = std::move(text);
    }
}

// ===========================================================================
// Public helpers
// ===========================================================================

std::optional<std::string> ChatPanel::ConsumePendingMessage() {
    if (!m_PendingOut) return std::nullopt;
    auto out = std::move(*m_PendingOut);
    m_PendingOut.reset();
    return out;
}

std::string ChatPanel::NowTimestamp() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&time, &local);
    return std::format("{:02d}:{:02d}", local.tm_hour, local.tm_min);
}

} // namespace Safira