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
// All sizing goes through ImGui::PushFont / GetFontSize / CalcTextSize.
// All positioned text uses SetCursorScreenPos + TextUnformatted instead of
// ImDrawList::AddText(ImFont*, float fontSize, ...).
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

// Draw text at an absolute screen position. Safe: never touches ImFont members.
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

static ImVec4 U32ToVec4(ImU32 col) {
    return ImGui::ColorConvertU32ToFloat4(col);
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

    ImGui::PushStyleColor(ImGuiCol_ChildBg, U32ToVec4(Colors.BgMain));
    ImGui::BeginChild("##ChatArea", { chatW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);

    if (activeIdx >= 0 && activeIdx < (int)conversations.size()
        && conversations[activeIdx].Messages)
    {
        auto& convo = conversations[activeIdx];
        RenderStatusIndicator(true, false, convo.Title);
        float msgH = ImGui::GetContentRegionAvail().y - kInputBarHeight - 8.0f;
        RenderMessages(*convo.Messages, chatW, msgH, ownUsername);
        RenderInputBar(chatW, ownUsername);
    } else {
        ImFont* bold = GetBoldFont();
        const char* title = "Safira";
        ImVec2 tSz = MeasureText(bold, title);
        ImGui::SetCursorPos({ (chatW - tSz.x) * 0.5f, avail.y * 0.38f });
        if (bold) ImGui::PushFont(bold);
        ImGui::TextColored(U32ToVec4(Colors.TextPrimary), "%s", title);
        if (bold) ImGui::PopFont();

        const char* sub = "Select a conversation or start a new one.";
        ImVec2 sSz = MeasureText(GetBodyFont(), sub);
        ImGui::SetCursorPosX((chatW - sSz.x) * 0.5f);
        ImGui::TextColored(U32ToVec4(Colors.TextMuted), "%s", sub);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (newIdx != activeIdx) return newIdx;
    return std::nullopt;
}

// ===========================================================================
// RenderChatArea -- standalone for PrivateChatSession windows
// ===========================================================================

void ChatPanel::RenderChatArea(
    std::vector<ChatEntry>& messages, const std::string& ownUsername,
    const std::string& peerUsername, bool connected, bool handshaking)
{
    RenderStatusIndicator(connected, handshaking, peerUsername);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, { p.x + w, p.y }, Colors.Divider, 1.0f);
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

    ImGui::PushStyleColor(ImGuiCol_ChildBg, U32ToVec4(Colors.BgSidebar));
    ImGui::BeginChild("##ChatSidebar", { width, height }, false,
                      ImGuiWindowFlags_NoScrollbar);

    const float pad = 14.0f;
    ImGui::SetCursorPos({ pad, pad });

    ImFont* bold = GetBoldFont();
    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored(U32ToVec4(Colors.TextPrimary), "Conversations");
    if (bold) ImGui::PopFont();

    ImGui::SameLine(width - pad - 26.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,       U32ToVec4(Colors.Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, U32ToVec4(Colors.AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  U32ToVec4(Colors.AccentHover));
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
                    Colors.Divider, 1.0f);
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
        if (selected)     bg = Colors.BgSidebarActive;
        else if (hovered) bg = Colors.BgSidebarHover;
        if (bg)
            ImGui::GetWindowDrawList()->AddRectFilled(
                cursor, { cursor.x + itemSz.x, cursor.y + itemSz.y }, bg, 6.0f);

        if (ImGui::InvisibleButton("##c", itemSz))
            result = i;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float tx = cursor.x + pad;

        // Avatar
        float ax = tx + kAvatarRadius;
        float ay = cursor.y + itemSz.y * 0.5f;
        char letter = c.Title.empty() ? '?' : (char)toupper(c.Title[0]);
        DrawAvatar(dl, ax, ay, kAvatarRadius, letter, Colors.Accent, Colors.AccentText);

        float textX = tx + kAvatarRadius * 2.0f + 10.0f;

        DrawTextAt(bold, { textX, cursor.y + 10.0f },
                   Colors.TextPrimary, c.Title.c_str());

        std::string preview = c.Preview.substr(0, 32);
        if (c.Preview.size() > 32) preview += "...";
        DrawTextAt(body, { textX, cursor.y + 30.0f },
                   Colors.TextSecondary, preview.c_str());

        if (!c.TimeLabel.empty()) {
            ImVec2 tSz = MeasureText(body, c.TimeLabel.c_str());
            DrawTextAt(body,
                { cursor.x + width - pad - tSz.x - 4.0f, cursor.y + 12.0f },
                Colors.TextMuted, c.TimeLabel.c_str());
        }

        if (c.HasUnread)
            dl->AddCircleFilled(
                { cursor.x + width - pad - 4.0f, cursor.y + itemSz.y * 0.5f },
                4.0f, Colors.UnreadDot, 12);

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
// ===========================================================================

void ChatPanel::RenderMessages(std::vector<ChatEntry>& messages,
                               float width, float height,
                               const std::string& ownUsername) {
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,         IM_COL32(255,255,255,30));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,  IM_COL32(255,255,255,55));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,   IM_COL32(255,255,255,80));

    ImGui::BeginChild("##MsgScroll", { width, height }, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (messages.empty()) {
        const char* hint = "No messages yet.";
        ImVec2 sz = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPos({ (width - sz.x) * 0.5f, (height - sz.y) * 0.5f });
        ImGui::TextColored(U32ToVec4(Colors.TextMuted), "%s", hint);
    } else {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        for (auto& msg : messages) {
            if (msg.Time.empty()) msg.Time = NowTimestamp();
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
// ===========================================================================

void ChatPanel::DrawBubble(ImDrawList* dl, const ChatEntry& msg,
                           float regionWidth, const std::string& ownUsername) {
    const bool isOwn    = (msg.Role == MessageRole::Own || msg.Who == ownUsername);
    const bool isSystem = (msg.Role == MessageRole::System || msg.Who == "System");

    if (isSystem) {
        ImVec2 sz = ImGui::CalcTextSize(msg.Text.c_str(), nullptr, false,
                                        regionWidth * 0.8f);
        ImGui::SetCursorPosX((regionWidth - sz.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, U32ToVec4(Colors.TextSystem));
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

    float boldH   = FontHeight(bold);
    float authorH = boldH + 2.0f;

    ImVec2 textSz   = MeasureText(body, msg.Text.c_str(), textWrapW);
    ImVec2 authorSz = MeasureText(bold, msg.Who.c_str());

    float bubbleW = std::max(textSz.x, authorSz.x) + kBubblePadX * 2.0f;
    float bubbleH = textSz.y + kBubblePadY * 2.0f + authorH;

    float marginX = 16.0f;
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    float bubbleX = isOwn
        ? cursor.x + regionWidth - bubbleW - marginX
        : cursor.x + marginX + avatarSpace;
    float bubbleY = cursor.y;

    // Avatar (peer only)
    if (!isOwn) {
        float ax = cursor.x + marginX + kAvatarRadius;
        float ay = bubbleY + kAvatarRadius + 2.0f;
        char letter = msg.Who.empty() ? '?' : (char)toupper(msg.Who[0]);
        DrawAvatar(dl, ax, ay, kAvatarRadius, letter,
                   Colors.Accent, Colors.AccentText);
    }

    // Bubble background
    ImU32 bubbleCol = isOwn ? Colors.BgOwnBubble : Colors.BgPeerBubble;
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
                    Colors.Divider, kBubbleRounding, 0, 1.0f);

    // Author name
    float textY = bubbleY + kBubblePadY;
    ImU32 nameCol = isOwn ? Colors.TextPrimary : Colors.Accent;
    DrawTextAt(bold, { bubbleX + kBubblePadX, textY }, nameCol, msg.Who.c_str());
    textY += authorH;

    // Body text
    DrawTextAt(body, { bubbleX + kBubblePadX, textY },
               Colors.TextPrimary, msg.Text.c_str(), textWrapW);

    // Timestamp
    if (!msg.Time.empty()) {
        ImVec2 tsSz = MeasureText(nullptr, msg.Time.c_str());
        DrawTextAt(nullptr,
            { bubbleX + bubbleW - tsSz.x - kBubblePadX,
              bubbleY + bubbleH + 2.0f },
            Colors.TextMuted, msg.Time.c_str());
    }

    // Advance cursor
    ImGui::SetCursorScreenPos({ cursor.x, bubbleY + bubbleH + 14.0f });
    ImGui::Dummy({ 0, 0 });
}

// ===========================================================================
// Avatar -- only dl->AddText we keep, wrapped in PushFont so ImGui resolves
// the size internally without us touching any ImFont fields.
// ===========================================================================

void ChatPanel::DrawAvatar(ImDrawList* dl, float cx, float cy, float radius,
                           char letter, ImU32 bgCol, ImU32 textCol) {
    dl->AddCircleFilled({ cx, cy }, radius, bgCol, 24);

    char buf[2] = { letter, '\0' };
    ImFont* bold = GetBoldFont();
    ImVec2 sz = MeasureText(bold, buf);

    // Use explicit font pointer + size — passing nullptr/0.0f can trigger
    // assertion in ImFontAtlasPackAddRect when _Data->FontSize is uninit.
    if (bold) ImGui::PushFont(bold);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                { cx - sz.x * 0.5f, cy - sz.y * 0.5f }, textCol, buf);
    if (bold) ImGui::PopFont();
}

// ===========================================================================
// Status indicator
// ===========================================================================

void ChatPanel::RenderStatusIndicator(bool connected, bool handshaking,
                                      const std::string& peer) {
    ImU32 dotCol;
    std::string label;

    if (connected) {
        dotCol = Colors.StatusOnline;
        label  = "Connected  (" + StatusProtocol + ")";
    } else if (handshaking) {
        dotCol = Colors.StatusPending;
        label  = "Handshaking...";
    } else {
        dotCol = Colors.StatusOffline;
        label  = "Disconnected";
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImFont* bold = GetBoldFont();
    if (bold) ImGui::PushFont(bold);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    ImGui::TextColored(U32ToVec4(Colors.TextPrimary), "%s", peer.c_str());
    if (bold) ImGui::PopFont();

    ImGui::SameLine(0.0f, 12.0f);

    ImVec2 dotPos = ImGui::GetCursorScreenPos();
    float textH = ImGui::GetFontSize();
    dl->AddCircleFilled({ dotPos.x + 4.0f, dotPos.y + textH * 0.5f },
                        4.0f, dotCol, 12);

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
    ImGui::TextColored(U32ToVec4(Colors.TextSecondary), "%s", label.c_str());
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
        Colors.InputShadow, kInputRounding + 1);
    dl->AddRectFilled(inputPos,
        { inputPos.x + inputW, inputPos.y + inputH },
        Colors.InputBg, kInputRounding);
    dl->AddRect(inputPos,
        { inputPos.x + inputW, inputPos.y + inputH },
        Colors.InputBorder, kInputRounding, 0, 1.2f);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        { 14.0f, (inputH - ImGui::GetFontSize()) * 0.5f });
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kInputRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,       IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Text, U32ToVec4(Colors.TextPrimary));

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

    ImGui::PushStyleColor(ImGuiCol_Button,       U32ToVec4(Colors.Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, U32ToVec4(Colors.AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  U32ToVec4(Colors.AccentHover));
    ImGui::PushStyleColor(ImGuiCol_Text,          U32ToVec4(Colors.AccentText));
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