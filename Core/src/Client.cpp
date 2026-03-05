#include "Client.h"

// ═════════════════════════════════════════════════════════════════════════════
// Client.cpp
//
// §3.2   Monadic init    – ParseAddress → ResolveHost → CreateSocket
//                          → CreateTLSContext → CreateSession, all composed
//                          via and_then / transform.
// §3.4   Typestate        – variant<Handshaking, Connected> dispatched via visit
// §5.5   Secret lifetimes – WolfContext / WolfSession freed by RAII
// ═════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <print>
#include <ranges>
#include <thread>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace Safira {

namespace {
constexpr const char* kTrustedServerCertPath = "server-cert.der";
constexpr size_t kMaxPendingSendQueue = 512;

[[nodiscard]] std::filesystem::path GetSafiraDataDir() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = (home && *home)
        ? std::filesystem::path(home) / ".safira"
        : std::filesystem::path(".safira");
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

[[nodiscard]] std::expected<std::vector<uint8_t>, ClientError>
ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::unexpected(ClientError::TrustStoreLoad);

    in.seekg(0, std::ios::end);
    const std::streamsize sz = in.tellg();
    if (sz <= 0)
        return std::unexpected(ClientError::TrustStoreLoad);
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> out(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(out.data()), sz))
        return std::unexpected(ClientError::TrustStoreLoad);

    return out;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]]
static bool IsWantIO(int err) noexcept {
    return err == WOLFSSL_ERROR_WANT_READ
        || err == WOLFSSL_ERROR_WANT_WRITE;
}

// ═════════════════════════════════════════════════════════════════════════════
// Ctor / Dtor
// ═════════════════════════════════════════════════════════════════════════════

Client::~Client() { Disconnect(); }

// ═════════════════════════════════════════════════════════════════════════════
// Public API
// ═════════════════════════════════════════════════════════════════════════════

void Client::ConnectToServer(std::string_view address) {
    if (m_Running.load(std::memory_order_acquire))
        return;

    m_NetworkExecutor.Stop();
    m_ServerAddress = std::string(address);
    if (!m_NetworkExecutor.Start("client-dtls")) {
        OnFatalError("failed to start client network executor");
        return;
    }

    if (!m_NetworkExecutor.Post([this] { NetworkThreadFunc(); })) {
        OnFatalError("failed to schedule client network loop");
        m_NetworkExecutor.Stop();
    }
}

void Client::Disconnect() {
    m_Running.store(false, std::memory_order_release);
    m_NetworkExecutor.Stop();
}

void Client::OnDataReceived(DataReceivedCallback fn)        { m_OnDataReceived       = std::move(fn); }
void Client::OnServerConnected(ServerConnectedCallback fn)  { m_OnServerConnected    = std::move(fn); }
void Client::OnServerDisconnected(ServerDisconnectedCallback fn) { m_OnServerDisconnected = std::move(fn); }

// ═════════════════════════════════════════════════════════════════════════════
// wolfSSL custom I/O  (static)
// ═════════════════════════════════════════════════════════════════════════════

int Client::IOSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
    auto* self = static_cast<Client*>(ctx);

    const ssize_t sent = ::send(self->m_Socket, buf,
                                static_cast<std::size_t>(sz), 0);
    return (sent < 0) ? WOLFSSL_CBIO_ERR_GENERAL
                      : static_cast<int>(sent);
}

int Client::IORecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
    auto* self = static_cast<Client*>(ctx);

    if (self->m_IncomingEncryptedBuffer.empty())
        return WOLFSSL_CBIO_ERR_WANT_READ;

    const auto count = std::min(
        static_cast<std::size_t>(sz),
        self->m_IncomingEncryptedBuffer.size());

    std::ranges::copy_n(self->m_IncomingEncryptedBuffer.begin(),
                        static_cast<std::ptrdiff_t>(count), buf);

    self->m_IncomingEncryptedBuffer.erase(
        self->m_IncomingEncryptedBuffer.begin(),
        self->m_IncomingEncryptedBuffer.begin() + static_cast<std::ptrdiff_t>(count));

    return static_cast<int>(count);
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.2 — Monadic init pipeline  (Slides 88-89)
//
// Each step is a pure(ish) function returning std::expected.  They compose
// naturally via and_then (flat-map) and transform (map):
//
//   ParseAddress
//     .and_then(resolve host)
//     .and_then(create + connect socket)
//     .and_then(create TLS context + session → bundle)
//
// If any step fails the error propagates untouched — same short-circuit
// semantics as Rust's `?` operator.
// ═════════════════════════════════════════════════════════════════════════════

std::expected<ParsedAddress, ClientError>
Client::ParseAddress(std::string_view address) {
    const auto colon = address.rfind(':');
    if (colon == std::string_view::npos)
        return std::unexpected(ClientError::AddressParse);

    auto host = std::string(address.substr(0, colon));
    uint16_t port = 0;
    auto portStr = address.substr(colon + 1);
    auto [ptr, ec] = std::from_chars(portStr.data(), portStr.data() + portStr.size(), port);
    if (ec != std::errc{} || port == 0)
        return std::unexpected(ClientError::AddressParse);

    return ParsedAddress{ std::move(host), port };
}

std::expected<std::string, ClientError>
Client::ResolveHost(const std::string& host) {
    // Fast path: already an IPv4 literal.
    in_addr tmp{};
    if (::inet_pton(AF_INET, host.c_str(), &tmp) == 1)
        return host;

    // DNS lookup.
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
        return std::unexpected(ClientError::HostResolve);

    std::array<char, INET_ADDRSTRLEN> buf{};
    ::inet_ntop(AF_INET,
        &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr,
        buf.data(), buf.size());

    ::freeaddrinfo(result);
    return std::string(buf.data());
}

std::expected<int, ClientError>
Client::CreateAndConnectSocket(const std::string& ip, uint16_t port) {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return std::unexpected(ClientError::SocketCreation);

    sockaddr_in server{
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = {},
        .sin_zero   = {},
    };

    if (::inet_pton(AF_INET, ip.c_str(), &server.sin_addr) != 1) {
        ::close(sock);
        return std::unexpected(ClientError::SocketConnect);
    }

    // "connect" on UDP sets the default peer for send()/recv().
    if (::connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
        ::close(sock);
        return std::unexpected(ClientError::SocketConnect);
    }

    ::fcntl(sock, F_SETFL, O_NONBLOCK);
    return sock;
}

std::expected<WolfContext, ClientError>
Client::CreateTLSContext() const {
    WolfContext ctx{ wolfSSL_CTX_new(wolfDTLSv1_3_client_method()) };
    if (!ctx)
        return std::unexpected(ClientError::ContextInit);

    // PQC — ML-KEM-512 key exchange (Change this before task begins).
    int groups[] = { WOLFSSL_ML_KEM_512 };
    wolfSSL_CTX_set_groups(ctx.get(), groups, 1);

    wolfSSL_CTX_SetIOSend(ctx.get(), IOSend);
    wolfSSL_CTX_SetIORecv(ctx.get(), IORecv);

    // Require server-authenticated DTLS using a pinned trust anchor.
    auto cert = ReadFileBytes(GetSafiraDataDir() / kTrustedServerCertPath);
    if (!cert)
        cert = ReadFileBytes(kTrustedServerCertPath); // legacy fallback
    if (!cert)
        return std::unexpected(cert.error());

    if (wolfSSL_CTX_load_verify_buffer(
            ctx.get(),
            cert->data(),
            static_cast<long>(cert->size()),
            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
        return std::unexpected(ClientError::TrustStoreLoad);

    wolfSSL_CTX_set_verify(ctx.get(), WOLFSSL_VERIFY_PEER, nullptr);

    return ctx;
}

std::expected<WolfSession, ClientError>
Client::CreateSession(WOLFSSL_CTX* ctx, int socket) const {
    WolfSession ssl{ wolfSSL_new(ctx) };
    if (!ssl)
        return std::unexpected(ClientError::SessionCreation);

    // I/O context is `this` — Client owns the socket and encrypted buffer.
    // The const_cast is safe: the I/O callbacks receive a non-const Client*
    // via the void* ctx, and the session only lives while the Client does.
    wolfSSL_SetIOReadCtx (ssl.get(), const_cast<Client*>(this));
    wolfSSL_SetIOWriteCtx(ssl.get(), const_cast<Client*>(this));
    wolfSSL_dtls_set_mtu (ssl.get(), kDtlsMtu);

    return ssl;
}

// ─────────────────────────────────────────────────────────────────────────────
// InitNetwork — the monadic chain.
//
// Two phases joined at the socket fd:
//
//   Phase 1:  ParseAddress → ResolveHost → CreateAndConnectSocket
//   Phase 2:  CreateTLSContext → CreateSession → bundle
//
// Phase 2 depends on the socket from Phase 1 (for I/O context), which is
// the natural join point.
// ─────────────────────────────────────────────────────────────────────────────
std::expected<ClientResources, ClientError>
Client::InitNetwork(std::string_view address) const {
    // Phase 1: address resolution → connected socket.
    auto socketResult = ParseAddress(address)
        .and_then([](ParsedAddress addr) {
            return ResolveHost(addr.Host)
                .transform([port = addr.Port](std::string ip) -> ParsedAddress {
                    return { std::move(ip), port };
                });
        })
        .and_then([](ParsedAddress resolved) {
            return CreateAndConnectSocket(resolved.Host, resolved.Port);
        });

    if (!socketResult) return std::unexpected(socketResult.error());
    const int sock = *socketResult;

    auto ctxResult = CreateTLSContext();
    if (!ctxResult) {
        ::close(sock);
        return std::unexpected(ctxResult.error());
    }
    WolfContext ctx = std::move(*ctxResult);

    auto sslResult = CreateSession(ctx.get(), sock);
    if (!sslResult) {
        ::close(sock);
        return std::unexpected(sslResult.error());
    }

    return ClientResources{ sock, std::move(ctx), std::move(*sslResult) };
}

// ═════════════════════════════════════════════════════════════════════════════
// Network thread
// ═════════════════════════════════════════════════════════════════════════════

void Client::NetworkThreadFunc() {
    m_Running.store(true, std::memory_order_release);
    m_Phase              = Handshaking{};
    m_SuppressShutdown.store(false, std::memory_order_release);
    m_ConnectionStatus.store(ConnectionStatus::Connecting, std::memory_order_release);
    {
        std::lock_guard lock(m_StateMutex);
        m_ConnectionDebugMessage.clear();
    }

    wolfSSL_Init();

    // ── Monadic init with or_else for structured error reporting ────────────
    auto resources = InitNetwork(m_ServerAddress)
        .or_else([this](ClientError e) -> std::expected<ClientResources, ClientError> {
            OnFatalError(std::format("{}: '{}'", Describe(e), m_ServerAddress));
            return std::unexpected(e);
        });

    if (!resources) {
        wolfSSL_Cleanup();
        return;
    }

    m_Socket = resources->Socket;
    m_Ctx    = std::move(resources->Ctx);
    m_SSL    = std::move(resources->SSL);

    std::println("[client] connecting to {}", m_ServerAddress);

    // ── Kick off the DTLS handshake ─────────────────────────────────────────
    {
        const int ret = wolfSSL_connect(m_SSL.get());
        if (ret != WOLFSSL_SUCCESS) {
            const int err = wolfSSL_get_error(m_SSL.get(), ret);
            if (!IsWantIO(err)) {
                OnFatalError(std::format("handshake init failed: err={}", err));
                m_Running.store(false, std::memory_order_release);
            }
        }
    }

    // ── Main loop ───────────────────────────────────────────────────────────
    std::array<uint8_t, 1500> rawBuf{};

    while (m_Running.load(std::memory_order_acquire)) {

        // 1. Receive incoming UDP datagrams.
        const ssize_t len = ::recv(m_Socket, rawBuf.data(), rawBuf.size(), 0);
        if (len > 0) {
            m_IncomingEncryptedBuffer.insert(
                m_IncomingEncryptedBuffer.end(),
                rawBuf.begin(), rawBuf.begin() + len);
        } else if (len < 0) {
            const int err = errno;
            if (err == EWOULDBLOCK || err == EAGAIN || err == EINTR) {
                // Non-fatal, no data available right now.
            } else if (err == ECONNREFUSED || err == ECONNRESET
                       || err == ENETUNREACH || err == EHOSTUNREACH || err == ENOTCONN) {
                spdlog::warn("[client] server became unreachable (errno={})", err);
                m_Running.store(false, std::memory_order_release);
                continue;
            } else {
                spdlog::error("[client] recv failed (errno={})", err);
                m_Running.store(false, std::memory_order_release);
                continue;
            }
        }

        // 2. §3.4 Typestate dispatch — compile-time exhaustive.
        std::visit(Overloaded{
            [this](Handshaking) { DriveHandshake(); },
            [this](Connected)   { DriveConnected(); },
        }, m_Phase);
        DrainSendQueue();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // ── Cleanup — RAII does the heavy lifting ───────────────────────────────
    if (m_SSL && !m_SuppressShutdown.load(std::memory_order_acquire))
        wolfSSL_shutdown(m_SSL.get());

    m_SSL.reset();
    m_Ctx.reset();
    wolfSSL_Cleanup();

    ::close(m_Socket);
    m_Socket = -1;

    m_Phase = Handshaking{};
    m_ConnectionStatus.store(ConnectionStatus::Disconnected, std::memory_order_release);
    {
        std::lock_guard lock(m_SendMutex);
        m_SendQueue.clear();
    }

    if (m_OnServerDisconnected)
        m_OnServerDisconnected();
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.4 — Typestate dispatch  (Slides 111-115)
//
// DriveHandshake and DriveConnected are called exclusively via std::visit
// on the ConnectionPhase variant.  Adding a third phase (e.g. Rekeying)
// causes a compile error until you add the handler.
// ═════════════════════════════════════════════════════════════════════════════

void Client::DriveHandshake() {
    const int ret = wolfSSL_connect(m_SSL.get());

    if (ret == WOLFSSL_SUCCESS) {
        m_Phase = Connected{};
        m_ConnectionStatus.store(ConnectionStatus::Connected, std::memory_order_release);
        std::println("[client] PQC handshake complete");

        if (m_OnServerConnected)
            m_OnServerConnected();
        return;
    }

    const int err = wolfSSL_get_error(m_SSL.get(), ret);
    if (IsWantIO(err)) return;

    // Real failure.
    {
        std::lock_guard lock(m_StateMutex);
        m_ConnectionDebugMessage = std::format("handshake failed: err={}", err);
        spdlog::error("[client] {}", m_ConnectionDebugMessage);
    }
    m_ConnectionStatus.store(ConnectionStatus::FailedToConnect, std::memory_order_release);
    m_Running.store(false, std::memory_order_release);
}

void Client::DriveConnected() {
    std::array<char, 4096> plaintext{};

    while (true) {
        const int bytes = wolfSSL_read(
            m_SSL.get(), plaintext.data(),
            static_cast<int>(plaintext.size()));

        if (bytes > 0) {
            if (m_OnDataReceived)
                m_OnDataReceived(
                    ByteSpan(reinterpret_cast<const uint8_t*>(plaintext.data()),
                             static_cast<size_t>(bytes)));
            continue;
        }

        if (const int err = wolfSSL_get_error(m_SSL.get(), bytes); err == WOLFSSL_ERROR_ZERO_RETURN) {
            spdlog::info("[client] server closed the connection");
            m_Running.store(false, std::memory_order_release);
        }
        break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Send — guarded by typestate
// ═════════════════════════════════════════════════════════════════════════════

void Client::Send(ByteSpan buf) {
    if (buf.empty()) return;
    if (m_ConnectionStatus.load(std::memory_order_acquire) != ConnectionStatus::Connected) {
        spdlog::warn("[client] send before handshake complete");
        return;
    }

    std::lock_guard lock(m_SendMutex);
    if (m_SendQueue.size() >= kMaxPendingSendQueue) {
        m_SendQueue.erase(m_SendQueue.begin());
        spdlog::warn("[client] send queue full, dropping oldest pending payload");
    }
    m_SendQueue.push_back(PendingClientSend{ .Data = { buf.begin(), buf.end() } });
}

void Client::DrainSendQueue() {
    std::vector<PendingClientSend> batch;
    {
        std::lock_guard lock(m_SendMutex);
        batch.swap(m_SendQueue);
    }

    for (const auto& send : batch) {
        DoSend(ByteSpan(send.Data.data(), send.Data.size()));
    }
}

void Client::DoSend(ByteSpan buf) {
    if (!m_SSL || !std::holds_alternative<Connected>(m_Phase))
        return;

    if (const int ret = wolfSSL_write(m_SSL.get(), buf.data(), static_cast<int>(buf.size())); ret <= 0) {
        const int err = wolfSSL_get_error(m_SSL.get(), ret);
        spdlog::error("[client] wolfSSL_write failed: {}", err);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §3.2 — Error messages  (Slides 91-92)
//
// Include the address/context so the developer can reproduce, but never
// include key material.
// ─────────────────────────────────────────────────────────────────────────────
void Client::OnFatalError(std::string_view msg) {
    spdlog::critical("[client] fatal: {}", msg);
    {
        std::lock_guard lock(m_StateMutex);
        m_ConnectionDebugMessage = std::string(msg);
    }
    m_ConnectionStatus.store(ConnectionStatus::FailedToConnect, std::memory_order_release);
    m_Running.store(false, std::memory_order_release);
}

} // namespace Safira
