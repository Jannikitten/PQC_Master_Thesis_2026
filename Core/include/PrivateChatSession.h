#ifndef PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H
#define PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H

// ═════════════════════════════════════════════════════════════════════════════
// PrivateChatSession.h — peer-to-peer TLS 1.3 chat via Botan
//                        (X25519/ML-KEM-768 key exchange)
//
// Refactored following Kleppmann & Hugenroth, "Cryptography and Protocol
// Engineering", Cambridge P79, Lent 2025.
//
//  §3.2  Result types     – std::expected for the responder init pipeline
//  §5.5  Secret lifetimes – UniqueSocket RAII wrapper for file descriptors
//  §5.5  Opinionated API  – Send takes string_view, not const string&
//  C++23                  – std::array, string_view, from_chars
// ═════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include "P2PCredentialGenerator.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <netinet/in.h>

namespace Botan::TLS { class Channel; }

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// §5.5 — RAII socket wrapper  (Slides 222-224)
//
// Every error path in the original code risked leaking a file descriptor.
// UniqueSocket ensures ::close() is called exactly once, and the
// .Release() escape hatch supports the responder's listen→accept swap.
// ─────────────────────────────────────────────────────────────────────────────
class UniqueSocket {
public:
    UniqueSocket() = default;
    explicit UniqueSocket(int fd) noexcept : m_Fd(fd) {}
    ~UniqueSocket() { Reset(); }

    UniqueSocket(const UniqueSocket&)            = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;

    UniqueSocket(UniqueSocket&& other) noexcept
        : m_Fd(other.m_Fd) { other.m_Fd = -1; }

    UniqueSocket& operator=(UniqueSocket&& other) noexcept {
        if (this != &other) { Reset(); m_Fd = other.m_Fd; other.m_Fd = -1; }
        return *this;
    }

    [[nodiscard]] int    Get()   const noexcept { return m_Fd; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_Fd >= 0; }

    /// Release ownership — returns the raw fd without closing it.
    int Release() noexcept { int fd = m_Fd; m_Fd = -1; return fd; }

    /// Close the fd and reset to -1.
    void Reset() noexcept;

private:
    int m_Fd = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
// §3.2 — Result types for the responder init pipeline
// ─────────────────────────────────────────────────────────────────────────────
enum class P2PError : uint8_t {
    SocketCreation,
    SocketBind,
    Listen,
    AddressParse,
    TcpConnect,
};

[[nodiscard]] constexpr std::string_view Describe(P2PError e) noexcept {
    switch (e) {
        case P2PError::SocketCreation: return "socket() failed";
        case P2PError::SocketBind:     return "bind() failed";
        case P2PError::Listen:         return "listen() failed";
        case P2PError::AddressParse:   return "could not parse peer address";
        case P2PError::TcpConnect:     return "TCP connect() failed";
    }
    return "unknown error";
}

// Forward-declared inside namespace Safira so RunLoop's parameter type matches.
class P2PCallbacks;

// ─────────────────────────────────────────────────────────────────────────────
// PrivateChatSession
// ─────────────────────────────────────────────────────────────────────────────
class PrivateChatSession {
public:
    explicit PrivateChatSession(std::string peerUsername);
    ~PrivateChatSession();

    PrivateChatSession(const PrivateChatSession&)            = delete;
    PrivateChatSession& operator=(const PrivateChatSession&) = delete;

    uint16_t StartAsResponder(P2PKeyType keyType = P2PKeyType::RSA_PSS);
    void     StartAsInitiator(std::string_view peerAddress);

    void Close();

    [[nodiscard]] bool IsConnected() const noexcept { return m_Connected.load(std::memory_order_acquire); }
    [[nodiscard]] bool IsClosed()    const noexcept { return !m_Running.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& GetPeerUsername() const noexcept { return m_PeerUsername; }

    void Send(std::string_view message);
    bool OnUIRender(const std::string& ownUsername, uint32_t ownColor);
    void AppendMessage(const std::string& who, const std::string& text,
                       uint32_t color = 0xFFFFFFFF);

private:
    // §3.2 — Pure-ish init helpers returning std::expected.
    [[nodiscard]] static std::expected<UniqueSocket, P2PError>
        CreateListenSocket();

    [[nodiscard]] static std::expected<UniqueSocket, P2PError>
        CreateAndConnectSocket(std::string_view ip, uint16_t port);

    void ResponderThreadFunc(P2PKeyType keyType);
    void InitiatorThreadFunc(std::string peerAddress);
    void RunLoop(Botan::TLS::Channel* channel, P2PCallbacks* callbacks);

    // ── State ────────────────────────────────────────────────────────────────
    std::string   m_PeerUsername;
    UniqueSocket  m_Socket;

    std::atomic<bool> m_Running   { false };
    std::atomic<bool> m_Connected { false };
    std::thread       m_Thread;

    std::vector<std::string> m_PendingOutbound;   // guarded by m_LogMutex

    struct LogEntry {
        std::string Who;
        std::string Text;
        uint32_t    Color;
    };
    std::vector<LogEntry> m_Log;
    std::mutex            m_LogMutex;

    std::array<char, 512> m_InputBuf {};
    bool                  m_WindowOpen     = true;
    bool                  m_ScrollToBottom = false;
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H