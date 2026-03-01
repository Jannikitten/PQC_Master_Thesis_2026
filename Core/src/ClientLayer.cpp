#include "ClientLayer.h"
#include "ServerPacket.h"
#include "ApplicationGUI.h"
#include "UI.h"
#include "BufferStream.h"
#include "misc/cpp/imgui_stdlib.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>

void ClientLayer::OnAttach() {
	m_ScratchBuffer.Allocate(1024);

	m_Client = std::make_unique<Safira::Client>();
	m_Client->SetServerConnectedCallback([this]() { OnConnected(); });
	m_Client->SetServerDisconnectedCallback([this]() { OnDisconnected(); });
	m_Client->SetDataReceivedCallback([this](const Safira::Buffer data) { OnDataReceived(data); });

	m_Console.SetMessageSendCallback([this](std::string_view message) { SendChatMessage(message); });

	LoadConnectionDetails(m_ConnectionDetailsFilePath);
}

void ClientLayer::OnDetach() {
	m_Client->Disconnect();
	// ^ currently disconnect is blocking

	m_ScratchBuffer.Release();
}

void ClientLayer::OnUIRender() {
	UI_ConnectionModal();

	m_Console.OnUIRender();
	UI_ClientList();
}

bool ClientLayer::IsConnected() const {
	return m_Client->GetConnectionStatus() == Safira::ConnectionStatus::Connected;
}

void ClientLayer::OnDisconnectButton() {
	m_Client->Disconnect();
}

void ClientLayer::UI_ConnectionModal() {
	if (!m_ConnectionModalOpen && m_Client->GetConnectionStatus() != Safira::ConnectionStatus::Connected)
	{
		ImGui::OpenPopup("Connect to server");
	}

	m_ConnectionModalOpen = ImGui::BeginPopupModal("Connect to server", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
	if (m_ConnectionModalOpen)
	{
		ImGui::Text("Your Name");
		ImGui::InputText("##username", &m_Username);

		ImGui::Text("Pick a color");
		ImGui::SameLine();
		ImGui::ColorEdit4("##color", m_ColorBuffer);

		ImGui::Text("Server Address");
		ImGui::InputText("##address", &m_ServerIP);
		ImGui::SameLine();
		if (ImGui::Button("Connect"))
		{
			m_Color = IM_COL32(
				m_ColorBuffer[0] * 255.0f,
				m_ColorBuffer[1] * 255.0f,
				m_ColorBuffer[2] * 255.0f,
				m_ColorBuffer[3] * 255.0f);

			// The new Client handles "host:port" and bare IPs natively.
			// Append the default port when the user hasn't specified one.
			std::string addressToConnect = m_ServerIP;
			if (addressToConnect.rfind(':') == std::string::npos)
				addressToConnect += ":8192";

			m_Client->ConnectToServer(addressToConnect);
		}

		if (Safira::UI::ButtonCentered("Quit"))
			Safira::ApplicationGUI::Get().Close();

		const auto status = m_Client->GetConnectionStatus();

		if (status == Safira::ConnectionStatus::Connected)
		{
			// Send username + colour immediately after transport connects
			Safira::BufferStreamWriter stream(m_ScratchBuffer);
			stream.WriteRaw<PacketType>(PacketType::ClientConnectionRequest);
			stream.WriteRaw<uint32_t>(m_Color);
			stream.WriteString(m_Username);

			m_Client->SendBuffer(stream.GetBuffer());

			SaveConnectionDetails(m_ConnectionDetailsFilePath);

			ImGui::CloseCurrentPopup();
		}
		else if (status == Safira::ConnectionStatus::FailedToConnect)
		{
			ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.1f, 1.0f), "Connection failed.");
			const auto& debugMessage = m_Client->GetConnectionDebugMessage();
			if (!debugMessage.empty())
				ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.1f, 1.0f), debugMessage.c_str());
		}
		else if (status == Safira::ConnectionStatus::Connecting)
		{
			ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Connecting...");
		}

		ImGui::EndPopup();
	}
}

void ClientLayer::UI_ClientList()
{
	ImGui::Begin("Users Online");
	ImGui::Text("Online: %d", m_ConnectedClients.size());

	static bool selected = false;
	for (const auto& [username, clientInfo] : m_ConnectedClients)
	{
		if (username.empty())
			continue;

		ImGui::PushStyleColor(ImGuiCol_Text, ImColor(clientInfo.Color).Value);
		ImGui::Selectable(username.c_str(), &selected);
		ImGui::PopStyleColor();
	}
	ImGui::End();
}

void ClientLayer::OnConnected()
{
	m_Console.ClearLog();
	// Welcome message is deferred until PacketType::MessageHistory is received
}

void ClientLayer::OnDisconnected()
{
	m_Console.AddItalicMessageWithColor(0xff8a8a8a, "Lost connection to server!");
}

void ClientLayer::OnDataReceived(const Safira::Buffer buffer)
{
	Safira::BufferStreamReader stream(buffer);

	PacketType type;
	stream.ReadRaw<PacketType>(type);

	switch (type)
	{
	case PacketType::Message:
	{
		std::string fromUsername, message;
		stream.ReadString(fromUsername);
		stream.ReadString(message);

		if (m_ConnectedClients.contains(fromUsername))
		{
			const auto& clientInfo = m_ConnectedClients.at(fromUsername);
			m_Console.AddTaggedMessageWithColor(clientInfo.Color, fromUsername, message);
		}
		else if (fromUsername == "SERVER")
		{
			m_Console.AddTaggedMessage(fromUsername, message);
		}
		else
		{
			std::cout << "[ERROR] Message from unknown user? This shouldn't happen..." << std::endl;
			m_Console.AddTaggedMessage(fromUsername, message);
		}
		break;
	}
	case PacketType::ClientConnectionRequest:
	{
		bool requestStatus;
		stream.ReadRaw<bool>(requestStatus);
		if (requestStatus)
		{
			m_ShowSuccessfulConnectionMessage = true;
		}
		else
		{
			m_Console.AddItalicMessageWithColor(0xfffa4a4a,
				"Server rejected connection with username {}", m_Username);
		}
		break;
	}
	case PacketType::ConnectionStatus:
		break;
	case PacketType::ClientList:
	{
		std::vector<UserInfo> clientList;
		stream.ReadArray(clientList);

		m_ConnectedClients.clear();
		for (const auto& client : clientList)
			m_ConnectedClients[client.Username] = client;
		break;
	}
	case PacketType::ClientConnect:
	{
		UserInfo newClient;
		stream.ReadObject(newClient);

		m_ConnectedClients[newClient.Username] = newClient;
		m_Console.AddItalicMessageWithColor(newClient.Color, "Welcome {}!", newClient.Username);
		break;
	}
	case PacketType::ClientUpdate:
		break;
	case PacketType::ClientDisconnect:
	{
		UserInfo disconnectedClient;
		stream.ReadObject(disconnectedClient);

		m_ConnectedClients.erase(disconnectedClient.Username);
		m_Console.AddItalicMessageWithColor(disconnectedClient.Color, "Goodbye {}!", disconnectedClient.Username);
		break;
	}
	case PacketType::ClientUpdateResponse:
		break;
	case PacketType::MessageHistory:
	{
		std::vector<ChatMessage> messageHistory;
		stream.ReadArray(messageHistory);
		for (const auto& message : messageHistory)
		{
			uint32_t userColor = 0xffffffff;
			if (m_ConnectedClients.contains(message.Username))
				userColor = m_ConnectedClients.at(message.Username).Color;

			m_Console.AddTaggedMessageWithColor(userColor, message.Username, message.Message);
		}

		if (m_ShowSuccessfulConnectionMessage)
		{
			m_ShowSuccessfulConnectionMessage = false;
			m_Console.AddItalicMessageWithColor(0xff8a8a8a,
				"Successfully connected to {} with username {}", m_ServerIP, m_Username);
		}
		break;
	}
	case PacketType::ServerShutdown:
	{
		m_Console.AddItalicMessage("Server is shutting down... goodbye!");
		m_Client->Disconnect();
		break;
	}
	case PacketType::ClientKick:
	{
		m_Console.AddItalicMessage("You have been kicked by server!");
		std::string reason;
		stream.ReadString(reason);
		if (!reason.empty())
			m_Console.AddItalicMessage("Reason: {}", reason);

		m_Client->Disconnect();
		break;
	}
	default:
		break;
	}
}

void ClientLayer::SendChatMessage(std::string_view message)
{
	std::string messageToSend(message);
	if (IsValidMessage(messageToSend))
	{
		Safira::BufferStreamWriter stream(m_ScratchBuffer);
		stream.WriteRaw<PacketType>(PacketType::Message);
		stream.WriteString(messageToSend);
		m_Client->SendBuffer(stream.GetBuffer());

		m_Console.AddTaggedMessageWithColor(m_Color | 0xff000000, m_Username, messageToSend);
	}
}

void ClientLayer::SaveConnectionDetails(const std::filesystem::path& filepath)
{
	YAML::Emitter out;
	{
		out << YAML::BeginMap;
		out << YAML::Key << "ConnectionDetails" << YAML::Value;
		out << YAML::BeginMap;
		out << YAML::Key << "Username" << YAML::Value << m_Username;
		out << YAML::Key << "Color"    << YAML::Value << m_Color;
		out << YAML::Key << "ServerIP" << YAML::Value << m_ServerIP;
		out << YAML::EndMap;
		out << YAML::EndMap;
	}

	std::ofstream fout(filepath);
	fout << out.c_str();
}

bool ClientLayer::LoadConnectionDetails(const std::filesystem::path& filepath)
{
	if (!std::filesystem::exists(filepath))
		return false;

	YAML::Node data;
	try
	{
		data = YAML::LoadFile(filepath.string());
	}
	catch (YAML::ParserException e)
	{
		std::cout << "[ERROR] Failed to load connection details " << filepath
		          << std::endl << e.what() << std::endl;
		return false;
	}

	auto rootNode = data["ConnectionDetails"];
	if (!rootNode)
		return false;

	m_Username = rootNode["Username"].as<std::string>();

	m_Color = rootNode["Color"].as<uint32_t>();
	ImVec4 color = ImColor(m_Color).Value;
	m_ColorBuffer[0] = color.x;
	m_ColorBuffer[1] = color.y;
	m_ColorBuffer[2] = color.z;
	m_ColorBuffer[3] = color.w;

	m_ServerIP = rootNode["ServerIP"].as<std::string>();

	return true;
}