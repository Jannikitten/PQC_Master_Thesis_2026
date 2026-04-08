#ifndef SAFIRA_DOMAIN_TYPES_CONNECTION_H
#define SAFIRA_DOMAIN_TYPES_CONNECTION_H

// ═════════════════════════════════════════════════════════════════════════════
// These enums describe the logical connection lifecycle visible to the
// application/presentation layers.  Transport-level details (wolfSSL
// sessions, sockets) remain in infrastructure/.
// ═════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string_view>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // ConnectionStatus — public connection state consumed by the UI
    // ─────────────────────────────────────────────────────────────────────────────
    enum class ConnectionStatus : uint8_t {
        Disconnected,
        Connecting,
        Connected,
        FailedToConnect,
    };

    [[nodiscard]] constexpr std::string_view Describe(ConnectionStatus s) noexcept {
        switch (s) {
            case ConnectionStatus::Disconnected:    return "Disconnected";
            case ConnectionStatus::Connecting:      return "Connecting";
            case ConnectionStatus::Connected:       return "Connected";
            case ConnectionStatus::FailedToConnect: return "Failed to Connect";
        }
        return "Unknown";
    }

} // namespace Safira

#endif // SAFIRA_DOMAIN_TYPES_CONNECTION_H
