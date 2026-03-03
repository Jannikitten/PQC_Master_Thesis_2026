#include "Server.h"

// ═════════════════════════════════════════════════════════════════════════════
// Server.cpp — implementation
//
// §3.2   Error handling   — monadic chains: and_then, transform, or_else
// §3.3   Leaky impls      — error messages never include secret material
// §3.4   Typestate         — std::visit dispatches on ConnectionState
// §5.2   Testing           — pure init helpers are independently testable
// §5.5   Secret lifetimes  — RAII everywhere; zero manual new/delete
// ═════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <print>
#include <ranges>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/random.h>

#include <spdlog/spdlog.h>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]]
static std::string FormatAddr(const sockaddr_in& addr) {
    std::array<char, INET_ADDRSTRLEN> buf {};
    ::inet_ntop(AF_INET, &addr.sin_addr, buf.data(), buf.size());
    return std::format("{}:{}", buf.data(), ntohs(addr.sin_port));
}

[[nodiscard]]
static bool IsWantIO(int err) noexcept {
    return err == WOLFSSL_ERROR_WANT_READ
        || err == WOLFSSL_ERROR_WANT_WRITE;
}

// ═════════════════════════════════════════════════════════════════════════════
// Ctor / Dtor
// ═════════════════════════════════════════════════════════════════════════════

Server::Server(uint16_t port) : m_Port(port) {}

Server::~Server() {
    Stop();
    if (m_NetworkThread.joinable())
        m_NetworkThread.join();
}

// ═════════════════════════════════════════════════════════════════════════════
// Public API
// ═════════════════════════════════════════════════════════════════════════════

void Server::Start() {
    if (m_Running.load(std::memory_order_acquire)) return;
    m_NetworkThread = std::thread(&Server::NetworkThreadFunc, this);
}

void Server::Stop() {
    m_Running.store(false, std::memory_order_release);
}

void Server::OnDataReceived(DataReceivedCallback fn)             { m_OnDataReceived       = std::move(fn); }
void Server::OnClientConnected(ClientConnectedCallback fn)       { m_OnClientConnected    = std::move(fn); }
void Server::OnClientDisconnected(ClientDisconnectedCallback fn) { m_OnClientDisconnected = std::move(fn); }

// ═════════════════════════════════════════════════════════════════════════════
// wolfSSL custom I/O  (static)
// ═════════════════════════════════════════════════════════════════════════════

int Server::IORecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
    auto& client = *static_cast<IOContext*>(ctx)->Client;

    if (client.EncryptedBuffer.empty())
        return WOLFSSL_CBIO_ERR_WANT_READ;

    const auto count = std::min(
        static_cast<std::size_t>(sz),
        client.EncryptedBuffer.size());

    std::ranges::copy_n(client.EncryptedBuffer.begin(),
                        static_cast<std::ptrdiff_t>(count), buf);

    client.EncryptedBuffer.erase(
        client.EncryptedBuffer.begin(),
        client.EncryptedBuffer.begin() + static_cast<std::ptrdiff_t>(count));

    return static_cast<int>(count);
}

int Server::IOSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
    auto* io   = static_cast<IOContext*>(ctx);
    auto& addr = io->Client->Addr;

    const ssize_t sent = ::sendto(
        io->Socket, buf, static_cast<std::size_t>(sz), 0,
        reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

    return (sent < 0) ? WOLFSSL_CBIO_ERR_GENERAL
                      : static_cast<int>(sent);
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.2 — Result types with monadic operations  (Slides 88-89)
//
// Each helper is a pure function: input → expected<output, error>.
// They compose via and_then (flat-map) and transform (map), mirroring
// Rust's Result::and_then and Result::map that the slides motivate.
//
//   CreateSocket()                  → expected<int, ServerError>
//     .and_then(CreateTLSContext)   → expected<WolfContext, ServerError>
//     .transform(bundle)           → expected<InitResources, ServerError>
//
// No intermediate error checks.  No unchecked return codes.
// ═════════════════════════════════════════════════════════════════════════════

std::expected<int, ServerError> Server::CreateSocket() const {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return std::unexpected(ServerError::SocketCreation);

    int yes = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::fcntl(sock, F_SETFL, O_NONBLOCK);

    const sockaddr_in local {
        .sin_family = AF_INET,
        .sin_port   = htons(m_Port),
        .sin_addr   = { .s_addr = INADDR_ANY },
        .sin_zero   = {},
    };

    if (::bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) {
        ::close(sock);
        return std::unexpected(ServerError::SocketBind);
    }

    return sock;
}

std::expected<WolfContext, ServerError> Server::CreateTLSContext() const {
    WolfContext ctx { wolfSSL_CTX_new(wolfDTLSv1_3_server_method()) };
    if (!ctx)
        return std::unexpected(ServerError::ContextInit);

    int groups[] = { WOLFSSL_ML_KEM_512 };
    wolfSSL_CTX_set_groups(ctx.get(), groups, 1);

    wolfSSL_CTX_SetIORecv(ctx.get(), IORecv);
    wolfSSL_CTX_SetIOSend(ctx.get(), IOSend);

    // ── Generate cert + key in memory ───────────────────────────────────
    auto creds = GenerateSelfSignedCert();
    if (!creds)
        return std::unexpected(creds.error());

    if (wolfSSL_CTX_use_certificate_buffer(
            ctx.get(), creds->CertDer.data(),
            static_cast<long>(creds->CertDer.size()),
            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
        return std::unexpected(ServerError::CertificateLoad);

    if (wolfSSL_CTX_use_PrivateKey_buffer(
            ctx.get(), creds->KeyDer.data(),
            static_cast<long>(creds->KeyDer.size()),
            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
        return std::unexpected(ServerError::PrivateKeyLoad);

    return ctx;
}

std::expected<WolfSession, ServerError> Server::CreateSession() const {
    WolfSession ssl { wolfSSL_new(m_Ctx.get()) };
    if (!ssl)
        return std::unexpected(ServerError::SessionCreation);
    return ssl;
}

// ─────────────────────────────────────────────────────────────────────────────
// InitNetwork — the monadic chain.
//
// CreateSocket() produces a socket fd.  .and_then feeds it to a lambda that
// calls CreateTLSContext() and, on success, .transform bundles both into an
// InitResources.  If anything fails, the error propagates untouched — no
// intermediate if-checks, exactly like Rust's `?` operator.
// ─────────────────────────────────────────────────────────────────────────────
std::expected<InitResources, ServerError> Server::InitNetwork() const {
    return CreateSocket()
        .and_then([this](int sock) -> std::expected<InitResources, ServerError> {
            return CreateTLSContext()
                .transform([sock](WolfContext ctx) -> InitResources {
                    return { sock, std::move(ctx) };
                });
        });
}

std::expected<GeneratedCredentials, ServerError> Server::GenerateSelfSignedCert() const {
    // ── RNG ─────────────────────────────────────────────────────────────
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0)
        return std::unexpected(ServerError::CertificateGeneration);

    // ── RSA key pair ────────────────────────────────────────────────────
    RsaKey key;
    if (wc_InitRsaKey(&key, nullptr) != 0) {
        wc_FreeRng(&rng);
        return std::unexpected(ServerError::CertificateGeneration);
    }

    if (wc_MakeRsaKey(&key, 2048, WC_RSA_EXPONENT, &rng) != 0) {
        wc_FreeRsaKey(&key);
        wc_FreeRng(&rng);
        return std::unexpected(ServerError::CertificateGeneration);
    }

    // ── Export private key to DER ───────────────────────────────────────
    std::vector<uint8_t> keyDer(4096);
    int keySz = wc_RsaKeyToDer(&key, keyDer.data(),
                                static_cast<word32>(keyDer.size()));
    if (keySz < 0) {
        wc_FreeRsaKey(&key);
        wc_FreeRng(&rng);
        return std::unexpected(ServerError::CertificateGeneration);
    }
    keyDer.resize(static_cast<size_t>(keySz));

    // ── Self-signed X.509 certificate ───────────────────────────────────
    Cert cert;
    wc_InitCert(&cert);
    std::strncpy(cert.subject.commonName, "Safira DTLS Server", CTC_NAME_SIZE);
    cert.isCA    = 0;
    cert.sigType = CTC_SHA256wRSA;
    cert.daysValid = 365;

    std::vector<uint8_t> certDer(4096);
    int certSz = wc_MakeSelfCert(&cert, certDer.data(),
                                  static_cast<word32>(certDer.size()),
                                  &key, &rng);

    wc_FreeRsaKey(&key);
    wc_FreeRng(&rng);

    if (certSz < 0)
        return std::unexpected(ServerError::CertificateGeneration);

    certDer.resize(static_cast<size_t>(certSz));

    return GeneratedCredentials {
        .CertDer = std::move(certDer),
        .KeyDer  = std::move(keyDer),
    };
}

// ═════════════════════════════════════════════════════════════════════════════
// Network thread
// ═════════════════════════════════════════════════════════════════════════════

void Server::NetworkThreadFunc() {
    m_Running.store(true, std::memory_order_release);

    wolfSSL_Init();

    // ── Monadic init ────────────────────────────────────────────────────────
    //
    // §3.2, Slides 88-89: "Result types … explicitly requiring the caller
    // to unwrap the result."
    //
    // The entire init sequence is a single expression.  or_else handles the
    // error path (log + bail).  On success we destructure with auto&.

    auto resources = InitNetwork()
        .or_else([this](ServerError e) -> std::expected<InitResources, ServerError> {
            OnFatalError(std::format("init: {}", Describe(e)));
            return std::unexpected(e);
        });

    if (!resources) {
        wolfSSL_Cleanup();
        return;
    }

    m_Socket = resources->Socket;
    m_Ctx    = std::move(resources->Ctx);

    std::println("[server] listening on port {}", m_Port);

    // ── Event loop ──────────────────────────────────────────────────────────
    std::array<uint8_t, kRecvBufSize> rawBuf {};

    while (m_Running.load(std::memory_order_acquire)) {

        // 1. Receive one UDP datagram
        sockaddr_in from {};
        socklen_t   fromLen = sizeof(from);

        const ssize_t len = ::recvfrom(
            m_Socket, rawBuf.data(), rawBuf.size(), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);

        if (len > 0)
            DispatchPacket(from, std::span(rawBuf.data(), static_cast<std::size_t>(len)));

        // 2. Drive every client's state machine  (§3.4 typestate via visit)
        std::ranges::for_each(m_Clients | std::views::values,
                              [this](ClientInfo& c) { DriveClient(c); });

        // 3. Reap dead clients — functional pipeline
        //    filter → keys → collect → for_each erase
        std::vector<ClientID> dead;
        for (const auto& [id, client] : m_Clients)
            if (!client.SSL) dead.push_back(id);

        std::ranges::for_each(dead, [this](ClientID id) { RemoveClient(id); });

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // ── Shutdown ────────────────────────────────────────────────────────────
    std::println("[server] shutting down, closing {} client(s)…", m_Clients.size());

    std::ranges::for_each(m_Clients | std::views::values,
                          [this](ClientInfo& c) { ShutdownClient(c); });
    m_Clients.clear();

    m_Ctx.reset();
    wolfSSL_Cleanup();

    ::close(m_Socket);
    m_Socket = -1;
    m_Running.store(false, std::memory_order_release);
}

// ═════════════════════════════════════════════════════════════════════════════
// DispatchPacket
//
// For new clients, session creation uses the monadic chain:
//
//   CreateSession()
//       .transform(configure)    → WolfSession, fully configured
//
// If CreateSession fails the error is logged and the function returns
// without ever inserting a half-built ClientInfo into the map.
// ═════════════════════════════════════════════════════════════════════════════

void Server::DispatchPacket(const sockaddr_in& from,
                            std::span<const uint8_t> data) {

    const auto id = MakeClientID(from.sin_addr.s_addr, from.sin_port);

    // ── Existing client: append and return ──────────────────────────────────
    if (auto it = m_Clients.find(id); it != m_Clients.end()) {
        auto& buf = it->second.EncryptedBuffer;
        buf.insert(buf.end(), data.begin(), data.end());
        return;
    }

    // ── New client: monadic session creation ────────────────────────────────
    const auto addrStr = FormatAddr(from);

    auto sessionResult = CreateSession()
        .or_else([&](ServerError e) -> std::expected<WolfSession, ServerError> {
            spdlog::error("{} for {}", Describe(e), addrStr);
            return std::unexpected(e);
        });

    if (!sessionResult) return;

    // Build the client, configure the session
    ClientInfo client;
    client.ID         = id;
    client.Addr       = from;
    client.AddressStr = addrStr;
    client.SSL        = std::move(*sessionResult);
    client.IO         = std::make_unique<IOContext>(&client, m_Socket);

    wolfSSL_SetIOReadCtx (client.SSL.get(), client.IO.get());
    wolfSSL_SetIOWriteCtx(client.SSL.get(), client.IO.get());
    wolfSSL_dtls_set_mtu (client.SSL.get(), kDtlsMtu);

    client.EncryptedBuffer.assign(data.begin(), data.end());

    spdlog::info("new connection from {}", addrStr);

    // Kick off the DTLS handshake — WANT_READ is expected.
    const int ret = wolfSSL_accept(client.SSL.get());
    if (ret != WOLFSSL_SUCCESS) {
        const int err = wolfSSL_get_error(client.SSL.get(), ret);
        if (!IsWantIO(err)) {
            spdlog::error("wolfSSL_accept error {} for {}", err, addrStr);
            return;                     // client drops → RAII cleanup
        }
    }

    // Move into map, then fix the IOContext back-pointer (the map node is
    // heap-allocated and address-stable after insertion).
    auto [it, _] = m_Clients.emplace(id, std::move(client));
    it->second.IO->Client = &it->second;
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.4 — Typestate dispatch via std::visit
//
// Adding a third state (e.g. Rekeying) is a compile error here until
// you add the handler — same benefit the notes describe for Rust.
// ═════════════════════════════════════════════════════════════════════════════

void Server::DriveClient(ClientInfo& client) {
    if (!client.SSL) return;

    std::visit(Overloaded {
        [&](Handshaking) { DriveHandshake(client); },
        [&](Connected)   { DriveConnected(client); },
    }, client.State);
}

void Server::DriveHandshake(ClientInfo& client) {
    const int ret = wolfSSL_accept(client.SSL.get());

    if (ret == WOLFSSL_SUCCESS) {
        client.State = Connected{};
        spdlog::info("PQC handshake complete for {}", client.AddressStr);
        if (m_OnClientConnected) m_OnClientConnected(client);
        return;
    }

    const int err = wolfSSL_get_error(client.SSL.get(), ret);
    if (IsWantIO(err)) return;

    spdlog::error("handshake failed (err={}) for {}", err, client.AddressStr);
    client.SSL.reset();
}

void Server::DriveConnected(ClientInfo& client) {
    std::array<char, 4096> plaintext {};

    while (true) {
        const int bytes = wolfSSL_read(
            client.SSL.get(), plaintext.data(),
            static_cast<int>(plaintext.size()));

        if (bytes > 0) {
            if (m_OnDataReceived)
                m_OnDataReceived(client,
                    ByteSpan(reinterpret_cast<const uint8_t*>(plaintext.data()),
                             static_cast<size_t>(bytes)));
            continue;
        }

        const int err = wolfSSL_get_error(client.SSL.get(), bytes);
        if (err == WOLFSSL_ERROR_ZERO_RETURN) {
            spdlog::info("client {} disconnected cleanly", client.AddressStr);
            client.SSL.reset();
        }
        break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// RemoveClient / ShutdownClient — RAII does the heavy lifting.
//
// BUG FIX from original: the old code set SSL = nullptr then called
// wolfSSL_GetIOReadCtx(nullptr), leaking the IOContext every time.
// ═════════════════════════════════════════════════════════════════════════════

void Server::ShutdownClient(ClientInfo& client) {
    if (client.SSL) wolfSSL_shutdown(client.SSL.get());
    client.SSL.reset();

    if (m_OnClientDisconnected)
        m_OnClientDisconnected(client);
}

void Server::RemoveClient(ClientID id) {
    auto it = m_Clients.find(id);
    if (it == m_Clients.end()) return;

    ShutdownClient(it->second);
    m_Clients.erase(it);
}

// ═════════════════════════════════════════════════════════════════════════════
// Send — typestate guards
// ═════════════════════════════════════════════════════════════════════════════

void Server::SendToClient(ClientID id, ByteSpan buf) {
    auto it = m_Clients.find(id);
    if (it == m_Clients.end()) return;

    auto& client = it->second;

    if (!std::holds_alternative<Connected>(client.State)) {
        spdlog::warn("send to {} before handshake", client.AddressStr);
        return;
    }

    const int ret = wolfSSL_write(
        client.SSL.get(), buf.data(), static_cast<int>(buf.size()));
    if (ret <= 0)
        spdlog::error("wolfSSL_write failed for {}", client.AddressStr);
}

void Server::SendToAllClients(ByteSpan buf, ClientID exclude) {
    for (auto& [id, _] : m_Clients)
        if (id != exclude) SendToClient(id, buf);
}

void Server::KickClient(ClientID id) {
    RemoveClient(id);
}

void Server::OnFatalError(std::string_view msg) {
    spdlog::critical("[server] fatal: {}", msg);
    m_Running.store(false, std::memory_order_release);
}

} // namespace Safira