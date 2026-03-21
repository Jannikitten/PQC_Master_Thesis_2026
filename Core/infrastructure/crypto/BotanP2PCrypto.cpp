#include "BotanP2PCrypto.h"

// ═════════════════════════════════════════════════════════════════════════════
// BotanP2PCrypto.cpp — all Botan TLS 1.3 code for P2P chat encryption
//
// This file contains:
//   • PQ TLS 1.3 policy (X25519/ML-KEM-768 hybrid key exchange)
//   • ServerCredentials / ClientCredentials (Botan Credentials_Manager)
//   • P2P credential generation and persistence (RSA-PSS / ML-DSA-65)
//   • TLS callbacks adapter
//   • Peer certificate fingerprint verification (TOFU)
//   • The pimpl Impl struct that owns the TLS channel
// ═════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <sys/stat.h>

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

// ═════════════════════════════════════════════════════════════════════════════
// ConfigureKeyExchange
//
// Programming Task 2
// ------------------
// TBA
//
// Hint: TBA
// ═════════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════════
// Filesystem helpers
// ═════════════════════════════════════════════════════════════════════════════

namespace {

[[nodiscard]] std::filesystem::path GetSafiraDataDir() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = (home && *home)
        ? std::filesystem::path(home) / ".safira"
        : std::filesystem::path(".safira");

    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    ::chmod(base.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
    return base;
}

void HardenFilePermissions(const std::filesystem::path& path) {
    ::chmod(path.string().c_str(), S_IRUSR | S_IWUSR);
}

[[nodiscard]] std::string SanitizeIdentityName(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')
            out.push_back(c);
        else
            out.push_back('_');
    }
    if (out.empty())
        out = "default";
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// Credential types & generation
// ═════════════════════════════════════════════════════════════════════════════

enum class P2PKeyType {
    RSA_PSS,     // Classical — works now
    ML_DSA_65,   // Full post-quantum (Botan 3.6+)
};

struct P2PKeyMaterial {
    std::shared_ptr<Botan::Private_Key>      Key;
    std::shared_ptr<Botan::X509_Certificate> Cert;
};

[[nodiscard]] P2PKeyMaterial GenerateP2PCredentials(
    P2PKeyType type = P2PKeyType::RSA_PSS,
    const std::string& cn = "safira-p2p")
{
    auto rng = std::make_shared<Botan::AutoSeeded_RNG>();

    std::shared_ptr<Botan::Private_Key> key;
    std::string sig_padding;

    switch (type) {
        case P2PKeyType::RSA_PSS:
            key = std::make_shared<Botan::RSA_PrivateKey>(*rng, 2048);
            sig_padding = "SHA-256";
            break;

        case P2PKeyType::ML_DSA_65:
            key = Botan::create_private_key("ML-DSA", *rng, "ML-DSA-65");
            sig_padding = "";
            break;
    }

    Botan::X509_Cert_Options opts(cn, 3650 * 24 * 60 * 60);

    auto cert = std::make_shared<Botan::X509_Certificate>(
        Botan::X509::create_self_signed_cert(opts, *key, sig_padding, *rng));

    return { key, cert };
}

[[nodiscard]] P2PKeyMaterial GenerateOrLoadP2PCredentials(
    std::string_view identityName,
    P2PKeyType type = P2PKeyType::RSA_PSS) {
    const auto safeName = SanitizeIdentityName(identityName);
    const std::filesystem::path dir = GetSafiraDataDir() / "p2p_identities";
    const std::filesystem::path keyPath = dir / (safeName + ".key.pem");
    const std::filesystem::path certPath = dir / (safeName + ".cert.pem");

    std::filesystem::create_directories(dir);
    ::chmod(dir.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);

    if (std::filesystem::exists(keyPath) && std::filesystem::exists(certPath)) {
        try {
            Botan::DataSource_Stream keySrc(keyPath.string(), true);
            auto keyUnique = Botan::PKCS8::load_key(keySrc);
            auto cert = std::make_shared<Botan::X509_Certificate>(certPath.string());
            if (keyUnique && cert) {
                auto key = std::shared_ptr<Botan::Private_Key>(std::move(keyUnique));
                return { key, cert };
            }
        } catch (...) {
            // Fall through to regeneration below.
        }
    }

    auto material = GenerateP2PCredentials(type, safeName);

    {
        std::ofstream keyOut(keyPath, std::ios::trunc);
        keyOut << Botan::PKCS8::PEM_encode(*material.Key);
        keyOut.flush();
    }
    HardenFilePermissions(keyPath);

    {
        std::ofstream certOut(certPath, std::ios::trunc);
        certOut << material.Cert->PEM_encode();
        certOut.flush();
    }
    HardenFilePermissions(certPath);

    return material;
}

// ═════════════════════════════════════════════════════════════════════════════
// Peer certificate fingerprint verification (TOFU)
// ═════════════════════════════════════════════════════════════════════════════

enum class PinVerificationStatus : uint8_t {
    Match,
    TrustedFirstUse,
    Mismatch,
};

struct PinVerificationResult {
    PinVerificationStatus Status = PinVerificationStatus::Mismatch;
    std::string Fingerprint;
};

[[nodiscard]] std::filesystem::path GetPeerPinStorePath() {
    return GetSafiraDataDir() / "KnownPeerFingerprints.txt";
}

[[nodiscard]] std::unordered_map<std::string, std::string> LoadPeerPins() {
    std::unordered_map<std::string, std::string> pins;
    std::ifstream in(GetPeerPinStorePath());
    if (!in) return pins;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string user, fp;
        if (!(iss >> user >> fp)) continue;
        pins[user] = fp;
    }
    return pins;
}

void SavePeerPins(const std::unordered_map<std::string, std::string>& pins) {
    const auto path = GetPeerPinStorePath();
    std::ofstream out(path, std::ios::trunc);
    for (const auto& [user, fp] : pins)
        out << user << ' ' << fp << '\n';
    out.flush();
    HardenFilePermissions(path);
}

[[nodiscard]] PinVerificationResult VerifyOrTrustPeerFingerprint(
    const std::string& peerUsername,
    const std::string& fingerprint)
{
    static std::mutex s_PinMutex;
    std::lock_guard lock(s_PinMutex);

    auto pins = LoadPeerPins();
    if (const auto it = pins.find(peerUsername); it != pins.end()) {
        return {
            .Status = (it->second == fingerprint)
                ? PinVerificationStatus::Match
                : PinVerificationStatus::Mismatch,
            .Fingerprint = it->second,
        };
    }

    pins[peerUsername] = fingerprint;
    SavePeerPins(pins);
    return { .Status = PinVerificationStatus::TrustedFirstUse, .Fingerprint = fingerprint };
}

// ═════════════════════════════════════════════════════════════════════════════
// PQ TLS 1.3 Policy
// ═════════════════════════════════════════════════════════════════════════════

class PQPolicy : public Botan::TLS::Default_Policy {
public:
    [[nodiscard]] Botan::TLS::Protocol_Version min_version() const {
        return Botan::TLS::Protocol_Version::TLS_V13;
    }

    [[nodiscard]] std::vector<Botan::TLS::Group_Params>
    key_exchange_groups() const override {
        return { Botan::TLS::Group_Params::HYBRID_X25519_ML_KEM_768 };
    }

    [[nodiscard]] std::vector<Botan::TLS::Group_Params>
    key_exchange_groups_to_offer() const override {
        return { Botan::TLS::Group_Params::HYBRID_X25519_ML_KEM_768 };
    }

    [[nodiscard]] bool require_cert_revocation_info() const override { return false; }

    [[nodiscard]] std::vector<Botan::TLS::Signature_Scheme>
    allowed_signature_schemes() const override {
        return {
            Botan::TLS::Signature_Scheme::RSA_PSS_SHA256,
            Botan::TLS::Signature_Scheme::RSA_PSS_SHA384,
            Botan::TLS::Signature_Scheme::RSA_PSS_SHA512,
            Botan::TLS::Signature_Scheme::ECDSA_SHA256,
            Botan::TLS::Signature_Scheme::ECDSA_SHA384,
            Botan::TLS::Signature_Scheme::ECDSA_SHA512,
            Botan::TLS::Signature_Scheme::RSA_PKCS1_SHA256,
            Botan::TLS::Signature_Scheme::RSA_PKCS1_SHA384,
            Botan::TLS::Signature_Scheme::RSA_PKCS1_SHA512,
        };
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// ServerCredentials / ClientCredentials
// ═════════════════════════════════════════════════════════════════════════════

class ServerCredentials : public Botan::Credentials_Manager {
public:
    explicit ServerCredentials(const P2PKeyMaterial& km)
        : m_Key(km.Key), m_Cert(km.Cert) {}

    std::vector<Botan::X509_Certificate> find_cert_chain(
        const std::vector<std::string>& cert_key_types,
        const std::vector<Botan::AlgorithmIdentifier>&,
        const std::vector<Botan::X509_DN>&,
        const std::string& type,
        const std::string&) override
    {
        if (type == "tls-server") {
            const std::string our_key_type = m_Key->algo_name();
            if (cert_key_types.empty()
                || std::ranges::find(cert_key_types, our_key_type) != cert_key_types.end())
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
    std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(
        const std::string&, const std::string&) override
    {
        return {};
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// TLS Callbacks adapter
// ═════════════════════════════════════════════════════════════════════════════

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
        if (m_Cbs.MessageReceived)
            m_Cbs.MessageReceived(
                m_PeerUsername,
                std::string(reinterpret_cast<const char*>(data.data()), data.size()),
                0xFFFFFFFF);
    }

    void tls_alert(Botan::TLS::Alert alert) override {
        if (alert.type() == Botan::TLS::Alert::CloseNotify) {
            if (m_Cbs.Closed)
                m_Cbs.Closed(std::format("{} closed the connection.", m_PeerUsername));
            m_CloseNotifyReceived = true;
        } else {
            spdlog::warn("[P2P-Crypto] TLS alert: {}", alert.type_string());
        }
    }

    void tls_session_activated() override {
        spdlog::info("[P2P-Crypto] TLS 1.3 + X25519/ML-KEM-768 handshake complete with {}",
                     m_PeerUsername);
        if (m_Cbs.Activated)
            m_Cbs.Activated();
        if (m_FirstUseTrusted && m_Cbs.MessageReceived) {
            m_Cbs.MessageReceived("System",
                std::format("First trusted fingerprint for {}: {}", m_PeerUsername, m_FirstUseFingerprint),
                0xFFCCAA66);
        }
        m_Activated = true;
    }

    void tls_verify_cert_chain(
        const std::vector<Botan::X509_Certificate>& cert_chain,
        const std::vector<std::optional<Botan::OCSP::Response>>&,
        const std::vector<Botan::Certificate_Store*>&,
        Botan::Usage_Type,
        std::string_view,
        const Botan::TLS::Policy&) override
    {
        if (cert_chain.empty()) {
            spdlog::info("[P2P-Crypto] Peer {} presented no client certificate — skipping verification",
                         m_PeerUsername);
            return;
        }

        const std::string expectedCN = SanitizeIdentityName(m_PeerUsername);
        const std::string certCN = cert_chain.front().subject_dn().get_first_attribute("X520.CommonName");
        if (certCN.empty())
            throw std::runtime_error("peer certificate missing common name");
        if (certCN != expectedCN)
            throw std::runtime_error(
                std::format("peer identity mismatch (expected '{}', got '{}')", expectedCN, certCN));

        const std::string fp = cert_chain.front().fingerprint("SHA-256");
        const auto pinResult = VerifyOrTrustPeerFingerprint(m_PeerUsername, fp);
        if (pinResult.Status == PinVerificationStatus::Mismatch)
            throw std::runtime_error("peer certificate fingerprint mismatch");
        if (pinResult.Status == PinVerificationStatus::TrustedFirstUse) {
            m_FirstUseTrusted     = true;
            m_FirstUseFingerprint = fp;
            spdlog::warn("[P2P-Crypto] First-use trust for {} fingerprint {}", m_PeerUsername, fp);
        }
    }

    [[nodiscard]] bool IsActivated()           const noexcept { return m_Activated; }
    [[nodiscard]] bool IsCloseNotifyReceived() const noexcept { return m_CloseNotifyReceived; }

private:
    BotanP2PCrypto::Callbacks m_Cbs;
    std::string               m_PeerUsername;
    bool                      m_Activated           = false;
    bool                      m_CloseNotifyReceived = false;
    bool                      m_FirstUseTrusted     = false;
    std::string               m_FirstUseFingerprint;
};

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
// BotanP2PCrypto::Impl — pimpl holding all Botan objects
// ═════════════════════════════════════════════════════════════════════════════

struct BotanP2PCrypto::Impl {
    std::shared_ptr<TLSCallbacksAdapter>                    CallbacksAdapter;
    std::shared_ptr<Botan::AutoSeeded_RNG>                  Rng;
    std::shared_ptr<Botan::TLS::Session_Manager_In_Memory>  SessionMgr;
    std::shared_ptr<Botan::Credentials_Manager>             Creds;
    std::shared_ptr<PQPolicy>                               Policy;
    std::unique_ptr<Botan::TLS::Channel>                    Channel;
};

// ═════════════════════════════════════════════════════════════════════════════
// BotanP2PCrypto public API
// ═════════════════════════════════════════════════════════════════════════════

BotanP2PCrypto::BotanP2PCrypto()  = default;
BotanP2PCrypto::~BotanP2PCrypto() = default;

void BotanP2PCrypto::Start(const Config& config, Callbacks callbacks) {
    m_Impl = std::make_unique<Impl>();

    m_Impl->Rng        = std::make_shared<Botan::AutoSeeded_RNG>();
    m_Impl->SessionMgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(m_Impl->Rng);
    m_Impl->Policy     = std::make_shared<PQPolicy>();
    m_Impl->CallbacksAdapter = std::make_shared<TLSCallbacksAdapter>(
        std::move(callbacks), config.PeerUsername);

    if (config.IsResponder) {
        auto km = GenerateOrLoadP2PCredentials(config.OwnUsername, P2PKeyType::RSA_PSS);
        m_Impl->Creds = std::make_shared<ServerCredentials>(km);

        m_Impl->Channel = std::make_unique<Botan::TLS::Server>(
            m_Impl->CallbacksAdapter,
            m_Impl->SessionMgr,
            m_Impl->Creds,
            m_Impl->Policy,
            m_Impl->Rng,
            false);
    } else {
        m_Impl->Creds = std::make_shared<ClientCredentials>();

        m_Impl->Channel = std::make_unique<Botan::TLS::Client>(
            m_Impl->CallbacksAdapter,
            m_Impl->SessionMgr,
            m_Impl->Creds,
            m_Impl->Policy,
            m_Impl->Rng,
            Botan::TLS::Server_Information("localhost", 0));
    }
}

void BotanP2PCrypto::FeedReceivedData(std::span<const uint8_t> data) {
    if (m_Impl && m_Impl->Channel)
        m_Impl->Channel->received_data(data);
}

void BotanP2PCrypto::Send(std::string_view message) {
    if (m_Impl && m_Impl->Channel)
        m_Impl->Channel->send(
            reinterpret_cast<const uint8_t*>(message.data()), message.size());
}

void BotanP2PCrypto::Close() {
    if (m_Impl && m_Impl->Channel && !m_Impl->Channel->is_closed()) {
        try { m_Impl->Channel->close(); } catch (...) {}
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

std::string BotanP2PCrypto::ProtocolDescription() {
    return "TLS 1.3 | X25519/ML-KEM-768";
}

} // namespace Safira
