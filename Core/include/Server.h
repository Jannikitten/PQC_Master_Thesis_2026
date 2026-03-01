#pragma once
#include "Common.h"

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <vector>
#include <../Core/include/Buffer.h>

#include <netinet/in.h>          // sockaddr_in
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Per-client state held by the server
// ─────────────────────────────────────────────────────────────────────────────
struct ClientInfo {
    ClientID    ID   = 0;
    sockaddr_in Addr {};                   // remote address (ip + port)
    std::string AddressStr;                // "a.b.c.d:port" for logging

    WOLFSSL*             SSL            = nullptr;
    std::vector<uint8_t> EncryptedBuffer;  // raw UDP bytes waiting to be read by wolfSSL
};

// ─────────────────────────────────────────────────────────────────────────────
// Server
// ─────────────────────────────────────────────────────────────────────────────
class Server {
public:
    using DataReceivedCallback       = std::function<void(ClientInfo&, Buffer)>;
    using ClientConnectedCallback    = std::function<void(ClientInfo&)>;
    using ClientDisconnectedCallback = std::function<void(ClientInfo&)>;

    explicit Server(int port);
    ~Server();

    // Non-copyable / non-movable
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    void Start();
    void Stop();
    bool IsRunning() const { return m_Running; }

    // ── Send helpers ────────────────────────────────────────────────────────
    // `reliable` is kept for API compatibility but DTLS is always
    // unreliable at the transport layer — use application-level sequencing
    // if you need reliability.
    void SendBufferToClient    (ClientID, Buffer, bool reliable = true);
    void SendBufferToAllClients(Buffer,   ClientID exclude = 0, bool reliable = true);
    void SendStringToClient    (ClientID, const std::string&, bool reliable = true);
    void SendStringToAllClients(const std::string&, ClientID exclude = 0, bool reliable = true);

    void KickClient(ClientID);

    // ── Callbacks ───────────────────────────────────────────────────────────
    void SetDataReceivedCallback      (const DataReceivedCallback&);
    void SetClientConnectedCallback   (const ClientConnectedCallback&);
    void SetClientDisconnectedCallback(const ClientDisconnectedCallback&);

private:
    // Network thread entry point
    void NetworkThreadFunc();

    // Packet dispatch: called for every UDP datagram that arrives
    void DispatchPacket(const sockaddr_in& from, const uint8_t* data, int len);

    // Drive the wolfSSL engine for one client (accept + read loop)
    void DriveClient(ClientInfo& client);

    // Remove a client cleanly, calling the disconnect callback
    void RemoveClient(ClientID id);

    void OnFatalError(const std::string& msg);

    // ── wolfSSL custom I/O callbacks (static so they match the C signature) ─
    static int IORecv(WOLFSSL*, char* buf, int sz, void* ctx);
    static int IOSend(WOLFSSL*, char* buf, int sz, void* ctx);

    // ── State ────────────────────────────────────────────────────────────────
    int               m_Port   = 0;
    int               m_Socket = -1;
    std::atomic<bool> m_Running{false};
    std::thread       m_NetworkThread;
    int               m_kDtlsMtu = 1400;

    WOLFSSL_CTX* m_CTX = nullptr;

    // All client access happens on the network thread; no extra lock needed.
    std::unordered_map<ClientID, ClientInfo> m_Clients;

    DataReceivedCallback       m_DataReceivedCallback;
    ClientConnectedCallback    m_ClientConnectedCallback;
    ClientDisconnectedCallback m_ClientDisconnectedCallback;
};

} // namespace Safira