#include "PrivateChatSession.h"

// ═════════════════════════════════════════════════════════════════════════════
// PrivateChatSession.cpp
//
// §3.2   Result types     – std::expected for socket creation pipelines
// §5.5   Secret lifetimes – UniqueSocket RAII for file descriptors
// §5.5   Opinionated API  – string_view parameters, no throwing port parse
// C++23                   – std::array, from_chars, ranges::find, string_view
// ═════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <botan/auto_rng.h>
#include <botan/certstor.h>
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

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace Safira {

namespace {
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
    if (!in)
        return pins;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string user;
        std::string fp;
        if (!(iss >> user >> fp))
            continue;
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

[[nodiscard]] PinVerificationResult VerifyOrTrustPeerFingerprint(const std::string& peerUsername,
                                                                 const std::string& fingerprint) {
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
} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// UniqueSocket
// ═════════════════════════════════════════════════════════════════════════════

void UniqueSocket::Reset() noexcept {
    if (m_Fd >= 0) {
        ::close(m_Fd);
        m_Fd = -1;
    }
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
            // Phase 2 TODO: Botan::TLS::Signature_Scheme::ML_DSA_65,
            Botan::TLS::Signature_Scheme::RSA_PKCS1_SHA256,
            Botan::TLS::Signature_Scheme::RSA_PKCS1_SHA384,
            Botan::TLS::Signature_Scheme::RSA_PKCS1_SHA512,
        };
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// ServerCredentials
//
// Botan 3 API: cert_chain() → find_cert_chain(),
//              Private_Key* → shared_ptr<Private_Key>
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

// ═════════════════════════════════════════════════════════════════════════════
// ClientCredentials — Phase 1: no cert verification
// ═════════════════════════════════════════════════════════════════════════════

class ClientCredentials : public Botan::Credentials_Manager {
public:
    std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(
        const std::string&, const std::string&) override
    {
        return {};
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// P2PCallbacks — Botan TLS channel callbacks
//
// Defined inside namespace Safira to match the forward-declaration in the
// header.
// ═════════════════════════════════════════════════════════════════════════════

class P2PCallbacks : public Botan::TLS::Callbacks {
public:
    P2PCallbacks(int socket, PrivateChatSession& owner, std::string peerUsername)
        : m_Socket(socket)
        , m_Owner(owner)
        , m_PeerUsername(std::move(peerUsername)) {}

    void tls_emit_data(std::span<const uint8_t> data) override {
        std::size_t total = 0;
        while (total < data.size()) {
            const ssize_t sent = ::send(
                m_Socket,
                data.data() + total,
                data.size() - total, 0);
            if (sent <= 0) {
                spdlog::error("[P2P] tls_emit_data: send() failed");
                return;
            }
            total += static_cast<std::size_t>(sent);
        }
    }

    void tls_record_received(uint64_t /*seq_no*/,
                             std::span<const uint8_t> data) override {
        m_Owner.AppendMessage(
            m_PeerUsername,
            std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    }

    void tls_alert(Botan::TLS::Alert alert) override {
        if (alert.type() == Botan::TLS::Alert::CloseNotify) {
            m_Owner.AppendMessage("System",
                std::format("{} closed the connection.", m_PeerUsername),
                0xFF888888);
            m_CloseNotifyReceived = true;
        } else {
            spdlog::warn("[P2P] TLS alert: {}", alert.type_string());
        }
    }

    void tls_session_activated() override {
        spdlog::info("[P2P] TLS 1.3 + X25519/ML-KEM-768 handshake complete with {}",
                     m_PeerUsername);
        m_Owner.AppendMessage("System",
            std::format("Connected to {} (Botan TLS 1.3 | X25519/ML-KEM-768)", m_PeerUsername),
            0xFF66CC66);
        if (m_FirstUseTrusted) {
            m_Owner.AppendMessage("System",
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
        if (cert_chain.empty())
            throw std::runtime_error("empty peer certificate chain");

        const std::string expectedCN = SanitizeIdentityName(m_PeerUsername);
        const std::string certCN = cert_chain.front().subject_dn().get_first_attribute("X520.CommonName");
        if (certCN.empty())
            throw std::runtime_error("peer certificate missing common name");
        if (certCN != expectedCN) {
            throw std::runtime_error(
                std::format("peer identity mismatch (expected '{}', got '{}')", expectedCN, certCN));
        }

        const std::string fp = cert_chain.front().fingerprint("SHA-256");
        const auto pinResult = VerifyOrTrustPeerFingerprint(m_PeerUsername, fp);
        if (pinResult.Status == PinVerificationStatus::Mismatch)
            throw std::runtime_error("peer certificate fingerprint mismatch");
        if (pinResult.Status == PinVerificationStatus::TrustedFirstUse) {
            m_FirstUseTrusted = true;
            m_FirstUseFingerprint = fp;
            spdlog::warn("[P2P] First-use trust for {} fingerprint {}", m_PeerUsername, fp);
        }
    }

    [[nodiscard]] bool IsActivated()           const noexcept { return m_Activated; }
    [[nodiscard]] bool IsCloseNotifyReceived() const noexcept { return m_CloseNotifyReceived; }

private:
    int                 m_Socket;
    PrivateChatSession& m_Owner;
    std::string         m_PeerUsername;
    bool                m_Activated           = false;
    bool                m_CloseNotifyReceived = false;
    bool                m_FirstUseTrusted     = false;
    std::string         m_FirstUseFingerprint;
};

// ─────────────────────────────────────────────────────────────────────────────
// Address parsing — from_chars instead of std::stoi (no-throw, §3.2)
// ─────────────────────────────────────────────────────────────────────────────

struct ParsedAddr {
    std::string Host;
    uint16_t    Port = 0;
};

[[nodiscard]]
static std::expected<ParsedAddr, P2PError> ParsePeerAddress(std::string_view addr) {
    const auto colon = addr.rfind(':');
    if (colon == std::string_view::npos)
        return std::unexpected(P2PError::AddressParse);

    auto host    = std::string(addr.substr(0, colon));
    auto portStr = addr.substr(colon + 1);

    uint16_t port = 0;
    auto [ptr, ec] = std::from_chars(portStr.data(), portStr.data() + portStr.size(), port);
    if (ec != std::errc{} || port == 0)
        return std::unexpected(P2PError::AddressParse);

    return ParsedAddr{ std::move(host), port };
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.2 — Socket init helpers returning std::expected  (Slides 88-89)
// ═════════════════════════════════════════════════════════════════════════════

std::expected<UniqueSocket, P2PError>
PrivateChatSession::CreateListenSocket() {
    UniqueSocket sock{ ::socket(AF_INET, SOCK_STREAM, 0) };
    if (!sock)
        return std::unexpected(P2PError::SocketCreation);

    int yes = 1;
    ::setsockopt(sock.Get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in local{
        .sin_family = AF_INET,
        .sin_port   = 0,           // OS picks an ephemeral port
        .sin_addr   = { .s_addr = INADDR_ANY },
        .sin_zero   = {},
    };

    if (::bind(sock.Get(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0)
        return std::unexpected(P2PError::SocketBind);

    if (::listen(sock.Get(), 1) < 0)
        return std::unexpected(P2PError::Listen);

    return sock;
}

std::expected<UniqueSocket, P2PError>
PrivateChatSession::CreateAndConnectSocket(std::string_view ip, uint16_t port) {
    UniqueSocket sock{ ::socket(AF_INET, SOCK_STREAM, 0) };
    if (!sock)
        return std::unexpected(P2PError::SocketCreation);

    sockaddr_in server{
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = {},
        .sin_zero   = {},
    };
    if (::inet_pton(AF_INET, std::string(ip).c_str(), &server.sin_addr) != 1)
        return std::unexpected(P2PError::AddressParse);

    if (::connect(sock.Get(), reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0)
        return std::unexpected(P2PError::TcpConnect);

    return sock;
}

// ═════════════════════════════════════════════════════════════════════════════
// Ctor / Dtor
// ═════════════════════════════════════════════════════════════════════════════

PrivateChatSession::PrivateChatSession(std::string own, std::string peer)
    : m_PeerUsername(std::move(peer))
    , m_OwnUsername(std::move(own)) {}

PrivateChatSession::~PrivateChatSession() { Close(); }

// ═════════════════════════════════════════════════════════════════════════════
// StartAsResponder
//
// Uses the std::expected init helper.  On failure the UniqueSocket inside
// the expected is destroyed, closing the fd — no leak.
// ═════════════════════════════════════════════════════════════════════════════

uint16_t PrivateChatSession::StartAsResponder(P2PKeyType keyType) {
    if (m_Running.load(std::memory_order_acquire)) {
        spdlog::warn("[P2P] StartAsResponder called while already running");
        return 0;
    }
    m_NetworkExecutor.Stop();

    auto listenResult = CreateListenSocket();
    if (!listenResult) {
        spdlog::error("[P2P] Responder: {}", Describe(listenResult.error()));
        return 0;
    }

    // Query the OS-assigned port.
    sockaddr_in assigned{};
    socklen_t len = sizeof(assigned);
    ::getsockname(listenResult->Get(), reinterpret_cast<sockaddr*>(&assigned), &len);
    const uint16_t port = ntohs(assigned.sin_port);

    // Transfer listen socket ownership to the session.
    m_Socket = std::move(*listenResult);

    spdlog::info("[P2P] Responder (TCP) listening on port {}", port);
    AppendMessage("System",
        std::format("Waiting for {} — TCP port {} (Botan TLS 1.3)...", m_PeerUsername, port),
        0xFF888888);

    m_Running.store(true, std::memory_order_release);
    if (!m_NetworkExecutor.Start("p2p-session")) {
        spdlog::error("[P2P] failed to start network executor");
        m_Running.store(false, std::memory_order_release);
        m_Socket.Reset();
        return 0;
    }
    if (!m_NetworkExecutor.Post([this, keyType] { ResponderThreadFunc(keyType); })) {
        spdlog::error("[P2P] failed to schedule responder loop");
        m_Running.store(false, std::memory_order_release);
        m_Socket.Reset();
        m_NetworkExecutor.Stop();
        return 0;
    }
    return port;
}

// ═════════════════════════════════════════════════════════════════════════════
// StartAsInitiator
// ═════════════════════════════════════════════════════════════════════════════

void PrivateChatSession::StartAsInitiator(std::string_view peerAddress) {
    if (m_Running.load(std::memory_order_acquire)) {
        spdlog::warn("[P2P] StartAsInitiator called while already running");
        return;
    }
    m_NetworkExecutor.Stop();

    m_Running.store(true, std::memory_order_release);
    if (!m_NetworkExecutor.Start("p2p-session")) {
        spdlog::error("[P2P] failed to start network executor");
        m_Running.store(false, std::memory_order_release);
        return;
    }
    if (!m_NetworkExecutor.Post([this, addr = std::string(peerAddress)] {
        InitiatorThreadFunc(addr);
    })) {
        spdlog::error("[P2P] failed to schedule initiator loop");
        m_Running.store(false, std::memory_order_release);
        m_NetworkExecutor.Stop();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Close
// ═════════════════════════════════════════════════════════════════════════════

void PrivateChatSession::Close() {
    m_Running.store(false, std::memory_order_release);
    m_Socket.Reset();
    m_NetworkExecutor.Stop();
    m_Connected.store(false, std::memory_order_release);
}

// ═════════════════════════════════════════════════════════════════════════════
// Send
// ═════════════════════════════════════════════════════════════════════════════

void PrivateChatSession::Send(std::string_view message) {
    if (!m_Connected.load(std::memory_order_acquire)) {
        spdlog::warn("[P2P] Send before handshake complete");
        return;
    }
    std::lock_guard lock(m_LogMutex);
    m_PendingOutbound.emplace_back(message);
}

// ═════════════════════════════════════════════════════════════════════════════
// ResponderThreadFunc
// ═════════════════════════════════════════════════════════════════════════════

void PrivateChatSession::ResponderThreadFunc(P2PKeyType keyType) {
    const int listenFd = m_Socket.Get();
    if (listenFd < 0) {
        m_Running.store(false, std::memory_order_release);
        return;
    }
    ::fcntl(listenFd, F_SETFL, O_NONBLOCK);

    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    int connFd = -1;

    while (m_Running.load(std::memory_order_acquire)) {
        connFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (connFd >= 0)
            break;

        const int err = errno;
        if (err == EWOULDBLOCK || err == EAGAIN || err == EINTR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (err != EBADF && err != EINVAL)
            spdlog::error("[P2P] accept() failed (errno={})", err);
        break;
    }

    if (connFd < 0 || !m_Running.load(std::memory_order_acquire)) {
        m_Running.store(false, std::memory_order_release);
        return;
    }

    m_Socket.Reset();
    m_Socket = UniqueSocket(connFd);

    std::array<char, INET_ADDRSTRLEN> peerIp{};
    ::inet_ntop(AF_INET, &peer.sin_addr, peerIp.data(), peerIp.size());
    spdlog::info("[P2P] TCP accepted from {}:{}", peerIp.data(), ntohs(peer.sin_port));

    try {
        auto km          = GenerateOrLoadP2PCredentials(m_OwnUsername, keyType);
        auto rng         = std::make_shared<Botan::AutoSeeded_RNG>();
        auto callbacks   = std::make_shared<P2PCallbacks>(m_Socket.Get(), *this, m_PeerUsername);
        auto session_mgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(rng);
        auto creds       = std::make_shared<ServerCredentials>(km);
        auto policy      = std::make_shared<PQPolicy>();

        Botan::TLS::Server channel(callbacks, session_mgr, creds, policy, rng, false);
        RunLoop(&channel, callbacks.get());
    } catch (const std::exception& ex) {
        spdlog::error("[P2P] Responder: {}", ex.what());
        AppendMessage("System", std::format("Error: {}", ex.what()), 0xFF4444FF);
    }

    m_Connected.store(false, std::memory_order_release);
    m_Running.store(false, std::memory_order_release);
}

// ═════════════════════════════════════════════════════════════════════════════
// InitiatorThreadFunc
//
// Uses ParsePeerAddress (from_chars, §3.2) and CreateAndConnectSocket
// (std::expected, RAII).
// ═════════════════════════════════════════════════════════════════════════════

void PrivateChatSession::InitiatorThreadFunc(std::string peerAddress) {
    auto parsed = ParsePeerAddress(peerAddress);
    if (!parsed) {
        spdlog::error("[P2P] {}: '{}'", Describe(parsed.error()), peerAddress);
        m_Running.store(false, std::memory_order_release);
        return;
    }

    AppendMessage("System",
        std::format("Connecting to {} at {}:{} (Botan TLS 1.3)...",
                    m_PeerUsername, parsed->Host, parsed->Port),
        0xFF888888);

    auto sockResult = CreateAndConnectSocket(parsed->Host, parsed->Port);
    if (!sockResult) {
        spdlog::error("[P2P] TCP connect to {} failed", peerAddress);
        AppendMessage("System", "TCP connect failed.", 0xFF4444FF);
        m_Running.store(false, std::memory_order_release);
        return;
    }

    m_Socket = std::move(*sockResult);

    try {
        auto rng         = std::make_shared<Botan::AutoSeeded_RNG>();
        auto callbacks   = std::make_shared<P2PCallbacks>(m_Socket.Get(), *this, m_PeerUsername);
        auto session_mgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(rng);
        auto creds       = std::make_shared<ClientCredentials>();
        auto policy      = std::make_shared<PQPolicy>();

        Botan::TLS::Client channel(callbacks, session_mgr, creds, policy, rng,
                                   Botan::TLS::Server_Information(parsed->Host, parsed->Port));
        RunLoop(&channel, callbacks.get());
    } catch (const std::exception& ex) {
        spdlog::error("[P2P] Initiator: {}", ex.what());
        AppendMessage("System", std::format("Error: {}", ex.what()), 0xFF4444FF);
    }

    m_Connected.store(false, std::memory_order_release);
    m_Running.store(false, std::memory_order_release);
}

// ═════════════════════════════════════════════════════════════════════════════
// RunLoop
// ═════════════════════════════════════════════════════════════════════════════

void PrivateChatSession::RunLoop(Botan::TLS::Channel* channel,
                                 P2PCallbacks* callbacks) {
    std::array<uint8_t, 16384> recvBuf{};

    ::fcntl(m_Socket.Get(), F_SETFL, O_NONBLOCK);

    while (m_Running.load(std::memory_order_acquire) && !channel->is_closed()) {

        const ssize_t len = ::recv(m_Socket.Get(), recvBuf.data(), recvBuf.size(), 0);
        if (len > 0) {
            channel->received_data(
                std::span<const uint8_t>(recvBuf.data(), static_cast<std::size_t>(len)));
        } else if (len == 0) {
            AppendMessage("System",
                std::format("{} disconnected.", m_PeerUsername), 0xFF888888);
            break;
        } else {
            const int err = errno;
            if (err == EWOULDBLOCK || err == EAGAIN || err == EINTR) {
                // no data this tick
            } else if (err == ECONNRESET || err == ENOTCONN || err == EPIPE) {
                AppendMessage("System",
                    std::format("{} disconnected.", m_PeerUsername), 0xFF888888);
                break;
            } else if ((err == EBADF || err == ENOTSOCK) &&
                       !m_Running.load(std::memory_order_acquire)) {
                break;
            } else {
                spdlog::warn("[P2P] recv() failed (errno={})", err);
                break;
            }
        }

        if (!m_Connected.load(std::memory_order_acquire) && callbacks->IsActivated())
            m_Connected.store(true, std::memory_order_release);

        // Drain outbound queue under the lock, then send outside it.
        if (m_Connected.load(std::memory_order_acquire)) {
            std::vector<std::string> pending;
            { std::lock_guard lock(m_LogMutex); pending.swap(m_PendingOutbound); }
            for (const auto& msg : pending)
                channel->send(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        }

        if (callbacks->IsCloseNotifyReceived()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Send close_notify while the socket is still alive.
    if (m_Socket && !channel->is_closed()) {
        try { channel->close(); } catch (...) {}
    }

    // Socket closed by RAII or explicitly by the caller (Close / dtor).
    // We reset it here so the thread doesn't leave a dangling fd for
    // Close() to double-close.
    m_Socket.Reset();
    m_Connected.store(false, std::memory_order_release);
}

// ═════════════════════════════════════════════════════════════════════════════
// AppendMessage
// ═════════════════════════════════════════════════════════════════════════════

void PrivateChatSession::AppendMessage(const std::string& who,
                                       const std::string& text,
                                       uint32_t color) {
    std::lock_guard lock(m_LogMutex);
    m_Log.push_back({ who, text, color });
    m_ScrollToBottom = true;
}

std::vector<ChatEntry>* PrivateChatSession::RefreshAndGetChatEntries(const std::string& ownUsername) {
    std::lock_guard lock(m_LogMutex);
    m_CachedEntries = BuildChatEntries(ownUsername);
    return &m_CachedEntries;
}

// ═════════════════════════════════════════════════════════════════════════════
// OnUIRender
// ═════════════════════════════════════════════════════════════════════════════

std::vector<ChatEntry> PrivateChatSession::BuildChatEntries(const std::string& ownUsername) const {
    // Must be called under m_LogMutex.
    std::vector<ChatEntry> entries;
    entries.reserve(m_Log.size());

    for (const auto& e : m_Log) {
        MessageRole role;
        if (e.Who == "System")
            role = MessageRole::System;
        else if (e.Who == ownUsername)
            role = MessageRole::Own;
        else
            role = MessageRole::Peer;

        entries.push_back({
            .Who   = e.Who,
            .Text  = e.Text,
            .Color = e.Color,
            .Role  = role,
            .Time  = {},
        });
    }
    return entries;
}

bool PrivateChatSession::OnUIRender(const std::string& ownUsername, uint32_t /*ownColor*/) {
    if (!m_WindowOpen) return false;

    const std::string title = std::format(
        "Private chat — {}###pc_{}", m_PeerUsername, m_PeerUsername);

    ImGui::SetNextWindowSize({ 520, 440 }, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &m_WindowOpen)) {
        ImGui::End();
        return m_WindowOpen;
    }

    // Snapshot the log into ChatEntry format under the lock.
    {
        std::lock_guard lock(m_LogMutex);
        m_CachedEntries = BuildChatEntries(ownUsername);
    }

    const bool connected   = m_Connected.load(std::memory_order_acquire);
    const bool handshaking = m_Running.load(std::memory_order_acquire) && !connected;

    // Render status bar + message bubbles + input bar.
    m_ChatPanel.RenderChatArea(
        m_CachedEntries, ownUsername, m_PeerUsername,
        connected, handshaking);

    // If the user submitted a message via the input bar, send it.
    if (auto msg = m_ChatPanel.ConsumePendingMessage()) {
        Send(*msg);
        AppendMessage(ownUsername, *msg, 0xFFFFFFFF);
    }

    ImGui::End();
    return m_WindowOpen;
}

} // namespace Safira
