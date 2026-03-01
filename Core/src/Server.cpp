#include "Server.h"

#include <arpa/inet.h>    // inet_ntop
#include <sys/socket.h>
#include <unistd.h>       // close()
#include <fcntl.h>        // fcntl / O_NONBLOCK
#include <cstring>
#include <format>
#include <print>
#include <spdlog/spdlog.h>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// A small helper passed through wolfSSL's I/O context so the static callbacks
// can reach both the per-client state and the shared UDP socket.
// ─────────────────────────────────────────────────────────────────────────────
struct IOContext {
    ClientInfo* Client = nullptr;   // packet buffer + remote addr
    int         Socket = -1;        // the server's shared UDP socket
};

// ─────────────────────────────────────────────────────────────────────────────
// Ctor / Dtor
// ─────────────────────────────────────────────────────────────────────────────
Server::Server(int port) : m_Port(port) {}

Server::~Server() {
    Stop();
    if (m_NetworkThread.joinable())
        m_NetworkThread.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public interface
// ─────────────────────────────────────────────────────────────────────────────
void Server::Start() {
    if (m_Running) return;
    m_NetworkThread = std::thread([this] { NetworkThreadFunc(); });
}

void Server::Stop() {
    m_Running = false;
}

void Server::SetDataReceivedCallback(const DataReceivedCallback& fn)       { m_DataReceivedCallback       = fn; }
void Server::SetClientConnectedCallback(const ClientConnectedCallback& fn) { m_ClientConnectedCallback    = fn; }
void Server::SetClientDisconnectedCallback(const ClientDisconnectedCallback& fn) { m_ClientDisconnectedCallback = fn; }

// ─────────────────────────────────────────────────────────────────────────────
// wolfSSL custom I/O  (static)
// ─────────────────────────────────────────────────────────────────────────────
int Server::IORecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
    auto* io     = static_cast<IOContext*>(ctx);
    auto& client = *io->Client;

    if (client.EncryptedBuffer.empty())
        return WOLFSSL_CBIO_ERR_WANT_READ;

    int copySz = std::min(sz, static_cast<int>(client.EncryptedBuffer.size()));
    std::memcpy(buf, client.EncryptedBuffer.data(), copySz);
    client.EncryptedBuffer.erase(
        client.EncryptedBuffer.begin(),
        client.EncryptedBuffer.begin() + copySz);
    return copySz;
}

int Server::IOSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
    auto* io     = static_cast<IOContext*>(ctx);
    auto& client = *io->Client;

    ssize_t sent = ::sendto(
        io->Socket, buf, sz, 0,
        reinterpret_cast<const sockaddr*>(&client.Addr),
        sizeof(client.Addr));

    if (sent < 0) return WOLFSSL_CBIO_ERR_GENERAL;
    return static_cast<int>(sent);
}

// ─────────────────────────────────────────────────────────────────────────────
// Network thread
// ─────────────────────────────────────────────────────────────────────────────
void Server::NetworkThreadFunc() {
    m_Running = true;

    // ── 1. Create UDP socket ────────────────────────────────────────────────
    m_Socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_Socket < 0) {
        OnFatalError("Failed to create UDP socket");
        return;
    }

    // Allow address reuse so we can restart quickly during development
    int yes = 1;
    ::setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // Non-blocking so our recvfrom loop doesn't hang
    ::fcntl(m_Socket, F_SETFL, O_NONBLOCK);

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(static_cast<uint16_t>(m_Port));
    local.sin_addr.s_addr = INADDR_ANY;

    if (::bind(m_Socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        OnFatalError(std::format("Failed to bind UDP socket on port {}", m_Port));
        return;
    }

    std::println("Server listening on port {}", m_Port);

    // ── 2. wolfSSL context ──────────────────────────────────────────────────
    wolfSSL_Init();
    m_CTX = wolfSSL_CTX_new(wolfDTLSv1_3_server_method());
    if (!m_CTX) {
        OnFatalError("wolfSSL_CTX_new failed");
        return;
    }

    // PQC: ML-KEM-512 key exchange
    // Bump the MTU — ML-KEM-512 public key is 800 B, ciphertext 768 B.
    // 1400 keeps us under typical Ethernet MTU (1500) after IP+UDP headers.
    constexpr int kDtlsMtu = 1400;

    int groups[] = { WOLFSSL_ML_KEM_512 };
    wolfSSL_CTX_set_groups(m_CTX, groups, 1);

    // Register our custom I/O callbacks at the context level
    wolfSSL_CTX_SetIORecv(m_CTX, IORecv);
    wolfSSL_CTX_SetIOSend(m_CTX, IOSend);

    // ── Authentication ──────────────────────────────────────────────────────
    //
    // PHASE 1 (now): Classical RSA certificate so the connection works while
    //                ML-KEM-512 key exchange is being validated.
    //
    //   Generate once:
    //     openssl req -x509 -newkey rsa:2048 -keyout server-key.pem \
    //       -out server.pem -days 3650 -nodes -subj "/CN=safira-server"
    //
    // PHASE 2 (TODO): Replace with ML-DSA-65 for fully post-quantum auth.
    //   Steps:
    //     1. Generate ML-DSA-65 key pair:
    //          wolfssl-keygen -mldsa65 -out server-mldsa.pem -outkey server-mldsa-key.pem
    //        or via wolfSSL's keygen example / oqs-provider openssl CLI.
    //     2. Swap the two CTX calls below to use the ML-DSA files.
    //     3. Un-comment wolfSSL_CTX_set_sigalgs(m_CTX, "ML-DSA-65") so wolfSSL
    //        advertises only the PQ signature scheme in the CertificateRequest.
    //     4. Update Client.cpp to load the same CA cert for verification instead
    //        of WOLFSSL_VERIFY_NONE.
    //
    if (wolfSSL_CTX_use_certificate_file(m_CTX, "server.pem", WOLFSSL_FILETYPE_PEM)
            != WOLFSSL_SUCCESS) {
        OnFatalError("Failed to load server.pem — see Phase 1 openssl command above");
        return;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(m_CTX, "server-key.pem", WOLFSSL_FILETYPE_PEM)
            != WOLFSSL_SUCCESS) {
        OnFatalError("Failed to load server-key.pem — see Phase 1 openssl command above");
        return;
    }
    // TODO PHASE 2: uncomment once ML-DSA cert files are generated
    // wolfSSL_CTX_set_sigalgs(m_CTX, "ML-DSA-65");

    // ── 3. Main event loop ──────────────────────────────────────────────────
    constexpr size_t kMtu = 1500;
    uint8_t rawBuf[kMtu];

    while (m_Running) {
        // ── 3a. Receive incoming UDP datagrams ──────────────────────────────
        sockaddr_in from{};
        socklen_t   fromLen = sizeof(from);

        ssize_t len = ::recvfrom(
            m_Socket, rawBuf, sizeof(rawBuf), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);

        if (len > 0) {
            DispatchPacket(from, rawBuf, static_cast<int>(len));
        }

        // ── 3b. Drive wolfSSL for every active client ───────────────────────
        for (auto& [id, client] : m_Clients) {
            DriveClient(client);
        }

        // ── 3c. Reap disconnected clients ───────────────────────────────────
        //
        // Build the removal list first to avoid mutating the map during iteration.
        std::vector<ClientID> toRemove;
        for (auto& [id, client] : m_Clients) {
            if (!client.SSL) toRemove.push_back(id);
        }
        for (ClientID id : toRemove) RemoveClient(id);

        // Yield to avoid spinning the CPU when idle
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // ── 4. Shutdown ──────────────────────────────────────────────────────────
    std::println("Server shutting down, closing {} client(s)...", m_Clients.size());

    for (auto& [id, client] : m_Clients) {
        if (client.SSL) {
            wolfSSL_shutdown(client.SSL);
            wolfSSL_free(client.SSL);
            client.SSL = nullptr;
        }
        if (m_ClientDisconnectedCallback)
            m_ClientDisconnectedCallback(client);
    }
    m_Clients.clear();

    wolfSSL_CTX_free(m_CTX);
    wolfSSL_Cleanup();
    m_CTX = nullptr;

    ::close(m_Socket);
    m_Socket  = -1;
    m_Running = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// DispatchPacket — route an incoming datagram to the right client (or create
// a new one if we haven't seen this (ip, port) pair before).
// ─────────────────────────────────────────────────────────────────────────────
void Server::DispatchPacket(const sockaddr_in& from, const uint8_t* data, int len) {
    ClientID id = MakeClientID(from.sin_addr.s_addr, from.sin_port);

    auto it = m_Clients.find(id);
    if (it == m_Clients.end()) {
        // ── New client ───────────────────────────────────────────────────────
        ClientInfo& client = m_Clients[id];
        client.ID   = id;
        client.Addr = from;

        char addrStr[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from.sin_addr, addrStr, sizeof(addrStr));
        client.AddressStr = std::string(addrStr) + ":" + std::to_string(ntohs(from.sin_port));

        // Create and configure a wolfSSL session for this client
        client.SSL = wolfSSL_new(m_CTX);
        if (!client.SSL) {
            spdlog::error("wolfSSL_new failed for {}", client.AddressStr);
            m_Clients.erase(id);
            return;
        }

        // Store the I/O context on the heap; it lives as long as the ClientInfo
        // (we could embed it in ClientInfo — this is fine for now).
        auto* ioCtx     = new IOContext{ &client, m_Socket };
        wolfSSL_SetIOReadCtx (client.SSL, ioCtx);
        wolfSSL_SetIOWriteCtx(client.SSL, ioCtx);

        wolfSSL_dtls_set_mtu(client.SSL, m_kDtlsMtu);  // must be set before handshake

        // Push the first datagram into the buffer before calling accept
        client.EncryptedBuffer.insert(client.EncryptedBuffer.end(), data, data + len);

        spdlog::info("New connection from {}", client.AddressStr);

        // Start the DTLS server handshake.
        // WOLFSSL_ERROR_WANT_READ is expected — we haven't received the full
        // ClientHello yet (or the cookie-verified second ClientHello).
        int ret = wolfSSL_accept(client.SSL);
        if (ret != WOLFSSL_SUCCESS) {
            int err = wolfSSL_get_error(client.SSL, ret);
            if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
                spdlog::error("wolfSSL_accept error {} for {}", err, client.AddressStr);
                wolfSSL_free(client.SSL);
                client.SSL = nullptr;
                m_Clients.erase(id);
            }
        }
    } else {
        // ── Existing client: feed the packet and let DriveClient handle it ──
        it->second.EncryptedBuffer.insert(
            it->second.EncryptedBuffer.end(), data, data + len);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DriveClient — advance the DTLS state machine and drain decrypted app data
// ─────────────────────────────────────────────────────────────────────────────
void Server::DriveClient(ClientInfo& client) {
    if (!client.SSL) return;

    // ── A. If handshake isn't finished yet, keep driving it ─────────────────
    if (!wolfSSL_is_init_finished(client.SSL)) {
        int ret = wolfSSL_accept(client.SSL);
        if (ret != WOLFSSL_SUCCESS) {
            int err = wolfSSL_get_error(client.SSL, ret);
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
                return; // normal — waiting for more packets

            spdlog::error("Handshake failed (err={}) for {}", err, client.AddressStr);
            client.SSL = nullptr; // flagged for removal
            return;
        }

        // Handshake just completed!
        spdlog::info("PQC handshake complete for {}", client.AddressStr);
        if (m_ClientConnectedCallback)
            m_ClientConnectedCallback(client);
        return; // start reading on the next iteration
    }

    // ── B. Handshake done — drain decrypted application data ────────────────
    char plaintext[4096];
    while (true) {
        int bytes = wolfSSL_read(client.SSL, plaintext, sizeof(plaintext));
        if (bytes > 0) {
            if (m_DataReceivedCallback)
                m_DataReceivedCallback(client, Buffer(plaintext, bytes));
        } else {
            int err = wolfSSL_get_error(client.SSL, bytes);
            if (err == WOLFSSL_ERROR_ZERO_RETURN) {
                // Peer sent a close_notify
                spdlog::info("Client {} disconnected cleanly", client.AddressStr);
                client.SSL = nullptr; // flagged for removal
            }
            // WANT_READ / WANT_WRITE are normal — no more data right now
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RemoveClient
// ─────────────────────────────────────────────────────────────────────────────
void Server::RemoveClient(ClientID id) {
    auto it = m_Clients.find(id);
    if (it == m_Clients.end()) return;

    ClientInfo& client = it->second;

    if (client.SSL) {
        wolfSSL_shutdown(client.SSL);
        wolfSSL_free(client.SSL);
        client.SSL = nullptr;
    }

    // Free the IOContext we allocated in DispatchPacket
    void* readCtx = wolfSSL_GetIOReadCtx(client.SSL ? client.SSL : nullptr);
    delete static_cast<IOContext*>(readCtx);

    if (m_ClientDisconnectedCallback)
        m_ClientDisconnectedCallback(client);

    m_Clients.erase(it);
}

// ─────────────────────────────────────────────────────────────────────────────
// Send helpers
// ─────────────────────────────────────────────────────────────────────────────
void Server::SendBufferToClient(ClientID id, Buffer buf, bool /*reliable*/) {
    auto it = m_Clients.find(id);
    if (it == m_Clients.end()) return;

    ClientInfo& client = it->second;
    if (!client.SSL || !wolfSSL_is_init_finished(client.SSL)) {
        spdlog::warn("Attempted to send to {} before handshake finished", client.AddressStr);
        return;
    }

    int ret = wolfSSL_write(client.SSL, buf.Data, static_cast<int>(buf.Size));
    if (ret <= 0)
        spdlog::error("wolfSSL_write failed for {}", client.AddressStr);
}

void Server::SendBufferToAllClients(Buffer buf, ClientID exclude, bool reliable) {
    for (auto& [id, client] : m_Clients) {
        if (id != exclude)
            SendBufferToClient(id, buf, reliable);
    }
}

void Server::SendStringToClient(ClientID id, const std::string& str, bool reliable) {
    SendBufferToClient(id, Buffer(str.data(), str.size()), reliable);
}

void Server::SendStringToAllClients(const std::string& str, ClientID exclude, bool reliable) {
    SendBufferToAllClients(Buffer(str.data(), str.size()), exclude, reliable);
}

void Server::KickClient(ClientID id) {
    auto it = m_Clients.find(id);
    if (it == m_Clients.end()) return;

    wolfSSL_shutdown(it->second.SSL);
    RemoveClient(id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Misc
// ─────────────────────────────────────────────────────────────────────────────
void Server::OnFatalError(const std::string& msg) {
    spdlog::critical("Server fatal error: {}", msg);
    m_Running = false;
}

// Expose kDtlsMtu for use in NetworkThreadFunc without polluting the header
constexpr int kDtlsMtu = 1400;

} // namespace Safira