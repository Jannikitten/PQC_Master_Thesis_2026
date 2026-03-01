#pragma once
#include "Common.h"

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
    void Disconnect();

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
    bool         m_HandshakeFinished = false;

    // Raw incoming UDP bytes waiting to be decrypted by wolfSSL
    std::vector<uint8_t> m_IncomingEncryptedBuffer;

    DataReceivedCallback       m_DataReceivedCallback;
    ServerConnectedCallback    m_ServerConnectedCallback;
    ServerDisconnectedCallback m_ServerDisconnectedCallback;
};

} // namespace Safira