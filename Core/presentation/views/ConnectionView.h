#ifndef PQC_MASTER_THESIS_2026_CONNECTIONVIEW_H
#define PQC_MASTER_THESIS_2026_CONNECTIONVIEW_H

// ═══════════════════════════════════════════════════════════════════════════════
// Pure view component following the ChatPanel pattern:
//   - Receives data as parameters (no Store dependency)
//   - Communicates back to the layer via callbacks
// ═══════════════════════════════════════════════════════════════════════════════

#include "AvatarCircle.h" // CropRect, DrawCropWidget
#include "Theme.h"

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>

namespace Safira {

enum class ConnectionStatus : uint8_t; // forward from DtlsClient.h

class ConnectionView {
public:
    // -- Rendering ------------------------------------------------------------

    /// Call every frame. Pass infrastructure + store status separately.
    void Render(ConnectionStatus clientStatus,
                ConnectionStatus storeStatus,
                const std::string& debugMessage,
                ImTextureID avatarTexture);

    /// Load initial fields from persisted settings (call once in OnAttach).
    void SetInitialFields(const std::string& username,
                          const std::string& address);

    /// Open the crop modal with the given preview texture.
    void OpenCropModal(int srcW, int srcH, CropRect defaultCrop,
                       ImTextureID previewTex);

    // -- Callbacks (ChatPanel-style) ------------------------------------------

    using ConnectCallback       = std::function<void(const std::string& addr,
                                                     const std::string& user)>;
    using QuitCallback          = std::function<void()>;
    using BrowseCallback        = std::function<std::optional<std::string>()>;
    using AvatarClearedCallback = std::function<void()>;
    using CropAppliedCallback   = std::function<void(const std::string& path,
                                                     const CropRect&)>;

    void SetOnConnect(ConnectCallback fn)             { m_OnConnect = std::move(fn); }
    void SetOnQuit(QuitCallback fn)                   { m_OnQuit = std::move(fn); }
    void SetOnBrowse(BrowseCallback fn)               { m_OnBrowse = std::move(fn); }
    void SetOnAvatarCleared(AvatarClearedCallback fn) { m_OnAvatarCleared = std::move(fn); }
    void SetOnCropApplied(CropAppliedCallback fn)     { m_OnCropApplied = std::move(fn); }

    // -- Query ----------------------------------------------------------------

    [[nodiscard]] bool IsCropModalOpen() const { return m_ShowCropModal; }

private:
    void RenderConnectionModal(ConnectionStatus clientStatus,
                               ConnectionStatus storeStatus,
                               const std::string& debugMessage,
                               ImTextureID avatarTexture);
    void RenderCropModal();

    // Input state
    std::string m_Username;
    std::string m_ServerIP  = "127.0.0.1";
    bool        m_Initialized = false;

    // Connection modal state
    bool m_ConnectionModalOpen = false;

    // Crop modal state
    bool        m_ShowCropModal  = false;
    CropRect    m_CropRect;
    int         m_CropSrcWidth   = 0;
    int         m_CropSrcHeight  = 0;
    ImTextureID m_CropPreviewTex = {};
    std::string m_CropImagePath;

    // Callbacks
    ConnectCallback       m_OnConnect;
    QuitCallback          m_OnQuit;
    BrowseCallback        m_OnBrowse;
    AvatarClearedCallback m_OnAvatarCleared;
    CropAppliedCallback   m_OnCropApplied;
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_CONNECTIONVIEW_H
