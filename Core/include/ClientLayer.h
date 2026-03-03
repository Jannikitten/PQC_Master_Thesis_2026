#ifndef PQC_MASTER_THESIS_2026_CLIENTLAYER_H
#define PQC_MASTER_THESIS_2026_CLIENTLAYER_H

#include "Layer.h"
#include "Client.h"
#include "ConsoleGUI.h"
#include "UserInfo.h"
#include "PrivateChatSession.h"
#include "ChatPanel.h"
#include "Image.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
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
    // ── UI sections ─────────────────────────────────────────────────────────
    void UI_ConnectionModal();
    void UI_IncomingInvites();
    void UI_UnifiedChatWindow();
    void UI_UserListSection(float width);
    void UI_ReportModal();
    void DrawUserIcon(uint8_t iconIndex, float size, bool clickable = false);

    // ── Helpers ─────────────────────────────────────────────────────────────
    void RebuildConversationList();
    void AddLobbyMessage(const std::string& who, const std::string& text,
                         uint32_t color,
                         Safira::MessageRole role = Safira::MessageRole::Peer);

    void OnConnected();
    void OnDisconnected();
    void OnDataReceived(Safira::ByteSpan data);

    void SendChatMessage(std::string_view message);
    void SendPrivateChatInvite(const std::string& targetUsername);
    void SendPrivateChatResponse(const std::string& toUsername, bool accepted);

    void StartPrivateChatAsInitiator(const std::string& peerUsername,
                                     const std::string& peerAddress);
    void StartPrivateChatAsResponder(const std::string& peerUsername);

    // ── New: Logout / Leave ────────────────────────────────────────────────
    void Logout();
    void LeavePrivateChat(const std::string& peerUsername);

    // ── Avatar image loading ───────────────────────────────────────────────
    void LoadAvatarImage(const std::string& filepath);

    void SaveConnectionDetails(const std::filesystem::path& filepath);
    bool LoadConnectionDetails(const std::filesystem::path& filepath);

    // ── Core state ──────────────────────────────────────────────────────────
    std::unique_ptr<Safira::Client>  m_Client;
    Safira::UI::ConsoleGUI           m_Console { "Chat" };

    std::string           m_ServerIP = "127.0.0.1";
    std::filesystem::path m_ConnectionDetailsFilePath = "ConnectionDetails.yaml";

    Safira::ByteBuffer m_ScratchBuffer;

    uint8_t     m_IconIndex = 0;
    std::string m_Username;
    uint32_t    m_Color = 0xFFFFFFFF;

    // ── Avatar image (replaces color picker) ────────────────────────────────
    std::string                   m_AvatarImagePath;
    std::shared_ptr<Safira::Image> m_AvatarImage;     // loaded image
    ImTextureID                   m_AvatarTexture = {};

    std::map<std::string, Safira::UserInfo> m_ConnectedClients;

    bool m_ConnectionModalOpen             = false;
    bool m_ShowSuccessfulConnectionMessage = false;

    // ── Chat panel ──────────────────────────────────────────────────────────
    Safira::ChatPanel                     m_ChatPanel;
    std::vector<Safira::ChatEntry>        m_LobbyMessages;
    std::mutex                            m_LobbyMutex;
    std::vector<Safira::ChatEntry>        m_LobbySnapshot;
    std::vector<Safira::ConversationInfo> m_ConversationList;
    int                                   m_ActiveConvoIdx = 0;

    struct IncomingInvite {
        std::string FromUsername;
    };
    std::vector<IncomingInvite> m_IncomingInvites;

    std::set<std::string> m_PendingOutgoingInvites;

    std::map<std::string, std::unique_ptr<Safira::PrivateChatSession>> m_PrivateChats;

    // ── Right-click context menu / report modal ────────────────────────────
    std::string m_ContextMenuTarget;        // username for right-click popup
    bool        m_ReportModalOpen = false;
    std::string m_ReportTarget;
    char        m_ReportReasonBuf[512] = {};
};

#endif // PQC_MASTER_THESIS_2026_CLIENTLAYER_H