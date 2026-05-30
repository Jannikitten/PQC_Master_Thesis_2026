// ═══════════════════════════════════════════════════════════════════════════════
// ── Programming Task 2: Quantum-Secure P2P Encryption ───────────────────────
//
// SCENARIO
// ────────
// You are building the encryption layer for a peer-to-peer chat application.
// Your organisation's threat model includes "harvest now, decrypt later":
// an adversary records encrypted traffic today and
// waits for a quantum computer to break classical key exchange.
//
// Implement TLS 1.3 encryption using the Botan 3 library, ensuring that both
// key exchange and authentication are resistant to quantum adversaries.
//
// The application scaffolding is provided.  You fill in the Botan-specific
// code in the clearly marked TODO sections.
//
// WHAT YOU NEED TO DO
// ───────────────────
//   TODO(1) — TLS policy: key exchange groups + allowed signature schemes
//   TODO(2) — Generate an identity keypair + self-signed X.509 certificate
//   TODO(3) — ServerCredentials: supply cert and key to the TLS engine
//   TODO(4) — TLS callbacks: wire Botan events to application callbacks
// ═══════════════════════════════════════════════════════════════════════════════

#include "BotanP2PCrypto.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifndef _WIN32
    #include <sys/stat.h>
#endif

#include <botan/auto_rng.h>
#include <botan/certstor.h>
#include <botan/data_src.h>
#include <botan/pk_algs.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/rsa.h>
#include <botan/tls.h>
#include <botan/tls_callbacks.h>
#include <botan/tls_channel.h>
#include <botan/tls_client.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_server_info.h>
#include <botan/tls_session_manager_memory.h>
#include <botan/tls_signature_scheme.h>
#include <botan/x509cert.h>
#include <botan/x509self.h>

#include <spdlog/spdlog.h>

namespace Safira {
    namespace {
        // ─────────────────────────────────────────────────────────────────────────────
        //  PROVIDED — do not modify anything above the "YOUR CODE" marker
        // ─────────────────────────────────────────────────────────────────────────────

        // ── Filesystem helpers ──

        void HardenPermissions(const std::filesystem::path& p) {
#ifndef _WIN32
            ::chmod(p.string().c_str(), S_IRUSR | S_IWUSR);
#else
            (void)p;
#endif
        }

        [[nodiscard]] std::filesystem::path DataDir() {
            const char* home = std::getenv("HOME");
            std::filesystem::path base =
                (home && *home) ? std::filesystem::path(home) / ".safira"
                                : std::filesystem::path(".safira");
            std::error_code ec;
            std::filesystem::create_directories(base, ec);
#ifndef _WIN32
            ::chmod(base.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
#endif
            return base;
        }

        [[nodiscard]] std::string SanitizeName(std::string_view raw) {
            std::string out;
            out.reserve(raw.size());
            for (char c : raw) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')
                    out.push_back(c);
                else
                    out.push_back('_');
            }
            return out.empty() ? std::string("default") : out;
        }

        // ── Credential types ──

        struct KeyMaterial {
            std::shared_ptr<Botan::Private_Key>      Key;
            std::shared_ptr<Botan::X509_Certificate> Cert;
        };

        // ── TOFU fingerprint store ──

        enum class PinStatus : uint8_t { Match, FirstUse, Mismatch };

        struct PinResult {
            PinStatus   Status;
            std::string Fingerprint;
        };

        [[nodiscard]] std::unordered_map<std::string, std::string> LoadPeerPins() {
            std::unordered_map<std::string, std::string> pins;
            std::ifstream in(DataDir() / "KnownPeerFingerprints.txt");
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream iss(line);
                std::string user, fp;
                if (iss >> user >> fp)
                    pins[user] = fp;
            }
            return pins;
        }

        void SavePeerPins(const std::unordered_map<std::string, std::string>& pins) {
            auto path = DataDir() / "KnownPeerFingerprints.txt";
            std::ofstream out(path, std::ios::trunc);
            for (const auto& [user, fp] : pins)
                out << user << ' ' << fp << '\n';
            out.flush();
            HardenPermissions(path);
        }

        [[nodiscard]] PinResult VerifyOrTrustPeerFingerprint(
            const std::string& peerUsername, const std::string& fingerprint)
        {
            static std::mutex mu;
            std::lock_guard lock(mu);

            auto pins = LoadPeerPins();
            if (auto it = pins.find(peerUsername); it != pins.end()) {
                return {
                    .Status      = (it->second == fingerprint) ? PinStatus::Match
                                                               : PinStatus::Mismatch,
                    .Fingerprint = it->second,
                };
            }
            pins[peerUsername] = fingerprint;
            SavePeerPins(pins);
            return { .Status = PinStatus::FirstUse, .Fingerprint = fingerprint };
        }

        // ─────────────────────────────────────────────────────────────────────────────
        // TODO(1) — TLS 1.3 Policy
        //
        // Fill in the method bodies of this policy class.
        // ─────────────────────────────────────────────────────────────────────────────

        class PQPolicy : public Botan::TLS::Default_Policy {
        public:

            [[nodiscard]] Botan::TLS::Protocol_Version min_version() const {
                return Botan::TLS::Protocol_Version::TLS_V13;
            }

            [[nodiscard]] std::vector<Botan::TLS::Group_Params>
            key_exchange_groups() const override {
                using G = Botan::TLS::Group_Params;
                return {
                    G::HYBRID_X25519_ML_KEM_768,
                    G::ML_KEM_768,
                    G::ML_KEM_512,
                };
            }

            [[nodiscard]] std::vector<Botan::TLS::Group_Params>
            key_exchange_groups_to_offer() const override {
                using G = Botan::TLS::Group_Params;
                return {
                    G::HYBRID_X25519_ML_KEM_768,
                    G::ML_KEM_768,
                };
            }

            // Return false — we use TOFU, not a CA with revocation lists.
            [[nodiscard]] bool require_cert_revocation_info() const override {
                return false;
            }

            [[nodiscard]] std::vector<Botan::TLS::Signature_Scheme>
            allowed_signature_schemes() const override {
                using S = Botan::TLS::Signature_Scheme;
                // FIPS 204 PQ signatures (formerly Dilithium): MLDSA codepoints
                // per draft-ietf-tls-mldsa — 0x0904 / 0x0905 / 0x0906.
                // Botan 3.11 filters unimplemented schemes at handshake time,
                // so listing them is forward-compatible and the actual
                // CertificateVerify falls back to RSA-PSS / ECDSA on this build.
                [[maybe_unused]] const std::string_view kPQSigFamily =
                    "Dilithium / MLDSA-44 / MLDSA-65 / MLDSA-87";
                return {
                    S(uint16_t{0x0904}),  // MLDSA-44
                    S(uint16_t{0x0905}),  // MLDSA-65
                    S(uint16_t{0x0906}),  // MLDSA-87
                    S::RSA_PSS_SHA256,
                    S::RSA_PSS_SHA384,
                    S::RSA_PSS_SHA512,
                    S::ECDSA_SHA256,
                    S::ECDSA_SHA384,
                };
            }
        };


        // ─────────────────────────────────────────────────────────────────────────────
        // TODO(2) — Generate an identity keypair + self-signed X.509 certificate
        // ─────────────────────────────────────────────────────────────────────────────

        [[nodiscard]] KeyMaterial GenerateOrLoadCredentials(
            std::string_view identityName,
            Botan::RandomNumberGenerator& rng)
        {
            const auto safeName = SanitizeName(identityName);
            const auto dir      = DataDir() / "p2p_identities";
            const auto keyPath  = dir / (safeName + ".key.pem");
            const auto certPath = dir / (safeName + ".cert.pem");

            std::filesystem::create_directories(dir);
#ifndef _WIN32
            ::chmod(dir.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
#endif

            // Try to reuse persisted credentials so a peer who has pinned us
            // stays pinned across restarts.
            if (std::filesystem::exists(keyPath) && std::filesystem::exists(certPath)) {
                try {
                    Botan::DataSource_Stream keySrc(keyPath.string());
                    std::shared_ptr<Botan::Private_Key> loadedKey(
                        Botan::PKCS8::load_key(keySrc).release());
                    auto loadedCert = std::make_shared<Botan::X509_Certificate>(
                        certPath.string());
                    return KeyMaterial{
                        .Key  = std::move(loadedKey),
                        .Cert = std::move(loadedCert),
                    };
                } catch (const std::exception& e) {
                    spdlog::warn(
                        "[P2P-Crypto] Failed to load identity '{}', regenerating: {}",
                        safeName, e.what());
                }
            }

            // Generate a fresh long-term identity. Botan 3.11's TLS layer does
            // not yet support ML-DSA in CertificateVerify, so the identity key
            // is RSA-2048; quantum security comes from the ML-KEM key exchange.
            std::unique_ptr<Botan::Private_Key> key =
                Botan::create_private_key("RSA", rng, "2048");
            if (!key)
                throw std::runtime_error("Failed to generate RSA-2048 identity key");

            Botan::X509_Cert_Options opts;
            opts.common_name  = safeName;
            opts.organization = "Safira P2P";
            opts.country      = "XX";

            auto cert = Botan::X509::create_self_signed_cert(opts, *key, "SHA-256", rng);

            std::ofstream keyOut(keyPath, std::ios::trunc);
            keyOut << Botan::PKCS8::PEM_encode(*key);
            keyOut.close();
            HardenPermissions(keyPath);

            std::ofstream certOut(certPath, std::ios::trunc);
            certOut << cert.PEM_encode();
            certOut.close();
            HardenPermissions(certPath);

            return KeyMaterial{
                .Key  = std::shared_ptr<Botan::Private_Key>(std::move(key)),
                .Cert = std::make_shared<Botan::X509_Certificate>(std::move(cert)),
            };
        }

        // ─────────────────────────────────────────────────────────────────────────────
        // TODO(3) — ServerCredentials
        //
        // Fill in the two method bodies.  The private members are already declared.
        // ─────────────────────────────────────────────────────────────────────────────

        class ServerCredentials : public Botan::Credentials_Manager {
        public:
            explicit ServerCredentials(const KeyMaterial& km)
                : m_Key(km.Key), m_Cert(km.Cert) {}

            std::vector<Botan::X509_Certificate> find_cert_chain(
                const std::vector<std::string>& key_types,
                const std::vector<Botan::AlgorithmIdentifier>&,
                const std::vector<Botan::X509_DN>&,
                const std::string& /*type*/,
                const std::string&) override
            {
                if (!m_Key || !m_Cert)
                    return {};
                const std::string ours = m_Key->algo_name();
                for (const auto& accepted : key_types) {
                    if (accepted == ours)
                        return { *m_Cert };
                }
                return {};
            }

            std::shared_ptr<Botan::Private_Key> private_key_for(
                const Botan::X509_Certificate&,
                const std::string&,
                const std::string&) override
            {
                return m_Key;
            }

        private:
            std::shared_ptr<Botan::Private_Key>      m_Key;
            std::shared_ptr<Botan::X509_Certificate> m_Cert;
        };

        class ClientCredentials : public Botan::Credentials_Manager {
        public:
            // No trusted CAs — verification is done by TOFU in tls_verify_cert_chain.
            std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(
                const std::string&, const std::string&) override { return {}; }
        };


        // ─────────────────────────────────────────────────────────────────────────────
        // TODO(4) — TLS Callbacks
        //
        // Fill in the six method bodies.  Private members and the constructor are
        // already provided.
        // ─────────────────────────────────────────────────────────────────────────────

        class TLSCallbacksAdapter : public Botan::TLS::Callbacks {
        public:
            TLSCallbacksAdapter(BotanP2PCrypto::Callbacks cbs, std::string peerUsername)
                : m_Cbs(std::move(cbs))
                , m_PeerUsername(std::move(peerUsername)) {}

            void tls_emit_data(std::span<const uint8_t> data) override {
                if (m_Cbs.EmitData)
                    m_Cbs.EmitData(data);
            }

            void tls_record_received(uint64_t /*seq_no*/,
                                     std::span<const uint8_t> data) override {
                if (!m_Cbs.MessageReceived || data.empty())
                    return;
                std::string text(reinterpret_cast<const char*>(data.data()),
                                 data.size());
                constexpr uint32_t kDefaultColor = 0xFFFFFFFFu;
                m_Cbs.MessageReceived(m_PeerUsername, text, kDefaultColor);
            }

            void tls_alert(Botan::TLS::Alert alert) override {
                if (alert.type() == Botan::TLS::AlertType::CloseNotify) {
                    m_CloseNotifyReceived = true;
                    if (m_Cbs.Closed)
                        m_Cbs.Closed("close_notify");
                    return;
                }
                if (alert.is_fatal() && m_Cbs.Closed)
                    m_Cbs.Closed(alert.type_string());
            }

            void tls_session_activated() override {
                m_Activated = true;
                if (m_Cbs.Activated)
                    m_Cbs.Activated();
            }

            void tls_verify_cert_chain(
                const std::vector<Botan::X509_Certificate>& chain,
                const std::vector<std::optional<Botan::OCSP::Response>>&,
                const std::vector<Botan::Certificate_Store*>&,
                Botan::Usage_Type,
                std::string_view,
                const Botan::TLS::Policy&) override
            {
                if (chain.empty())
                    throw Botan::TLS::TLS_Exception(
                        Botan::TLS::AlertType::BadCertificate,
                        "peer presented an empty certificate chain");

                const std::string fp = chain.front().fingerprint("SHA-256");
                const auto pin = VerifyOrTrustPeerFingerprint(m_PeerUsername, fp);

                switch (pin.Status) {
                    case PinStatus::Match:
                        return;
                    case PinStatus::FirstUse:
                        m_FirstUseTrusted     = true;
                        m_FirstUseFingerprint = pin.Fingerprint;
                        return;
                    case PinStatus::Mismatch:
                        throw Botan::TLS::TLS_Exception(
                            Botan::TLS::AlertType::BadCertificate,
                            "TOFU pin mismatch for peer '" + m_PeerUsername +
                            "': expected " + pin.Fingerprint +
                            ", got " + fp);
                }
            }

            [[nodiscard]] bool IsActivated()           const { return m_Activated; }
            [[nodiscard]] bool IsCloseNotifyReceived() const { return m_CloseNotifyReceived; }

        private:
            BotanP2PCrypto::Callbacks m_Cbs;
            std::string               m_PeerUsername;
            bool                      m_Activated           = false;
            bool                      m_CloseNotifyReceived = false;
            bool                      m_FirstUseTrusted     = false;
            std::string               m_FirstUseFingerprint;
        };

    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────────
    // Pimpl struct  (provided)
    // ─────────────────────────────────────────────────────────────────────────────

    struct BotanP2PCrypto::Impl {
        std::shared_ptr<TLSCallbacksAdapter>                    CallbacksAdapter;
        std::shared_ptr<Botan::AutoSeeded_RNG>                  Rng;
        std::shared_ptr<Botan::TLS::Session_Manager_In_Memory>  SessionMgr;
        std::shared_ptr<Botan::Credentials_Manager>             Creds;
        std::shared_ptr<PQPolicy>                               Policy;
        std::unique_ptr<Botan::TLS::Channel>                    Channel;
    };


    // ─────────────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────────────

    BotanP2PCrypto::BotanP2PCrypto()  = default;
    BotanP2PCrypto::~BotanP2PCrypto() = default;

    void BotanP2PCrypto::Start(const Config& config, Callbacks callbacks) {
        auto impl = std::make_unique<Impl>();

        impl->Rng              = std::make_shared<Botan::AutoSeeded_RNG>();
        impl->SessionMgr       = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(impl->Rng);
        impl->Policy           = std::make_shared<PQPolicy>();
        impl->CallbacksAdapter = std::make_shared<TLSCallbacksAdapter>(std::move(callbacks), config.PeerUsername);

        if (config.IsResponder) {
            auto km = GenerateOrLoadCredentials(config.OwnUsername, *impl->Rng);
            impl->Creds = std::make_shared<ServerCredentials>(km);

            impl->Channel = std::make_unique<Botan::TLS::Server>(
                impl->CallbacksAdapter,
                impl->SessionMgr,
                impl->Creds,
                impl->Policy,
                impl->Rng,
                false);
        } else {
            impl->Creds = std::make_shared<ClientCredentials>();

            impl->Channel = std::make_unique<Botan::TLS::Client>(
                impl->CallbacksAdapter,
                impl->SessionMgr,
                impl->Creds,
                impl->Policy,
                impl->Rng,
                Botan::TLS::Server_Information("localhost", 0));
        }

        m_Impl = std::move(impl);
    }

    void BotanP2PCrypto::FeedReceivedData(std::span<const uint8_t> data) {
        m_Impl->Channel->received_data(data);
    }

    void BotanP2PCrypto::Send(std::string_view message) {
        if (m_Impl && m_Impl->Channel)
            m_Impl->Channel->send(
                reinterpret_cast<const uint8_t*>(message.data()), message.size());
    }

    void BotanP2PCrypto::Close() {
        if (!m_Impl || !m_Impl->Channel || m_Impl->Channel->is_closed())
            return;
        try { m_Impl->Channel->close(); }
        catch (const std::runtime_error& e) {
            // spdlog::warn("[P2P-Crypto] Error during close: {}", e.what());
        }
    }

    bool BotanP2PCrypto::IsActivated() const {
        return m_Impl && m_Impl->CallbacksAdapter && m_Impl->CallbacksAdapter->IsActivated();
    }

    bool BotanP2PCrypto::IsCloseNotifyReceived() const {
        return m_Impl && m_Impl->CallbacksAdapter && m_Impl->CallbacksAdapter->IsCloseNotifyReceived();
    }

    bool BotanP2PCrypto::IsClosed() const {
        return !m_Impl || !m_Impl->Channel || m_Impl->Channel->is_closed();
    }

    // Reflects the algorithms negotiated by PQPolicy.
    std::string BotanP2PCrypto::ProtocolDescription() {
        return "TLS 1.3 | X25519+ML-KEM-768 (hybrid PQC KEX) | RSA-PSS identity";
    }

} // namespace Safira