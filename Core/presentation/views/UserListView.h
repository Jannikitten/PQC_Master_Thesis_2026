#ifndef PQC_MASTER_THESIS_2026_USERLISTVIEW_H
#define PQC_MASTER_THESIS_2026_USERLISTVIEW_H

// =============================================================================
// UserListView.h -- Sidebar user list with context menu + report modal
//
// Pure view component (no Store dependency). Renders the list of online users,
// handles right-click context menus, and owns the report-user modal.
// =============================================================================

#include "Theme.h"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Safira {

// Data passed in by the orchestrator (ClientLayer).
struct UserListEntry {
    std::string Username;
    uint32_t    Color       = 0xFFFFFFFF;
    ImTextureID AvatarTex   = {};
    bool        IsOwnUser   = false;
    bool        InPrivateChat = false;
    bool        InvitePending = false;
};

class UserListView {
public:
    void Render(const std::vector<UserListEntry>& users, float width);

    // Callbacks
    using InviteCallback = std::function<void(const std::string& username)>;
    using ReportCallback = std::function<void(const std::string& username,
                                              const std::string& reason)>;

    void SetOnInvite(InviteCallback fn) { m_OnInvite = std::move(fn); }
    void SetOnReport(ReportCallback fn) { m_OnReport = std::move(fn); }

private:
    void RenderReportModal();

    // Report modal state
    std::string m_ContextMenuTarget;
    bool        m_ReportModalOpen = false;
    std::string m_ReportTarget;
    char        m_ReportReasonBuf[512] = {};

    // Callbacks
    InviteCallback m_OnInvite;
    ReportCallback m_OnReport;
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_USERLISTVIEW_H
