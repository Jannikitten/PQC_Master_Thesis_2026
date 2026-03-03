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
// ═════════════════════════════════════════════════════════════════════════════

#include "Common.h"
#include "Types.h"
#include "WolfTypes.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
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
};

[[nodiscard]] constexpr std::string_view Describe(ServerError e) noexcept {
    switch (e) {
        case ServerError::SocketCreation:  return "failed to create UDP socket";
        case ServerError::SocketBind:      return "failed to bind UDP socket";
        case ServerError::ContextInit:     return "wolfSSL_CTX_new returned nullptr";
        case ServerError::CertificateLoad: return "failed to load server.pem";
        case ServerError::PrivateKeyLoad:  return "failed to load server-key.pem";
        case ServerError::SessionCreation: return "wolfSSL_new returned nullptr";
    }
    return "unknown error";
}

// §5.5 — RAII wrappers live in WolfTypes.h (shared with Client.h).

// ─────────────────────────────────────────────────────────────────────────────
// §3.4 — Typestate pattern  (Slides 111-115)
//
// std::variant models the same state-machine idea the notes show with Rust
// generics.  std::visit forces the caller to handle every state.
// ─────────────────────────────────────────────────────────────────────────────
struct Handshaking {};
struct Connected   {};

using ConnectionState = std::variant<Handshaking, Connected>;

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
    ConnectionState           State { Handshaking{} };
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

// ─────────────────────────────────────────────────────────────────────────────
// Server
// ─────────────────────────────────────────────────────────────────────────────
class Server {
public:
    using DataReceivedCallback       = std::function<void(ClientInfo&, ByteSpan)>;
    using ClientConnectedCallback    = std::function<void(ClientInfo&)>;
    using ClientDisconnectedCallback = std::function<void(ClientInfo&)>;

    explicit Server(uint16_t port);
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

    // §5.5 Opinionated API — no "reliable" flag.
    void SendToClient    (ClientID id, ByteSpan buf);
    void SendToAllClients(ByteSpan buf, ClientID exclude = {});

    void KickClient(ClientID id);

    void OnDataReceived      (DataReceivedCallback fn);
    void OnClientConnected   (ClientConnectedCallback fn);
    void OnClientDisconnected(ClientDisconnectedCallback fn);

private:
    // §3.2 + §5.5: Pure helpers returning std::expected — composed via
    // and_then / transform in NetworkThreadFunc.
    [[nodiscard]] std::expected<int, ServerError>             CreateSocket() const;
    [[nodiscard]] std::expected<WolfContext, ServerError>      CreateTLSContext() const;
    [[nodiscard]] std::expected<WolfSession, ServerError>      CreateSession() const;
    [[nodiscard]] std::expected<InitResources, ServerError>    InitNetwork() const;

    void NetworkThreadFunc();
    void DispatchPacket(const sockaddr_in& from, std::span<const uint8_t> data);

    // §3.4 Typestate — dispatched via std::visit
    void DriveClient    (ClientInfo& client);
    void DriveHandshake (ClientInfo& client);
    void DriveConnected (ClientInfo& client);

    void RemoveClient(ClientID id);
    void ShutdownClient(ClientInfo& client);

    void OnFatalError(std::string_view msg);

    static int IORecv(WOLFSSL*, char* buf, int sz, void* ctx);
    static int IOSend(WOLFSSL*, char* buf, int sz, void* ctx);

    static constexpr int    kDtlsMtu     = 1400;
    static constexpr size_t kRecvBufSize = 1500;

    uint16_t          m_Port   = 0;
    int               m_Socket = -1;
    std::atomic<bool> m_Running { false };
    std::thread       m_NetworkThread;

    WolfContext m_Ctx;

    std::unordered_map<ClientID, ClientInfo> m_Clients;

    DataReceivedCallback       m_OnDataReceived;
    ClientConnectedCallback    m_OnClientConnected;
    ClientDisconnectedCallback m_OnClientDisconnected;
};

} // namespace Safira
#endif // PQC_MASTER_THESIS_2026_SERVER_H