#ifndef PQC_MASTER_THESIS_2026_CLIENTLAYER_H
#define PQC_MASTER_THESIS_2026_CLIENTLAYER_H

#include "Layer.h"
#include "Client.h"
#include "Console.h"
#include "UserInfo.h"

#include <set>
#include <filesystem>

class ClientLayer : public Safira::Layer {
public:
    virtual void OnAttach() override;
    virtual void OnDetach() override;
    virtual void OnUIRender() override;

    bool IsConnected() const;
    void OnDisconnectButton();
private:
    // UI
    void UI_ConnectionModal();
    void UI_ClientList();

    // Server event callbacks
    void OnConnected();
    void OnDisconnected();
    void OnDataReceived(const Safira::Buffer buffer);

    void SendChatMessage(std::string_view message);
    void SaveConnectionDetails(const std::filesystem::path& filepath);
    bool LoadConnectionDetails(const std::filesystem::path& filepath);

    std::unique_ptr<Safira::Client> m_Client;
    Safira::UI::Console m_Console{ "Chat" };
    std::string m_ServerIP;
    std::filesystem::path m_ConnectionDetailsFilePath = "ConnectionDetails.yaml";

    Safira::Buffer m_ScratchBuffer;

    float m_ColorBuffer[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    std::string m_Username;
    uint32_t m_Color = 0xffffffff;

    std::map<std::string, UserInfo> m_ConnectedClients;
    bool m_ConnectionModalOpen = false;
    bool m_ShowSuccessfulConnectionMessage = false;
};

#endif //PQC_MASTER_THESIS_2026_CLIENTLAYER_H