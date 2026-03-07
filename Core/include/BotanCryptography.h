#ifndef PQC_MASTER_THESIS_2026_BOTAN_CRYPTOGRAPHY_H
#define PQC_MASTER_THESIS_2026_BOTAN_CRYPTOGRAPHY_H

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Botan {
    class Private_Key;
    class X509_Certificate;
}
namespace Botan::TLS {
    class Channel;
    class Callbacks;
}

namespace Safira::Crypto {

    enum class P2PKeyType {
        RSA_PSS,     // Classical
        ML_DSA_65,   // Full post-quantum (Botan 3.6+)
    };

    struct P2PKeyMaterial {
        std::shared_ptr<Botan::Private_Key>      Key;
        std::shared_ptr<Botan::X509_Certificate> Cert;
    };

    [[nodiscard]] P2PKeyMaterial GenerateOrLoadP2PCredentials(
        std::string_view identityName,
        P2PKeyType type = P2PKeyType::RSA_PSS);

    struct TlsEffects {
        std::function<void(std::span<const uint8_t>)> onEmitNetwork;
        std::function<void(std::span<const uint8_t>)> onMessageReceived;
        std::function<void(bool, std::string)>        onHandshakeComplete;
        std::function<void(std::string, uint32_t, bool)> onSystemLog;
    };

    struct TlsState {
        std::shared_ptr<Botan::TLS::Callbacks> callbacks;
        std::shared_ptr<Botan::TLS::Channel>   channel;
    };

    enum class TlsError { InitFailed, InvalidState };

    [[nodiscard]] std::expected<TlsState, TlsError>
    CreateServerState(const P2PKeyMaterial& keyMaterial, std::string peerUsername, TlsEffects effects);

    [[nodiscard]] std::expected<TlsState, TlsError>
    CreateClientState(const std::string& host, uint16_t port, std::string peerUsername, TlsEffects effects);

    void ProcessEncryptedData(const TlsState& state, std::span<const uint8_t> encryptedData);
    void EncryptAndSend(const TlsState& state, std::string_view plaintext);
    void CloseTls(const TlsState& state);

    [[nodiscard]] bool IsActive(const TlsState& state) noexcept;
    [[nodiscard]] bool IsClosed(const TlsState& state) noexcept;

} // namespace Safira::Crypto

#endif // PQC_MASTER_THESIS_2026_BOTAN_CRYPTOGRAPHY_H