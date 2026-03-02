#ifndef PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H
#define PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <netinet/in.h>
#include "P2PCredentialGenerator.h"

// Botan forward-declaration — no namespace nesting needed for the channel base class
namespace Botan::TLS { class Channel; }

namespace Safira {

// P2PCallbacks is defined in the .cpp.
// It MUST be forward-declared here, inside namespace Safira, so that
// RunLoop's parameter type Safira::P2PCallbacks* matches the definition.
// If it were declared before the namespace the compiler would see ::P2PCallbacks
// (global) vs Safira::P2PCallbacks (definition) and refuse to match them.
class P2PCallbacks;

class PrivateChatSession {
public:
    explicit PrivateChatSession(std::string peerUsername);
    ~PrivateChatSession();

    PrivateChatSession(const PrivateChatSession&)            = delete;
    PrivateChatSession& operator=(const PrivateChatSession&) = delete;

    uint16_t StartAsResponder(P2PKeyType keyType = P2PKeyType::RSA_PSS);
    void     StartAsInitiator(const std::string& peerAddress);

    void Close();

    bool IsConnected() const { return m_Connected; }
    bool IsClosed()    const { return !m_Running;  }
    const std::string& GetPeerUsername() const { return m_PeerUsername; }

    void Send(const std::string& message);
    bool OnUIRender(const std::string& ownUsername, uint32_t ownColor);
    void AppendMessage(const std::string& who, const std::string& text,
                       uint32_t color = 0xFFFFFFFF);

private:
    void ResponderThreadFunc(P2PKeyType keyType);
    void InitiatorThreadFunc(std::string peerAddress);
    void RunLoop(Botan::TLS::Channel* channel, P2PCallbacks* callbacks);

    std::string       m_PeerUsername;
    int               m_Socket = -1;

    std::atomic<bool> m_Running  {false};
    std::atomic<bool> m_Connected{false};
    std::thread       m_Thread;

    std::vector<std::string> m_PendingOutbound;  // guarded by m_LogMutex

    struct LogEntry { std::string Who, Text; uint32_t Color; };
    std::vector<LogEntry> m_Log;
    std::mutex            m_LogMutex;
    char                  m_InputBuf[512] = {};
    bool                  m_WindowOpen     = true;
    bool                  m_ScrollToBottom = false;
};

} // namespace Safira

#endif //PQC_MASTER_THESIS_2026_PRIVATECHATSESSION_H