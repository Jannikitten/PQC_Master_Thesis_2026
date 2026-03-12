#ifndef PQC_MASTER_THESIS_2026_CLIENTLAYER_H
#define PQC_MASTER_THESIS_2026_CLIENTLAYER_H

#include "Layer.h"
#include "DtlsClient.h"
#include "ConsoleGUIView.h"
#include "UserInfoSerialize.h"
#include "AvatarCircle.h"
#include "FileDialog.h"
#include "P2PSession.h"
#include "ChatView.h"
#include "Image.h"
#include "Store.h"
#include "ClientState.h"
#include "ClientAction.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
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
    // -- UI sections ----------------------------------------------------------
    void UI_ConnectionModal();
    void UI_CropModal();
    void UI_IncomingInvites();
    void UI_UnifiedChatWindow();
    void UI_UserListSection(float width);
    void UI_ReportModal();

    // -- Avatar drawing -------------------------------------------------------
    void DrawAvatarCircle(ImDrawList* dl, ImVec2 center, float radius,
                          uint32_t color, const std::string& username,
                          ImTextureID tex);

    // -- Helpers --------------------------------------------------------------
    void RebuildConversationList();
    void AddLobbyMessage(const std::string& who, const std::string& text,
                         uint32_t color,
                         Safira::MessageRole role = Safira::MessageRole::Peer);

    // -- Store-based dispatch helpers -----------------------------------------
    void Dispatch(Safira::ClientActionVariant action);
    void Logout();
    void LeavePrivateChat(const std::string& peerUsername);

    // -- Presentation sync (called by effect middleware OnActionProcessed) -----
    void OnActionProcessed(const Safira::ClientActionVariant& action,
                           const Safira::ClientState& state);

    // -- Avatar image pipeline ------------------------------------------------
    void LoadAvatarImage(const std::string& filepath);
    void UploadPeerAvatarTexture(const std::string& username,
                                 const std::vector<uint8_t>& avatarData);

    void SaveConnectionDetails(const std::filesystem::path& filepath);
    bool LoadConnectionDetails(const std::filesystem::path& filepath);

    // =========================================================================
    // State
    // =========================================================================

    // -- Application store (single source of truth for domain state) -----------
    std::unique_ptr<Safira::Store<Safira::ClientState, Safira::ClientActionVariant>>
        m_Store;

    // -- Infrastructure -------------------------------------------------------
    std::unique_ptr<Safira::Client>  m_Client;
    Safira::UI::ConsoleGUI           m_Console { "Chat" };
    std::filesystem::path m_ConnectionDetailsFilePath = "ConnectionDetails.yaml";

    // -- Presentation-only state (NOT in the store) ---------------------------

    // Own avatar (GPU texture + processed wire bytes)
    std::string                    m_AvatarImagePath;
    std::shared_ptr<Safira::Image> m_AvatarImage;
    ImTextureID                    m_AvatarTexture = {};
    std::vector<uint8_t>           m_ProcessedAvatarBytes;  // raw RGBA for wire

    // Crop UI state
    bool                           m_ShowCropModal = false;
    Safira::CropRect               m_CropRect;
    int                            m_CropSrcWidth  = 0;
    int                            m_CropSrcHeight = 0;
    std::shared_ptr<Safira::Image> m_CropPreviewImage;
    ImTextureID                    m_CropPreviewTex = {};

    // Peer avatar texture cache
    struct PeerAvatarCache {
        std::shared_ptr<Safira::Image> Image;
        ImTextureID                    Tex = {};
        size_t                         DataHash = 0;
    };
    std::map<std::string, PeerAvatarCache> m_PeerAvatars;

    bool m_ConnectionModalOpen             = false;
    bool m_ShowSuccessfulConnectionMessage = false;

    // Chat panel (presentation-enriched messages with textures, timestamps)
    Safira::ChatPanel                     m_ChatPanel;
    std::vector<Safira::ChatEntry>        m_LobbyMessages;
    std::mutex                            m_LobbyMutex;
    std::vector<Safira::ChatEntry>        m_LobbySnapshot;
    std::vector<Safira::ConversationInfo> m_ConversationList;
    int                                   m_ActiveConvoIdx = 0;

    // P2P sessions (managed by effect handler callbacks)
    std::map<std::string, std::unique_ptr<Safira::PrivateChatSession>> m_PrivateChats;

    // Right-click context menu / report modal
    std::string m_ContextMenuTarget;
    bool        m_ReportModalOpen = false;
    std::string m_ReportTarget;
    char        m_ReportReasonBuf[512] = {};
};

#endif // PQC_MASTER_THESIS_2026_CLIENTLAYER_H
