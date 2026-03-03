#ifndef PQC_MASTER_THESIS_2026_CLIENT_H
#define PQC_MASTER_THESIS_2026_CLIENT_H

// ═════════════════════════════════════════════════════════════════════════════
// Client.h — DTLS 1.3 client with ML-KEM-512 post-quantum key exchange
//
// Refactored following Kleppmann & Hugenroth, "Cryptography and Protocol
// Engineering", Cambridge P79, Lent 2025.
//
//  §3.2  Result types     – std::expected with monadic and_then / transform
//                           for the init pipeline (resolve → socket → TLS).
//  §3.4  Typestate         – ConnectionPhase = variant<Handshaking, Connected>
//                           replaces the bare bool m_HandshakeFinished.
//  §5.5  Secret lifetimes – WolfContext / WolfSession (RAII, from WolfTypes.h)
//  §5.5  Opinionated API  – "reliable" flag removed; DTLS is best-effort.
// ═════════════════════════════════════════════════════════════════════════════

#include "Types.h"
#include "WolfTypes.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Public connection status — consumed by the UI layer.
// ─────────────────────────────────────────────────────────────────────────────
enum class ConnectionStatus : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    FailedToConnect,
};

// ─────────────────────────────────────────────────────────────────────────────
// §3.2 — Result types  (Slides 88-89)
// ─────────────────────────────────────────────────────────────────────────────
enum class ClientError : uint8_t {
    AddressParse,
    HostResolve,
    SocketCreation,
    SocketConnect,
    ContextInit,
    SessionCreation,
    HandshakeInit,
};

[[nodiscard]] constexpr std::string_view Describe(ClientError e) noexcept {
    switch (e) {
        case ClientError::AddressParse:    return "could not parse host:port";
        case ClientError::HostResolve:     return "could not resolve hostname";
        case ClientError::SocketCreation:  return "failed to create UDP socket";
        case ClientError::SocketConnect:   return "UDP connect() failed";
        case ClientError::ContextInit:     return "wolfSSL_CTX_new failed";
        case ClientError::SessionCreation: return "wolfSSL_new failed";
        case ClientError::HandshakeInit:   return "wolfSSL_connect initiation failed";
    }
    return "unknown error";
}

// ─────────────────────────────────────────────────────────────────────────────
// §3.4 — Typestate  (Slides 111-115)
//
// Internal network-thread state.  The main loop dispatches via std::visit
// so the compiler enforces every phase is handled.  Replaces the old
// m_HandshakeFinished bool.
// ─────────────────────────────────────────────────────────────────────────────
struct Handshaking {};
struct Connected   {};

using ConnectionPhase = std::variant<Handshaking, Connected>;

// ─────────────────────────────────────────────────────────────────────────────
// §5.5 — Init resources bundle for the monadic pipeline.
// ─────────────────────────────────────────────────────────────────────────────
struct ParsedAddress {
    std::string Host;
    uint16_t    Port = 0;
};

struct ClientResources {
    int         Socket;
    WolfContext  Ctx;
    WolfSession  SSL;
};

// ─────────────────────────────────────────────────────────────────────────────
// Client
// ─────────────────────────────────────────────────────────────────────────────
class Client {
public:
    using DataReceivedCallback       = std::function<void(ByteSpan)>;
    using ServerConnectedCallback    = std::function<void()>;
    using ServerDisconnectedCallback = std::function<void()>;

    Client()  = default;
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    /// address = "ip:port" or "hostname:port"
    void ConnectToServer(std::string_view address);

    /// Full disconnect — blocks until the network thread exits.
    /// Call from outside the network thread (UI, app shutdown).
    void Disconnect();

    /// Signal-only disconnect from inside a network-thread callback.
    /// Suppresses close_notify to avoid stale packets.
    void RequestDisconnect() noexcept {
        m_Running.store(false, std::memory_order_release);
        m_SuppressShutdown = true;
    }

    [[nodiscard]] bool IsConnected() const noexcept {
        return m_ConnectionStatus.load(std::memory_order_acquire)
               == ConnectionStatus::Connected;
    }
    [[nodiscard]] ConnectionStatus GetConnectionStatus() const noexcept {
        return m_ConnectionStatus.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::string& GetConnectionDebugMessage() const noexcept {
        return m_ConnectionDebugMessage;
    }

    // §5.5 Opinionated API — no "reliable" flag.
    void Send(ByteSpan buf);

    void OnDataReceived      (DataReceivedCallback fn);
    void OnServerConnected   (ServerConnectedCallback fn);
    void OnServerDisconnected(ServerDisconnectedCallback fn);

private:
    // ── §3.2 + §5.5: Pure-ish init helpers, composed via and_then ───────────
    [[nodiscard]] static std::expected<ParsedAddress, ClientError>
        ParseAddress(std::string_view address);

    [[nodiscard]] static std::expected<std::string, ClientError>
        ResolveHost(const std::string& host);

    [[nodiscard]] static std::expected<int, ClientError>
        CreateAndConnectSocket(const std::string& ip, uint16_t port);

    [[nodiscard]] std::expected<WolfContext, ClientError>
        CreateTLSContext() const;

    [[nodiscard]] std::expected<WolfSession, ClientError>
        CreateSession(WOLFSSL_CTX* ctx, int socket) const;

    /// The full monadic init pipeline.
    [[nodiscard]] std::expected<ClientResources, ClientError>
        InitNetwork(std::string_view address) const;

    // ── Network loop ────────────────────────────────────────────────────────
    void NetworkThreadFunc();

    // §3.4 Typestate dispatch
    void DriveHandshake();
    void DriveConnected();

    void OnFatalError(std::string_view msg);

    // wolfSSL custom I/O (static, C-linkage-compatible)
    static int IOSend(WOLFSSL*, char* buf, int sz, void* ctx);
    static int IORecv(WOLFSSL*, char* buf, int sz, void* ctx);

    // ── Constants ────────────────────────────────────────────────────────────
    static constexpr int kDtlsMtu = 1400;

    // ── State ────────────────────────────────────────────────────────────────
    std::string m_ServerAddress;

    int               m_Socket = -1;
    std::atomic<bool> m_Running { false };
    std::thread       m_NetworkThread;

    std::atomic<ConnectionStatus> m_ConnectionStatus { ConnectionStatus::Disconnected };
    std::string                   m_ConnectionDebugMessage;

    WolfContext  m_Ctx;
    WolfSession  m_SSL;

    // §3.4 — replaces the old bool m_HandshakeFinished.
    ConnectionPhase m_Phase { Handshaking{} };

    bool m_SuppressShutdown = false;

    std::vector<uint8_t> m_IncomingEncryptedBuffer;

    DataReceivedCallback       m_OnDataReceived;
    ServerConnectedCallback    m_OnServerConnected;
    ServerDisconnectedCallback m_OnServerDisconnected;
};

} // namespace Safira
#endif // PQC_MASTER_THESIS_2026_CLIENT_H