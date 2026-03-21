#ifndef SAFIRA_INFRASTRUCTURE_CRYPTO_WOLFSSLCRYPTO_H
#define SAFIRA_INFRASTRUCTURE_CRYPTO_WOLFSSLCRYPTO_H

// ═════════════════════════════════════════════════════════════════════════════
// WolfSSLCrypto.h — wolfSSL cryptography configuration
//
// Centralises the DTLS key-exchange algorithm selection and server
// certificate generation so that both DtlsClient and DtlsServer share
// a single, clearly documented configuration point.
//
// ── Programming Task 1 ──────────────────────────────────────────────────────
// The function ConfigureKeyExchange() currently uses a classical
// key-exchange algorithm.  Your task is to change it so that the DTLS
// connection uses post-quantum cryptography (ML-KEM-512).
// ═════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

struct WOLFSSL_CTX;

namespace Safira {

// Result type for certificate generation — matches the struct used by
// DtlsServer so it can be passed directly.
struct WolfSSLCredentials {
    std::vector<uint8_t> CertDer;
    std::vector<uint8_t> KeyDer;
};

// Error codes for certificate generation.
enum class WolfSSLCryptoError : uint8_t {
    CertificateGeneration,
};

class WolfSSLCrypto {
public:
    /// Configure the key-exchange groups on a wolfSSL TLS context.
    ///
    /// Programming Task 1: Change the key-exchange algorithm used here
    /// so that the DTLS connection is protected against quantum computers.
    static void ConfigureKeyExchange(WOLFSSL_CTX* ctx);

    /// Generate a self-signed RSA-2048 server certificate in DER format.
    static std::expected<WolfSSLCredentials, WolfSSLCryptoError> GenerateSelfSignedCert();

    /// Human-readable protocol label shown in the chat status bar.
    static std::string ProtocolDescription();
};

} // namespace Safira

#endif // SAFIRA_INFRASTRUCTURE_CRYPTO_WOLFSSLCRYPTO_H
