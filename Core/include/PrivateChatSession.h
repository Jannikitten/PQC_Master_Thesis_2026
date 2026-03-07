#ifndef PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H
#define PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H

#include "BotanCryptography.h"
#include "PrivateChatCore.h"
#include "NetworkExecutor.h"
#include "ChatPanel.h"

#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <netinet/in.h>
#include <unistd.h>

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
    void Reset() noexcept {
        if (m_Fd >= 0) { ::close(m_Fd); m_Fd = -1; }
    }

private:
    int m_Fd = -1;
};

enum class P2PError : uint8_t {
    SocketCreation,
    SocketBind,
    Listen,
    AddressParse,
    TcpConnect,
    TlsInit
};

[[nodiscard]] constexpr std::string_view Describe(P2PError e) noexcept {
    switch (e) {
        case P2PError::SocketCreation: return "socket() failed";
        case P2PError::SocketBind:     return "bind() failed";
        case P2PError::Listen:         return "listen() failed";
        case P2PError::AddressParse:   return "could not parse peer address";
        case P2PError::TcpConnect:     return "TCP connect() failed";
        case P2PError::TlsInit:        return "TLS initialization failed";
    }
    return "unknown error";
}

class PrivateChatSession {
public:
    PrivateChatSession(std::string ownUsername, std::string peerUsername);
    ~PrivateChatSession();

    PrivateChatSession(const PrivateChatSession&)            = delete;
    PrivateChatSession& operator=(const PrivateChatSession&) = delete;

    uint16_t StartAsResponder(Crypto::P2PKeyType keyType = Crypto::P2PKeyType::RSA_PSS);
    void     StartAsInitiator(std::string_view peerAddress);
    void     Close();

    void Send(std::string message);
    bool OnUIRender(const std::string& ownUsername, uint32_t ownColor);

    [[nodiscard]] bool IsConnected() const noexcept { std::lock_guard lock(m_StateMutex); return m_State.IsConnected; }
    [[nodiscard]] bool IsRunning() const noexcept { std::lock_guard lock(m_StateMutex); return m_State.IsRunning; }
    [[nodiscard]] bool IsClosed() const noexcept { std::lock_guard lock(m_StateMutex); return !m_WindowOpen; }
    [[nodiscard]] const std::string& GetPeerUsername() const noexcept { return m_PeerUsername; }

    void AppendMessage(const std::string& who, const std::string& text, uint32_t color = 0xFFFFFFFF);
    std::vector<ChatEntry>* RefreshAndGetChatEntries(const std::string& ownUsername);

private:
    [[nodiscard]] static std::expected<UniqueSocket, P2PError> CreateListenSocket();
    [[nodiscard]] static std::expected<UniqueSocket, P2PError> CreateAndConnectSocket(std::string_view ip, uint16_t port);

    template <typename Event>
    void Dispatch(Event&& event);

    [[nodiscard]] Crypto::TlsEffects CreateChatEffects();

    void InitiatorThreadFunc(std::string peerAddress);
    void ResponderThreadFunc(Crypto::P2PKeyType keyType);
    void RunLoop();

    std::string         m_PeerUsername;
    std::string         m_OwnUsername;
    UniqueSocket        m_Socket;
    Crypto::TlsState    m_TlsState;
    NetworkExecutor     m_NetworkExecutor;

    mutable std::mutex  m_StateMutex;
    Crypto::ChatState   m_State;

    bool                   m_WindowOpen = true; // Kept thread-safe via lock_guard where needed
    ChatPanel              m_ChatPanel;
    std::vector<ChatEntry> m_CachedEntries;
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H