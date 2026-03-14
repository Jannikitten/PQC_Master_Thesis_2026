#ifndef PQC_MASTER_THESIS_2026_CONVERSATIONLISTVIEW_H
#define PQC_MASTER_THESIS_2026_CONVERSATIONLISTVIEW_H

// =============================================================================
// ConversationListView.h -- Sidebar conversation item rendering
//
// Pure view component. Renders a scrollable list of conversations (Lobby,
// private chats) with avatar circles, truncated titles, previews, and
// hover/selection highlighting. Returns the new active index when clicked.
// =============================================================================

#include "ChatTypes.h"

#include <imgui.h>

#include <optional>
#include <vector>

namespace Safira {

class ConversationListView {
public:
    /// Render conversation items. Returns new active index if user clicked one.
    std::optional<int> Render(const std::vector<ConversationInfo>& conversations,
                              int activeIdx, float sidebarWidth);
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_CONVERSATIONLISTVIEW_H
