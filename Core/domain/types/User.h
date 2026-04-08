#ifndef SAFIRA_DOMAIN_TYPES_USER_H
#define SAFIRA_DOMAIN_TYPES_USER_H

// ═════════════════════════════════════════════════════════════════════════════
// No infrastructure dependencies.  Serialization is provided separately
// by infrastructure/serialization/PacketCodec.
// ═════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // Avatar constants
    // ─────────────────────────────────────────────────────────────────────────────
    static constexpr int    kAvatarPixelSize = 16;
    static constexpr std::size_t kMaxAvatarBytes  = kAvatarPixelSize * kAvatarPixelSize * 4;

    namespace AvatarColors {
        static constexpr int kCount = 8;

        // ABGR colours (ImGui IM_COL32 byte order)
        static constexpr uint32_t kPalette[kCount] = {
            0xFF4444EE, // Red
            0xFF44CC44, // Green
            0xFFEE8844, // Orange
            0xFF44BBEE, // Sky blue
            0xFFEEEE44, // Yellow
            0xFFCC44CC, // Purple
            0xFF44EECC, // Teal
            0xFFEEEEEE, // White
        };
    } // namespace AvatarColors

    // ─────────────────────────────────────────────────────────────────────────────
    // UserInfo — per-client metadata
    // ─────────────────────────────────────────────────────────────────────────────
    struct UserInfo {
        uint32_t              Color      = 0xFFFFFFFF;
        std::string           Username;
        std::vector<uint8_t>  AvatarData;    // raw RGBA, ≤ kMaxAvatarBytes
    };

} // namespace Safira

#endif // SAFIRA_DOMAIN_TYPES_USER_H
