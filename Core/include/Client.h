#ifndef PQC_MASTER_THESIS_2026_CLIENT_H
#define PQC_MASTER_THESIS_2026_CLIENT_H

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <../Core/include/Buffer.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace Safira {
    enum class ConnectionStatus {
        Disconnected,
        Connecting,
        Connected,
        FailedToConnect,
    };

    class Client {
    public:
        using DataReceivedCallback      = std::function<void(Buffer)>;
        using ServerConnectedCallback   = std::function<void()>;
        using ServerDisconnectedCallback= std::function<void()>;

        Client()  = default;
        ~Client();

        Client(const Client&)            = delete;
        Client& operator=(const Client&) = delete;

        // address can be "ip:port" or "hostname:port"
        void ConnectToServer(const std::string& address);

        // Full disconnect: stops the network thread and blocks until it exits.
        // Call this from OUTSIDE the network thread (e.g. UI button, app shutdown).
        void Disconnect();

        // Signal-only disconnect: sets the stop flag without joining the thread.
        // Use this from INSIDE a network-thread callback (DataReceived, etc.) to
        // avoid a thread joining itself (which causes SIGABRT / deadlock).
        // Also suppresses the outgoing close_notify so the server doesn't receive
        // a stale UDP packet and mistake it for a new incoming connection.
        void RequestDisconnect() { m_Running = false; m_SuppressShutdown = true; }

        bool IsConnected() const { return m_ConnectionStatus == ConnectionStatus::Connected; }
        ConnectionStatus GetConnectionStatus() const { return m_ConnectionStatus; }
        const std::string& GetConnectionDebugMessage() const { return m_ConnectionDebugMessage; }

        // Only valid after the DTLS handshake finishes (IsConnected() returns true
        // AND m_HandshakeFinished is set).  Logs a warning otherwise.
        void SendBuffer(Buffer buffer, bool reliable = true);
        void SendString(const std::string& str, bool reliable = true);

        void SetDataReceivedCallback      (const DataReceivedCallback&);
        void SetServerConnectedCallback   (const ServerConnectedCallback&);
        void SetServerDisconnectedCallback(const ServerDisconnectedCallback&);

    private:
        void NetworkThreadFunc();
        void OnFatalError(const std::string& msg);

        // wolfSSL custom I/O (static C callbacks)
        static int IOSend(WOLFSSL*, char* buf, int sz, void* ctx);
        static int IORecv(WOLFSSL*, char* buf, int sz, void* ctx);

        // ── State ────────────────────────────────────────────────────────────────
        std::string m_ServerAddress;

        int               m_Socket = -1;
        std::atomic<bool> m_Running{false};
        std::thread       m_NetworkThread;

        std::atomic<ConnectionStatus> m_ConnectionStatus{ConnectionStatus::Disconnected};
        std::string                   m_ConnectionDebugMessage;

        WOLFSSL_CTX* m_CTX    = nullptr;
        WOLFSSL*     m_SSL    = nullptr;
        bool         m_HandshakeFinished  = false;
        // Set by RequestDisconnect() to skip wolfSSL_shutdown on the client side
        // when the server already closed the session (kick / server shutdown).
        // Prevents a stale close_notify from being misread as a new connection.
        bool         m_SuppressShutdown   = false;

        // Raw incoming UDP bytes waiting to be decrypted by wolfSSL
        std::vector<uint8_t> m_IncomingEncryptedBuffer;

        DataReceivedCallback       m_DataReceivedCallback;
        ServerConnectedCallback    m_ServerConnectedCallback;
        ServerDisconnectedCallback m_ServerDisconnectedCallback;
    };

} // namespace Safira
#endif //PQC_MASTER_THESIS_2026_CLIENT_H