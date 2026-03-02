#ifndef PQC_MASTER_THESIS_2026_SERVERLAYER_H
#define PQC_MASTER_THESIS_2026_SERVERLAYER_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerLayer.h — application layer sitting above Server
//
// Refactored following Kleppmann & Hugenroth, "Cryptography and Protocol
// Engineering", Cambridge P79, Lent 2025.
//
//  §3.2  Result types    – FindClientID returns std::optional<ClientID>,
//                          not a bare 0 sentinel.
//  §3.4  Strong types    – ClientID is a struct; no implicit int comparisons.
//  §5.3  Serialization   – packet builder helper (BuildPacket) centralises
//                          the scratch-buffer pattern to one call site.
//  C++23                 – ranges, string_view, structured bindings.
// ═════════════════════════════════════════════════════════════════════════════

#include "Layer.h"
#include "Server.h"
#include "Console.h"
#include "UserInfo.h"

#include <filesystem>
#include <map>
#include <optional>

class ServerLayer : public Safira::Layer {
public:
    void OnAttach()          override;
    void OnDetach()          override;
    void OnUpdate(float ts)  override;
    void OnUIRender()        override;

private:
    // ── Server event callbacks (signatures match refactored Server) ──────────
    void OnClientConnected   (Safira::ClientInfo& clientInfo);
    void OnClientDisconnected(Safira::ClientInfo& clientInfo);
    void OnDataReceived      (Safira::ClientInfo& clientInfo, Safira::Buffer buffer);

    // ── Incoming message handlers ───────────────────────────────────────────
    void OnMessageReceived        (const Safira::ClientInfo&, std::string_view message);
    void OnClientConnectionRequest(const Safira::ClientInfo&, uint32_t userColor, std::string_view username);
    void OnClientUpdate           (const Safira::ClientInfo&, uint32_t userColor, std::string_view username);

    // ── Outgoing packets ────────────────────────────────────────────────────
    void SendClientList                     (const Safira::ClientInfo& clientInfo);
    void SendClientListToAllClients         ();
    void SendClientConnect                  (const Safira::ClientInfo& clientInfo);
    void SendClientDisconnect               (const Safira::ClientInfo& clientInfo);
    void SendClientConnectionRequestResponse(const Safira::ClientInfo&, bool response);
    void SendClientUpdateResponse           (const Safira::ClientInfo& clientInfo);
    void SendMessageToAllClients            (const Safira::ClientInfo& from, std::string_view message);
    void SendMessageHistory                 (const Safira::ClientInfo& clientInfo);
    void SendServerShutdownToAllClients     ();
    void SendClientKick                     (const Safira::ClientInfo&, std::string_view reason);

    // ── Private chat signalling ─────────────────────────────────────────────
    void ForwardPrivateChatInvite   (Safira::ClientID targetID, const std::string& fromUsername);
    void ForwardPrivateChatConnectTo(Safira::ClientID initiatorID,
                                     const std::string& responderUsername,
                                     const std::string& responderIPAndPort);
    void ForwardPrivateChatDeclined (Safira::ClientID initiatorID,
                                     const std::string& responderUsername);

    // ── Commands ────────────────────────────────────────────────────────────
    bool KickUser(std::string_view username, std::string_view reason = "");
    void Quit();
    void OnCommand(std::string_view command);

    // ── Helpers ─────────────────────────────────────────────────────────────
    [[nodiscard]] bool IsValidUsername(const std::string& username) const;

    [[nodiscard]] const std::string& GetClientUsername(Safira::ClientID id) const;
    [[nodiscard]] uint32_t           GetClientColor   (Safira::ClientID id) const;

    // §3.2 — std::optional instead of returning 0 as a "not found" sentinel.
    // The caller must explicitly check .has_value() before using the ID.
    [[nodiscard]] std::optional<Safira::ClientID> FindClientID(const std::string& username) const;

    // §5.3 — Serialization helper: builds a packet into m_ScratchBuffer and
    // returns a view over it.  Centralises the write-then-slice pattern so
    // every Send* method doesn't repeat the boilerplate.
    template <typename WriteFn>
    [[nodiscard]] Safira::Buffer BuildPacket(WriteFn&& writeFn);

    void SendChatMessage(std::string_view message);
    void SaveMessageHistoryToFile (const std::filesystem::path&);
    bool LoadMessageHistoryFromFile(const std::filesystem::path&);

    // ── Private-chat invite cleanup on disconnect ───────────────────────────
    void CleanupPendingInvites(Safira::ClientID disconnectedID,
                               const std::string& disconnectedUsername);

    // ── Members ─────────────────────────────────────────────────────────────
    std::unique_ptr<Safira::Server>                      m_Server;
    Console                                              m_Console { "Server Console" };
    std::vector<Safira::ChatMessage>                     m_MessageHistory;
    std::filesystem::path                                m_MessageHistoryFilePath;

    Safira::Buffer                                       m_ScratchBuffer;

    std::map<Safira::ClientID, Safira::UserInfo>         m_ConnectedClients;

    // Pending private chat invites: responder_username → initiator_ClientID.
    // Cleared when the responder accepts/declines or either party disconnects.
    std::map<std::string, Safira::ClientID>              m_PendingPrivateChatInvites;

    static constexpr float kClientListInterval = 10.0f;
    float m_ClientListTimer = kClientListInterval;
};

#endif // PQC_MASTER_THESIS_2026_SERVERLAYER_H