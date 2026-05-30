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
// The application scaffolding is provided. You fill in the Botan-specific
// code in the clearly marked TODO sections.
//
// WHAT YOU NEED TO DO
// ───────────────────
// TODO(1) — TLS policy: key exchange groups + allowed signature schemes
// TODO(2) — Generate an identity keypair + self-signed X.509 certificate
// TODO(3) — ServerCredentials: supply cert and key to the TLS engine
// TODO(4) — TLS callbacks: wire Botan events to application callbacks
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
// PROVIDED — do not modify anything above the "YOUR CODE" marker
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
    std::shared_ptr<Botan::Private_Key> Key;
    std::shared_ptr<Botan::X509_Certificate> Cert;
};

// ── TOFU fingerprint store ──
enum class PinStatus : uint8_t { Match, FirstUse, Mismatch };
struct PinResult {
    PinStatus  Status;
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
// TODO(1) — TLS 1.3 Policy   [IMPLEMENTED]
//
// Strategy:
//   Key exchange  → Hybrid X25519 + ML-KEM-768  (NIST FIPS 203)
//                   Pure ML-KEM-768 as secondary offer
//   Authentication→ Dilithium-6x5-r3  (ML-DSA Level 3, NIST FIPS 204)
//
// Java analogy: this is like calling
//   SSLParameters.setCipherSuites()/setNamedGroups() in a custom SSLContext
//   configured with a BouncyCastle PQC provider.
// ─────────────────────────────────────────────────────────────────────────────
class PQPolicy : public Botan::TLS::Default_Policy {
public:
    // Require TLS 1.3 minimum — no downgrade to 1.2
    [[nodiscard]] Botan::TLS::Protocol_Version min_version() const {
        return Botan::TLS::Protocol_Version::TLS_V13;
    }

    // Groups accepted during the handshake (server-side preference list).
    // X25519MLKEM768 is the IETF hybrid group (X25519 + ML-KEM-768) that
    // provides classical AND quantum security simultaneously.
    [[nodiscard]] std::vector<Botan::TLS::Group_Params>
    key_exchange_groups() const override {
        return {
            Botan::TLS::Group_Params::X25519MLKEM768, // Hybrid — preferred
            Botan::TLS::Group_Params::MLKEM768,        // Pure PQ — fallback
        };
    }

    // Groups we proactively send a key-share for in the ClientHello.
    // Offering the hybrid first avoids an extra round-trip for most peers.
    [[nodiscard]] std::vector<Botan::TLS::Group_Params>
    key_exchange_groups_to_offer() const override {
        return key_exchange_groups();
    }

    // Return false — we use TOFU, not a CA with revocation lists.
    [[nodiscard]] bool require_cert_revocation_info() const override {
        return false;
    }

    // Allow only ML-DSA (Dilithium) for authentication.
    // Dilithium-6x5-r3 corresponds to ML-DSA-6x5 (NIST FIPS 204 Level 3).
    [[nodiscard]] std::vector<Botan::TLS::Signature_Scheme>
    allowed_signature_schemes() const override {
        return {
            Botan::TLS::Signature_Scheme("Dilithium-6x5-r3"),
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TODO(2) — Generate an identity keypair + self-signed X.509 certificate
//           [IMPLEMENTED]
//
// Key algorithm: Dilithium-6x5-r3 (ML-DSA Level 3, NIST FIPS 204).
//   - Lattice-based signature scheme; no known efficient quantum attack.
//   - Level 3 gives ~192-bit equivalent classical security.
//
// Java analogy:
//   KeyPairGenerator kpg = KeyPairGenerator.getInstance("Dilithium", "BCPQC");
//   kpg.initialize(new DilithiumParameterSpec(DilithiumParameterSpec.dilithium3));
//   KeyPair kp = kpg.generateKeyPair();
//   // Then build an X.509 cert with JcaX509v3CertificateBuilder + JcaContentSignerBuilder
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] KeyMaterial GenerateOrLoadCredentials(
    std::string_view identityName,
    Botan::RandomNumberGenerator& rng)
{
    const auto safeName  = SanitizeName(identityName);
    const auto dir       = DataDir() / "p2p_identities";
    const auto keyPath   = dir / (safeName + ".key.pem");
    const auto certPath  = dir / (safeName + ".cert.pem");

    std::filesystem::create_directories(dir);
#ifndef _WIN32
    ::chmod(dir.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
#endif

    // ── Try loading existing credentials from disk ────────────────────────────
    if (std::filesystem::exists(keyPath) && std::filesystem::exists(certPath)) {
        try {
            Botan::DataSource_Stream keyIn(keyPath.string());
            auto key = Botan::PKCS8::load_key(keyIn);

            Botan::DataSource_Stream certIn(certPath.string());
            auto cert = std::make_shared<Botan::X509_Certificate>(certIn);

            spdlog::info("[P2P-Crypto] Loaded existing identity for '{}'", safeName);
            return KeyMaterial{
                .Key  = std::shared_ptr<Botan::Private_Key>(key.release()),
                .Cert = cert,
            };
        } catch (const std::exception& e) {
            spdlog::warn("[P2P-Crypto] Could not load credentials for '{}': {}. "
                         "Regenerating...", safeName, e.what());
        }
    }

    // ── Generate a new Dilithium keypair ─────────────────────────────────────
    // Botan::create_private_key dispatches on the algorithm name.
    // "Dilithium" selects the Dilithium family; "Dilithium-6x5-r3" pins
    // the parameter set (k=6, l=5, round 3 — i.e. Dilithium3 / ML-DSA-6x5).
    auto key = Botan::create_private_key("Dilithium", rng, "Dilithium-6x5-r3");

    // ── Build a self-signed X.509 certificate ────────────────────────────────
    Botan::X509_Cert_Options opts;
    opts.common_name  = std::string(identityName);
    opts.organization = "Safira P2P";
    opts.country      = "XX";
    opts.not_after    = "20380101000000Z";  // ~12 years validity
    opts.self_signed(true);

    // SHA-512 is the hash used for the cert structure itself.
    // Dilithium's internal commitment hash is unaffected by this parameter.
    auto cert_obj = Botan::X509::create_self_signed_cert(opts, *key, "SHA-512", rng);
    auto cert     = std::make_shared<Botan::X509_Certificate>(cert_obj);

    // ── Persist private key (PEM, owner-readable only) ───────────────────────
    {
        std::ofstream out(keyPath);
        out << Botan::PKCS8::PEM_encode(*key);
        out.flush();
        HardenPermissions(keyPath);
    }

    // ── Persist certificate ──────────────────────────────────────────────────
    {
        std::ofstream out(certPath);
        out << cert->PEM_encode();
        out.flush();
    }

    spdlog::info("[P2P-Crypto] Generated new Dilithium-6x5-r3 identity for '{}'",
                 safeName);

    return KeyMaterial{
        .Key  = std::shared_ptr<Botan::Private_Key>(key.release()),
        .Cert = cert,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// TODO(3) — ServerCredentials   [IMPLEMENTED]
//
// Botan calls find_cert_chain() to ask: "do you have a certificate whose
// public key algorithm is in key_types?"  We return our Dilithium cert when
// the algorithm name matches.
//
// private_key_for() is then called with the matching certificate to retrieve
// the corresponding private key for signing the handshake.
//
// Java analogy: implementing javax.net.ssl.X509KeyManager's
//   getCertificateChain() and getPrivateKey() methods.
// ─────────────────────────────────────────────────────────────────────────────
class ServerCredentials : public Botan::Credentials_Manager {
public:
    explicit ServerCredentials(const KeyMaterial& km)
        : m_Key(km.Key), m_Cert(km.Cert) {}

    std::vector<Botan::X509_Certificate> find_cert_chain(
        const std::vector<std::string>& key_types,
        const std::vector<Botan::AlgorithmIdentifier>&,
        const std::vector<Botan::X509_DN>&,
        const std::string& /* type */,
        const std::string& /* context */) override
    {
        // Return our certificate only when the TLS engine is asking for
        // the algorithm our key implements (e.g. "Dilithium").
        for (const auto& kt : key_types) {
            if (kt == m_Key->algo_name()) {
                return { *m_Cert };
            }
        }
        return {};
    }

    std::shared_ptr<Botan::Private_Key> private_key_for(
        const Botan::X509_Certificate& /* cert */,
        const std::string& /* type */,
        const std::string& /* context */) override
    {
        return m_Key;
    }

private:
    std::shared_ptr<Botan::Private_Key>    m_Key;
    std::shared_ptr<Botan::X509_Certificate> m_Cert;
};

class ClientCredentials : public Botan::Credentials_Manager {
public:
    // No trusted CAs — verification is done by TOFU in tls_verify_cert_chain.
    std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(
        const std::string&, const std::string&) override { return {}; }
};

// ─────────────────────────────────────────────────────────────────────────────
// TODO(4) — TLS Callbacks   [IMPLEMENTED]
//
// Botan's TLS engine communicates with the application through this adapter.
// Each virtual method corresponds to a TLS event:
//
//   tls_emit_data        → ciphertext to send over the network
//   tls_record_received  → plaintext message received from peer
//   tls_alert            → peer sent a TLS alert (including close_notify)
//   tls_session_activated→ handshake complete; session is live
//   tls_verify_cert_chain→ validate the peer's certificate (TOFU logic here)
//
// Java analogy: implementing HandshakeCompletedListener +
//   SSLEngine.wrap()/unwrap() data routing in NIO.
// ─────────────────────────────────────────────────────────────────────────────
class TLSCallbacksAdapter : public Botan::TLS::Callbacks {
public:
    TLSCallbacksAdapter(BotanP2PCrypto::Callbacks cbs, std::string peerUsername)
        : m_Cbs(std::move(cbs))
        , m_PeerUsername(std::move(peerUsername)) {}

    // Botan wants to send encrypted bytes over the wire.
    // Forward them to the network layer via the OnSendData callback.
    void tls_emit_data(std::span<const uint8_t> data) override {
        if (m_Cbs.OnSendData)
            m_Cbs.OnSendData(std::string_view(
                reinterpret_cast<const char*>(data.data()), data.size()));
    }

    // Botan has decrypted an incoming TLS record.
    // Forward the plaintext message to the application layer.
    void tls_record_received(uint64_t /*seq_no*/,
                              std::span<const uint8_t> data) override {
        if (m_Cbs.OnMessageReceived)
            m_Cbs.OnMessageReceived(std::string_view(
                reinterpret_cast<const char*>(data.data()), data.size()));
    }

    // Botan received a TLS alert from the peer.
    // close_notify means the peer initiated an orderly shutdown.
    void tls_alert(Botan::TLS::Alert alert) override {
        if (alert.type() == Botan::TLS::Alert::CLOSE_NOTIFY)
            m_CloseNotifyReceived = true;
        if (m_Cbs.OnAlert)
            m_Cbs.OnAlert(alert.type_string());
    }

    // TLS handshake completed — the session is now active.
    void tls_session_activated() override {
        m_Activated = true;
        if (m_Cbs.OnConnected)
            m_Cbs.OnConnected();
    }

    // Validate the peer's certificate chain.
    //
    // We use Trust-On-First-Use (TOFU): the first time we see a peer's
    // certificate, we accept it and pin its SHA-256 fingerprint to disk.
    // On subsequent connections we verify the fingerprint matches.
    // A mismatch aborts the handshake — possible MITM attack.
    //
    // Note: we intentionally skip CA validation (no trusted CA list is
    // provided by ClientCredentials), so Botan will call this method
    // even for self-signed certificates.
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
                Botan::TLS::Alert::BAD_CERTIFICATE,
                "Empty certificate chain received from peer");

        const auto& peer_cert         = chain.front();
        const std::string fingerprint = peer_cert.fingerprint("SHA-256");

        auto result = VerifyOrTrustPeerFingerprint(m_PeerUsername, fingerprint);

        switch (result.Status) {
        case PinStatus::Match:
            // Fingerprint matches stored pin — peer is known and trusted.
            break;

        case PinStatus::FirstUse:
            // First connection: pin the certificate (TOFU).
            m_FirstUseTrusted    = true;
            m_FirstUseFingerprint = fingerprint;
            spdlog::info("[P2P-Crypto] TOFU: Pinned certificate for peer '{}': {}",
                         m_PeerUsername, fingerprint);
            break;

        case PinStatus::Mismatch:
            // Fingerprint changed — abort to protect against MITM.
            throw Botan::TLS::TLS_Exception(
                Botan::TLS::Alert::BAD_CERTIFICATE,
                std::format(
                    "Certificate fingerprint mismatch for peer '{}'! "
                    "Stored: {}, received: {}. Possible MITM attack.",
                    m_PeerUsername, result.Fingerprint, fingerprint));
        }
    }

    [[nodiscard]] bool IsActivated()           const { return m_Activated;           }
    [[nodiscard]] bool IsCloseNotifyReceived() const { return m_CloseNotifyReceived; }

private:
    BotanP2PCrypto::Callbacks m_Cbs;
    std::string               m_PeerUsername;
    bool m_Activated            = false;
    bool m_CloseNotifyReceived  = false;
    bool m_FirstUseTrusted      = false;
    std::string m_FirstUseFingerprint;
};

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Pimpl struct (provided)
// ─────────────────────────────────────────────────────────────────────────────
struct BotanP2PCrypto::Impl {
    std::shared_ptr<TLSCallbacksAdapter>                 CallbacksAdapter;
    std::shared_ptr<Botan::AutoSeeded_RNG>               Rng;
    std::shared_ptr<Botan::TLS::Session_Manager_In_Memory> SessionMgr;
    std::shared_ptr<Botan::Credentials_Manager>          Creds;
    std::shared_ptr<PQPolicy>                            Policy;
    std::unique_ptr<Botan::TLS::Channel>                 Channel;
};

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

BotanP2PCrypto::BotanP2PCrypto()  = default;
BotanP2PCrypto::~BotanP2PCrypto() = default;

void BotanP2PCrypto::Start(const Config& config, Callbacks callbacks) {
    auto impl = std::make_unique<Impl>();

    impl->Rng        = std::make_shared<Botan::AutoSeeded_RNG>();
    impl->SessionMgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(impl->Rng);
    impl->Policy     = std::make_shared<PQPolicy>();
    impl->CallbacksAdapter = std::make_shared<TLSCallbacksAdapter>(
        std::move(callbacks), config.PeerUsername);

    if (config.IsResponder) {
        auto km     = GenerateOrLoadCredentials(config.OwnUsername, *impl->Rng);
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
    return m_Impl && m_Impl->CallbacksAdapter &&
           m_Impl->CallbacksAdapter->IsActivated();
}

bool BotanP2PCrypto::IsCloseNotifyReceived() const {
    return m_Impl && m_Impl->CallbacksAdapter &&
           m_Impl->CallbacksAdapter->IsCloseNotifyReceived();
}

bool BotanP2PCrypto::IsClosed() const {
    return !m_Impl || !m_Impl->Channel || m_Impl->Channel->is_closed();
}

// Updated to reflect the implemented algorithms.
std::string BotanP2PCrypto::ProtocolDescription() {
    return "TLS 1.3 | X25519/ML-KEM-768 + ML-DSA (Dilithium-6x5-r3)";
}

} // namespace Safira
