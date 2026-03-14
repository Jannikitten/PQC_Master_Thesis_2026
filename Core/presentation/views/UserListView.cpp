#include "UserListView.h"
#include "SidebarHelpers.h"

using Safira::Theme;
using Safira::U32ToVec4;

namespace Safira {

// =============================================================================
// Render — user list + context menu
// =============================================================================

void UserListView::Render(const std::vector<UserListEntry>& users,
                           float /*width*/) {
    ImFont* bold = Sidebar::BoldFont();
    const auto& t = Theme::Get();

    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored(U32ToVec4(t.TextPrimary), "Online (%d)",
                       static_cast<int>(users.size()));
    if (bold) ImGui::PopFont();

    ImGui::Spacing();

    constexpr float kIconSize = 20.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    std::string deferredInviteTarget;

    for (const auto& user : users) {
        if (user.Username.empty()) continue;

        constexpr float itemPad = 14.0f;
        ImGui::SetCursorPosX(itemPad);
        const ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float  radius = kIconSize * 0.45f;
        const ImVec2 center = { pos.x + kIconSize * 0.5f,
                                pos.y + kIconSize * 0.5f };

        Sidebar::DrawAvatarCircle(dl, center, radius, user.Color,
                                   user.Username, user.AvatarTex);
        ImGui::Dummy({ kIconSize, kIconSize });

        if (!user.IsOwnUser) {
            const std::string popupId = "##UserCtx_" + user.Username;
            if (ImGui::IsItemHovered()
                && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                m_ContextMenuTarget = user.Username;
                ImGui::OpenPopup(popupId.c_str());
            }

            ImGui::PushStyleColor(ImGuiCol_PopupBg,       t.BgPopupAlt);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  t.BgFrameHovered);
            ImGui::PushStyleColor(ImGuiCol_Text,           t.TextPrimary);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);

            if (ImGui::BeginPopup(popupId.c_str())) {
                if (user.InPrivateChat) {
                    ImGui::TextDisabled("Already in private chat");
                } else if (user.InvitePending) {
                    ImGui::TextDisabled("Invite pending...");
                } else {
                    if (ImGui::Selectable("Invite to private chat")) {
                        deferredInviteTarget = user.Username;
                    }
                }

                ImGui::Separator();

                if (ImGui::Selectable("Report user to server")) {
                    m_ReportTarget = user.Username;
                    m_ReportReasonBuf[0] = '\0';
                    m_ReportModalOpen = true;
                }

                ImGui::EndPopup();
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImColor(t.TextPrimary).Value);
        ImGui::TextUnformatted(user.Username.c_str());
        ImGui::PopStyleColor();

        if (user.InPrivateChat) {
            ImGui::SameLine();
            ImGui::TextDisabled("(private)");
        } else if (user.InvitePending) {
            ImGui::SameLine();
            ImGui::TextDisabled("(invited)");
        }
    }

    // Dispatch deferred invite
    if (!deferredInviteTarget.empty() && m_OnInvite) {
        m_OnInvite(deferredInviteTarget);
    }

    // Report modal (rendered here since it's triggered by the context menu)
    RenderReportModal();
}

// =============================================================================
// Report Modal
// =============================================================================

void UserListView::RenderReportModal() {
    if (m_ReportModalOpen) {
        ImGui::OpenPopup("Report User##ReportModal");
        m_ReportModalOpen = false;
    }

    const auto& t = Theme::Get();
    ImGui::PushStyleColor(ImGuiCol_PopupBg, t.BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Border,   t.ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,     t.TextPrimary);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,  t.BgFrame);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal("Report User##ReportModal", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Report %s", m_ReportTarget.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(U32ToVec4(t.TextSecondary), "Reason:");
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextMultiline("##ReportReason", m_ReportReasonBuf,
                                   sizeof(m_ReportReasonBuf),
                                   { 300, 80 });

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        t.Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   t.AccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text,           t.AccentText);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Submit Report", { 130, 0 })) {
            std::string reason(m_ReportReasonBuf);
            if (!reason.empty() && m_OnReport)
                m_OnReport(m_ReportTarget, reason);
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,        t.DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Cancel", { 80, 0 })) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

} // namespace Safira
