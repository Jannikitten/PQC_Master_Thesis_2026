#ifndef PQC_MASTER_THESIS_2026_WOLFTYPES_H
#define PQC_MASTER_THESIS_2026_WOLFTYPES_H

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