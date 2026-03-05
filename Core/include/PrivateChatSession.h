#ifndef PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H
#define PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H

// PrivateChatSession.h -- peer-to-peer TLS 1.3 chat via Botan
//                         (X25519/ML-KEM-768 key exchange)

#include <algorithm>
#include "P2PCredentialGenerator.h"
#include "ChatPanel.h"
#include "NetworkExecutor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <netinet/in.h>

namespace Botan::TLS { class Channel; }

namespace Safira {

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

    int Release() noexcept { int fd = m_Fd; m_Fd = -1; return fd; }
    void Reset() noexcept;

private:
    int m_Fd = -1;
};

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

class P2PCallbacks;

class PrivateChatSession {
public:
    PrivateChatSession(std::string ownUsername, std::string peerUsername);
    ~PrivateChatSession();

    PrivateChatSession(const PrivateChatSession&)            = delete;
    PrivateChatSession& operator=(const PrivateChatSession&) = delete;

    uint16_t StartAsResponder(P2PKeyType keyType = P2PKeyType::RSA_PSS);
    void     StartAsInitiator(std::string_view peerAddress);

    void Close();

    [[nodiscard]] bool IsConnected() const noexcept { return m_Connected.load(std::memory_order_acquire); }
    [[nodiscard]] bool IsRunning() const noexcept { return m_Running.load(std::memory_order_acquire); }
    [[nodiscard]] bool IsClosed()    const noexcept { return !m_Running.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& GetPeerUsername() const noexcept { return m_PeerUsername; }

    void Send(std::string_view message);
    std::vector<ChatEntry> BuildChatEntries(const std::string& ownUsername) const;
    bool OnUIRender(const std::string& ownUsername, uint32_t ownColor);
    void AppendMessage(const std::string& who, const std::string& text,
                       uint32_t color = 0xFFFFFFFF);

    /// Thread-safe: locks m_LogMutex, rebuilds cached entries, returns pointer.
    /// The returned pointer is stable until the next call to this method.
    /// Used by ClientLayer::RebuildConversationList().
    std::vector<ChatEntry>* RefreshAndGetChatEntries(const std::string& ownUsername);

private:
    [[nodiscard]] static std::expected<UniqueSocket, P2PError>
        CreateListenSocket();

    [[nodiscard]] static std::expected<UniqueSocket, P2PError>
        CreateAndConnectSocket(std::string_view ip, uint16_t port);

    void ResponderThreadFunc(P2PKeyType keyType);
    void InitiatorThreadFunc(std::string peerAddress);
    void RunLoop(Botan::TLS::Channel* channel, P2PCallbacks* callbacks);

    std::string   m_PeerUsername;
    std::string   m_OwnUsername;
    UniqueSocket  m_Socket;

    std::atomic<bool> m_Running   { false };
    std::atomic<bool> m_Connected { false };
    NetworkExecutor   m_NetworkExecutor;

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
    ChatPanel              m_ChatPanel;
    std::vector<ChatEntry> m_CachedEntries;
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H
