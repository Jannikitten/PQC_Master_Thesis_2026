// ═══════════════════════════════════════════════════════════════════════════════
// ── Programming Task 1: Make Key Exchange Connection Quantum-Secure ────────────
//
// SCENARIO
// ────────
// The DTLS connection currently uses a CLASSICAL key-exchange scheme.
// Your task is to modify the code in this class so that the DTLS 
// handshake is protected against adversaries with quantum capabilities.
// ═══════════════════════════════════════════════════════════════════════════════

#include "WolfSSLCrypto.h"

#include <cstring>
#include <vector>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/rsa.h>

namespace Safira {

    void WolfSSLCrypto::ConfigureKeyExchange(WOLFSSL_CTX* ctx) {
        int groups[] = { WOLFSSL_ML_KEM_512 };
        wolfSSL_CTX_set_groups(ctx, groups, 1);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // ProtocolDescription
    // ─────────────────────────────────────────────────────────────────────────────

    std::string WolfSSLCrypto::ProtocolDescription() {
        return "DTLS 1.3 | ECC-SECP256R1";
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // GenerateSelfSignedCert — RSA-2048, SHA-256, 365-day validity
    // ─────────────────────────────────────────────────────────────────────────────

    std::expected<WolfSSLCredentials, WolfSSLCryptoError>
    WolfSSLCrypto::GenerateSelfSignedCert() {
        WC_RNG rng;
        if (wc_InitRng(&rng) != 0)
            return std::unexpected(WolfSSLCryptoError::CertificateGeneration);

        RsaKey key;
        if (wc_InitRsaKey(&key, nullptr) != 0) {
            wc_FreeRng(&rng);
            return std::unexpected(WolfSSLCryptoError::CertificateGeneration);
        }

        if (wc_MakeRsaKey(&key, 2048, WC_RSA_EXPONENT, &rng) != 0) {
            wc_FreeRsaKey(&key);
            wc_FreeRng(&rng);
            return std::unexpected(WolfSSLCryptoError::CertificateGeneration);
        }

        std::vector<uint8_t> keyDer(4096);
        int keySz = wc_RsaKeyToDer(&key, keyDer.data(),
                                    static_cast<word32>(keyDer.size()));
        if (keySz < 0) {
            wc_FreeRsaKey(&key);
            wc_FreeRng(&rng);
            return std::unexpected(WolfSSLCryptoError::CertificateGeneration);
        }
        keyDer.resize(static_cast<std::size_t>(keySz));

        Cert cert;
        wc_InitCert(&cert);
        std::strncpy(cert.subject.commonName, "Safira DTLS Server", CTC_NAME_SIZE);
        cert.isCA      = 0;
        cert.sigType   = CTC_SHA256wRSA;
        cert.daysValid = 365;

        std::vector<uint8_t> certDer(4096);
        int certSz = wc_MakeSelfCert(&cert, certDer.data(),
                                      static_cast<word32>(certDer.size()),
                                      &key, &rng);

        wc_FreeRsaKey(&key);
        wc_FreeRng(&rng);

        if (certSz < 0)
            return std::unexpected(WolfSSLCryptoError::CertificateGeneration);

        certDer.resize(static_cast<std::size_t>(certSz));

        return WolfSSLCredentials{
            .CertDer = std::move(certDer),
            .KeyDer  = std::move(keyDer),
        };
    }

} // namespace Safira
