#ifndef PQC_MASTER_THESIS_2026_AVATAR_UTILS_H
#define PQC_MASTER_THESIS_2026_AVATAR_UTILS_H

// ═════════════════════════════════════════════════════════════════════════════
// AvatarUtils.h — avatar image processing pipeline
//
// Pipeline:  Load file → (optional square crop) → resize to 64×64 RGBA
//
// Produces a std::vector<uint8_t> of raw RGBA pixels suitable for wire
// transfer via ConnectionRequestPacket::AvatarData.
//
// Dependencies:
//   stb_image.h          — image loading
//
// Avatar data is sent as raw RGBA pixels (kAvatarPixelSize × kAvatarPixelSize).
// For reduced wire size, add stb_image_write.h and encode to JPEG.
//
// All functions are header-only for simplicity.
// ═════════════════════════════════════════════════════════════════════════════

#include "UserInfo.h"    // kAvatarPixelSize, kMaxAvatarBytes

#include <imgui.h>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// RawImage — RAII wrapper for stb_image pixel data
// ─────────────────────────────────────────────────────────────────────────────
struct RawImage {
    std::unique_ptr<uint8_t[], decltype(&stbi_image_free)> Pixels{ nullptr, stbi_image_free };
    int Width    = 0;
    int Height   = 0;
    int Channels = 0;

    [[nodiscard]] bool Valid() const { return Pixels != nullptr && Width > 0 && Height > 0; }
    [[nodiscard]] bool IsSquare() const { return Width == Height; }
    [[nodiscard]] bool NeedsCrop() const { return Valid() && !IsSquare(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// CropRect — defines the square region the user selected
// ─────────────────────────────────────────────────────────────────────────────
struct CropRect {
    int X = 0;
    int Y = 0;
    int Size = 0;   // square side length
};

// ─────────────────────────────────────────────────────────────────────────────
// Load an image from disk.  Always requests 4 channels (RGBA).
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline RawImage LoadImageFromFile(const std::string& path) {
    RawImage img;
    uint8_t* px = stbi_load(path.c_str(), &img.Width, &img.Height, &img.Channels, 4);
    img.Pixels.reset(px);
    img.Channels = 4;
    return img;
}

// ─────────────────────────────────────────────────────────────────────────────
// Crop to a square region (copies pixels into a new buffer).
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline std::vector<uint8_t>
CropSquare(const RawImage& img, const CropRect& crop) {
    const int side = crop.Size;
    std::vector<uint8_t> out(side * side * 4);

    for (int row = 0; row < side; ++row) {
        const int srcRow = crop.Y + row;
        if (srcRow < 0 || srcRow >= img.Height) continue;

        const uint8_t* srcLine = img.Pixels.get() + srcRow * img.Width * 4;
        uint8_t*       dstLine = out.data() + row * side * 4;

        const int copyStart = std::max(0, crop.X);
        const int copyEnd   = std::min(img.Width, crop.X + side);
        const int copyW     = copyEnd - copyStart;
        if (copyW <= 0) continue;

        std::memcpy(dstLine + (copyStart - crop.X) * 4,
                    srcLine + copyStart * 4,
                    copyW * 4);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Default center crop — picks the largest centered square.
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline CropRect DefaultCenterCrop(const RawImage& img) {
    const int side = std::min(img.Width, img.Height);
    return {
        (img.Width  - side) / 2,
        (img.Height - side) / 2,
        side
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Simple bilinear downscale to kAvatarPixelSize × kAvatarPixelSize RGBA.
// (For production you'd use stb_image_resize2, but this avoids the dep.)
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline std::vector<uint8_t>
ResizeSquare(const uint8_t* srcPixels, int srcSide, int dstSide) {
    std::vector<uint8_t> dst(dstSide * dstSide * 4);
    const float scale = static_cast<float>(srcSide) / static_cast<float>(dstSide);

    for (int dy = 0; dy < dstSide; ++dy) {
        for (int dx = 0; dx < dstSide; ++dx) {
            const float sx = (dx + 0.5f) * scale - 0.5f;
            const float sy = (dy + 0.5f) * scale - 0.5f;

            const int x0 = std::clamp(static_cast<int>(sx), 0, srcSide - 1);
            const int y0 = std::clamp(static_cast<int>(sy), 0, srcSide - 1);
            const int x1 = std::min(x0 + 1, srcSide - 1);
            const int y1 = std::min(y0 + 1, srcSide - 1);

            const float fx = sx - std::floor(sx);
            const float fy = sy - std::floor(sy);

            for (int c = 0; c < 4; ++c) {
                float v00 = srcPixels[(y0 * srcSide + x0) * 4 + c];
                float v10 = srcPixels[(y0 * srcSide + x1) * 4 + c];
                float v01 = srcPixels[(y1 * srcSide + x0) * 4 + c];
                float v11 = srcPixels[(y1 * srcSide + x1) * 4 + c];

                float top = v00 + (v10 - v00) * fx;
                float bot = v01 + (v11 - v01) * fx;
                float val = top + (bot - top) * fy;

                dst[(dy * dstSide + dx) * 4 + c] =
                    static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
            }
        }
    }
    return dst;
}

// No JPEG compression — avatar data is raw RGBA pixels on the wire.
// To add JPEG compression later, include stb_image_write.h from the stb repo.

// ─────────────────────────────────────────────────────────────────────────────
// Full pipeline: load → crop → resize → raw RGBA bytes
//
// Returns std::nullopt if the file can't be loaded or the result exceeds
// kMaxAvatarBytes.  The returned vector is raw RGBA pixel data at
// kAvatarPixelSize × kAvatarPixelSize (suitable for direct upload via
// Safira::Image on the receiving end).
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline std::optional<std::vector<uint8_t>>
ProcessAvatarImage(const std::string& path, const CropRect& crop) {
    auto img = LoadImageFromFile(path);
    if (!img.Valid()) return std::nullopt;

    // Crop
    auto cropped = CropSquare(img, crop);

    // Resize to target pixel size
    auto resized = ResizeSquare(cropped.data(), crop.Size, kAvatarPixelSize);

    if (resized.size() > kMaxAvatarBytes)
        return std::nullopt;

    return resized;
}

// ─────────────────────────────────────────────────────────────────────────────
// ImGui crop widget — renders a preview of the image with a draggable
// square crop overlay.  Returns the updated CropRect.
//
// previewTex: ImTextureID of the full loaded image (uploaded to GPU)
// imgW, imgH: original image dimensions
// crop:       current crop rect (in image-pixel coordinates)
// previewH:   widget height in screen pixels
// ─────────────────────────────────────────────────────────────────────────────
inline CropRect DrawCropWidget(ImTextureID previewTex, int imgW, int imgH,
                               CropRect crop, float previewH = 300.0f) {
    if (!previewTex || imgW <= 0 || imgH <= 0) return crop;

    const float aspect = static_cast<float>(imgW) / static_cast<float>(imgH);
    const float previewW = previewH * aspect;
    const float scale = previewH / static_cast<float>(imgH);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Draw the full image
    dl->AddImage(previewTex, origin,
                 { origin.x + previewW, origin.y + previewH });

    // Dim the areas outside the crop
    ImU32 dimCol = IM_COL32(0, 0, 0, 140);

    float cx = origin.x + crop.X * scale;
    float cy = origin.y + crop.Y * scale;
    float cs = crop.Size * scale;

    // Top dim
    dl->AddRectFilled(origin, { origin.x + previewW, cy }, dimCol);
    // Bottom dim
    dl->AddRectFilled({ origin.x, cy + cs }, { origin.x + previewW, origin.y + previewH }, dimCol);
    // Left dim
    dl->AddRectFilled({ origin.x, cy }, { cx, cy + cs }, dimCol);
    // Right dim
    dl->AddRectFilled({ cx + cs, cy }, { origin.x + previewW, cy + cs }, dimCol);

    // Crop border
    dl->AddRect({ cx, cy }, { cx + cs, cy + cs }, IM_COL32(255, 255, 255, 220), 0.0f, 0, 2.0f);

    // Rule-of-thirds guides
    ImU32 guideCol = IM_COL32(255, 255, 255, 60);
    for (int i = 1; i <= 2; ++i) {
        float frac = static_cast<float>(i) / 3.0f;
        dl->AddLine({ cx + cs * frac, cy }, { cx + cs * frac, cy + cs }, guideCol);
        dl->AddLine({ cx, cy + cs * frac }, { cx + cs, cy + cs * frac }, guideCol);
    }

    // Invisible button for drag interaction
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##CropArea", { previewW, previewH });

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        crop.X += static_cast<int>(delta.x / scale);
        crop.Y += static_cast<int>(delta.y / scale);
    }

    // Scroll wheel to resize crop square
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (std::abs(wheel) > 0.01f) {
            int delta = static_cast<int>(wheel * 10.0f);
            int newSize = std::clamp(crop.Size + delta, 64, std::min(imgW, imgH));
            // Keep centered
            int diff = newSize - crop.Size;
            crop.X -= diff / 2;
            crop.Y -= diff / 2;
            crop.Size = newSize;
        }
    }

    // Clamp crop to image bounds
    crop.Size = std::clamp(crop.Size, 64, std::min(imgW, imgH));
    crop.X = std::clamp(crop.X, 0, imgW - crop.Size);
    crop.Y = std::clamp(crop.Y, 0, imgH - crop.Size);

    ImGui::SetCursorScreenPos({ origin.x, origin.y + previewH + 8.0f });
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                       "Drag to move crop. Scroll to resize.");

    return crop;
}

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_AVATAR_UTILS_H