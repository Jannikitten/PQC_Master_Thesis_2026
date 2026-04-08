#ifndef PQC_MASTER_THESIS_2026_AVATARMANAGER_H
#define PQC_MASTER_THESIS_2026_AVATARMANAGER_H

// ═════════════════════════════════════════════════════════════════════════════
// Presentation-layer service (NOT a view). Manages the pipeline from raw
// image files / wire bytes to ImTextureID handles. No Store dependency.
// ═════════════════════════════════════════════════════════════════════════════

#include "AvatarCircle.h"
#include "Image.h"

#include <imgui.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Safira {

    struct ChatEntry;   // forward (ChatView.h)

    class AvatarManager {
    public:
        // ── Own avatar pipeline ──

        struct LoadResult {
            bool NeedsCrop = false;
            int  SrcW      = 0;
            int  SrcH      = 0;
        };

        LoadResult LoadFromFile(const std::string& filepath);

        /// Process after crop: reload, crop, resize, upload to GPU.
        void ApplyCrop(const std::string& filepath, const CropRect& crop);

        void ClearOwn();

        [[nodiscard]] ImTextureID                OwnTexture()      const { return m_OwnTex; }
        [[nodiscard]] const std::vector<uint8_t>& OwnBytes()       const { return m_OwnBytes; }
        [[nodiscard]] const std::string&          OwnImagePath()   const { return m_ImagePath; }

        /// Crop preview texture (valid between LoadFromFile→ApplyCrop/ClearCrop).
        [[nodiscard]] ImTextureID CropPreviewTex()  const { return m_CropPreviewTex; }
        [[nodiscard]] int         CropSrcWidth()    const { return m_CropSrcW; }
        [[nodiscard]] int         CropSrcHeight()   const { return m_CropSrcH; }
        [[nodiscard]] CropRect    DefaultCrop()     const { return m_DefaultCrop; }

        void ClearCropPreview();

        // ── Peer avatar cache ──

        void CachePeer(const std::string& username,
                       const std::vector<uint8_t>& avatarData);
        void RemovePeer(const std::string& username);
        void ClearAllPeers();

        [[nodiscard]] ImTextureID PeerTexture(const std::string& username) const;

        /// Patch AvatarTex field of messages whose Who matches a cached peer.
        void PatchAvatarsInMessages(std::vector<ChatEntry>& msgs,
                                    const std::string& ownUsername) const;

    private:
        // Own avatar
        std::string                    m_ImagePath;
        std::shared_ptr<Image>         m_OwnImage;
        ImTextureID                    m_OwnTex = {};
        std::vector<uint8_t>           m_OwnBytes;

        // Crop preview (transient)
        int                            m_CropSrcW = 0;
        int                            m_CropSrcH = 0;
        CropRect                       m_DefaultCrop;
        std::shared_ptr<Image>         m_CropPreviewImage;
        ImTextureID                    m_CropPreviewTex = {};

        // Peer cache
        struct PeerEntry {
            std::shared_ptr<Image>      Img;
            ImTextureID                 Tex      = {};
            std::size_t                 DataHash = 0;
        };
        std::map<std::string, PeerEntry> m_Peers;

        static std::size_t HashBytes(const std::vector<uint8_t>& data);
    };

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_AVATARMANAGER_H
