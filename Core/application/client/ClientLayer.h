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

// Extracted view components
#include "AvatarManager.h"
#include "ConnectionView.h"
#include "InvitePopupView.h"

namespace Safira::Middleware { struct ClientEffectHandler; }

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

    // -- OnAttach sub-steps ---------------------------------------------------
    void WireEffectHandler(Safira::Middleware::ClientEffectHandler& handler);
    void WireViewCallbacks();
    void WireInfrastructureCallbacks();

    // -- Chat window (data preparation + result dispatch) ---------------------
    void RenderChatWindow();

    // -- Persistence ----------------------------------------------------------
    void SaveConnectionDetails(const std::filesystem::path& filepath);

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

    // -- Extracted view components --------------------------------------------
    Safira::AvatarManager        m_AvatarManager;
    Safira::ConnectionView       m_ConnectionView;
    Safira::InvitePopupView      m_InvitePopup;

    // -- Chat panel (presentation-enriched messages with textures) -------------
    Safira::ChatPanel                     m_ChatPanel;
    std::vector<Safira::ChatEntry>        m_LobbyMessages;
    std::mutex                            m_LobbyMutex;
    std::vector<Safira::ChatEntry>        m_LobbySnapshot;
    std::vector<Safira::ConversationInfo> m_ConversationList;
    int                                   m_ActiveConvoIdx = 0;

    // -- P2P sessions (managed by effect handler callbacks) -------------------
    std::map<std::string, std::unique_ptr<Safira::PrivateChatSession>> m_PrivateChats;

    // -- Presentation flags ---------------------------------------------------
    bool m_ShowSuccessfulConnectionMessage = false;
};

#endif // PQC_MASTER_THESIS_2026_CLIENTLAYER_H
