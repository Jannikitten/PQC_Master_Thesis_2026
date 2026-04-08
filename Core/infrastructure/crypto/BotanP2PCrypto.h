#ifndef SAFIRA_INFRASTRUCTURE_CRYPTO_BOTANP2PCRYPTO_H
#define SAFIRA_INFRASTRUCTURE_CRYPTO_BOTANP2PCRYPTO_H

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Safira {

    class BotanP2PCrypto {
    public:
        BotanP2PCrypto();
        ~BotanP2PCrypto();

        BotanP2PCrypto(const BotanP2PCrypto&)            = delete;
        BotanP2PCrypto& operator=(const BotanP2PCrypto&) = delete;

        // ── Configuration ────────────────────────────────────────────────────────

        struct Config {
            int         SocketFd     = -1;
            bool        IsResponder  = false;
            std::string OwnUsername;
            std::string PeerUsername;
        };

        struct Callbacks {
            // Called when the TLS layer has encrypted data to send over the wire.
            std::function<void(std::span<const uint8_t>)> EmitData;

            // Called when a decrypted message is received from the peer.
            std::function<void(const std::string& from,
                               const std::string& text,
                               uint32_t color)> MessageReceived;

            // Called once when the TLS handshake completes successfully.
            std::function<void()> Activated;

            // Called when the peer sends close_notify or disconnects.
            std::function<void(const std::string& message)> Closed;
        };

        // ── Lifecycle ────────────────────────────────────────────────────────────

        // Initialise the TLS channel (server or client role) and begin the
        // handshake.  The channel is ready to process data after this call.
        void Start(const Config& config, Callbacks callbacks);

        // Feed raw bytes received from the TCP socket into the TLS engine.
        // Decrypted application data will arrive via the MessageReceived callback.
        void FeedReceivedData(std::span<const uint8_t> data);

        // Encrypt and send a plaintext chat message to the peer.
        void Send(std::string_view message);

        // Send close_notify and tear down the TLS channel.
        void Close();

        // ── Queries ──────────────────────────────────────────────────────────────

        [[nodiscard]] bool IsActivated()           const;
        [[nodiscard]] bool IsCloseNotifyReceived() const;
        [[nodiscard]] bool IsClosed()              const;

        // Human-readable protocol label for the UI status bar.
        static std::string ProtocolDescription();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Safira

#endif // SAFIRA_INFRASTRUCTURE_CRYPTO_BOTANP2PCRYPTO_H
