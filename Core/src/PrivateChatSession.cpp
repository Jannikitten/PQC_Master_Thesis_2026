#include "PrivateChatSession.h"
#include <charconv>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <spdlog/spdlog.h>
#include <imgui.h>

namespace Safira {

    struct ParsedAddr {
        std::string Host;
        uint16_t    Port = 0;
    };

    [[nodiscard]] static std::expected<ParsedAddr, P2PError> ParsePeerAddress(std::string_view addr) {
        const auto colon = addr.rfind(':');
        if (colon == std::string_view::npos) return std::unexpected(P2PError::AddressParse);

        auto host    = std::string(addr.substr(0, colon));
        auto portStr = addr.substr(colon + 1);

        uint16_t port = 0;
        auto [ptr, ec] = std::from_chars(portStr.data(), portStr.data() + portStr.size(), port);
        if (ec != std::errc{} || port == 0) return std::unexpected(P2PError::AddressParse);

        return ParsedAddr{ std::move(host), port };
    }

    std::expected<UniqueSocket, P2PError> PrivateChatSession::CreateListenSocket() {
        UniqueSocket sock{ ::socket(AF_INET, SOCK_STREAM, 0) };
        if (!sock) return std::unexpected(P2PError::SocketCreation);

        int yes = 1;
        ::setsockopt(sock.Get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in local{
            .sin_family = AF_INET,
            .sin_port   = 0,
            .sin_addr   = { .s_addr = INADDR_ANY },
            .sin_zero   = {},
        };

        if (::bind(sock.Get(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0)
            return std::unexpected(P2PError::SocketBind);

        if (::listen(sock.Get(), 1) < 0)
            return std::unexpected(P2PError::Listen);

        return sock;
    }

    std::expected<UniqueSocket, P2PError> PrivateChatSession::CreateAndConnectSocket(std::string_view ip, uint16_t port) {
        UniqueSocket sock{ ::socket(AF_INET, SOCK_STREAM, 0) };
        if (!sock) return std::unexpected(P2PError::SocketCreation);

        sockaddr_in server{
            .sin_family = AF_INET,
            .sin_port   = htons(port),
            .sin_addr   = {},
            .sin_zero   = {},
        };
        if (::inet_pton(AF_INET, std::string(ip).c_str(), &server.sin_addr) != 1)
            return std::unexpected(P2PError::AddressParse);

        if (::connect(sock.Get(), reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0)
            return std::unexpected(P2PError::TcpConnect);

        return sock;
    }

    template <typename Event>
    void PrivateChatSession::Dispatch(Event&& event) {
        std::lock_guard lock(m_StateMutex);
        m_State = Crypto::Reduce(std::move(m_State), std::forward<Event>(event));
    }

    PrivateChatSession::PrivateChatSession(std::string own, std::string peer)
        : m_PeerUsername(std::move(peer)), m_OwnUsername(std::move(own)) {}

    PrivateChatSession::~PrivateChatSession() { Close(); }

    void PrivateChatSession::Close() {
        Dispatch(Crypto::EventConnectionState{ .IsConnected = false, .IsRunning = false });
        Crypto::CloseTls(m_TlsState);
        m_Socket.Reset();

        // It is safe to stop the executor here because Close() is called from
        // the UI thread (Destructor) or explicit user action, NOT the network thread.
        m_NetworkExecutor.Stop();
    }

    void PrivateChatSession::Send(std::string message) {
        Dispatch(Crypto::EventMessageSent{ std::move(message) });
    }

    Crypto::TlsEffects PrivateChatSession::CreateChatEffects() {
        return Crypto::TlsEffects{
            .onEmitNetwork = [this](std::span<const uint8_t> data) {
                std::size_t total = 0;
                while (total < data.size()) {
                    const ssize_t sent = ::send(m_Socket.Get(), data.data() + total, data.size() - total, 0);
                    if (sent <= 0) return;
                    total += static_cast<std::size_t>(sent);
                }
            },
            .onMessageReceived = [this](std::span<const uint8_t> data) {
                std::string text(reinterpret_cast<const char*>(data.data()), data.size());
                Dispatch(Crypto::EventMessageReceived{ m_PeerUsername, std::move(text) });
            },
            .onHandshakeComplete = [this](bool firstUseTrusted, std::string fingerprint) {
                spdlog::info("[P2P] TLS 1.3 Handshake complete with {}", m_PeerUsername);
                Dispatch(Crypto::EventMessageReceived{ "System",
                    std::format("Connected to {} (Botan TLS 1.3)",
                     m_PeerUsername), 0xFF66CC66
                });
                if (firstUseTrusted) {
                    Dispatch(Crypto::EventMessageReceived{ "System",
                        std::format("First trusted fingerprint: {}",
                         fingerprint), 0xFFCCAA66
                    });
                }
                Dispatch(Crypto::EventConnectionState{ .IsConnected = true, .IsRunning = true });
            },
            .onSystemLog = [this](std::string msg, uint32_t color, bool isClose) {
                Dispatch(Crypto::EventMessageReceived{ "System", std::move(msg), color });
                if (isClose) Dispatch(Crypto::EventConnectionState{ .IsConnected = false, .IsRunning = false });
            }
        };
    }

    uint16_t PrivateChatSession::StartAsResponder(Crypto::P2PKeyType keyType) {
        if (IsRunning()) {
            spdlog::warn("[P2P] StartAsResponder called while already running");
            return 0;
        }
        m_NetworkExecutor.Stop();

        auto listenResult = CreateListenSocket();
        if (!listenResult) {
            spdlog::error("[P2P] Responder: {}", Describe(listenResult.error()));
            return 0;
        }

        sockaddr_in assigned{};
        socklen_t len = sizeof(assigned);
        ::getsockname(listenResult->Get(), reinterpret_cast<sockaddr*>(&assigned), &len);
        const uint16_t port = ntohs(assigned.sin_port);

        m_Socket = std::move(*listenResult);
        spdlog::info("[P2P] Responder (TCP) listening on port {}", port);
        Dispatch(Crypto::EventMessageReceived{ "System",
            std::format("Waiting for {} — TCP port {} (Botan TLS 1.3)...",
             m_PeerUsername, port), 0xFF888888
        });
        Dispatch(Crypto::EventConnectionState{ .IsConnected = false, .IsRunning = true });

        if (!m_NetworkExecutor.Start("p2p-session") ||
            !m_NetworkExecutor.Post([this, keyType] { ResponderThreadFunc(keyType); })) {
            spdlog::error("[P2P] failed to schedule responder loop");
            Close();
            return 0;
        }
        return port;
    }

    void PrivateChatSession::StartAsInitiator(std::string_view peerAddress) {
        if (IsRunning()) {
            spdlog::warn("[P2P] StartAsInitiator called while already running");
            return;
        }

        m_NetworkExecutor.Stop();
        Dispatch(Crypto::EventConnectionState{ .IsConnected = false, .IsRunning = true });

        if (!m_NetworkExecutor.Start("p2p-session") ||
            !m_NetworkExecutor.Post([this, addr = std::string(peerAddress)] { InitiatorThreadFunc(addr); })) {
            spdlog::error("[P2P] failed to schedule initiator loop");
            Close();
        }
    }

    void PrivateChatSession::ResponderThreadFunc(Crypto::P2PKeyType keyType) {
        const int listenFd = m_Socket.Get();
        if (listenFd < 0) return;
        ::fcntl(listenFd, F_SETFL, O_NONBLOCK);

        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        int connFd = -1;

        while (IsRunning()) {
            connFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (connFd >= 0) break;
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            break;
        }

        if (connFd < 0 || !IsRunning()) return;

        m_Socket = UniqueSocket(connFd);
        ::fcntl(m_Socket.Get(), F_SETFL, O_NONBLOCK);

        const auto km = Crypto::GenerateOrLoadP2PCredentials(m_OwnUsername, keyType);
        if (auto stateResult = Crypto::CreateServerState(km, m_PeerUsername, CreateChatEffects())) {
            m_TlsState = std::move(stateResult.value());
            RunLoop();
        } else {
            Dispatch(Crypto::EventMessageReceived{
                "System",
                "TLS initialization failed. Please check your Botan cryptography implementation.",
                0xFF4444FF });
        }

        // Safely mark the thread as done without triggering a self-join deadlock
        Dispatch(Crypto::EventConnectionState{ .IsConnected = false, .IsRunning = false });
    }

    void PrivateChatSession::InitiatorThreadFunc(std::string peerAddress) {
        auto result = ParsePeerAddress(peerAddress)
            .and_then([this](const ParsedAddr& addr) {
                Dispatch(Crypto::EventMessageReceived{ "System",
                    std::format("Connecting to {} at {}:{} (Botan TLS 1.3)...",
                        m_PeerUsername, addr.Host, addr.Port), 0xFF888888
                });
                return CreateAndConnectSocket(addr.Host, addr.Port)
                    .transform([&addr](UniqueSocket sock) {
                        return std::make_pair(std::move(sock), addr);
                    });
            })
            .and_then([this](auto pair) {
                auto& [sock, addr] = pair;
                m_Socket = std::move(sock);
                ::fcntl(m_Socket.Get(), F_SETFL, O_NONBLOCK);
                return Crypto::CreateClientState(addr.Host, addr.Port, m_PeerUsername, CreateChatEffects())
                    // Botan crypto implementation not finished or correct
                    .transform_error([](Crypto::TlsError) { return P2PError::TlsInit; });
            });

        if (result) {
            m_TlsState = std::move(result.value());
            RunLoop();
        } else {
            Dispatch(Crypto::EventMessageReceived{ "System",
                std::format("Connection failed: {}",
                    Describe(result.error())), 0xFF4444FF
            });
        }

        // Safely mark the thread as done without triggering a self-join deadlock
        Dispatch(Crypto::EventConnectionState{ .IsConnected = false, .IsRunning = false });
    }

    void PrivateChatSession::RunLoop() {
        std::array<uint8_t, 16384> recvBuf{};

        while (IsRunning() && !Crypto::IsClosed(m_TlsState)) {
            const ssize_t len = ::recv(m_Socket.Get(), recvBuf.data(), recvBuf.size(), 0);

            if (len > 0) {
                Crypto::ProcessEncryptedData(m_TlsState, std::span<const uint8_t>(recvBuf.data(), len));
            } else if (len == 0) {
                Dispatch(Crypto::EventMessageReceived{ "System", std::format("{} disconnected.", m_PeerUsername), 0xFF888888 });
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
                if (errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE) {
                    Dispatch(Crypto::EventMessageReceived{ "System", std::format("{} disconnected.", m_PeerUsername), 0xFF888888 });
                }
                break;
            }

            std::vector<std::string> pending;
            {
                std::lock_guard lock(m_StateMutex);
                pending = m_State.PendingOutbound;
                if (!pending.empty()) m_State = Crypto::Reduce(std::move(m_State), Crypto::EventMessagesFlushed{});
            }

            for (const auto& msg : pending) {
                Crypto::EncryptAndSend(m_TlsState, msg);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void PrivateChatSession::AppendMessage(const std::string& who, const std::string& text, uint32_t color) {
        Dispatch(Crypto::EventMessageReceived{ who, text, color });
    }

    std::vector<ChatEntry>* PrivateChatSession::RefreshAndGetChatEntries(const std::string& ownUsername) {
        std::lock_guard lock(m_StateMutex);
        m_CachedEntries = Crypto::TransformLogToEntries(m_State.Log, ownUsername);
        return &m_CachedEntries;
    }

    bool PrivateChatSession::OnUIRender(const std::string& ownUsername, uint32_t) {
        if (!m_WindowOpen) return false;

        Crypto::ChatState stateSnapshot;
        {
            std::lock_guard lock(m_StateMutex);
            stateSnapshot = m_State;
        }

        auto entries = Crypto::TransformLogToEntries(stateSnapshot.Log, ownUsername);

        ImGui::SetNextWindowSize({ 520, 440 }, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(std::format("Private chat — {}###pc_{}", m_PeerUsername, m_PeerUsername).c_str(), &m_WindowOpen)) {

            m_ChatPanel.RenderChatArea(entries, ownUsername, m_PeerUsername, stateSnapshot.IsConnected, stateSnapshot.IsRunning && !stateSnapshot.IsConnected);

            if (auto msg = m_ChatPanel.ConsumePendingMessage()) {
                Send(*msg);
                Dispatch(Crypto::EventMessageReceived{ ownUsername, *msg, 0xFFFFFFFF });
            }
        }
        ImGui::End();
        return m_WindowOpen;
    }

} // namespace Safira