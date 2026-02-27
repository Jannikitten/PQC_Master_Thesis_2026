#ifndef PQC_MASTER_THESIS_2026_SERVERLAYER_H
#define PQC_MASTER_THESIS_2026_SERVERLAYER_H

#include "Layer.h"
#include "Server.h"
#include "Console.h"
#include "UserInfo.h"
#include <filesystem>

class ServerLayer : public Safira::Layer {
public:
	virtual void OnAttach() override;
	virtual void OnDetach() override;
	virtual void OnUpdate(float ts) override;
	virtual void OnUIRender() override;
private:
	// Server event callbacks
	void OnClientConnected(const Safira::ClientInfo& clientInfo);
	void OnClientDisconnected(const Safira::ClientInfo& clientInfo);
	void OnDataReceived(const Safira::ClientInfo& clientInfo, const Safira::Buffer buffer);

	////////////////////////////////////////////////////////////////////////////////
	// Handle incoming messages
	////////////////////////////////////////////////////////////////////////////////
	void OnMessageReceived(const Safira::ClientInfo& clientInfo, std::string_view message);
	void OnClientConnectionRequest(const Safira::ClientInfo& clientInfo, uint32_t userColor, std::string_view username);
	void OnClientUpdate(const Safira::ClientInfo& clientInfo, uint32_t userColor, std::string_view username);

	////////////////////////////////////////////////////////////////////////////////
	// Handle outgoing messages
	////////////////////////////////////////////////////////////////////////////////
	void SendClientList(const Safira::ClientInfo& clientInfo);
	void SendClientListToAllClients();
	void SendClientConnect(const Safira::ClientInfo& clientInfo);
	void SendClientDisconnect(const Safira::ClientInfo& clientInfo);
	void SendClientConnectionRequestResponse(const Safira::ClientInfo& clientInfo, bool response);
	void SendClientUpdateResponse(const Safira::ClientInfo& clientInfo);
	void SendMessageToAllClients(const Safira::ClientInfo& fromClient, std::string_view message);
	void SendMessageHistory(const Safira::ClientInfo& clientInfo);
	void SendServerShutdownToAllClients();
	void SendClientKick(const Safira::ClientInfo& clientInfo, std::string_view reason);
	////////////////////////////////////////////////////////////////////////////////

	////////////////////////////////////////////////////////////////////////////////
	// Commands
	////////////////////////////////////////////////////////////////////////////////
	bool KickUser(std::string_view username, std::string_view reason = "");
	void Quit();
	////////////////////////////////////////////////////////////////////////////////

	bool IsValidUsername(const std::string& username) const;
	const std::string& GetClientUsername(Safira::ClientID clientID) const;
	uint32_t GetClientColor(Safira::ClientID clientID) const;

	void SendChatMessage(std::string_view message);
	void OnCommand(std::string_view command);
	void SaveMessageHistoryToFile(const std::filesystem::path& filepath);
	bool LoadMessageHistoryFromFile(const std::filesystem::path& filepath);
private:
	std::unique_ptr<Safira::Server> m_Server;
	Safira::UI::Console m_Console{ "Server Console" };
	std::vector<ChatMessage> m_MessageHistory;
	std::filesystem::path m_MessageHistoryFilePath;

	Safira::Buffer m_ScratchBuffer;

	std::map<Safira::ClientID, UserInfo> m_ConnectedClients;

	// Send client list every ten seconds
	const float m_ClientListInterval = 10.0f;
	float m_ClientListTimer = m_ClientListInterval;
};

#endif //PQC_MASTER_THESIS_2026_SERVERLAYER_H