#include "ChatView.h"
#include "GuiApp.h"
#include "SidebarHelpers.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // Font helpers -- ZERO direct ImFont member access.
    // ─────────────────────────────────────────────────────────────────────────────

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

    ChatPanel::ChatPanel() = default;

    // ─────────────────────────────────────────────────────────────────────────────
    // RenderFullLayout — complete chat window: sidebar (users + conversations)
    //                    + chat area + separator overlay
    // ─────────────────────────────────────────────────────────────────────────────

    FullLayoutOutput ChatPanel::RenderFullLayout(const FullLayoutInput& input) {
        FullLayoutOutput out;

        const ImVec2 outerAvail = ImGui::GetContentRegionAvail();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Get().PanelBgVec4());
        ImGui::BeginChild("##ChatPanel", outerAvail, false,
                           ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        int activeIdx = input.ActiveConvoIdx;
        if (input.Conversations &&
            activeIdx >= static_cast<int>(input.Conversations->size()))
            activeIdx = std::max(0,
                static_cast<int>(input.Conversations->size()) - 1);

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        constexpr float sideW = 260.0f;
        const float chatW = avail.x - sideW;

        // -- Left sidebar --------------------------------------------------------
        RenderSidebar(input, sideW, avail.y, out);

        // Sidebar vertical separator
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 sidebarEnd = ImGui::GetCursorScreenPos();
            float panelTop = sidebarEnd.y - avail.y;
            dl->AddLine({ sidebarEnd.x, panelTop },
                        { sidebarEnd.x, panelTop + avail.y },
                        Theme::Get().Separator, 1.0f);
        }
        ImGui::PopStyleColor(); // sidebar ChildBg

        // -- Right chat area -----------------------------------------------------
        ImGui::SameLine(0.0f, 0.0f);
        RenderChatSection(input, chatW, avail.y, out);

        // -- Separator overlay ---------------------------------------------------
        RenderSeparatorOverlay(ImGui::GetWindowPos(), avail.x, avail.y,
                               sideW, !input.SuppressOverlay);

        ImGui::EndChild(); // ##ChatPanel

        // Resolve new active index
        if (out.NewActiveIdx && *out.NewActiveIdx != input.ActiveConvoIdx) {
            // keep it
        } else {
            out.NewActiveIdx = std::nullopt;
        }

        return out;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // RenderChatArea -- standalone chat area (used inside UI_UnifiedChatWindow)
    // ─────────────────────────────────────────────────────────────────────────────

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

    // ─────────────────────────────────────────────────────────────────────────────
    // RenderSidebar — user list + divider + conversation list
    // ─────────────────────────────────────────────────────────────────────────────

    void ChatPanel::RenderSidebar(const FullLayoutInput& input, float sideW,
                                   float height, FullLayoutOutput& out) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Get().PanelBgVec4());
        ImGui::BeginChild("##Sidebar", { sideW, height }, false,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();

        constexpr float pad = 14.0f;

        // -- User list section ---------------------------------------------------
        const float userListH = std::min(
            static_cast<float>(input.Users.size()) * 26.0f + 36.0f,
            height * 0.35f);

        ImGui::SetCursorPos({ 0, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::BeginChild("##UserSection", { sideW, userListH }, false);
        ImGui::PopStyleVar();
        ImGui::SetCursorPos({ pad, 8.0f });

        m_UserListView.Render(input.Users, sideW);
        ImGui::EndChild();

        // -- Divider -------------------------------------------------------------
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine({ p.x + pad, p.y },
                        { p.x + sideW - pad, p.y },
                        Theme::Get().Divider, 1.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        }

        // -- Conversation list ---------------------------------------------------
        if (input.Conversations) {
            ImGui::BeginChild("##ConvoList", { 0, 0 }, false);
            if (auto newIdx = m_ConversationListView.Render(
                    *input.Conversations, input.ActiveConvoIdx, sideW))
                out.NewActiveIdx = *newIdx;
            ImGui::EndChild();
        }

        ImGui::EndChild(); // ##Sidebar
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // RenderChatSection — right-side chat area
    // ─────────────────────────────────────────────────────────────────────────────

    void ChatPanel::RenderChatSection(const FullLayoutInput& input, float chatW,
                                       float height, FullLayoutOutput& out) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Get().PanelBgVec4());
        ImGui::BeginChild("##ChatArea", { chatW, height }, false,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();

        if (input.Messages) {
            // Configure private/lobby mode
            m_PrivateMode   = input.IsPrivateChat;
            m_PeerAvatarTex = input.PeerAvatar;
            m_OwnAvatarTex  = input.OwnAvatar;
            StatusProtocol  = input.StatusProtocol;

            // Wire leave callback to flag
            m_LeaveRequested = false;
            if (input.IsPrivateChat) {
                m_OnLeave = [this]() { m_LeaveRequested = true; };
            } else {
                m_OnLeave = nullptr;
            }

            ImGui::SetCursorPos({ 14.0f, 8.0f });

            RenderChatArea(*input.Messages, input.Username, input.ConvoTitle,
                           input.IsConnected, input.IsHandshaking);

            if (auto msg = ConsumePendingMessage())
                out.PendingMessage = std::move(msg);

            if (m_LeaveRequested)
                out.LeaveRequested = true;
        } else {
            const char* sub = "Select a conversation or start a new one.";
            ImVec2 sSz = ImGui::CalcTextSize(sub);
            ImGui::SetCursorPos({ (chatW - sSz.x) * 0.5f, height * 0.45f });
            ImGui::TextColored({ 0.45f, 0.45f, 0.45f, 1.0f }, "%s", sub);
        }

        ImGui::EndChild(); // ##ChatArea
        ImGui::PopStyleColor();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // RenderSeparatorOverlay — top + vertical separator lines
    // ─────────────────────────────────────────────────────────────────────────────

    void ChatPanel::RenderSeparatorOverlay(ImVec2 origin, float width,
                                            float height, float sideW,
                                            bool visible) {
        if (!visible) return;
        if (ImGui::IsPopupOpen("Report User##ReportModal")) return;

        const ImU32 lineCol = Theme::Get().Separator;

        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize({ width, height });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

        ImGuiWindowFlags lineFlags =
              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoDocking    | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("##SeparatorOverlay", nullptr, lineFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(origin, { origin.x + width, origin.y },
                        lineCol, 1.0f);
            dl->AddLine({ origin.x + sideW, origin.y },
                        { origin.x + sideW, origin.y + height },
                        lineCol, 1.0f);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Messages
    //
    // BUG FIX: Removed lazy timestamp fill (msg.Time = NowTimestamp()) that was
    // called during rendering. Timestamps must be set at message creation time
    // in ClientLayer::AddLobbyMessage() and packet handlers.
    // ─────────────────────────────────────────────────────────────────────────────

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

    // ─────────────────────────────────────────────────────────────────────────────
    // Bubble
    // ─────────────────────────────────────────────────────────────────────────────

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

        // Requested UX: own messages on the left, peers on the right.
        const bool peerOnRight = !isOwn;

        // In private mode, no avatar shown next to bubbles.
        float avatarOffset = (m_PrivateMode || !peerOnRight) ? 0.0f : avatarSpace;

        float bubbleX = peerOnRight
            ? cursor.x + regionWidth - bubbleW - marginX - avatarOffset
            : cursor.x + marginX;
        float bubbleY = cursor.y;

        // Avatar (peer only, NOT in private mode) — anchored on the right.
        if (peerOnRight && !m_PrivateMode) {
            float ax = cursor.x + regionWidth - marginX - kAvatarRadius;
            float ay = bubbleY + kAvatarRadius + 2.0f;
            char letter = msg.Who.empty() ? '?' : (char)toupper(msg.Who[0]);
            DrawAvatar(dl, ax, ay, kAvatarRadius, letter,
                       Theme::Get().SendBtn, Theme::Get().SendBtnText, msg.AvatarTex);
        }

        // Bubble background — selective corner rounding for tail effect
        // Own messages: sharp top-right (tail).  Peer messages: sharp top-left (tail).
        ImU32 bubbleCol = isOwn ? Theme::Get().BgOwnBubble : Theme::Get().BgPeerBubble;

        ImDrawFlags cornerFlags = peerOnRight
            ? (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft
               | ImDrawFlags_RoundCornersBottomRight)
            : (ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomLeft
               | ImDrawFlags_RoundCornersBottomRight);

        dl->AddRectFilled({ bubbleX, bubbleY },
                          { bubbleX + bubbleW, bubbleY + bubbleH },
                          bubbleCol, kBubbleRounding, cornerFlags);

        // Border on peer bubbles — same corner flags so border matches fill exactly
        if (!isOwn)
            dl->AddRect({ bubbleX, bubbleY },
                        { bubbleX + bubbleW, bubbleY + bubbleH },
                        Theme::Get().Divider, kBubbleRounding, cornerFlags, 1.0f);

        // Author name (skipped in private mode) — uses purple for peer
        float textY = bubbleY + kBubblePadY;
        if (showAuthor) {
            ImU32 nameCol = isOwn ? Theme::Get().TextPrimary : Theme::Get().SendBtn;
            DrawTextAt(bold, { bubbleX + kBubblePadX, textY }, nameCol, msg.Who.c_str());
            textY += authorH;
        }

        // Body text
        DrawTextAt(body, { bubbleX + kBubblePadX, textY },
                   Theme::Get().TextPrimary, msg.Text.c_str(), textWrapW);

        // Timestamp
        float tsAdvance = 0.0f;
        if (!msg.Time.empty()) {
            ImVec2 tsSz = MeasureText(nullptr, msg.Time.c_str());
            DrawTextAt(nullptr,
                { bubbleX + bubbleW - tsSz.x - kBubblePadX,
                  bubbleY + bubbleH + 2.0f },
                Theme::Get().TextMuted, msg.Time.c_str());
            tsAdvance = tsSz.y + 2.0f;
        }

        // Advance cursor with a small visual gap between bubbles.
        constexpr float kBubbleGap = 4.0f;
        ImGui::SetCursorScreenPos({ cursor.x, bubbleY + bubbleH + tsAdvance + kBubbleGap });
        ImGui::Dummy({ 0, 0 });
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Avatar -- supports optional ImTextureID for image avatars
    //
    // When tex != 0: renders a circular image using AddImageRounded.
    // When tex == 0: falls back to colored circle + centered letter.
    // ─────────────────────────────────────────────────────────────────────────────

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

    // ─────────────────────────────────────────────────────────────────────────────
    // Status indicator
    //
    // In private mode: shows peer avatar (if set) + name + leave button
    // ─────────────────────────────────────────────────────────────────────────────

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

                ImGui::PushStyleColor(ImGuiCol_Button,         IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  Theme::Get().LogoutBtnHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   Theme::Get().LogoutBtnActive);
                ImGui::PushStyleColor(ImGuiCol_Text,           Theme::Get().LogoutIcon);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { btnPadX + 2.0f, btnPadY + 3.0f });

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

    // ─────────────────────────────────────────────────────────────────────────────
    // Input bar
    // ─────────────────────────────────────────────────────────────────────────────

    void ChatPanel::RenderInputBar(float areaWidth, const std::string&) {
        float pad = 16.0f;
        float inputH = kInputBarHeight - 12.0f;
        float btnW = 88.0f;
        float inputW = areaWidth - pad * 2.0f - btnW - 10.0f;

        ImGui::SetCursorPos({ pad, ImGui::GetCursorPosY() + 4.0f });
        const float inputCursorY = ImGui::GetCursorPosY();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 inputPos = ImGui::GetCursorScreenPos();

        // ── Input field background ──────────────────────────────────────────
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

        // ── Input text field ────────────────────────────────────────────────
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

        // ── Send button — compact professional pill style ───────────────────
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetCursorPosY(inputCursorY);

        const bool hasText = m_InputBuf[0] != '\0';

        ImU32 btnBg      = hasText ? Theme::Get().SendBtn          : Theme::Get().SendBtnMuted;
        ImU32 btnHover   = hasText ? Theme::Get().SendBtnHover     : Theme::Get().SendBtnMutedHover;
        ImU32 btnActive  = hasText ? Theme::Get().SendBtnActive    : Theme::Get().SendBtnMuted;
        ImU32 btnTextCol = hasText ? Theme::Get().SendBtnText      : Theme::Get().TextMuted;

        dl->AddRectFilled(
            { ImGui::GetCursorScreenPos().x - 1.0f, inputPos.y - 1.0f },
            { ImGui::GetCursorScreenPos().x + btnW + 1.0f, inputPos.y + inputH + 2.0f },
            Theme::Get().InputShadow, 12.0f);

        ImGui::PushStyleColor(ImGuiCol_Button,         btnBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  btnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnActive);
        ImGui::PushStyleColor(ImGuiCol_Text,           btnTextCol);
        ImGui::PushStyleColor(ImGuiCol_Border,         Theme::Get().InputBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 10.0f, 6.0f });

        bool clicked = ImGui::Button("Send##SendBtn", { btnW, inputH });

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);

        // ── Submit handling ─────────────────────────────────────────────────
        if ((submitted || clicked) && hasText) {
            std::string text(m_InputBuf);
            m_InputBuf[0] = '\0';
            m_FocusInput = true;
            m_ScrollToBottom = true;
            m_PendingOut = std::move(text);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Public helpers
    // ─────────────────────────────────────────────────────────────────────────────

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
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        return std::format("{:02d}:{:02d}", local.tm_hour, local.tm_min);
    }

} // namespace Safira
