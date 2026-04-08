#include "P2PSession.h"
#include "BotanP2PCrypto.h"

// ═════════════════════════════════════════════════════════════════════════════
// When BotanP2PCrypto is available and initialised, the TCP stream is
// wrapped in TLS 1.3 (X25519/ML-KEM-768).  Otherwise, messages are sent
// as plaintext over TCP and a warning is displayed.
// ═════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <format>
#include <mutex>
#include <ranges>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // UniqueSocket
    // ─────────────────────────────────────────────────────────────────────────────

    void UniqueSocket::Reset() noexcept {
        if (m_Fd >= 0) {
            ::close(m_Fd);
            m_Fd = -1;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Address parsing
    // ─────────────────────────────────────────────────────────────────────────────

    struct ParsedAddr {
        std::string Host;
        uint16_t    Port = 0;
    };

    [[nodiscard]]
    static std::expected<ParsedAddr, P2PError> ParsePeerAddress(std::string_view addr) {
        const auto colon = addr.rfind(':');
        if (colon == std::string_view::npos)
            return std::unexpected(P2PError::AddressParse);

        auto host    = std::string(addr.substr(0, colon));
        auto portStr = addr.substr(colon + 1);

        uint16_t port = 0;
        auto [ptr, ec] = std::from_chars(portStr.data(), portStr.data() + portStr.size(), port);
        if (ec != std::errc{} || port == 0)
            return std::unexpected(P2PError::AddressParse);

        return ParsedAddr{ std::move(host), port };
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Socket helpers
    // ─────────────────────────────────────────────────────────────────────────────

    std::expected<UniqueSocket, P2PError>
    PrivateChatSession::CreateListenSocket() {
        UniqueSocket sock{ ::socket(AF_INET, SOCK_STREAM, 0) };
        if (!sock)
            return std::unexpected(P2PError::SocketCreation);

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

    std::expected<UniqueSocket, P2PError>
    PrivateChatSession::CreateAndConnectSocket(std::string_view ip, uint16_t port) {
        UniqueSocket sock{ ::socket(AF_INET, SOCK_STREAM, 0) };
        if (!sock)
            return std::unexpected(P2PError::SocketCreation);

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

    // ─────────────────────────────────────────────────────────────────────────────
    // Ctor / Dtor
    // ─────────────────────────────────────────────────────────────────────────────

    PrivateChatSession::PrivateChatSession(std::string own, std::string peer)
        : m_PeerUsername(std::move(peer))
        , m_OwnUsername(std::move(own)) {}

    PrivateChatSession::~PrivateChatSession() { Close(); }

    // ─────────────────────────────────────────────────────────────────────────────
    // SetupEncryption — tries to create and start BotanP2PCrypto
    // ─────────────────────────────────────────────────────────────────────────────

    bool PrivateChatSession::SetupEncryption(bool isResponder) {
        try {
            m_Crypto = std::make_unique<BotanP2PCrypto>();

            BotanP2PCrypto::Config cfg{
                .SocketFd    = m_Socket.Get(),
                .IsResponder = isResponder,
                .OwnUsername = m_OwnUsername,
                .PeerUsername = m_PeerUsername,
            };

            BotanP2PCrypto::Callbacks cbs{
                .EmitData = [this](std::span<const uint8_t> data) {
                    SendRaw(data);
                },
                .MessageReceived = [this](const std::string& from,
                                          const std::string& text,
                                          uint32_t color) {
                    AppendMessage(from, text, color);
                },
                .Activated = [this]() {
                    m_Connected.store(true, std::memory_order_release);
                    AppendMessage("System",
                        std::format("Encrypted connection established with {} (TLS 1.3 | X25519/ML-KEM-768)",
                                    m_PeerUsername),
                        0xFF66CC66);
                },
                .Closed = [this](const std::string& msg) {
                    AppendMessage("System", msg, 0xFF888888);
                },
            };

            m_Crypto->Start(cfg, std::move(cbs));
            m_Encrypted.store(true, std::memory_order_release);
            return true;
        } catch (const std::exception& ex) {
            spdlog::warn("[P2P] Encryption setup failed: {} — falling back to plaintext", ex.what());
            m_Crypto.reset();
            m_Encrypted.store(false, std::memory_order_release);
            return false;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // SendRaw — write raw bytes to the TCP socket
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::SendRaw(std::span<const uint8_t> data) {
        std::size_t total = 0;
        while (total < data.size()) {
            const ssize_t sent = ::send(
                m_Socket.Get(),
                data.data() + total,
                data.size() - total, 0);
            if (sent <= 0) {
                spdlog::error("[P2P] SendRaw: send() failed");
                return;
            }
            total += static_cast<std::size_t>(sent);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // StartAsResponder
    // ─────────────────────────────────────────────────────────────────────────────

    uint16_t PrivateChatSession::StartAsResponder() {
        if (m_Running.load(std::memory_order_acquire)) {
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
        AppendMessage("System",
            std::format("Waiting for {} — TCP port {}...", m_PeerUsername, port),
            0xFF888888);

        m_Running.store(true, std::memory_order_release);
        if (!m_NetworkExecutor.Start("p2p-session")) {
            spdlog::error("[P2P] failed to start network executor");
            m_Running.store(false, std::memory_order_release);
            m_Socket.Reset();
            return 0;
        }
        if (!m_NetworkExecutor.Post([this] { ResponderThreadFunc(); })) {
            spdlog::error("[P2P] failed to schedule responder loop");
            m_Running.store(false, std::memory_order_release);
            m_Socket.Reset();
            m_NetworkExecutor.Stop();
            return 0;
        }
        return port;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // StartAsInitiator
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::StartAsInitiator(std::string_view peerAddress) {
        if (m_Running.load(std::memory_order_acquire)) {
            spdlog::warn("[P2P] StartAsInitiator called while already running");
            return;
        }
        m_NetworkExecutor.Stop();

        m_Running.store(true, std::memory_order_release);
        if (!m_NetworkExecutor.Start("p2p-session")) {
            spdlog::error("[P2P] failed to start network executor");
            m_Running.store(false, std::memory_order_release);
            return;
        }
        if (!m_NetworkExecutor.Post([this, addr = std::string(peerAddress)] {
            InitiatorThreadFunc(addr);
        })) {
            spdlog::error("[P2P] failed to schedule initiator loop");
            m_Running.store(false, std::memory_order_release);
            m_NetworkExecutor.Stop();
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Close
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::Close() {
        m_Running.store(false, std::memory_order_release);
        m_Socket.Reset();
        m_NetworkExecutor.Stop();
        m_Connected.store(false, std::memory_order_release);
        m_Encrypted.store(false, std::memory_order_release);
        m_Crypto.reset();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Send
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::Send(std::string_view message) {
        if (!m_Connected.load(std::memory_order_acquire)) {
            spdlog::warn("[P2P] Send before connection ready");
            return;
        }
        std::lock_guard lock(m_LogMutex);
        m_PendingOutbound.emplace_back(message);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // ResponderThreadFunc
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::ResponderThreadFunc() {
        const int listenFd = m_Socket.Get();
        if (listenFd < 0) {
            m_Running.store(false, std::memory_order_release);
            return;
        }
        ::fcntl(listenFd, F_SETFL, O_NONBLOCK);

        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        int connFd = -1;

        while (m_Running.load(std::memory_order_acquire)) {
            connFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (connFd >= 0) break;

            const int err = errno;
            if (err == EWOULDBLOCK || err == EAGAIN || err == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (err != EBADF && err != EINVAL)
                spdlog::error("[P2P] accept() failed (errno={})", err);
            break;
        }

        if (connFd < 0 || !m_Running.load(std::memory_order_acquire)) {
            m_Running.store(false, std::memory_order_release);
            return;
        }

        m_Socket.Reset();
        m_Socket = UniqueSocket(connFd);

        std::array<char, INET_ADDRSTRLEN> peerIp{};
        ::inet_ntop(AF_INET, &peer.sin_addr, peerIp.data(), peerIp.size());
        spdlog::info("[P2P] TCP accepted from {}:{}", peerIp.data(), ntohs(peer.sin_port));

        // Try to set up encryption — falls back to plaintext on failure.
        if (!SetupEncryption(/*isResponder=*/true)) {
            // Plaintext mode — immediately connected.
            m_Connected.store(true, std::memory_order_release);
            AppendMessage("System",
                "WARNING: This chat is NOT encrypted! Messages are sent in plaintext.",
                0xFFFF4444);
        }

        RunLoop();

        m_Connected.store(false, std::memory_order_release);
        m_Running.store(false, std::memory_order_release);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // InitiatorThreadFunc
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::InitiatorThreadFunc(std::string peerAddress) {
        auto parsed = ParsePeerAddress(peerAddress);
        if (!parsed) {
            spdlog::error("[P2P] {}: '{}'", Describe(parsed.error()), peerAddress);
            m_Running.store(false, std::memory_order_release);
            return;
        }

        AppendMessage("System",
            std::format("Connecting to {} at {}:{}...",
                        m_PeerUsername, parsed->Host, parsed->Port),
            0xFF888888);

        auto sockResult = CreateAndConnectSocket(parsed->Host, parsed->Port);
        if (!sockResult) {
            spdlog::error("[P2P] TCP connect to {} failed", peerAddress);
            AppendMessage("System", "TCP connect failed.", 0xFF4444FF);
            m_Running.store(false, std::memory_order_release);
            return;
        }

        m_Socket = std::move(*sockResult);

        // Try to set up encryption — falls back to plaintext on failure.
        if (!SetupEncryption(/*isResponder=*/false)) {
            m_Connected.store(true, std::memory_order_release);
            AppendMessage("System",
                "WARNING: This chat is NOT encrypted! Messages are sent in plaintext.",
                0xFFFF4444);
        }

        RunLoop();

        m_Connected.store(false, std::memory_order_release);
        m_Running.store(false, std::memory_order_release);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // RunLoop — works in both encrypted (via m_Crypto) and plaintext mode
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::RunLoop() {
        std::array<uint8_t, 16384> recvBuf{};

        ::fcntl(m_Socket.Get(), F_SETFL, O_NONBLOCK);

        while (m_Running.load(std::memory_order_acquire)) {
            const ssize_t len = ::recv(m_Socket.Get(), recvBuf.data(), recvBuf.size(), 0);

            if (len > 0) {
                auto data = std::span<const uint8_t>(recvBuf.data(), static_cast<std::size_t>(len));

                if (m_Crypto) {
                    // Encrypted: feed raw bytes into TLS engine.
                    m_Crypto->FeedReceivedData(data);
                } else {
                    // Plaintext: data IS the message.
                    AppendMessage(m_PeerUsername,
                        std::string(reinterpret_cast<const char*>(data.data()), data.size()));
                }
            } else if (len == 0) {
                AppendMessage("System",
                    std::format("{} disconnected.", m_PeerUsername), 0xFF888888);
                break;
            } else {
                const int err = errno;
                if (err == EWOULDBLOCK || err == EAGAIN || err == EINTR) {
                    // No data this tick.
                } else if (err == ECONNRESET || err == ENOTCONN || err == EPIPE) {
                    AppendMessage("System",
                        std::format("{} disconnected.", m_PeerUsername), 0xFF888888);
                    break;
                } else if ((err == EBADF || err == ENOTSOCK) &&
                           !m_Running.load(std::memory_order_acquire)) {
                    break;
                } else {
                    spdlog::warn("[P2P] recv() failed (errno={})", err);
                    break;
                }
            }

            // Check if crypto handshake completed.
            if (m_Crypto && !m_Connected.load(std::memory_order_acquire)
                && m_Crypto->IsActivated()) {
                m_Connected.store(true, std::memory_order_release);
            }

            // Drain outbound queue.
            if (m_Connected.load(std::memory_order_acquire)) {
                std::vector<std::string> pending;
                { std::lock_guard lock(m_LogMutex); pending.swap(m_PendingOutbound); }

                for (const auto& msg : pending) {
                    if (m_Crypto) {
                        m_Crypto->Send(msg);
                    } else {
                        // Plaintext: send directly.
                        SendRaw(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(msg.data()), msg.size()));
                    }
                }
            }

            if (m_Crypto && m_Crypto->IsCloseNotifyReceived()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Graceful close.
        if (m_Crypto && m_Socket) {
            m_Crypto->Close();
        }

        m_Socket.Reset();
        m_Connected.store(false, std::memory_order_release);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // AppendMessage
    // ─────────────────────────────────────────────────────────────────────────────

    void PrivateChatSession::AppendMessage(const std::string& who,
                                           const std::string& text,
                                           uint32_t color) {
        std::lock_guard lock(m_LogMutex);
        m_Log.push_back({ who, text, color });
        m_ScrollToBottom = true;
    }

    std::vector<ChatEntry>* PrivateChatSession::RefreshAndGetChatEntries(const std::string& ownUsername) {
        std::lock_guard lock(m_LogMutex);
        m_CachedEntries = BuildChatEntries(ownUsername);
        return &m_CachedEntries;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // BuildChatEntries / OnUIRender
    // ─────────────────────────────────────────────────────────────────────────────

    std::vector<ChatEntry> PrivateChatSession::BuildChatEntries(const std::string& ownUsername) const {
        std::vector<ChatEntry> entries;
        entries.reserve(m_Log.size());

        for (const auto& e : m_Log) {
            MessageRole role;
            if (e.Who == "System")
                role = MessageRole::System;
            else if (e.Who == ownUsername)
                role = MessageRole::Own;
            else
                role = MessageRole::Peer;

            entries.push_back({
                .Who   = e.Who,
                .Text  = e.Text,
                .Color = e.Color,
                .Role  = role,
                .Time  = {},
            });
        }
        return entries;
    }

    bool PrivateChatSession::OnUIRender(const std::string& ownUsername, uint32_t /*ownColor*/) {
        if (!m_WindowOpen) return false;

        const std::string title = std::format(
            "Private chat — {}###pc_{}", m_PeerUsername, m_PeerUsername);

        ImGui::SetNextWindowSize({ 520, 440 }, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &m_WindowOpen)) {
            ImGui::End();
            return m_WindowOpen;
        }

        {
            std::lock_guard lock(m_LogMutex);
            m_CachedEntries = BuildChatEntries(ownUsername);
        }

        const bool connected   = m_Connected.load(std::memory_order_acquire);
        const bool handshaking = m_Running.load(std::memory_order_acquire) && !connected;

        m_ChatPanel.RenderChatArea(
            m_CachedEntries, ownUsername, m_PeerUsername,
            connected, handshaking);

        if (auto msg = m_ChatPanel.ConsumePendingMessage()) {
            Send(*msg);
            AppendMessage(ownUsername, *msg, 0xFFFFFFFF);
        }

        ImGui::End();
        return m_WindowOpen;
    }

} // namespace Safira
