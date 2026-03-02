#ifndef PQC_MASTER_THESIS_2026_COMMON_H
#define PQC_MASTER_THESIS_2026_COMMON_H

#include <cstdint>
#include <cstring>

namespace Safira {
    // A ClientID is the FNV-1a hash of the client's (ip, port) pair so it
    // is stable, cheap to compute, and fits in a 64-bit map key.
    using ClientID = uint64_t;

    inline ClientID MakeClientID(uint32_t ip, uint16_t port) {
        // FNV-1a 64-bit
        uint64_t hash = 14695981039346656037ULL;
        auto mix = [&](uint8_t byte) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        };
        mix((ip >>  0) & 0xFF);
        mix((ip >>  8) & 0xFF);
        mix((ip >> 16) & 0xFF);
        mix((ip >> 24) & 0xFF);
        mix((port >> 0) & 0xFF);
        mix((port >> 8) & 0xFF);
        return hash;
    }

} // namespace Safira
#endif //PQC_MASTER_THESIS_2026_APPLICATIONCONSOLE_H