#ifndef PQC_MASTER_THESIS_2026_CLIENTLAYER_H
#define PQC_MASTER_THESIS_2026_CLIENTLAYER_H

// ═════════════════════════════════════════════════════════════════════════════
// ClientLayer.h — UI and application logic sitting above Client
//
//  §5.3  Serialization  – BufferWriter + SerializePacket (concept-based)
//  C++23                – ranges, string_view, erase_if, std::visit
// ═════════════════════════════════════════════════════════════════════════════

#include "Layer.h"
#include "Client.h"
#include "ConsoleGUI.h"
#include "UserInfo.h"
#include "PrivateChatSession.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ClientLayer : public Safira::Layer {
public:
    void OnAttach()   override;
    void OnDetach()   override;
    void OnUIRender() override;

    [[nodiscard]] bool IsConnected() const;
    void OnDisconnectButton();

private:
    // ── UI ──────────────────────────────────────────────────────────────────
    void UI_ConnectionModal();
    void UI_ClientList();
    void UI_IncomingInvites();
    void UI_PrivateChatWindows();
    void DrawUserIcon(uint8_t iconIndex, float size, bool clickable = false);

    // ── Server event callbacks ──────────────────────────────────────────────
    void OnConnected();
    void OnDisconnected();
    void OnDataReceived(Safira::ByteSpan data);

    // ── Outgoing ────────────────────────────────────────────────────────────
    void SendChatMessage(std::string_view message);
    void SendPrivateChatInvite(const std::string& targetUsername);
    void SendPrivateChatResponse(const std::string& toUsername, bool accepted);

    // ── P2P helpers ─────────────────────────────────────────────────────────
    void StartPrivateChatAsInitiator(const std::string& peerUsername,
                                     const std::string& peerAddress);
    void StartPrivateChatAsResponder(const std::string& peerUsername);

    // ── Persistence ─────────────────────────────────────────────────────────
    void SaveConnectionDetails(const std::filesystem::path& filepath);
    bool LoadConnectionDetails(const std::filesystem::path& filepath);

    // ── Data ────────────────────────────────────────────────────────────────
    std::unique_ptr<Safira::Client>  m_Client;
    Safira::UI::ConsoleGUI           m_Console { "Chat" };

    std::string           m_ServerIP = "127.0.0.1";
    std::filesystem::path m_ConnectionDetailsFilePath = "ConnectionDetails.yaml";

    Safira::ByteBuffer m_ScratchBuffer;

    float    m_ColorBuffer[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    uint8_t  m_IconIndex      = 0;
    std::string m_Username;
    uint32_t    m_Color = 0xFFFFFFFF;

    std::map<std::string, Safira::UserInfo> m_ConnectedClients;

    bool m_ConnectionModalOpen             = false;
    bool m_ShowSuccessfulConnectionMessage = false;

    // ── Private chat state ──────────────────────────────────────────────────
    struct IncomingInvite {
        std::string FromUsername;
    };
    std::vector<IncomingInvite> m_IncomingInvites;

    std::set<std::string> m_PendingOutgoingInvites;

    std::map<std::string, std::unique_ptr<Safira::PrivateChatSession>> m_PrivateChats;
};

#endif // PQC_MASTER_THESIS_2026_CLIENTLAYER_H
