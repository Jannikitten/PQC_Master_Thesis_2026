#ifndef PQC_MASTER_THESIS_2026_SERVER_H
#define PQC_MASTER_THESIS_2026_SERVER_H

// ═════════════════════════════════════════════════════════════════════════════
// Server.h — DTLS 1.3 server with ML-KEM-512 post-quantum key exchange
//
// Refactored following Kleppmann & Hugenroth, "Cryptography and Protocol
// Engineering", Cambridge P79, Lent 2025.
//
//  §3.2  Result types      – std::expected with monadic and_then / transform
//  §3.4  Strong types      – ClientID is a struct (Common.h)
//  §3.4  Typestate          – ConnectionState = variant<Handshaking, Connected>
//  §5.5  Secret lifetimes  – RAII wrappers (WolfContext / WolfSession)
//  §5.5  Pure functions    – CreateSocket / CreateTLSContext are pure helpers
//  §5.5  Opinionated API   – no "reliable" flag; DTLS is always best-effort
//
// ── New in v2 ────────────────────────────────────────────────────────────────
//  - ServerConfig          – compile-time + runtime tunables
//  - Handshake timeout     – reaps clients stuck in Handshaking
//  - Max connection cap    – rejects new clients beyond the limit
//  - ClientMetrics         – per-client stats (uptime, bytes, messages)
//  - Thread-safe send queue– ServerLayer can enqueue sends from any thread
//  - Graceful shutdown     – close_notify to all before teardown
// ═════════════════════════════════════════════════════════════════════════════

#include "Common.h"
#include "Types.h"
#include "WolfTypes.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <netinet/in.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// §3.2 — Result types  (Slides 88-89)
// ─────────────────────────────────────────────────────────────────────────────
enum class ServerError : uint8_t {
    SocketCreation,
    SocketBind,
    ContextInit,
    CertificateLoad,
    PrivateKeyLoad,
    SessionCreation,
    CertificateGeneration,
};

[[nodiscard]] constexpr std::string_view Describe(ServerError e) noexcept {
    switch (e) {
        case ServerError::SocketCreation:        return "failed to create UDP socket";
        case ServerError::SocketBind:            return "failed to bind UDP socket";
        case ServerError::ContextInit:           return "wolfSSL_CTX_new returned nullptr";
        case ServerError::CertificateLoad:       return "failed to load server.pem";
        case ServerError::PrivateKeyLoad:        return "failed to load server-key.pem";
        case ServerError::SessionCreation:       return "wolfSSL_new returned nullptr";
        case ServerError::CertificateGeneration: return "failed to generate self-signed certificate";
    }
    return "unknown error";
}

// ─────────────────────────────────────────────────────────────────────────────
// Server configuration — tunables grouped in one place.
//
// Compile-time defaults can be overridden by passing a ServerConfig to the
// constructor.  Set what you need; leave the rest at sane defaults.
// ─────────────────────────────────────────────────────────────────────────────
struct ServerConfig {
    uint16_t Port                    = 8192;
    uint32_t MaxClients              = 64;
    float    HandshakeTimeoutSeconds = 10.0f;
    int      DtlsMtu                = 1400;
    size_t   RecvBufSize            = 1500;
};

// ─────────────────────────────────────────────────────────────────────────────
// §3.4 — Typestate pattern  (Slides 111-115)
// ─────────────────────────────────────────────────────────────────────────────
struct Handshaking {};
struct Connected   {};

using ConnectionState = std::variant<Handshaking, Connected>;

// ─────────────────────────────────────────────────────────────────────────────
// Per-client metrics — consumed by ServerLayer for admin commands / logging.
// ─────────────────────────────────────────────────────────────────────────────
struct ClientMetrics {
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    TimePoint ConnectedAt    = Clock::now();
    uint64_t  MessagesRecv   = 0;        // application-level messages
    uint64_t  BytesRecv      = 0;        // raw UDP payload in
    uint64_t  BytesSent      = 0;        // raw UDP payload out

    [[nodiscard]] double UptimeSeconds() const noexcept {
        return std::chrono::duration<double>(Clock::now() - ConnectedAt).count();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// I/O context + per-client state
// ─────────────────────────────────────────────────────────────────────────────
struct ClientInfo;

struct IOContext {
    ClientInfo* Client = nullptr;
    int         Socket = -1;
};

struct ClientInfo {
    ClientID                  ID {};
    sockaddr_in               Addr {};
    std::string               AddressStr;

    WolfSession               SSL;
    std::unique_ptr<IOContext> IO;
    ConnectionState           State   { Handshaking{} };
    ClientMetrics             Metrics {};
    std::vector<uint8_t>      EncryptedBuffer;

    ClientInfo() = default;
    ClientInfo(ClientInfo&&) noexcept = default;
    ClientInfo& operator=(ClientInfo&&) noexcept = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// §5.5 — Pure init helpers return this bundle so the monadic chain
// (and_then / transform) can thread state without side-effects.
// ─────────────────────────────────────────────────────────────────────────────
struct InitResources {
    int         Socket;
    WolfContext Ctx;
};

struct GeneratedCredentials {
    std::vector<uint8_t> CertDer;
    std::vector<uint8_t> KeyDer;
};

// ─────────────────────────────────────────────────────────────────────────────
// Thread-safe send queue entry — lets ServerLayer (UI thread) enqueue sends
// that the network thread drains each tick.  Avoids cross-thread wolfSSL
// calls which are not reentrant.
// ─────────────────────────────────────────────────────────────────────────────
struct PendingSend {
    ClientID             Target {};
    std::vector<uint8_t> Data;
    bool                 Broadcast = false;
    ClientID             Exclude {};
};

// ─────────────────────────────────────────────────────────────────────────────
// Server
// ─────────────────────────────────────────────────────────────────────────────
class Server {
public:
    using DataReceivedCallback       = std::function<void(ClientInfo&, ByteSpan)>;
    using ClientConnectedCallback    = std::function<void(ClientInfo&)>;
    using ClientDisconnectedCallback = std::function<void(ClientInfo&)>;

    explicit Server(const ServerConfig& config = {});
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&)                 = delete;
    Server& operator=(Server&&)      = delete;

    void Start();
    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept {
        return m_Running.load(std::memory_order_acquire);
    }

    [[nodiscard]] const ServerConfig& GetConfig() const noexcept {
        return m_Config;
    }

    /// Thread-safe: readable from any thread without locking.
    [[nodiscard]] size_t GetClientCount() const noexcept {
        return m_ClientCount.load(std::memory_order_acquire);
    }

    // ── Thread-safe send API ────────────────────────────────────────────────
    // Enqueue into a mutex-protected queue; the network thread drains it
    // once per tick.  Safe to call from any thread (UI, callbacks, etc.).

    void SendToClient    (ClientID id, ByteSpan buf);
    void SendToAllClients(ByteSpan buf, ClientID exclude = {});

    void KickClient(ClientID id);

    // ── Read-only client snapshot ───────────────────────────────────────────
    [[nodiscard]] std::optional<ClientMetrics> GetClientMetrics(ClientID id) const;

    // ── Callbacks ───────────────────────────────────────────────────────────
    void OnDataReceived      (DataReceivedCallback fn);
    void OnClientConnected   (ClientConnectedCallback fn);
    void OnClientDisconnected(ClientDisconnectedCallback fn);

private:
    // §3.2 + §5.5: Pure helpers returning std::expected
    [[nodiscard]] std::expected<int, ServerError>                  CreateSocket() const;
    [[nodiscard]] std::expected<WolfContext, ServerError>          CreateTLSContext() const;
    [[nodiscard]] std::expected<WolfSession, ServerError>         CreateSession() const;
    [[nodiscard]] std::expected<InitResources, ServerError>       InitNetwork() const;
    [[nodiscard]] std::expected<GeneratedCredentials, ServerError> GenerateSelfSignedCert() const;

    void NetworkThreadFunc();
    void DispatchPacket(const sockaddr_in& from, std::span<const uint8_t> data);

    // §3.4 Typestate dispatch
    void DriveClient    (ClientInfo& client);
    void DriveHandshake (ClientInfo& client);
    void DriveConnected (ClientInfo& client);

    // ── Housekeeping (called once per tick) ─────────────────────────────────
    void ReapStaleHandshakes();
    void DrainSendQueue();
    void ProcessPendingKicks();

    // ── Internal send — network-thread only, no locking ─────────────────────
    void DoSendToClient    (ClientID id, ByteSpan buf);
    void DoSendToAllClients(ByteSpan buf, ClientID exclude);

    void RemoveClient(ClientID id);
    void ShutdownClient(ClientInfo& client);

    void OnFatalError(std::string_view msg);

    static int IORecv(WOLFSSL*, char* buf, int sz, void* ctx);
    static int IOSend(WOLFSSL*, char* buf, int sz, void* ctx);

    // ── Config ───────────────────────────────────────────────────────────────
    ServerConfig m_Config;

    // ── State ────────────────────────────────────────────────────────────────
    int               m_Socket = -1;
    std::atomic<bool> m_Running { false };
    std::thread       m_NetworkThread;

    WolfContext m_Ctx;

    std::unordered_map<ClientID, ClientInfo> m_Clients;

    /// Atomic client count — thread-safe read from UI without locking.
    std::atomic<size_t> m_ClientCount { 0 };

    // ── Thread-safe send queue ───────────────────────────────────────────────
    mutable std::mutex       m_SendMutex;
    std::vector<PendingSend> m_SendQueue;

    // ── Thread-safe kick queue ───────────────────────────────────────────────
    mutable std::mutex       m_KickMutex;
    std::vector<ClientID>    m_PendingKicks;

    // ── Callbacks ────────────────────────────────────────────────────────────
    DataReceivedCallback       m_OnDataReceived;
    ClientConnectedCallback    m_OnClientConnected;
    ClientDisconnectedCallback m_OnClientDisconnected;
};

} // namespace Safira
#endif // PQC_MASTER_THESIS_2026_SERVER_H