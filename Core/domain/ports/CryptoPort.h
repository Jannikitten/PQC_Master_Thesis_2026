#ifndef SAFIRA_DOMAIN_PORTS_CRYPTOPORT_H
#define SAFIRA_DOMAIN_PORTS_CRYPTOPORT_H

// ═════════════════════════════════════════════════════════════════════════════
// Defines what the application layer needs from a crypto provider
// without prescribing the library (wolfSSL, Botan, OpenSSL, etc.).
// ═════════════════════════════════════════════════════════════════════════════

#include "Types.h"

#include <concepts>
#include <expected>
#include <string>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // P2PCredentialProvider — generates or loads TLS credentials for P2P sessions
    // ─────────────────────────────────────────────────────────────────────────────
    template <typename T>
    concept P2PCredentialProvider = requires(T& provider) {
        { provider.GetCertificatePath() } -> std::convertible_to<std::string>;
        { provider.GetPrivateKeyPath() }  -> std::convertible_to<std::string>;
        { provider.EnsureCredentials() }  -> std::same_as<std::expected<void, std::string>>;
    };

} // namespace Safira

#endif // SAFIRA_DOMAIN_PORTS_CRYPTOPORT_H
