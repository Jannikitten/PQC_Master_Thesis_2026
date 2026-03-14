#ifndef PQC_MASTER_THESIS_2026_SIDEBARHELPERS_H
#define PQC_MASTER_THESIS_2026_SIDEBARHELPERS_H

// =============================================================================
// SidebarHelpers.h -- Shared font + drawing helpers for sidebar views
//
// Header-only utilities used by UserListView, ConversationListView, and any
// other sidebar component that renders avatar circles or truncated text.
// =============================================================================

#include "GuiApp.h"
#include "Theme.h"

#include <imgui.h>

#include <cstring>
#include <string>

namespace Safira::Sidebar {

inline ImFont* BodyFont() {
    ImFont* f = ApplicationGUI::GetFont("Default");
    return f ? f : ImGui::GetFont();
}

inline ImFont* BoldFont() {
    ImFont* f = ApplicationGUI::GetFont("Bold");
    return f ? f : BodyFont();
}

inline ImVec2 MeasureText(ImFont* f, const char* text, float wrapWidth = 0.0f) {
    if (f) ImGui::PushFont(f);
    ImVec2 sz = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
    if (f) ImGui::PopFont();
    return sz;
}

inline void DrawTextTruncated(ImFont* f, ImVec2 pos, ImU32 col,
                               const char* text, float maxW) {
    if (f) ImGui::PushFont(f);

    ImVec2 fullSz = ImGui::CalcTextSize(text);
    if (fullSz.x <= maxW) {
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    } else {
        const char* ellipsis = "...";
        float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
        float targetW = maxW - ellipsisW;
        if (targetW < 0) targetW = 0;

        int lo = 0, hi = static_cast<int>(std::strlen(text));
        int best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            float w = ImGui::CalcTextSize(text, text + mid).x;
            if (w <= targetW) { best = mid; lo = mid + 1; }
            else              { hi = mid - 1; }
        }

        std::string trunc(text, best);
        trunc += ellipsis;
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, trunc.c_str());
    }

    if (f) ImGui::PopFont();
}

inline void DrawAvatarCircle(ImDrawList* dl, ImVec2 center, float radius,
                              uint32_t color, const std::string& username,
                              ImTextureID tex) {
    const auto& t = Theme::Get();
    if (tex) {
        dl->AddImageRounded(tex,
            { center.x - radius, center.y - radius },
            { center.x + radius, center.y + radius },
            { 0, 0 }, { 1, 1 },
            t.AvatarImageTint, radius);
    } else {
        dl->AddCircleFilled(center, radius, color, 24);
        dl->AddCircle(center, radius, t.IconOutline, 0, 1.5f);

        char letter = username.empty()
            ? '?'
            : static_cast<char>(toupper(username[0]));
        char buf[2] = { letter, '\0' };

        ImFont* bold = BoldFont();
        ImVec2 lsz = MeasureText(bold, buf);
        if (bold) ImGui::PushFont(bold);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { center.x - lsz.x * 0.5f, center.y - lsz.y * 0.5f },
                    t.AvatarLetterCol, buf);
        if (bold) ImGui::PopFont();
    }
}

} // namespace Safira::Sidebar

#endif // PQC_MASTER_THESIS_2026_SIDEBARHELPERS_H
