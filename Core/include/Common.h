#ifndef PQC_MASTER_THESIS_2026_COMMON_H
#define PQC_MASTER_THESIS_2026_COMMON_H

// ═════════════════════════════════════════════════════════════════════════════
// Common.h — project-wide types
//
// §3.4 — Strong Types  (Slides 98-104)
//
//   "Strong type avoids mixing up different keys."
//
// ClientID is a distinct type, not a bare uint64_t.  The compiler rejects
// any attempt to silently mix it with a port, fd, byte count, or other
// integer — exactly the property the notes motivate for cryptographic keys.
// ═════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <functional>

namespace Safira {

    struct ClientID {
        uint64_t Value = 0;

        constexpr bool operator==(const ClientID&)  const noexcept = default;
        constexpr auto operator<=>(const ClientID&) const noexcept = default;

        /// False for the default-constructed / null sentinel.
        explicit constexpr operator bool() const noexcept { return Value != 0; }
    };

    /// Factory — FNV-1a 64-bit hash of (ip, port).  constexpr for compile-time tests.
    constexpr ClientID MakeClientID(uint32_t ip, uint16_t port) noexcept {
        uint64_t hash = 14695981039346656037ULL;
        auto mix = [&](uint8_t byte) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        };
        mix(static_cast<uint8_t>((ip >>  0) & 0xFF));
        mix(static_cast<uint8_t>((ip >>  8) & 0xFF));
        mix(static_cast<uint8_t>((ip >> 16) & 0xFF));
        mix(static_cast<uint8_t>((ip >> 24) & 0xFF));
        mix(static_cast<uint8_t>((port >> 0) & 0xFF));
        mix(static_cast<uint8_t>((port >> 8) & 0xFF));
        return ClientID{ hash };
    }

} // namespace Safira

template<>
struct std::hash<Safira::ClientID> {
    std::size_t operator()(Safira::ClientID id) const noexcept {
        return std::hash<uint64_t>{}(id.Value);
    }
};

#endif // PQC_MASTER_THESIS_2026_COMMON_H