#include "ClientLayer.h"
#include "ClientWiring.h"
#include "PacketSerialize.h"
#include "GuiApp.h"
#include "Theme.h"
#include "WolfSSLCrypto.h"
#include "BotanP2PCrypto.h"

using Safira::Theme;

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <format>

#include <spdlog/spdlog.h>

// ─────────────────────────────────────────────────────────────────────────────
// Store dispatch helper
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::Dispatch(Safira::ClientActionVariant action) {
    m_Store->Dispatch(std::move(action));
}

// ─────────────────────────────────────────────────────────────────────────────
// Layer lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::OnAttach() {
    m_Client = std::make_unique<Safira::Client>();

    Safira::Middleware::ClientEffectHandler effectHandler;
    WireEffectHandler(effectHandler);

    // Load connection details (before store exists)
    std::string loadedUsername, loadedServerIP;
    if (std::filesystem::exists(m_ConnectionDetailsFilePath)) {
        try {
            auto data = YAML::LoadFile(m_ConnectionDetailsFilePath.string());
            auto root = data["ConnectionDetails"];
            if (root) {
                loadedUsername  = root["Username"].as<std::string>("");
                loadedServerIP = root["ServerIP"].as<std::string>("127.0.0.1");
                std::string avatarPath =
                    root["AvatarImagePath"].as<std::string>("");
                if (!avatarPath.empty())
                    m_AvatarManager.LoadFromFile(avatarPath);
            }
        } catch (const YAML::ParserException& e) {
            spdlog::error("failed to parse {}: {}",
                m_ConnectionDetailsFilePath.string(), e.what());
        }
    }

    // Create wired store
    Safira::ClientState initialState;
    initialState.Username      = loadedUsername;
    initialState.ServerAddress = loadedServerIP;

    m_Store = Safira::CreateClientStore(
        std::move(initialState), std::move(effectHandler));

    m_ConnectionView.SetInitialFields(loadedUsername, loadedServerIP);
    WireViewCallbacks();
    WireInfrastructureCallbacks();

    Safira::ApplicationGUI::Get().m_OnLogout = [this]() { Logout(); };
}

// ─────────────────────────────────────────────────────────────────────────────
// WireEffectHandler
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::WireEffectHandler(
    Safira::Middleware::ClientEffectHandler& h)
{
    h.SendToServer = [this](Safira::ByteSpan data) {
        m_Client->Send(data);
    };

    h.ConnectToServer = [this](const std::string& address) {
        m_Client->ConnectToServer(address);
    };

    h.Disconnect = [this]() {
        m_Client->RequestDisconnect();
    };

    h.StartP2PResponder = [this](const std::string& peerUsername) {
        const auto& state = m_Store->GetState();
        auto session = std::make_unique<Safira::PrivateChatSession>(
            state.Username, peerUsername);
        const uint16_t port = session->StartAsResponder();
        if (port == 0) {
            m_Console.AddItalicMessage(
                "Failed to start P2P listener for {}", peerUsername);
            return;
        }
        m_PrivateChats[peerUsername] = std::move(session);

        Safira::ByteBuffer scratch(256);
        Safira::BufferWriter w(scratch);
        Safira::SerializePacket(w, Safira::PrivateChatResponsePacket{
            peerUsername, true, port });
        m_Client->Send(w.Written());

        m_Console.AddItalicMessage(
            "Accepted private chat with {}. Waiting for connection...",
            peerUsername);
    };

    h.StartP2PInitiator = [this](const std::string& peerUsername,
                                  const std::string& address) {
        const auto& state = m_Store->GetState();
        auto session = std::make_unique<Safira::PrivateChatSession>(
            state.Username, peerUsername);
        session->StartAsInitiator(address);
        m_PrivateChats[peerUsername] = std::move(session);
        m_Console.AddItalicMessage(
            "Connecting to {} for private chat...", peerUsername);
    };

    h.CloseP2P = [this](const std::string& peerUsername) {
        if (auto it = m_PrivateChats.find(peerUsername);
            it != m_PrivateChats.end()) {
            it->second->Close();
            m_PrivateChats.erase(it);
        }
    };

    h.CloseAllP2P = [this]() {
        for (auto& [name, session] : m_PrivateChats)
            session->Close();
        m_PrivateChats.clear();
    };

    h.SaveDetails = [this]() {
        SaveConnectionDetails(m_ConnectionDetailsFilePath);
    };

    h.LogInfo = [this](const std::string& msg) {
        m_Console.AddItalicMessage("{}", msg);
    };

    h.LogItalic = [this](const std::string& msg) {
        m_Console.AddItalicMessage("{}", msg);
    };

    h.LogTagged = [this](const std::string& msg,
                          const std::string& tag, uint32_t color) {
        m_Console.AddTaggedMessageWithColor(color, tag, msg);
    };

    h.OnActionProcessed = [this](const Safira::ClientActionVariant& action,
                                  const Safira::ClientState& state) {
        OnActionProcessed(action, state);
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// WireViewCallbacks
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::WireViewCallbacks() {
    m_ConnectionView.SetOnConnect([this](const std::string& addr,
                                         const std::string& user) {
        Dispatch(Safira::ClientAction::ConnectRequested{
            addr, user, m_AvatarManager.OwnBytes() });

        auto& a = Safira::ApplicationGUI::Get();
        a.m_TitlebarUserName  = user;
        a.m_TitlebarAvatarTex = m_AvatarManager.OwnTexture();
    });

    m_ConnectionView.SetOnQuit([]() {
        Safira::ApplicationGUI::Get().Close();
    });

    m_ConnectionView.SetOnBrowse([this]() -> std::optional<std::string> {
        auto path = Safira::FileDialog::OpenImage();
        if (!path) return std::nullopt;

        auto result = m_AvatarManager.LoadFromFile(*path);
        if (result.NeedsCrop) {
            m_ConnectionView.OpenCropModal(
                result.SrcW, result.SrcH,
                m_AvatarManager.DefaultCrop(),
                m_AvatarManager.CropPreviewTex());
        }
        return path;
    });

    m_ConnectionView.SetOnAvatarCleared([this]() {
        m_AvatarManager.ClearOwn();
    });

    m_ConnectionView.SetOnCropApplied([this](const std::string& /*path*/,
                                              const Safira::CropRect& crop) {
        m_AvatarManager.ApplyCrop(m_AvatarManager.OwnImagePath(), crop);
    });

    m_InvitePopup.SetOnResponse([this](const std::string& from, bool accepted) {
        Dispatch(Safira::ClientAction::RespondToPrivateChatInvite{
            from, accepted });
    });

    m_ChatPanel.GetUserListView().SetOnInvite(
        [this](const std::string& username) {
            Dispatch(Safira::ClientAction::SendPrivateChatInvite{ username });
            AddLobbyMessage("System",
                std::format("Invited {} to a private chat.", username),
                Theme::Get().TextSystem, Safira::MessageRole::System);
        });

    m_ChatPanel.GetUserListView().SetOnReport(
        [this](const std::string& username, const std::string& reason) {
            std::string reportMsg = std::format("/report {} {}", username, reason);
            Dispatch(Safira::ClientAction::SendMessage{ reportMsg });
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// WireInfrastructureCallbacks
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::WireInfrastructureCallbacks() {
    auto& app = Safira::ApplicationGUI::Get();

    m_Client->OnServerConnected([this, &app]() {
        app.QueueEvent([this] {
            Dispatch(Safira::ClientAction::TlsHandshakeComplete{});

            m_Console.ClearLog();
            {
                std::lock_guard<std::mutex> lock(m_LobbyMutex);
                m_LobbyMessages.clear();
            }

            auto& a = Safira::ApplicationGUI::Get();
            a.m_TitlebarConnected = true;
        });
    });

    m_Client->OnServerDisconnected([this, &app]() {
        app.QueueEvent([this] {
            Dispatch(Safira::ClientAction::Disconnected{});

            m_Console.AddItalicMessageWithColor(
                0xFF8A8A8A, "Lost connection to server!");
            AddLobbyMessage("System", "Lost connection to server!",
                            Theme::Get().TextSystem,
                            Safira::MessageRole::System);

            auto& a = Safira::ApplicationGUI::Get();
            a.m_TitlebarConnected = false;
        });
    });

    m_Client->OnDataReceived([this, &app](Safira::ByteSpan data) {
        std::vector<uint8_t> copied(data.begin(), data.end());
        app.QueueEvent([this, payload = std::move(copied)]() mutable {
            Dispatch(Safira::ClientAction::DataReceived{
                std::move(payload) });
        });
    });

    m_Console.SetMessageSendCallback([this](std::string_view msg) {
        std::string message(msg);
        if (message == "/afk" || message == "/away") {
            auto& a = Safira::ApplicationGUI::Get();
            a.m_UserManualAway = !a.m_UserManualAway;
            const char* status = a.m_UserManualAway ? "Away" : "Online";
            AddLobbyMessage("System",
                std::format("Status changed to {}.", status),
                Theme::Get().TextSystem, Safira::MessageRole::System);
            if (!a.m_UserManualAway)
                a.m_LastActivityTime = std::chrono::steady_clock::now();
            return;
        }

        Dispatch(Safira::ClientAction::SendMessage{ message });
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// OnDetach / OnUIRender / IsConnected / OnDisconnectButton
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::OnDetach() {
    for (auto& [name, session] : m_PrivateChats)
        session->Close();
    m_PrivateChats.clear();
    m_Client->Disconnect();
}

void ClientLayer::OnUIRender() {
    const auto clientStatus = m_Client->GetConnectionStatus();
    const auto storeStatus  = m_Store ? m_Store->GetState().Status
                                       : Safira::ConnectionStatus::Disconnected;
    const auto debugMsg     = m_Client->GetConnectionDebugMessage();

    m_ConnectionView.Render(clientStatus, storeStatus, debugMsg,
                            m_AvatarManager.OwnTexture());

    if (m_Store) {
        const auto& state = m_Store->GetState();
        std::string inviteFrom;
        if (!state.IncomingInvites.empty())
            inviteFrom = state.IncomingInvites.front().FromUsername;
        m_InvitePopup.Render(inviteFrom);
    }

    RenderChatWindow();

    std::erase_if(m_PrivateChats, [](auto& pair) {
        return pair.second->IsClosed();
    });
}

bool ClientLayer::IsConnected() const {
    return m_Store
        && m_Store->GetState().Status == Safira::ConnectionStatus::Connected;
}

void ClientLayer::OnDisconnectButton() {
    Dispatch(Safira::ClientAction::DisconnectRequested{});
}

// ─────────────────────────────────────────────────────────────────────────────
// OnActionProcessed -- presentation sync after store state update
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::OnActionProcessed(
    const Safira::ClientActionVariant& action,
    const Safira::ClientState& state)
{
    namespace CA = Safira::ClientAction;

    std::visit(Safira::Overloaded{
        [&](const CA::Connected&) {
            m_ShowSuccessfulConnectionMessage = true;
            m_Console.AddItalicMessageWithColor(
                0xFF8A8A8A, "Welcome {}!", state.Username);
            AddLobbyMessage("System",
                std::format("Welcome {}!", state.Username),
                Theme::Get().TextSystem, Safira::MessageRole::System);
        },

        [&](const CA::ConnectionFailed&) {
            m_Console.AddItalicMessageWithColor(
                0xFFFA4A4A,
                "Server rejected connection with username {}",
                state.Username);
            AddLobbyMessage("System",
                std::format("Server rejected username {}", state.Username),
                0xFFFA4A4A, Safira::MessageRole::System);
        },

        [&](const CA::MessageReceived& a) {
            uint32_t col = state.ConnectedClients.contains(a.From)
                           ? state.ConnectedClients.at(a.From).Color
                           : 0xFFFFFFFF;
            if (a.From == "SERVER") col = 0xFFFFFFFF;
            m_Console.AddTaggedMessageWithColor(col, a.From, a.Message);

            if (a.From == state.Username) return;
            AddLobbyMessage(a.From, a.Message, col,
                            Safira::MessageRole::Peer);
        },

        [&](const CA::ClientListReceived& a) {
            for (const auto& u : a.Clients) {
                if (u.Username == state.Username) continue;
                m_AvatarManager.CachePeer(u.Username, u.AvatarData);
            }
            {
                std::lock_guard<std::mutex> lock(m_LobbyMutex);
                m_AvatarManager.PatchAvatarsInMessages(
                    m_LobbyMessages, state.Username);
            }
        },

        [&](const CA::ClientConnectedEvent& a) {
            if (a.Client.Username != state.Username) {
                m_Console.AddItalicMessageWithColor(
                    a.Client.Color, "Welcome {}!", a.Client.Username);
                m_AvatarManager.CachePeer(
                    a.Client.Username, a.Client.AvatarData);
                AddLobbyMessage("System",
                    std::format("{} joined the lobby.", a.Client.Username),
                    Theme::Get().TextSystem, Safira::MessageRole::System);
            }
        },

        [&](const CA::ClientDisconnectedEvent& a) {
            {
                std::lock_guard<std::mutex> lock(m_LobbyMutex);
                for (auto& msg : m_LobbyMessages) {
                    if (msg.Who == a.Client.Username)
                        msg.AvatarTex = {};
                }
            }
            m_AvatarManager.RemovePeer(a.Client.Username);
            m_Console.AddItalicMessageWithColor(
                a.Client.Color, "Goodbye {}!", a.Client.Username);
            AddLobbyMessage("System",
                std::format("Goodbye {}!", a.Client.Username),
                Theme::Get().TextSystem, Safira::MessageRole::System);
        },

        [&](const CA::MessageHistoryReceived& a) {
            for (const auto& m : a.Messages) {
                uint32_t col = state.ConnectedClients.contains(m.Username)
                               ? state.ConnectedClients.at(m.Username).Color
                               : 0xFFFFFFFF;
                m_Console.AddTaggedMessageWithColor(
                    col, m.Username, m.Message);

                Safira::MessageRole role = (m.Username == state.Username)
                    ? Safira::MessageRole::Own
                    : Safira::MessageRole::Peer;
                AddLobbyMessage(m.Username, m.Message, col, role);
            }
            if (m_ShowSuccessfulConnectionMessage) {
                m_ShowSuccessfulConnectionMessage = false;
                m_Console.AddItalicMessageWithColor(
                    0xFF8A8A8A,
                    "Successfully connected to {} with username {}",
                    state.ServerAddress, state.Username);
                AddLobbyMessage("System",
                    std::format("Connected to {} as {}",
                                state.ServerAddress, state.Username),
                    Theme::Get().TextSystem, Safira::MessageRole::System);
            }
        },

        [&](const CA::ServerShutdownReceived&) {
            m_Console.AddItalicMessage("Server is shutting down... goodbye!");
            AddLobbyMessage("System", "Server is shutting down...",
                            Theme::Get().TextSystem,
                            Safira::MessageRole::System);
        },

        [&](const CA::Kicked& a) {
            m_Console.AddItalicMessage("You have been kicked by server!");
            AddLobbyMessage("System", "You have been kicked!",
                            0xFFFA4A4A, Safira::MessageRole::System);
            if (!a.Reason.empty()) {
                m_Console.AddItalicMessage("Reason: {}", a.Reason);
                AddLobbyMessage("System",
                    std::format("Reason: {}", a.Reason),
                    0xFFFA4A4A, Safira::MessageRole::System);
            }
        },

        [&](const CA::PrivateChatDeclined& a) {
            m_Console.AddItalicMessage(
                "{} declined your private chat request.", a.PeerUsername);
            AddLobbyMessage("System",
                std::format("{} declined your chat request.", a.PeerUsername),
                Theme::Get().TextSystem, Safira::MessageRole::System);
        },

        [&](const CA::SendMessage& a) {
            std::string msg = a.Message;
            if (!Safira::IsValidMessage(msg)) return;
            AddLobbyMessage(state.Username, msg,
                            state.Color | 0xFF000000,
                            Safira::MessageRole::Own);
        },

        [&](const auto&) {},
    }, action);
}

// ─────────────────────────────────────────────────────────────────────────────
// AddLobbyMessage
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::AddLobbyMessage(const std::string& who,
                                  const std::string& text,
                                  uint32_t color,
                                  Safira::MessageRole role) {
    std::lock_guard<std::mutex> lock(m_LobbyMutex);
    m_LobbyMessages.push_back({
        .Who       = who,
        .Text      = text,
        .Color     = color,
        .Role      = role,
        .Time      = Safira::ChatPanel::NowTimestamp(),
        .AvatarTex = {},
    });

    const auto& state = m_Store->GetState();

    if (who == state.Username && m_AvatarManager.OwnTexture()) {
        m_LobbyMessages.back().AvatarTex = m_AvatarManager.OwnTexture();
    } else {
        auto peerTex = m_AvatarManager.PeerTexture(who);
        if (peerTex)
            m_LobbyMessages.back().AvatarTex = peerTex;
    }

    if (m_ActiveConvoIdx == 0)
        m_ChatPanel.RequestScrollToBottom();
}

// ─────────────────────────────────────────────────────────────────────────────
// RebuildConversationList
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::RebuildConversationList() {
    m_ConversationList.clear();

    {
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        m_LobbySnapshot = m_LobbyMessages;
    }

    m_ConversationList.push_back({
        .Title     = "Lobby",
        .Preview   = m_LobbySnapshot.empty()
                         ? "Send a message..."
                         : m_LobbySnapshot.back().Text.substr(0, 36),
        .TimeLabel = "",
        .Messages  = &m_LobbySnapshot,
        .HasUnread = false,
        .AvatarTex = {},
    });

    const auto& state = m_Store->GetState();

    for (auto& [peer, session] : m_PrivateChats) {
        auto* entries = session->RefreshAndGetChatEntries(state.Username);

        m_ConversationList.push_back({
            .Title     = peer,
            .Preview   = entries->empty()
                             ? "..."
                             : entries->back().Text.substr(0, 36),
            .TimeLabel = session->IsConnected() ? "online" : "",
            .Messages  = entries,
            .HasUnread = false,
            .AvatarTex = m_AvatarManager.PeerTexture(peer),
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Logout / LeavePrivateChat
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::Logout() {
    Dispatch(Safira::ClientAction::DisconnectRequested{});

    m_ActiveConvoIdx = 0;
    {
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        m_LobbyMessages.clear();
    }
    m_LobbySnapshot.clear();
    m_ConversationList.clear();
    m_Console.ClearLog();
    m_AvatarManager.ClearAllPeers();

    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarUserName.clear();
    app.m_TitlebarConnected  = false;
    app.m_TitlebarAvatarTex   = ImTextureID{};
    app.m_UserManualAway      = false;
}

void ClientLayer::LeavePrivateChat(const std::string& peerUsername) {
    Dispatch(Safira::ClientAction::LeavePrivateChat{ peerUsername });
    m_ActiveConvoIdx = 0;

    AddLobbyMessage("System",
        std::format("Left private chat with {}.", peerUsername),
        Theme::Get().TextSystem, Safira::MessageRole::System);
}

// ─────────────────────────────────────────────────────────────────────────────
// RenderChatWindow -- data preparation + dispatch (zero ImGui code)
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::RenderChatWindow() {
    if (!Safira::ApplicationGUI::Get().IsChatPanelVisible())
        return;

    RebuildConversationList();

    const auto& state = m_Store->GetState();

    // ── Build layout input ──
    Safira::FullLayoutInput input;

    // User list entries
    for (const auto& [username, info] : state.ConnectedClients) {
        if (username.empty()) continue;
        Safira::UserListEntry entry;
        entry.Username      = username;
        entry.Color         = info.Color;
        entry.IsOwnUser     = (username == state.Username);
        entry.InPrivateChat = m_PrivateChats.contains(username);
        entry.InvitePending = state.PendingOutgoingInvites.contains(username);
        entry.AvatarTex     = entry.IsOwnUser
            ? m_AvatarManager.OwnTexture()
            : m_AvatarManager.PeerTexture(username);
        input.Users.push_back(std::move(entry));
    }

    // Conversations
    input.Conversations = &m_ConversationList;
    input.ActiveConvoIdx = m_ActiveConvoIdx;
    input.Username = state.Username;

    // Determine active conversation properties
    const bool isPrivate = (m_ActiveConvoIdx > 0);
    input.IsPrivateChat = isPrivate;
    std::string peerName;

    if (m_ActiveConvoIdx >= 0
        && m_ActiveConvoIdx < static_cast<int>(m_ConversationList.size())
        && m_ConversationList[m_ActiveConvoIdx].Messages)
    {
        input.Messages = m_ConversationList[m_ActiveConvoIdx].Messages;
        input.ConvoTitle = m_ConversationList[m_ActiveConvoIdx].Title;

        if (m_ActiveConvoIdx == 0) {
            input.IsConnected = IsConnected();
            input.StatusProtocol = Safira::WolfSSLCrypto::ProtocolDescription();
        } else {
            int sessionIdx = 0;
            for (auto& [peer, session] : m_PrivateChats) {
                if (sessionIdx == m_ActiveConvoIdx - 1) {
                    input.IsConnected   = session->IsConnected();
                    input.IsHandshaking = session->IsRunning()
                                          && !input.IsConnected;
                    input.StatusProtocol = session->IsEncrypted()
                        ? Safira::BotanP2PCrypto::ProtocolDescription()
                        : "TCP | No encryption";
                    peerName = peer;
                    break;
                }
                sessionIdx++;
            }
            input.PeerName = peerName;
            input.OwnAvatar  = m_AvatarManager.OwnTexture();
            input.PeerAvatar = m_AvatarManager.PeerTexture(peerName);
        }
    }

    // Suppress overlay when modals are open
    input.SuppressOverlay = !IsConnected()
        || (m_Store && !m_Store->GetState().IncomingInvites.empty());

    // ── Render and handle output ──
    auto output = m_ChatPanel.RenderFullLayout(input);

    if (output.NewActiveIdx)
        m_ActiveConvoIdx = *output.NewActiveIdx;

    if (output.LeaveRequested && !peerName.empty())
        LeavePrivateChat(peerName);

    if (output.PendingMessage) {
        if (m_ActiveConvoIdx == 0) {
            Dispatch(Safira::ClientAction::SendMessage{ *output.PendingMessage });
        } else {
            int sessionIdx = 0;
            for (auto& [peer, session] : m_PrivateChats) {
                if (sessionIdx == m_ActiveConvoIdx - 1) {
                    session->Send(*output.PendingMessage);
                    session->AppendMessage(
                        state.Username, *output.PendingMessage, 0xFFFFFFFF);
                    break;
                }
                sessionIdx++;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────────

void ClientLayer::SaveConnectionDetails(
    const std::filesystem::path& filepath)
{
    const auto& state = m_Store->GetState();

    YAML::Emitter out;
    out << YAML::BeginMap
        << YAML::Key << "ConnectionDetails" << YAML::Value << YAML::BeginMap
            << YAML::Key << "Username"        << YAML::Value << state.Username
            << YAML::Key << "ServerIP"        << YAML::Value << state.ServerAddress
            << YAML::Key << "AvatarImagePath" << YAML::Value << m_AvatarManager.OwnImagePath()
        << YAML::EndMap
        << YAML::EndMap;

    std::ofstream fout(filepath);
    fout << out.c_str();
}
