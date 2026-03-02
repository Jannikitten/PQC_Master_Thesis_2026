#include "ServerLayer.h"
#include "ServerPacket.h"
#include "SafiraAssert.h"
#include "BufferStream.h"
#include "StringUtils.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <fstream>

void ServerLayer::OnAttach() {
	const int Port = 8192;

	m_ScratchBuffer.Allocate(8192);

	m_Server = std::make_unique<Safira::Server>(Port);
	m_Server->SetClientConnectedCallback([this](const Safira::ClientInfo& clientInfo) { OnClientConnected(clientInfo); });
	m_Server->SetClientDisconnectedCallback([this](const Safira::ClientInfo& clientInfo) { OnClientDisconnected(clientInfo); });
	m_Server->SetDataReceivedCallback([this](const Safira::ClientInfo& clientInfo, const Safira::Buffer data) { OnDataReceived(clientInfo, data); });
	m_Server->Start();

	m_MessageHistoryFilePath = "MessageHistory.yaml";

	m_Console.AddTaggedMessage("Info", "Loading message history...");
	LoadMessageHistoryFromFile(m_MessageHistoryFilePath);

	for (const auto& message : m_MessageHistory)
		m_Console.AddTaggedMessage(message.Username, message.Message);

	m_Console.AddTaggedMessage("Info", "Started server on port {}", Port);

	m_Console.SetMessageSendCallback([this](std::string_view message) { SendChatMessage(message); });
}

void ServerLayer::OnDetach() {
	m_Server->Stop();

	m_ScratchBuffer.Release();
}

void ServerLayer::OnUpdate(float ts) {
	m_ClientListTimer -= ts;
	if (m_ClientListTimer < 0) {
		m_ClientListTimer = m_ClientListInterval;
		SendClientListToAllClients();

		SaveMessageHistoryToFile(m_MessageHistoryFilePath);
	}
}

void ServerLayer::OnUIRender() {
	m_Console.OnUIRender();
}

void ServerLayer::OnClientConnected(const Safira::ClientInfo& clientInfo) {
	// Full registration is deferred to PacketType::ClientConnectionRequest
}

void ServerLayer::OnClientDisconnected(const Safira::ClientInfo& clientInfo) {
	if (m_ConnectedClients.contains(clientInfo.ID)) {
		SendClientDisconnect(clientInfo);
		const auto& userInfo = m_ConnectedClients.at(clientInfo.ID);
		m_Console.AddItalicMessage("Client {} disconnected", userInfo.Username);
		m_ConnectedClients.erase(clientInfo.ID);
	}
	else {
		// Client disconnected before completing the handshake — normal for
		// DTLS where a new peer might send a single datagram and disappear.
		// AddressStr replaces the old ConnectionDesc field.
		std::cout << "[WARN] OnClientDisconnected - unknown client ID="
		          << clientInfo.ID << " addr=" << clientInfo.AddressStr << std::endl;
	}
}

void ServerLayer::OnDataReceived(const Safira::ClientInfo& clientInfo, const Safira::Buffer buffer) {
	Safira::BufferStreamReader stream(buffer);

	PacketType type;
	bool success = stream.ReadRaw<PacketType>(type);
	WL_CORE_VERIFY(success);
	if (!success)
		return;

	switch (type) {
		case PacketType::Message: {
			if (!m_ConnectedClients.contains(clientInfo.ID)) {
				// Data from a peer that never completed ClientConnectionRequest
				m_Console.AddMessage("Rejected data from unregistered client ID={} addr={}",
				                     clientInfo.ID, clientInfo.AddressStr);
				return;
			}

			std::string message;
			if (stream.ReadString(message)) {
				if (IsValidMessage(message)) {
					const auto& client = m_ConnectedClients.at(clientInfo.ID);

					m_MessageHistory.push_back({ client.Username, message });
					m_Console.AddTaggedMessageWithColor(client.Color | 0xff000000, client.Username, message);
					SendMessageToAllClients(clientInfo, message);
				}
			}
			break;
		}
		case PacketType::ClientConnectionRequest: {
			uint32_t requestedColor;
			std::string requestedUsername;
			stream.ReadRaw<uint32_t>(requestedColor);

			if (stream.ReadString(requestedUsername)) {
				bool isValidUsername = IsValidUsername(requestedUsername);
				SendClientConnectionRequestResponse(clientInfo, isValidUsername);

				if (isValidUsername) {
					m_Console.AddMessage("Welcome {} (color {}) from {}",
					                     requestedUsername, requestedColor,
					                     clientInfo.AddressStr);  // AddressStr replaces ConnectionDesc
					auto& client     = m_ConnectedClients[clientInfo.ID];
					client.Username  = requestedUsername;
					client.Color     = requestedColor;

					SendClientConnect(clientInfo);
					SendClientList(clientInfo);
					SendMessageHistory(clientInfo);
				}
				else {
					m_Console.AddMessage(
					    "Client connection rejected: username='{}' color={} addr={}",
					    requestedUsername, requestedColor, clientInfo.AddressStr);
					m_Console.AddMessage("Reason: invalid username");
				}
			}
			break;
		}
		default:
			break;
	}
}

void ServerLayer::OnMessageReceived(const Safira::ClientInfo& clientInfo, std::string_view message) {}
void ServerLayer::OnClientConnectionRequest(const Safira::ClientInfo& clientInfo, uint32_t userColor, std::string_view username) {}
void ServerLayer::OnClientUpdate(const Safira::ClientInfo& clientInfo, uint32_t userColor, std::string_view username) {}

void ServerLayer::SendClientList(const Safira::ClientInfo& clientInfo) {
	std::vector<UserInfo> clientList;
	clientList.reserve(m_ConnectedClients.size());
	for (const auto& [id, info] : m_ConnectedClients)
		clientList.push_back(info);

	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::ClientList);
	stream.WriteArray(clientList);

	m_Server->SendBufferToClient(clientInfo.ID,
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendClientListToAllClients() {
	std::vector<UserInfo> clientList;
	clientList.reserve(m_ConnectedClients.size());
	for (const auto& [id, info] : m_ConnectedClients)
		clientList.push_back(info);

	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::ClientList);
	stream.WriteArray(clientList);

	m_Server->SendBufferToAllClients(
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendClientConnect(const Safira::ClientInfo& newClient) {
	WL_VERIFY(m_ConnectedClients.contains(newClient.ID));
	const auto& newClientInfo = m_ConnectedClients.at(newClient.ID);

	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::ClientConnect);
	stream.WriteObject(newClientInfo);

	m_Server->SendBufferToAllClients(
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()), newClient.ID);
}

void ServerLayer::SendClientDisconnect(const Safira::ClientInfo& clientInfo) {
	const auto& userInfo = m_ConnectedClients.at(clientInfo.ID);

	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::ClientDisconnect);
	stream.WriteObject(userInfo);

	m_Server->SendBufferToAllClients(
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()), clientInfo.ID);
}

void ServerLayer::SendClientConnectionRequestResponse(const Safira::ClientInfo& clientInfo, bool response) {
	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::ClientConnectionRequest);
	stream.WriteRaw<bool>(response);

	m_Server->SendBufferToClient(clientInfo.ID,
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendClientUpdateResponse(const Safira::ClientInfo& clientInfo) {}

void ServerLayer::SendMessageToAllClients(const Safira::ClientInfo& fromClient, std::string_view message) {
	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::Message);
	stream.WriteString(GetClientUsername(fromClient.ID));
	stream.WriteString(message);

	m_Server->SendBufferToAllClients(
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()), fromClient.ID);
}

void ServerLayer::SendMessageHistory(const Safira::ClientInfo& clientInfo) {
	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::MessageHistory);
	stream.WriteArray(m_MessageHistory);

	m_Server->SendBufferToClient(clientInfo.ID,
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendServerShutdownToAllClients() {
	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::ServerShutdown);

	m_Server->SendBufferToAllClients(
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

void ServerLayer::SendClientKick(const Safira::ClientInfo& clientInfo, std::string_view reason) {
	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::ClientKick);
	stream.WriteString(std::string(reason));

	m_Server->SendBufferToClient(clientInfo.ID,
	    Safira::Buffer(m_ScratchBuffer, stream.GetStreamPosition()));
}

bool ServerLayer::KickUser(std::string_view username, std::string_view reason) {
	for (const auto& [clientID, userInfo] : m_ConnectedClients) {
		if (userInfo.Username == username) {
			Safira::ClientInfo clientInfo;
			clientInfo.ID = clientID;
			SendClientKick(clientInfo, reason);
			// KickClient -> RemoveClient already fires m_ClientDisconnectedCallback
			// -> OnClientDisconnected, which erases from m_ConnectedClients.
			// Do NOT call OnClientDisconnected manually here or it fires twice,
			// causing the "unknown client ID" warning.
			m_Server->KickClient(clientID);
			return true;
		}
	}
	return false;
}

void ServerLayer::Quit() {
	SendServerShutdownToAllClients();
	m_Server->Stop();
}

bool ServerLayer::IsValidUsername(const std::string& username) const {
	for (const auto& [id, client] : m_ConnectedClients) {
		if (client.Username == username)
			return false;
	}
	return true;
}

const std::string& ServerLayer::GetClientUsername(Safira::ClientID clientID) const {
	WL_VERIFY(m_ConnectedClients.contains(clientID));
	return m_ConnectedClients.at(clientID).Username;
}

uint32_t ServerLayer::GetClientColor(Safira::ClientID clientID) const {
	WL_VERIFY(m_ConnectedClients.contains(clientID));
	return m_ConnectedClients.at(clientID).Color;
}

void ServerLayer::SendChatMessage(std::string_view message) {
	if (message[0] == '/') {
		OnCommand(message);
		return;
	}

	Safira::BufferStreamWriter stream(m_ScratchBuffer);
	stream.WriteRaw<PacketType>(PacketType::Message);
	stream.WriteString(std::string_view("SERVER"));
	stream.WriteString(message);
	m_Server->SendBufferToAllClients(stream.GetBuffer());

	m_Console.AddTaggedMessage("SERVER", message);
	m_MessageHistory.push_back({ "SERVER", std::string(message) });
}

void ServerLayer::OnCommand(std::string_view command) {
	if (command.size() < 2 || command[0] != '/')
		return;

	std::string_view commandStr(&command[1], command.size() - 1);
	auto tokens = Safira::Utils::SplitString(commandStr, ' ');

	if (tokens[0] == "kick") {
		if (tokens.size() == 2 || tokens.size() == 3) {
			std::string_view reason = tokens.size() == 3 ? tokens[2] : "";
			if (KickUser(tokens[1], reason)) {
				m_Console.AddItalicMessage("User {} has been kicked.", tokens[1]);
				if (!reason.empty())
					m_Console.AddItalicMessage("  Reason: {}", reason);
			}
			else {
				m_Console.AddItalicMessage("Could not kick user {}; user not found.", tokens[1]);
			}
		}
		else {
			m_Console.AddItalicMessage("Kick command requires single argument, eg. /kick <username>");
		}
	}
}

void ServerLayer::SaveMessageHistoryToFile(const std::filesystem::path& filepath) {
	YAML::Emitter out;
	{
		out << YAML::BeginMap;
		out << YAML::Key << "MessageHistory" << YAML::Value;
		out << YAML::BeginSeq;
		for (const auto& chatMessage : m_MessageHistory) {
			out << YAML::BeginMap;
			out << YAML::Key << "User"    << YAML::Value << chatMessage.Username;
			out << YAML::Key << "Message" << YAML::Value << chatMessage.Message;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;
	}

	std::ofstream fout(filepath);
	fout << out.c_str();
}

bool ServerLayer::LoadMessageHistoryFromFile(const std::filesystem::path& filepath) {
	if (!std::filesystem::exists(filepath))
		return false;

	m_MessageHistory.clear();

	YAML::Node data;
	try {
		data = YAML::LoadFile(filepath.string());
	}
	catch (YAML::ParserException e) {
		std::cout << "[ERROR] Failed to load message history " << filepath
		          << std::endl << e.what() << std::endl;
		return false;
	}

	auto rootNode = data["MessageHistory"];
	if (!rootNode)
		return false;

	m_MessageHistory.reserve(rootNode.size());
	for (const auto& node : rootNode)
		m_MessageHistory.emplace_back(node["User"].as<std::string>(), node["Message"].as<std::string>());

	return true;
}