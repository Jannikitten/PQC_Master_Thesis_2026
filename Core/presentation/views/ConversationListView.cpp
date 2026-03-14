#include "ConversationListView.h"
#include "SidebarHelpers.h"
#include "Theme.h"

#include <cstdint>
#include <map>
#include <string>

namespace Safira {

std::optional<int> ConversationListView::Render(
    const std::vector<ConversationInfo>& conversations,
    int activeIdx, float sidebarWidth)
{
    std::optional<int> newIdx;

    ImFont* bold = Sidebar::BoldFont();
    ImFont* body = Sidebar::BodyFont();
    if (body) ImGui::PushFont(body);

    for (int i = 0; i < static_cast<int>(conversations.size()); ++i) {
        const auto& c = conversations[i];
        ImGui::PushID(i);

        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 itemSz = { sidebarWidth - 4.0f, 54.0f };
        const bool hovered  = ImGui::IsMouseHoveringRect(
            cursor, { cursor.x + itemSz.x, cursor.y + itemSz.y });

        ImU32 bg = 0;
        if (i == activeIdx) bg = Theme::Get().BgItemSelected;
        else if (hovered)   bg = Theme::Get().BgItemHovered;

        if (bg)
            ImGui::GetWindowDrawList()->AddRectFilled(
                cursor,
                { cursor.x + itemSz.x, cursor.y + itemSz.y },
                bg, 6.0f);

        if (ImGui::InvisibleButton("##c", itemSz))
            newIdx = i;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        constexpr float kR   = 14.0f;
        constexpr float kPad = 14.0f;
        const float tx = cursor.x + kPad;
        const float ax = tx + kR;
        const float ay = cursor.y + itemSz.y * 0.5f;

        uint32_t avatarCol = Theme::Get().Accent;
        if (i == 0)
            avatarCol = Theme::Get().LobbyAvatar;

        Sidebar::DrawAvatarCircle(dl, { ax, ay }, kR, avatarCol,
                                   c.Title, c.AvatarTex);

        const float textX     = tx + kR * 2.0f + 10.0f;
        const float rightEdge = cursor.x + sidebarWidth - kPad - 4.0f;

        float titleMaxW = rightEdge - textX;
        if (!c.TimeLabel.empty()) {
            ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
            titleMaxW -= (tSz.x + 8.0f);
        }
        Sidebar::DrawTextTruncated(bold, { textX, cursor.y + 8.0f },
                                    Theme::Get().ConvoTitleCol,
                                    c.Title.c_str(), titleMaxW);

        const float previewMaxW = rightEdge - textX;
        Sidebar::DrawTextTruncated(body, { textX, cursor.y + 28.0f },
                                    Theme::Get().ConvoPreviewCol,
                                    c.Preview.c_str(), previewMaxW);

        if (!c.TimeLabel.empty()) {
            ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                { cursor.x + sidebarWidth - kPad - tSz.x - 4.0f,
                  cursor.y + 10.0f },
                Theme::Get().ConvoTimeCol,
                c.TimeLabel.c_str());
        }

        ImGui::PopID();
    }

    if (body) ImGui::PopFont();

    return newIdx;
}

} // namespace Safira
