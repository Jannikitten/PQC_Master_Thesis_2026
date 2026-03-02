#include "PrivateChatSession.h"

// Use short-form includes — CMake adds botan/install/include/botan-3 to the
// include path via the Botan::Botan target, so <botan/xyz.h> resolves cleanly.
// Never use relative ../botan/install/... paths in source files.
#include <botan/auto_rng.h>
#include <botan/certstor.h>
#include <botan/data_src.h>
#include <botan/pkcs8.h>
#include <botan/tls.h>
#include <botan/tls_callbacks.h>
#include <botan/tls_channel.h>
#include <botan/tls_client.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_server_info.h>
#include <botan/tls_session_manager_memory.h>
#include <botan/x509cert.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>
#include <imgui.h>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// PQ TLS 1.3 Policy
// ─────────────────────────────────────────────────────────────────────────────
class PQPolicy : public Botan::TLS::Default_Policy {
public:
    Botan::TLS::Protocol_Version min_version() const {
        return Botan::TLS::Protocol_Version::TLS_V13;
    }
    std::vector<Botan::TLS::Group_Params> key_exchange_groups() const override {
        return { Botan::TLS::Group_Params::HYBRID_X25519_ML_KEM_768 };
    }
    std::vector<Botan::TLS::Group_Params> key_exchange_groups_to_offer() const override {
        return { Botan::TLS::Group_Params::HYBRID_X25519_ML_KEM_768 };
    }
    bool require_cert_revocation_info() const override { return false; }
};

// ─────────────────────────────────────────────────────────────────────────────
// ServerCredentials
//
// Botan 3 API changes vs Botan 2:
//   • cert_chain()       → find_cert_chain()
//   • Private_Key*       → std::shared_ptr<Private_Key>   (private_key_for)
// ─────────────────────────────────────────────────────────────────────────────
class ServerCredentials : public Botan::Credentials_Manager {
public:
    ServerCredentials(const std::string& certPath, const std::string& keyPath) {
        Botan::DataSource_Stream certSrc(certPath);
        m_Cert = std::make_unique<Botan::X509_Certificate>(certSrc);

        Botan::DataSource_Stream keySrc(keyPath);
        // load_key returns unique_ptr<Private_Key>; convert to shared_ptr so
        // we can return it from private_key_for() without a use-after-free.
        m_Key = Botan::PKCS8::load_key(keySrc);
    }

    // Botan 3: method is find_cert_chain, not cert_chain
    std::vector<Botan::X509_Certificate> find_cert_chain(
        const std::vector<std::string>& /*cert_key_types*/,
        const std::vector<Botan::AlgorithmIdentifier>& /*cert_sig_schemes*/,
        const std::string& /*type*/,
        const std::string& /*context*/)
    {
        return { *m_Cert };
    }

    // Botan 3: must return shared_ptr<Private_Key>, NOT Private_Key*
    std::shared_ptr<Botan::Private_Key> private_key_for(
        const Botan::X509_Certificate& /*cert*/,
        const std::string& /*type*/,
        const std::string& /*context*/) override
    {
        return m_Key;
    }

private:
    std::unique_ptr<Botan::X509_Certificate> m_Cert;
    std::shared_ptr<Botan::Private_Key>      m_Key;  // shared_ptr matches return type
};

// ─────────────────────────────────────────────────────────────────────────────
// ClientCredentials — Phase 1: no cert verification
// ─────────────────────────────────────────────────────────────────────────────
class ClientCredentials : public Botan::Credentials_Manager {
public:
    std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(
        const std::string& /*type*/, const std::string& /*context*/) override
    {
        return {};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// P2PCallbacks
//
// Defined inside namespace Safira — matches the forward-declaration in the
// header.  If this were in the global namespace, RunLoop's P2PCallbacks*
// parameter would refer to ::P2PCallbacks (global, incomplete) rather than
// Safira::P2PCallbacks (this class), and the compiler would reject it.
// ─────────────────────────────────────────────────────────────────────────────
class P2PCallbacks : public Botan::TLS::Callbacks {
public:
    P2PCallbacks(int socket, PrivateChatSession& owner, std::string peerUsername)
        : m_Socket(socket), m_Owner(owner), m_PeerUsername(std::move(peerUsername)) {}

    void tls_emit_data(std::span<const uint8_t> data) override {
        ssize_t total = 0;
        while (total < static_cast<ssize_t>(data.size())) {
            ssize_t sent = ::send(m_Socket, data.data() + total, data.size() - total, 0);
            if (sent <= 0) { spdlog::error("[P2P] tls_emit_data: send() failed"); return; }
            total += sent;
        }
    }

    void tls_record_received(uint64_t /*seq_no*/,
                              std::span<const uint8_t> data) override {
        m_Owner.AppendMessage(m_PeerUsername,
            std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    }

    void tls_alert(Botan::TLS::Alert alert) override {
        if (alert.type() == Botan::TLS::Alert::CloseNotify) {
            m_Owner.AppendMessage("System",
                std::format("{} closed the connection.", m_PeerUsername), 0xFF888888);
            m_CloseNotifyReceived = true;
        } else {
            spdlog::warn("[P2P] TLS alert: {}", alert.type_string());
        }
    }

    void tls_session_activated() override {
        spdlog::info("[P2P] TLS 1.3 + X25519/ML-KEM-768 handshake complete with {}", m_PeerUsername);
        m_Owner.AppendMessage("System",
            std::format("Connected to {} (Botan TLS 1.3 | X25519/ML-KEM-768)", m_PeerUsername),
            0xFF66CC66);
        m_Activated = true;
    }

    // Phase 1: accept any cert without verification
    // TODO Phase 2: remove this and provide a real trust store in ClientCredentials
    void tls_verify_cert_chain(
        const std::vector<Botan::X509_Certificate>& /*chain*/,
        const std::vector<std::optional<Botan::OCSP::Response>>& /*ocsp*/,
        const std::vector<Botan::Certificate_Store*>& /*trusted*/,
        Botan::Usage_Type /*usage*/,
        const std::string& /*hostname*/,
        const Botan::TLS::Policy& /*policy*/) {}

    bool IsActivated()           const { return m_Activated;           }
    bool IsCloseNotifyReceived() const { return m_CloseNotifyReceived; }

private:
    int                 m_Socket;
    PrivateChatSession& m_Owner;
    std::string         m_PeerUsername;
    bool                m_Activated           = false;
    bool                m_CloseNotifyReceived = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────
static std::pair<std::string, uint16_t> SplitAddr(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos) return {addr, 0};
    uint16_t port = 0;
    try { port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1))); }
    catch (...) {}
    return {addr.substr(0, colon), port};
}

// ─────────────────────────────────────────────────────────────────────────────
// Ctor / Dtor
// ─────────────────────────────────────────────────────────────────────────────
PrivateChatSession::PrivateChatSession(std::string peer)
    : m_PeerUsername(std::move(peer)) {}

PrivateChatSession::~PrivateChatSession() { Close(); }

// ─────────────────────────────────────────────────────────────────────────────
// StartAsResponder
// ─────────────────────────────────────────────────────────────────────────────
uint16_t PrivateChatSession::StartAsResponder(const std::string& certPath,
                                              const std::string& keyPath) {
    m_Socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_Socket < 0) { spdlog::error("[P2P] socket() failed"); return 0; }

    int yes = 1;
    ::setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = 0;
    local.sin_addr.s_addr = INADDR_ANY;
    ::bind(m_Socket, reinterpret_cast<sockaddr*>(&local), sizeof(local));
    ::listen(m_Socket, 1);

    sockaddr_in assigned{};
    socklen_t len = sizeof(assigned);
    ::getsockname(m_Socket, reinterpret_cast<sockaddr*>(&assigned), &len);
    uint16_t port = ntohs(assigned.sin_port);

    spdlog::info("[P2P] Responder (TCP) listening on port {}", port);
    AppendMessage("System",
        std::format("Waiting for {} — TCP port {} (Botan TLS 1.3)...", m_PeerUsername, port),
        0xFF888888);

    m_Running = true;
    m_Thread  = std::thread([this, certPath, keyPath] { ResponderThreadFunc(certPath, keyPath); });
    return port;
}

// ─────────────────────────────────────────────────────────────────────────────
// StartAsInitiator
// ─────────────────────────────────────────────────────────────────────────────
void PrivateChatSession::StartAsInitiator(const std::string& peerAddress) {
    m_Running = true;
    m_Thread  = std::thread([this, peerAddress] { InitiatorThreadFunc(peerAddress); });
}

// ─────────────────────────────────────────────────────────────────────────────
// Close
// ─────────────────────────────────────────────────────────────────────────────
void PrivateChatSession::Close() {
    m_Running = false;
    if (m_Socket >= 0) ::shutdown(m_Socket, SHUT_RDWR);
    if (m_Thread.joinable()) m_Thread.join();
    if (m_Socket >= 0) { ::close(m_Socket); m_Socket = -1; }
    m_Connected = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Send
// ─────────────────────────────────────────────────────────────────────────────
void PrivateChatSession::Send(const std::string& message) {
    if (!m_Connected) { spdlog::warn("[P2P] Send before handshake complete"); return; }
    std::lock_guard lock(m_LogMutex);
    m_PendingOutbound.push_back(message);
}

// ─────────────────────────────────────────────────────────────────────────────
// ResponderThreadFunc
// ─────────────────────────────────────────────────────────────────────────────
void PrivateChatSession::ResponderThreadFunc(std::string certPath, std::string keyPath) {
    int listenSock = m_Socket;
    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    int connSock = ::accept(listenSock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    ::close(listenSock);
    m_Socket = connSock;

    if (connSock < 0 || !m_Running) { m_Running = false; return; }

    char peerIp[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp));
    spdlog::info("[P2P] TCP accepted from {}:{}", peerIp, ntohs(peer.sin_port));

    try {
        auto rng         = std::make_shared<Botan::AutoSeeded_RNG>();
        auto callbacks   = std::make_shared<P2PCallbacks>(connSock, *this, m_PeerUsername);
        auto session_mgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(rng);
        auto creds       = std::make_shared<ServerCredentials>(certPath, keyPath);
        auto policy      = std::make_shared<PQPolicy>();

        Botan::TLS::Server channel(callbacks, session_mgr, creds, policy, rng, false);
        RunLoop(&channel, callbacks.get());
    } catch (const std::exception& ex) {
        spdlog::error("[P2P] Responder: {}", ex.what());
        AppendMessage("System", std::format("Error: {}", ex.what()), 0xFF4444FF);
    }

    m_Connected = false;
    m_Running   = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// InitiatorThreadFunc
// ─────────────────────────────────────────────────────────────────────────────
void PrivateChatSession::InitiatorThreadFunc(std::string peerAddress) {
    auto [ip, port] = SplitAddr(peerAddress);
    if (port == 0 || ip.empty()) {
        spdlog::error("[P2P] Invalid peer address: {}", peerAddress);
        m_Running = false; return;
    }

    m_Socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_Socket < 0) { m_Running = false; return; }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port   = htons(port);
    ::inet_pton(AF_INET, ip.c_str(), &server.sin_addr);

    AppendMessage("System",
        std::format("Connecting to {} at {}:{} (Botan TLS 1.3)...", m_PeerUsername, ip, port),
        0xFF888888);

    if (::connect(m_Socket, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
        spdlog::error("[P2P] TCP connect to {} failed", peerAddress);
        AppendMessage("System", "TCP connect failed.", 0xFF4444FF);
        m_Running = false; return;
    }

    try {
        auto rng         = std::make_shared<Botan::AutoSeeded_RNG>();
        auto callbacks   = std::make_shared<P2PCallbacks>(m_Socket, *this, m_PeerUsername);
        auto session_mgr = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(rng);
        auto creds       = std::make_shared<ClientCredentials>();
        auto policy      = std::make_shared<PQPolicy>();

        Botan::TLS::Client channel(callbacks, session_mgr, creds, policy, rng,
                                   Botan::TLS::Server_Information(ip, port));
        RunLoop(&channel, callbacks.get());
    } catch (const std::exception& ex) {
        spdlog::error("[P2P] Initiator: {}", ex.what());
        AppendMessage("System", std::format("Error: {}", ex.what()), 0xFF4444FF);
    }

    m_Connected = false;
    m_Running   = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// RunLoop
// ─────────────────────────────────────────────────────────────────────────────
void PrivateChatSession::RunLoop(Botan::TLS::Channel* channel, P2PCallbacks* callbacks) {
    constexpr int kBufSize = 16384;
    uint8_t recvBuf[kBufSize];

    ::fcntl(m_Socket, F_SETFL, O_NONBLOCK);

    while (m_Running && !channel->is_closed()) {
        ssize_t len = ::recv(m_Socket, recvBuf, sizeof(recvBuf), 0);
        if (len > 0) {
            channel->received_data(std::span<const uint8_t>(recvBuf, len));
        } else if (len == 0) {
            AppendMessage("System", std::format("{} disconnected.", m_PeerUsername), 0xFF888888);
            break;
        }

        if (!m_Connected && callbacks->IsActivated())
            m_Connected = true;

        if (m_Connected) {
            std::vector<std::string> pending;
            { std::lock_guard lock(m_LogMutex); pending.swap(m_PendingOutbound); }
            for (const auto& msg : pending)
                channel->send(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        }

        if (callbacks->IsCloseNotifyReceived()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    try { channel->close(); } catch (...) {}
    ::close(m_Socket);
    m_Socket    = -1;
    m_Connected = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// AppendMessage
// ─────────────────────────────────────────────────────────────────────────────
void PrivateChatSession::AppendMessage(const std::string& who,
                                       const std::string& text,
                                       uint32_t color) {
    std::lock_guard lock(m_LogMutex);
    m_Log.push_back({who, text, color});
    m_ScrollToBottom = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// OnUIRender
// ─────────────────────────────────────────────────────────────────────────────
bool PrivateChatSession::OnUIRender(const std::string& ownUsername, uint32_t /*ownColor*/) {
    if (!m_WindowOpen) return false;

    std::string title = std::format("Private chat — {}###pc_{}", m_PeerUsername, m_PeerUsername);
    ImGui::SetNextWindowSize({480, 400}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &m_WindowOpen)) { ImGui::End(); return m_WindowOpen; }

    if (m_Connected)
        ImGui::TextColored({0.3f, 0.9f, 0.3f, 1.0f}, "● Connected  (Botan TLS 1.3 | X25519/ML-KEM-768)");
    else if (m_Running)
        ImGui::TextColored({0.9f, 0.8f, 0.2f, 1.0f}, "● Handshaking...");
    else
        ImGui::TextColored({0.8f, 0.3f, 0.3f, 1.0f}, "● Disconnected");
    ImGui::Separator();

    float inputH = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    ImGui::BeginChild("##pc_log", {0, -inputH}, true);
    {
        std::lock_guard lock(m_LogMutex);
        for (const auto& e : m_Log) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImColor(e.Color).Value);
            ImGui::TextUnformatted((e.Who + ": ").c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(e.Text.c_str());
            ImGui::PopStyleColor();
        }
        if (m_ScrollToBottom) { ImGui::SetScrollHereY(1.0f); m_ScrollToBottom = false; }
    }
    ImGui::EndChild();

    bool sendNow = false;
    ImGui::SetNextItemWidth(-60.0f);
    if (ImGui::InputText("##pc_input", m_InputBuf, sizeof(m_InputBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) sendNow = true;
    ImGui::SameLine();
    if (ImGui::Button("Send")) sendNow = true;

    if (sendNow && m_InputBuf[0] != '\0') {
        std::string msg(m_InputBuf);
        Send(msg);
        AppendMessage(ownUsername, msg, 0xFFFFFFFF);
        m_InputBuf[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::End();
    return m_WindowOpen;
}

} // namespace Safira