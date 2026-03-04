#ifndef PQC_MASTER_THESIS_2026_CLIENTLAYER_H
#define PQC_MASTER_THESIS_2026_CLIENTLAYER_H

#include "Layer.h"
#include "Client.h"
#include "ConsoleGUI.h"
#include "UserInfo.h"
#include "AvatarUtils.h"
#include "FileDialog.h"
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
    void UI_CropModal();
    void UI_IncomingInvites();
    void UI_UnifiedChatWindow();
    void UI_UserListSection(float width);
    void UI_ReportModal();

    // ── Avatar drawing ──────────────────────────────────────────────────────
    void DrawAvatarCircle(ImDrawList* dl, ImVec2 center, float radius,
                          uint32_t color, const std::string& username,
                          ImTextureID tex);

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

    void Logout();
    void LeavePrivateChat(const std::string& peerUsername);

    // ── Avatar image pipeline ──────────────────────────────────────────────
    void LoadAvatarImage(const std::string& filepath);
    void UploadPeerAvatarTexture(const std::string& username,
                                 const std::vector<uint8_t>& jpegData);

    void SaveConnectionDetails(const std::filesystem::path& filepath);
    bool LoadConnectionDetails(const std::filesystem::path& filepath);

    // ── Core state ──────────────────────────────────────────────────────────
    std::unique_ptr<Safira::Client>  m_Client;
    Safira::UI::ConsoleGUI           m_Console { "Chat" };

    std::string           m_ServerIP = "127.0.0.1";
    std::filesystem::path m_ConnectionDetailsFilePath = "ConnectionDetails.yaml";

    Safira::ByteBuffer m_ScratchBuffer;

    std::string m_Username;
    uint32_t    m_Color = 0xFFFFFFFF;   // server-assigned, kept for local use

    // ── Avatar image (replaces icon / colour picker) ────────────────────────
    std::string                    m_AvatarImagePath;
    std::shared_ptr<Safira::Image> m_AvatarImage;        // GPU texture (own)
    ImTextureID                    m_AvatarTexture = {};
    std::vector<uint8_t>           m_AvatarBytes;        // processed JPEG for wire

    // ── Crop UI state ───────────────────────────────────────────────────────
    bool                           m_ShowCropModal = false;
    Safira::CropRect               m_CropRect;
    int                            m_CropSrcWidth  = 0;
    int                            m_CropSrcHeight = 0;
    std::shared_ptr<Safira::Image> m_CropPreviewImage;   // full image on GPU
    ImTextureID                    m_CropPreviewTex = {};

    // ── Peer avatar texture cache ───────────────────────────────────────────
    struct PeerAvatarCache {
        std::shared_ptr<Safira::Image> Image;
        ImTextureID                    Tex = {};
        size_t                         DataHash = 0;
    };
    std::map<std::string, PeerAvatarCache> m_PeerAvatars;

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
    std::string m_ContextMenuTarget;
    bool        m_ReportModalOpen = false;
    std::string m_ReportTarget;
    char        m_ReportReasonBuf[512] = {};
};

#endif // PQC_MASTER_THESIS_2026_CLIENTLAYER_H