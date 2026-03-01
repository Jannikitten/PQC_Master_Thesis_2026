#include "Client.h"

#include <arpa/inet.h>     // inet_pton, inet_ntop
#include <sys/socket.h>
#include <netdb.h>         // getaddrinfo
#include <unistd.h>        // close()
#include <fcntl.h>         // O_NONBLOCK
#include <cstring>
#include <format>
#include <print>
#include <spdlog/spdlog.h>

namespace Safira {
    namespace {
        // ─────────────────────────────────────────────────────────────────────────────
        // Resolve "hostname" or "a.b.c.d" → IPv4 dotted-decimal string.
        // Returns empty string on failure.
        // ─────────────────────────────────────────────────────────────────────────────
        std::string ResolveAddress(const std::string& host) {
            // Try parsing as a bare IPv4 address first
            in_addr tmp{};
            if (::inet_pton(AF_INET, host.c_str(), &tmp) == 1)
                return host;

            // Otherwise do a DNS lookup
            addrinfo hints{};
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;

            addrinfo* result = nullptr;
            if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
                return {};

            char buf[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET,
                &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr,
                buf, sizeof(buf));

            ::freeaddrinfo(result);
            return buf;
        }

        // Split "host:port" → {host, port}.  Returns port=0 on parse failure.
        std::pair<std::string, uint16_t> SplitAddress(const std::string& addr) {
            auto colon = addr.rfind(':');
            if (colon == std::string::npos) return {addr, 0};

            std::string host = addr.substr(0, colon);
            uint16_t    port = 0;
            try { port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1))); }
            catch (...) {}
            return {host, port};
        }
    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────────
    // Dtor
    // ─────────────────────────────────────────────────────────────────────────────
    Client::~Client() {
        Disconnect();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Public interface
    // ─────────────────────────────────────────────────────────────────────────────
    void Client::ConnectToServer(const std::string& address) {
        if (m_Running) return;
        if (m_NetworkThread.joinable()) m_NetworkThread.join();

        m_ServerAddress = address;
        m_NetworkThread = std::thread([this] { NetworkThreadFunc(); });
    }

    void Client::Disconnect() {
        m_Running = false;
        if (m_NetworkThread.joinable()) m_NetworkThread.join();
    }

    void Client::SetDataReceivedCallback(const DataReceivedCallback& fn)       { m_DataReceivedCallback       = fn; }
    void Client::SetServerConnectedCallback(const ServerConnectedCallback& fn) { m_ServerConnectedCallback    = fn; }
    void Client::SetServerDisconnectedCallback(const ServerDisconnectedCallback& fn) { m_ServerDisconnectedCallback = fn; }

    // ─────────────────────────────────────────────────────────────────────────────
    // wolfSSL custom I/O (static)
    // ─────────────────────────────────────────────────────────────────────────────
    int Client::IOSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
        auto* self = static_cast<Client*>(ctx);

        ssize_t sent = ::send(self->m_Socket, buf, sz, 0);
        if (sent < 0) return WOLFSSL_CBIO_ERR_GENERAL;
        return static_cast<int>(sent);
    }

    int Client::IORecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
        auto* self = static_cast<Client*>(ctx);

        if (self->m_IncomingEncryptedBuffer.empty())
            return WOLFSSL_CBIO_ERR_WANT_READ;

        int copySz = std::min(sz, static_cast<int>(self->m_IncomingEncryptedBuffer.size()));
        std::memcpy(buf, self->m_IncomingEncryptedBuffer.data(), copySz);
        self->m_IncomingEncryptedBuffer.erase(
            self->m_IncomingEncryptedBuffer.begin(),
            self->m_IncomingEncryptedBuffer.begin() + copySz);
        return copySz;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Network thread
    // ─────────────────────────────────────────────────────────────────────────────
    void Client::NetworkThreadFunc() {
        m_Running              = true;
        m_HandshakeFinished    = false;
        m_ConnectionStatus     = ConnectionStatus::Connecting;
        m_ConnectionDebugMessage.clear();

        // ── 1. Resolve address ──────────────────────────────────────────────────
        auto [hostPart, port] = SplitAddress(m_ServerAddress);
        if (port == 0) {
            OnFatalError(std::format("Could not parse port from '{}'", m_ServerAddress));
            return;
        }

        std::string ipStr = ResolveAddress(hostPart);
        if (ipStr.empty()) {
            OnFatalError(std::format("Could not resolve host '{}'", hostPart));
            return;
        }

        std::println("Connecting to {}:{}", ipStr, port);

        // ── 2. Create and connect UDP socket ────────────────────────────────────
        m_Socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_Socket < 0) {
            OnFatalError("Failed to create UDP socket");
            return;
        }

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port   = htons(port);
        if (::inet_pton(AF_INET, ipStr.c_str(), &server.sin_addr) != 1) {
            OnFatalError(std::format("inet_pton failed for '{}'", ipStr));
            return;
        }

        // "connect" on a UDP socket just sets the default peer so we can use
        // send()/recv() instead of sendto()/recvfrom(), and filters out packets
        // from other sources.
        if (::connect(m_Socket, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
            OnFatalError("UDP connect() failed");
            return;
        }

        // Non-blocking so our recv loop never hangs
        ::fcntl(m_Socket, F_SETFL, O_NONBLOCK);

        // ── 3. wolfSSL context + session ────────────────────────────────────────
        wolfSSL_Init();
        m_CTX = wolfSSL_CTX_new(wolfDTLSv1_3_client_method());
        if (!m_CTX) {
            OnFatalError("wolfSSL_CTX_new failed");
            return;
        }

        int groups[] = { WOLFSSL_ML_KEM_512 };
        wolfSSL_CTX_set_groups(m_CTX, groups, 1);

        wolfSSL_CTX_SetIOSend(m_CTX, IOSend);
        wolfSSL_CTX_SetIORecv(m_CTX, IORecv);

        // Skip certificate verification for now — add your CA cert in production:
        // wolfSSL_CTX_load_verify_locations(m_CTX, "ca-cert.pem", nullptr);
        wolfSSL_CTX_set_verify(m_CTX, WOLFSSL_VERIFY_NONE, nullptr);

        m_SSL = wolfSSL_new(m_CTX);
        if (!m_SSL) {
            OnFatalError("wolfSSL_new failed");
            return;
        }

        wolfSSL_SetIOReadCtx (m_SSL, this);
        wolfSSL_SetIOWriteCtx(m_SSL, this);
        wolfSSL_dtls_set_mtu (m_SSL, 1400);   // match server MTU

        // ── 4. Start the DTLS handshake ─────────────────────────────────────────
        // wolfSSL_connect() sends the ClientHello; the response will arrive later.
        {
            int ret = wolfSSL_connect(m_SSL);
            if (ret != WOLFSSL_SUCCESS) {
                int err = wolfSSL_get_error(m_SSL, ret);
                if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
                    OnFatalError(std::format("wolfSSL_connect initiation failed: {}", err));
                    return;
                }
            }
        }

        // Mark as "connecting at the UDP level" — the app-level Connected event
        // fires only after the DTLS handshake completes below.
        m_ConnectionStatus = ConnectionStatus::Connecting;

        constexpr size_t kMtu = 1500;
        uint8_t rawBuf[kMtu];

        // ── 5. Main loop ─────────────────────────────────────────────────────────
        while (m_Running) {
            // ── 5a. Receive incoming UDP datagrams ───────────────────────────────
            ssize_t len = ::recv(m_Socket, rawBuf, sizeof(rawBuf), 0);
            if (len > 0) {
                m_IncomingEncryptedBuffer.insert(
                    m_IncomingEncryptedBuffer.end(), rawBuf, rawBuf + len);
            }

            // ── 5b. Advance state machine ────────────────────────────────────────
            if (!m_HandshakeFinished) {
                // Keep driving wolfSSL_connect() until the handshake completes
                int ret = wolfSSL_connect(m_SSL);
                if (ret == WOLFSSL_SUCCESS) {
                    m_HandshakeFinished    = true;
                    m_ConnectionStatus     = ConnectionStatus::Connected;
                    std::println("PQC handshake complete!");

                    if (m_ServerConnectedCallback)
                        m_ServerConnectedCallback();
                } else {
                    int err = wolfSSL_get_error(m_SSL, ret);
                    if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
                        // Real failure
                        m_ConnectionDebugMessage = std::format("Handshake failed: err={}", err);
                        spdlog::error("{}", m_ConnectionDebugMessage);
                        m_ConnectionStatus = ConnectionStatus::FailedToConnect;
                        break;
                    }
                    // WANT_READ / WANT_WRITE → just wait for more packets
                }
            } else {
                // ── 5c. Drain decrypted application data ─────────────────────────
                char plaintext[4096];
                while (true) {
                    int bytes = wolfSSL_read(m_SSL, plaintext, sizeof(plaintext));
                    if (bytes > 0) {
                        if (m_DataReceivedCallback)
                            m_DataReceivedCallback(Buffer(plaintext, bytes));
                    } else {
                        int err = wolfSSL_get_error(m_SSL, bytes);
                        if (err == WOLFSSL_ERROR_ZERO_RETURN) {
                            // Server sent close_notify
                            spdlog::info("Server closed the connection");
                            m_Running = false;
                        }
                        break; // WANT_READ or error — try again next iteration
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // ── 6. Cleanup ───────────────────────────────────────────────────────────
        if (m_SSL) {
            wolfSSL_shutdown(m_SSL);
            wolfSSL_free(m_SSL);
            m_SSL = nullptr;
        }
        if (m_CTX) {
            wolfSSL_CTX_free(m_CTX);
            m_CTX = nullptr;
        }
        wolfSSL_Cleanup();

        ::close(m_Socket);
        m_Socket = -1;

        m_HandshakeFinished = false;
        m_ConnectionStatus  = ConnectionStatus::Disconnected;

        if (m_ServerDisconnectedCallback)
            m_ServerDisconnectedCallback();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Send
    // ─────────────────────────────────────────────────────────────────────────────
    void Client::SendBuffer(Buffer buf, bool /*reliable*/) {
        if (!m_SSL || !m_HandshakeFinished) {
            spdlog::warn("SendBuffer: PQC handshake not finished yet");
            return;
        }

        int ret = wolfSSL_write(m_SSL, buf.Data, static_cast<int>(buf.Size));
        if (ret <= 0)
            spdlog::error("wolfSSL_write failed: {}", wolfSSL_get_error(m_SSL, ret));
    }

    void Client::SendString(const std::string& str, bool reliable) {
        SendBuffer(Buffer(str.data(), str.size()), reliable);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Misc
    // ─────────────────────────────────────────────────────────────────────────────
    void Client::OnFatalError(const std::string& msg) {
        spdlog::critical("Client fatal error: {}", msg);
        m_ConnectionDebugMessage = msg;
        m_ConnectionStatus       = ConnectionStatus::FailedToConnect;
        m_Running                = false;
    }
} // namespace Safira