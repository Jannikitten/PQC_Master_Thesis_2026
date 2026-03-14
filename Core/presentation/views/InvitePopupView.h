#ifndef PQC_MASTER_THESIS_2026_INVITEPOPUPVIEW_H
#define PQC_MASTER_THESIS_2026_INVITEPOPUPVIEW_H

// =============================================================================
// InvitePopupView.h -- Incoming P2P private chat invite notification
//
// Header-only. Pure view component (no Store dependency).
// =============================================================================

#include "Theme.h"

#include <imgui.h>

#include <functional>
#include <string>

namespace Safira {

class InvitePopupView {
public:
    using ResponseCallback = std::function<void(const std::string& fromUser,
                                                bool accepted)>;

    void SetOnResponse(ResponseCallback fn) { m_OnResponse = std::move(fn); }

    /// Call every frame. Pass the inviting username (empty = no invite).
    void Render(const std::string& fromUsername) {
        if (fromUsername.empty()) return;

        const std::string popupId = "Private Chat Request##" + fromUsername;

        ImGui::OpenPopup(popupId.c_str());

        const auto& t = Theme::Get();
        ImGui::PushStyleColor(ImGuiCol_PopupBg, t.BgPopupAlt);
        ImGui::PushStyleColor(ImGuiCol_Text,    t.TextPrimary);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

        bool open = true;
        if (ImGui::BeginPopupModal(popupId.c_str(), &open,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s wants to chat with you privately.",
                        fromUsername.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            constexpr float btnW   = 120.0f;
            constexpr float btnGap = 8.0f;
            const float totalW = btnW * 2.0f + btnGap;
            const float availW = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() + (availW - totalW) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button,        t.SendBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.SendBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   t.SendBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,           t.SendBtnText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

            if (ImGui::Button("Accept", { btnW, 0 })) {
                if (m_OnResponse) m_OnResponse(fromUsername, true);
                ImGui::CloseCurrentPopup();
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);

            ImGui::SameLine(0, btnGap);

            ImGui::PushStyleColor(ImGuiCol_Button,        t.DeclineBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.DeclineBtnHover);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

            if (ImGui::Button("Decline", { btnW, 0 })) {
                if (m_OnResponse) m_OnResponse(fromUsername, false);
                ImGui::CloseCurrentPopup();
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            ImGui::EndPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (!open) {
            if (m_OnResponse) m_OnResponse(fromUsername, false);
        }
    }

private:
    ResponseCallback m_OnResponse;
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_INVITEPOPUPVIEW_H
