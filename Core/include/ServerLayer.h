#ifndef PQC_MASTER_THESIS_2026_SERVER_LAYER_H
#define PQC_MASTER_THESIS_2026_SERVER_LAYER_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerLayer.h — application-level server logic
//
// Sits above the DTLS transport (Server.h) and implements:
//   - User registration and validation
//   - Chat message relay + history persistence (YAML)
//   - Private chat invite / accept / decline brokering
//   - Admin commands (/kick, /mute, /list, /stats, /broadcast, /motd, /help)
//   - Rate limiting and flood protection
//   - MOTD (message of the day)
//
// Types reused from the Safira library:
//   UserInfo      (UserInfo.h)     — per-client metadata
//   ChatMessage   (UserInfo.h)     — username + message pair for history
//   ClientID      (Common.h)       — strong client identifier
//   ClientInfo    (Server.h)       — per-connection transport state
//   ByteSpan      (Types.h)        — non-owning byte view
// ═════════════════════════════════════════════════════════════════════════════

#include "Server.h"         // Server, ServerConfig, ClientInfo, ClientID, ClientMetrics
#include "ServerPacket.h"   // packet types, BufferWriter, UserInfo, ChatMessage
#include "Layer.h"          // Safira::Layer base class
#include "Console.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Per-client rate-limit state  (sliding window)
// ─────────────────────────────────────────────────────────────────────────────
struct RateLimitEntry {
    std::vector<std::chrono::steady_clock::time_point> Timestamps;
    int Violations = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ServerLayer
// ─────────────────────────────────────────────────────────────────────────────
class ServerLayer : public Safira::Layer {
public:
    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float ts) override;
    void OnUIRender() override;

    void Quit();
    bool KickUser(std::string_view username, std::string_view reason);

private:
    // ── Server callbacks ────────────────────────────────────────────────────
    void OnClientConnected   (Safira::ClientInfo& clientInfo);
    void OnClientDisconnected(Safira::ClientInfo& clientInfo);
    void OnDataReceived      (Safira::ClientInfo& clientInfo, Safira::ByteSpan data);

    // ── Private chat brokering ──────────────────────────────────────────────
    void CleanupPendingInvites(Safira::ClientID disconnectedID,
                               const std::string& disconnectedUsername);
    void ForwardPrivateChatInvite   (Safira::ClientID targetID,    const std::string& fromUsername);
    void ForwardPrivateChatConnectTo(Safira::ClientID initiatorID, const std::string& responderUsername,
                                     const std::string& responderIPAndPort);
    void ForwardPrivateChatDeclined (Safira::ClientID initiatorID, const std::string& responderUsername);

    // ── Send helpers ────────────────────────────────────────────────────────
    void SendClientList                  (const Safira::ClientInfo& clientInfo);
    void SendClientListToAllClients      ();
    void SendClientConnect               (const Safira::ClientInfo& newClient);
    void SendClientDisconnect            (const Safira::ClientInfo& clientInfo);
    void SendClientConnectionRequestResponse(const Safira::ClientInfo& clientInfo, bool response);
    void SendMessageToAllClients         (const Safira::ClientInfo& from, std::string_view message);
    void SendMessageHistory              (const Safira::ClientInfo& clientInfo);
    void SendServerShutdownToAllClients  ();
    void SendClientKick                  (const Safira::ClientInfo& clientInfo, std::string_view reason);
    void SendChatMessage                 (std::string_view message);
    void EnqueueEvent(std::function<void()>&& fn);
    void DrainQueuedEvents();

    // ── Commands ────────────────────────────────────────────────────────────
    void OnCommand(std::string_view command);

    // ── Rate limiting ───────────────────────────────────────────────────────
    bool IsRateLimited(Safira::ClientID id);

    // ── Username validation ─────────────────────────────────────────────────
    [[nodiscard]] std::optional<std::string> ValidateUsername(const std::string& username) const;
    [[nodiscard]] bool IsValidUsername(const std::string& username) const;

    // ── Lookup helpers ──────────────────────────────────────────────────────
    [[nodiscard]] const std::string& GetClientUsername(Safira::ClientID id) const;
    [[nodiscard]] uint32_t           GetClientColor   (Safira::ClientID id) const;
    [[nodiscard]] std::optional<Safira::ClientID> FindClientID(const std::string& username) const;

    // ── Persistence ─────────────────────────────────────────────────────────
    void SaveMessageHistoryToFile(const std::filesystem::path& filepath);
    bool LoadMessageHistoryFromFile(const std::filesystem::path& filepath);

    // ── Members ─────────────────────────────────────────────────────────────
    std::unique_ptr<Safira::Server> m_Server;
    Console                         m_Console;
    std::vector<uint8_t>            m_ScratchBuffer;

    std::unordered_map<Safira::ClientID, Safira::UserInfo> m_ConnectedClients;
    std::unordered_map<Safira::ClientID, Safira::ClientID> m_PendingPrivateChatInvites; // responder -> initiator
    std::unordered_map<Safira::ClientID, RateLimitEntry>   m_RateLimitState;
    std::unordered_set<std::string>                        m_MutedUsers;
    std::mutex                                             m_EventMutex;
    std::queue<std::function<void()>>                      m_PendingEvents;

    std::string m_Motd;

    // ChatMessage from UserInfo.h — reused directly for history persistence.
    std::vector<Safira::ChatMessage>  m_MessageHistory;
    std::filesystem::path             m_MessageHistoryFilePath;

    float m_ClientListTimer = 0.0f;
    static constexpr float kClientListInterval = 5.0f;
};

#endif // PQC_MASTER_THESIS_2026_SERVER_LAYER_H
