#ifndef PQC_MASTER_THESIS_2026_WOLFTYPES_H
#define PQC_MASTER_THESIS_2026_WOLFTYPES_H

// ═════════════════════════════════════════════════════════════════════════════
// WolfTypes.h — RAII wrappers for wolfSSL objects
//
// §5.5 — Managing secret lifetimes  (Slides 222-224)
//
//   "We want to minimise the lifetime of secrets in memory."
//   "Rust: Drop handler can automatically erase memory."
//
// C++ equivalent: unique_ptr with a custom deleter.  WolfSession's deleter
// calls wolfSSL_free which internally zeroises session keys.
//
// Shared between Server.h and Client.h so neither depends on the other.
// ═════════════════════════════════════════════════════════════════════════════

#include <memory>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace Safira {

    struct WolfContextDeleter {
        void operator()(WOLFSSL_CTX* ctx) const noexcept {
            if (ctx) wolfSSL_CTX_free(ctx);
        }
    };

    struct WolfSessionDeleter {
        void operator()(WOLFSSL* ssl) const noexcept {
            if (ssl) wolfSSL_free(ssl);
        }
    };

    using WolfContext = std::unique_ptr<WOLFSSL_CTX, WolfContextDeleter>;
    using WolfSession = std::unique_ptr<WOLFSSL,     WolfSessionDeleter>;

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_WOLFTYPES_H