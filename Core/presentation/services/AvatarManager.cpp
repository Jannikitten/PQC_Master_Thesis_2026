#include "AvatarManager.h"
#include "ChatTypes.h"         // ChatEntry
#include "UserInfoSerialize.h" // kAvatarPixelSize, kMaxAvatarBytes

#include <spdlog/spdlog.h>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // Hash helper (FNV-1a 64-bit)
    // ─────────────────────────────────────────────────────────────────────────────

    std::size_t AvatarManager::HashBytes(const std::vector<uint8_t>& data) {
        std::size_t h = 14695981039346656037ULL;
        for (auto b : data) {
            h ^= static_cast<std::size_t>(b);
            h *= 1099511628211ULL;
        }
        return h;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Own avatar
    // ─────────────────────────────────────────────────────────────────────────────

    AvatarManager::LoadResult AvatarManager::LoadFromFile(const std::string& filepath) {
        if (filepath.empty()) return {};

        auto raw = LoadImageFromFile(filepath);
        if (!raw.Valid()) {
            spdlog::warn("Failed to load avatar image: {}", filepath);
            return {};
        }

        m_ImagePath = filepath;

        if (raw.NeedsCrop()) {
            m_CropSrcW     = raw.Width;
            m_CropSrcH     = raw.Height;
            m_DefaultCrop   = DefaultCenterCrop(raw);

            m_CropPreviewImage = std::make_shared<Image>(
                static_cast<uint32_t>(raw.Width),
                static_cast<uint32_t>(raw.Height),
                ImageFormat::RGBA, raw.Pixels.get());
            m_CropPreviewTex = (ImTextureID)m_CropPreviewImage->GetDescriptorSet();

            spdlog::info("Avatar {}x{} is not square -- opening crop UI",
                         raw.Width, raw.Height);
            return { .NeedsCrop = true, .SrcW = raw.Width, .SrcH = raw.Height };
        }

        // Already square — process directly.
        auto crop = DefaultCenterCrop(raw);
        auto rgba = ProcessAvatarImage(filepath, crop);
        if (!rgba) {
            spdlog::warn("Avatar processing failed for {}", filepath);
            return {};
        }
        m_OwnBytes = std::move(*rgba);

        m_OwnImage = std::make_shared<Image>(
            static_cast<uint32_t>(raw.Width),
            static_cast<uint32_t>(raw.Height),
            ImageFormat::RGBA, raw.Pixels.get());
        m_OwnTex = (ImTextureID)m_OwnImage->GetDescriptorSet();

        spdlog::info("Avatar loaded: {}x{} from {} ({} bytes raw RGBA)",
                     raw.Width, raw.Height, filepath, m_OwnBytes.size());
        return {};
    }

    void AvatarManager::ApplyCrop(const std::string& filepath,
                                   const CropRect& crop) {
        auto rgba = ProcessAvatarImage(filepath, crop);
        if (rgba) {
            m_OwnBytes = std::move(*rgba);

            auto raw = LoadImageFromFile(filepath);
            if (raw.Valid()) {
                auto cropped = CropSquare(raw, crop);
                auto resized = ResizeSquare(cropped.data(), crop.Size,
                                             kAvatarPixelSize);
                m_OwnImage = std::make_shared<Image>(
                    kAvatarPixelSize, kAvatarPixelSize,
                    ImageFormat::RGBA, resized.data());
                m_OwnTex = (ImTextureID)m_OwnImage->GetDescriptorSet();
            }
            spdlog::info("Avatar cropped and processed ({} bytes raw RGBA)",
                         m_OwnBytes.size());
        } else {
            spdlog::warn("Avatar crop/compress failed");
        }

        ClearCropPreview();
    }

    void AvatarManager::ClearOwn() {
        m_OwnTex   = {};
        m_OwnImage = nullptr;
        m_OwnBytes.clear();
        m_ImagePath.clear();
        ClearCropPreview();
    }

    void AvatarManager::ClearCropPreview() {
        m_CropPreviewImage = nullptr;
        m_CropPreviewTex   = {};
        m_CropSrcW = 0;
        m_CropSrcH = 0;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Peer avatar cache
    // ─────────────────────────────────────────────────────────────────────────────

    void AvatarManager::CachePeer(const std::string& username,
                                   const std::vector<uint8_t>& avatarData) {
        if (avatarData.empty()) {
            m_Peers.erase(username);
            return;
        }

        const std::size_t hash = HashBytes(avatarData);
        if (auto it = m_Peers.find(username); it != m_Peers.end()) {
            if (it->second.DataHash == hash) return;   // already cached
        }

        const uint32_t side = kAvatarPixelSize;
        const std::size_t expected = side * side * 4;
        if (avatarData.size() != expected) {
            spdlog::warn("Avatar for {} has unexpected size {} (expected {})",
                         username, avatarData.size(), expected);
            m_Peers.erase(username);
            return;
        }

        auto img = std::make_shared<Image>(side, side, ImageFormat::RGBA,
                                            avatarData.data());
        PeerEntry entry;
        entry.Img      = std::move(img);
        entry.Tex      = (ImTextureID)entry.Img->GetDescriptorSet();
        entry.DataHash = hash;
        m_Peers[username] = std::move(entry);
    }

    void AvatarManager::RemovePeer(const std::string& username) {
        m_Peers.erase(username);
    }

    void AvatarManager::ClearAllPeers() {
        m_Peers.clear();
    }

    ImTextureID AvatarManager::PeerTexture(const std::string& username) const {
        auto it = m_Peers.find(username);
        return (it != m_Peers.end()) ? it->second.Tex : ImTextureID{};
    }

    void AvatarManager::PatchAvatarsInMessages(std::vector<ChatEntry>& msgs,
                                                const std::string& ownUsername) const {
        for (auto& msg : msgs) {
            if (msg.Who == ownUsername && m_OwnTex) {
                msg.AvatarTex = m_OwnTex;
            } else if (auto it = m_Peers.find(msg.Who); it != m_Peers.end()) {
                msg.AvatarTex = it->second.Tex;
            }
        }
    }

} // namespace Safira
