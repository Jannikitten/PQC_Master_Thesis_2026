#ifndef PQC_MASTER_THESIS_2026_SERVERLAYER_H
#define PQC_MASTER_THESIS_2026_SERVERLAYER_H

#include "Layer.h"
#include "Server.h"
#include "Console.h"
#include "UserInfo.h"

#include <map>
#include <filesystem>

class ServerLayer : public Safira::Layer {
public:
    virtual void OnAttach()          override;
    virtual void OnDetach()          override;
    virtual void OnUpdate(float ts)  override;
    virtual void OnUIRender()        override;

private:
    // ── Server event callbacks ────────────────────────────────────────────────
    void OnClientConnected   (const Safira::ClientInfo& clientInfo);
    void OnClientDisconnected(const Safira::ClientInfo& clientInfo);
    void OnDataReceived      (const Safira::ClientInfo& clientInfo, const Safira::Buffer buffer);

    // ── Incoming message handlers ─────────────────────────────────────────────
    void OnMessageReceived        (const Safira::ClientInfo&, std::string_view message);
    void OnClientConnectionRequest(const Safira::ClientInfo&, uint32_t userColor, std::string_view username);
    void OnClientUpdate           (const Safira::ClientInfo&, uint32_t userColor, std::string_view username);

    // ── Outgoing packets — existing ───────────────────────────────────────────
    void SendClientList                  (const Safira::ClientInfo& clientInfo);
    void SendClientListToAllClients      ();
    void SendClientConnect               (const Safira::ClientInfo& clientInfo);
    void SendClientDisconnect            (const Safira::ClientInfo& clientInfo);
    void SendClientConnectionRequestResponse(const Safira::ClientInfo&, bool response);
    void SendClientUpdateResponse        (const Safira::ClientInfo& clientInfo);
    void SendMessageToAllClients         (const Safira::ClientInfo& from, std::string_view message);
    void SendMessageHistory              (const Safira::ClientInfo& clientInfo);
    void SendServerShutdownToAllClients  ();
    void SendClientKick                  (const Safira::ClientInfo&, std::string_view reason);

    // ── Outgoing packets — private chat signalling ────────────────────────────
    // Forward the invite to targetID so they can accept/decline
    void ForwardPrivateChatInvite   (Safira::ClientID targetID, const std::string& fromUsername);
    // Tell the initiator where to connect (after responder accepted and bound a port)
    void ForwardPrivateChatConnectTo(Safira::ClientID initiatorID,
                                     const std::string& responderUsername,
                                     const std::string& responderIPAndPort);
    // Tell the initiator the invite was declined
    void ForwardPrivateChatDeclined (Safira::ClientID initiatorID,
                                     const std::string& responderUsername);

    // ── Commands ─────────────────────────────────────────────────────────────
    bool KickUser(std::string_view username, std::string_view reason = "");
    void Quit();
    void OnCommand(std::string_view command);

    // ── Helpers ──────────────────────────────────────────────────────────────
    bool               IsValidUsername  (const std::string& username) const;
    const std::string& GetClientUsername(Safira::ClientID) const;
    uint32_t           GetClientColor   (Safira::ClientID) const;
    // Returns ClientID for a username, or 0 if not found
    Safira::ClientID   FindClientID     (const std::string& username) const;

    void SendChatMessage(std::string_view message);
    void SaveMessageHistoryToFile (const std::filesystem::path&);
    bool LoadMessageHistoryFromFile(const std::filesystem::path&);

    // ── Members ───────────────────────────────────────────────────────────────
    std::unique_ptr<Safira::Server> m_Server;
    Console                         m_Console{"Server Console"};
    std::vector<Safira::ChatMessage>        m_MessageHistory;
    std::filesystem::path           m_MessageHistoryFilePath;

    Safira::Buffer m_ScratchBuffer;

    std::map<Safira::ClientID, Safira::UserInfo> m_ConnectedClients;

    // Pending private chat invites: responder_username → initiator_ClientID
    // Cleared when the responder accepts/declines, or when either party disconnects.
    std::map<std::string, Safira::ClientID> m_PendingPrivateChatInvites;

    const float m_ClientListInterval = 10.0f;
    float       m_ClientListTimer    = m_ClientListInterval;
};

#endif //PQC_MASTER_THESIS_2026_SERVERLAYER_H