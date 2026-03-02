#ifndef PQC_MASTER_THESIS_2026_CLIENTLAYER_H
#define PQC_MASTER_THESIS_2026_CLIENTLAYER_H

#include "Layer.h"
#include "Client.h"
#include "ConsoleGUI.h"
#include "UserInfo.h"
#include "PrivateChatSession.h"

#include <map>
#include <set>
#include <vector>
#include <memory>
#include <string>
#include <filesystem>

class ClientLayer : public Safira::Layer {
public:
    virtual void OnAttach()   override;
    virtual void OnDetach()   override;
    virtual void OnUIRender() override;

    bool IsConnected() const;
    void OnDisconnectButton();

private:
    // ── UI ────────────────────────────────────────────────────────────────────
    void UI_ConnectionModal();
    void UI_ClientList();
    void UI_IncomingInvites();          // modal: "X wants to chat privately"
    void UI_PrivateChatWindows();       // renders all active P2P sessions
    void DrawUserIcon(uint8_t iconIndex, float size, bool clickable = false);

    // ── Server event callbacks ────────────────────────────────────────────────
    void OnConnected();
    void OnDisconnected();
    void OnDataReceived(Safira::Buffer buffer);

    // ── Outgoing ─────────────────────────────────────────────────────────────
    void SendChatMessage(std::string_view message);
    void SendPrivateChatInvite(const std::string& targetUsername);
    void SendPrivateChatResponse(const std::string& toUsername, bool accepted);

    // ── P2P helpers ───────────────────────────────────────────────────────────
    // Called when server tells us to connect to peerAddress (initiator role)
    void StartPrivateChatAsInitiator(const std::string& peerUsername,
                                     const std::string& peerAddress);
    // Called when we accepted and need to tell the server our listen port
    void StartPrivateChatAsResponder(const std::string& peerUsername);

    // ── Persistence ──────────────────────────────────────────────────────────
    void SaveConnectionDetails(const std::filesystem::path& filepath);
    bool LoadConnectionDetails(const std::filesystem::path& filepath);

    // ── Data ─────────────────────────────────────────────────────────────────
    std::unique_ptr<Safira::Client> m_Client;
    Safira::UI::ConsoleGUI          m_Console{"Chat"};

    std::string           m_ServerIP = "127.0.0.1";
    std::filesystem::path m_ConnectionDetailsFilePath = "ConnectionDetails.yaml";

    Safira::Buffer m_ScratchBuffer;

    float    m_ColorBuffer[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    uint8_t  m_IconIndex      = 0;          // chosen icon for this user
    std::string m_Username;
    uint32_t    m_Color = 0xFFFFFFFF;

    std::map<std::string, Safira::UserInfo> m_ConnectedClients;

    bool m_ConnectionModalOpen            = false;
    bool m_ShowSuccessfulConnectionMessage= false;

    // ── Private chat state ────────────────────────────────────────────────────

    // Invites WE received that are awaiting user response: username → display info
    struct IncomingInvite {
        std::string FromUsername;
    };
    std::vector<IncomingInvite> m_IncomingInvites;

    // Invites WE sent, waiting for the server to relay a response: target username
    std::set<std::string> m_PendingOutgoingInvites;

    // Active P2P sessions keyed by peer username
    std::map<std::string, std::unique_ptr<Safira::PrivateChatSession>> m_PrivateChats;
};

#endif //PQC_MASTER_THESIS_2026_CLIENTLAYER_H