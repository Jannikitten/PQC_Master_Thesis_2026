#include "ClientLayer.h"
#include "ClientWiring.h"
#include "PacketSerialize.h"
#include "GuiApp.h"
#include "Theme.h"
#include "UIWidgets.h"
#include "misc/cpp/imgui_stdlib.h"

using Safira::Theme;
using Safira::U32ToVec4;

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <numeric>
#include <ranges>
#include <format>

#include <spdlog/spdlog.h>

// ═════════════════════════════════════════════════════════════════════════════
// Font-safe helpers
// ═════════════════════════════════════════════════════════════════════════════

namespace {

ImFont* SidebarBodyFont() {
    ImFont* f = Safira::ApplicationGUI::GetFont("Default");
    return f ? f : ImGui::GetFont();
}

ImFont* SidebarBoldFont() {
    ImFont* f = Safira::ApplicationGUI::GetFont("Bold");
    return f ? f : SidebarBodyFont();
}

ImVec2 SidebarMeasureText(ImFont* f, const char* text, float wrapWidth = 0.0f) {
    if (f) ImGui::PushFont(f);
    ImVec2 sz = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
    if (f) ImGui::PopFont();
    return sz;
}

void SidebarDrawTextTruncated(ImFont* f, ImVec2 pos, ImU32 col,
                              const char* text, float maxW) {
    if (f) ImGui::PushFont(f);

    ImVec2 fullSz = ImGui::CalcTextSize(text);
    if (fullSz.x <= maxW) {
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, text);
    } else {
        const char* ellipsis = "...";
        float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
        float availW = maxW - ellipsisW;
        if (availW < 0.0f) availW = 0.0f;

        int len = (int)strlen(text);
        int lo = 0, hi = len, best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            ImVec2 sz = ImGui::CalcTextSize(text, text + mid);
            if (sz.x <= availW) {
                best = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }

        std::string trunc(text, best);
        trunc += ellipsis;
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), pos, col, trunc.c_str());
    }

    if (f) ImGui::PopFont();
}

size_t HashAvatarData(const std::vector<uint8_t>& data) {
    size_t h = 14695981039346656037ULL;
    for (auto b : data) {
        h ^= static_cast<size_t>(b);
        h *= 1099511628211ULL;
    }
    return h;
}

} // anon namespace

// ═════════════════════════════════════════════════════════════════════════════
// DrawAvatarCircle
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::DrawAvatarCircle(ImDrawList* dl, ImVec2 center, float radius,
                                   uint32_t color, const std::string& username,
                                   ImTextureID tex) {
    if (tex) {
        dl->AddImageRounded(tex,
            { center.x - radius, center.y - radius },
            { center.x + radius, center.y + radius },
            { 0, 0 }, { 1, 1 },
            Theme::Get().AvatarImageTint, radius);
    } else {
        dl->AddCircleFilled(center, radius, color, 24);
        dl->AddCircle(center, radius, Theme::Get().IconOutline, 0, 1.5f);

        char letter = username.empty()
            ? '?'
            : static_cast<char>(toupper(username[0]));
        char buf[2] = { letter, '\0' };

        ImFont* bold = SidebarBoldFont();
        ImVec2 lsz = SidebarMeasureText(bold, buf);
        if (bold) ImGui::PushFont(bold);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { center.x - lsz.x * 0.5f, center.y - lsz.y * 0.5f },
                    Theme::Get().AvatarLetterCol, buf);
        if (bold) ImGui::PopFont();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Store dispatch helper + state accessor
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::Dispatch(Safira::ClientActionVariant action) {
    m_Store->Dispatch(std::move(action));
}

// ═════════════════════════════════════════════════════════════════════════════
// Layer lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::OnAttach() {
    m_Client = std::make_unique<Safira::Client>();
    auto& app = Safira::ApplicationGUI::Get();

    // -- Build effect handler -------------------------------------------------
    Safira::Middleware::ClientEffectHandler effectHandler;

    effectHandler.SendToServer = [this](Safira::ByteSpan data) {
        m_Client->Send(data);
    };

    effectHandler.ConnectToServer = [this](const std::string& address) {
        m_Client->ConnectToServer(address);
    };

    effectHandler.Disconnect = [this]() {
        m_Client->RequestDisconnect();
    };

    effectHandler.StartP2PResponder = [this](const std::string& peerUsername) {
        const auto& state = m_Store->GetState();
        auto session = std::make_unique<Safira::PrivateChatSession>(
            state.Username, peerUsername);
        const uint16_t port = session->StartAsResponder(
            Safira::P2PKeyType::RSA_PSS);
        if (port == 0) {
            m_Console.AddItalicMessage(
                "Failed to start P2P listener for {}", peerUsername);
            return;
        }
        m_PrivateChats[peerUsername] = std::move(session);

        // Send the accept response with our listen port
        Safira::ByteBuffer scratch(256);
        Safira::BufferWriter w(scratch);
        Safira::SerializePacket(w, Safira::PrivateChatResponsePacket{
            peerUsername, true, port });
        m_Client->Send(w.Written());

        m_Console.AddItalicMessage(
            "Accepted private chat with {}. Waiting for connection...",
            peerUsername);
    };

    effectHandler.StartP2PInitiator = [this](
        const std::string& peerUsername, const std::string& address)
    {
        const auto& state = m_Store->GetState();
        auto session = std::make_unique<Safira::PrivateChatSession>(
            state.Username, peerUsername);
        session->StartAsInitiator(address);
        m_PrivateChats[peerUsername] = std::move(session);
        m_Console.AddItalicMessage(
            "Connecting to {} for private chat...", peerUsername);
    };

    effectHandler.CloseP2P = [this](const std::string& peerUsername) {
        if (auto it = m_PrivateChats.find(peerUsername);
            it != m_PrivateChats.end())
        {
            it->second->Close();
            m_PrivateChats.erase(it);
        }
    };

    effectHandler.CloseAllP2P = [this]() {
        for (auto& [name, session] : m_PrivateChats)
            session->Close();
        m_PrivateChats.clear();
    };

    effectHandler.SaveDetails = [this]() {
        SaveConnectionDetails(m_ConnectionDetailsFilePath);
    };

    effectHandler.LogInfo = [this](const std::string& msg) {
        m_Console.AddItalicMessage("{}", msg);
    };

    effectHandler.LogItalic = [this](const std::string& msg) {
        m_Console.AddItalicMessage("{}", msg);
    };

    effectHandler.LogTagged = [this](const std::string& msg,
                                     const std::string& tag, uint32_t color)
    {
        m_Console.AddTaggedMessageWithColor(color, tag, msg);
    };

    effectHandler.OnActionProcessed = [this](
        const Safira::ClientActionVariant& action,
        const Safira::ClientState& state)
    {
        OnActionProcessed(action, state);
    };

    // -- Load connection details (before store exists) ------------------------
    std::string loadedUsername, loadedServerIP;
    {
        if (std::filesystem::exists(m_ConnectionDetailsFilePath)) {
            try {
                auto data = YAML::LoadFile(
                    m_ConnectionDetailsFilePath.string());
                auto root = data["ConnectionDetails"];
                if (root) {
                    loadedUsername = root["Username"].as<std::string>("");
                    loadedServerIP = root["ServerIP"].as<std::string>(
                        "127.0.0.1");
                    m_AvatarImagePath =
                        root["AvatarImagePath"].as<std::string>("");
                    if (!m_AvatarImagePath.empty())
                        LoadAvatarImage(m_AvatarImagePath);
                }
            } catch (const YAML::ParserException& e) {
                spdlog::error("failed to parse {}: {}",
                    m_ConnectionDetailsFilePath.string(), e.what());
            }
        }
    }

    // -- Create wired store ---------------------------------------------------
    Safira::ClientState initialState;
    initialState.Username      = loadedUsername;
    initialState.ServerAddress = loadedServerIP;

    m_Store = Safira::CreateClientStore(
        std::move(initialState), std::move(effectHandler));

    // -- Wire infrastructure callbacks ----------------------------------------
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

    app.m_OnLogout = [this]() { Logout(); };
}

void ClientLayer::OnDetach() {
    for (auto& [name, session] : m_PrivateChats)
        session->Close();
    m_PrivateChats.clear();
    m_Client->Disconnect();
}

void ClientLayer::OnUIRender() {
    UI_ConnectionModal();
    UI_CropModal();
    UI_IncomingInvites();
    UI_ReportModal();
    UI_UnifiedChatWindow();

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

// ═════════════════════════════════════════════════════════════════════════════
// OnActionProcessed — presentation sync after store state update
// ═════════════════════════════════════════════════════════════════════════════

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
                UploadPeerAvatarTexture(u.Username, u.AvatarData);
            }

            std::vector<std::string> removedUsers;
            std::erase_if(m_PeerAvatars, [&](const auto& pair) {
                const bool remove =
                    !state.ConnectedClients.contains(pair.first);
                if (remove) removedUsers.push_back(pair.first);
                return remove;
            });
            if (!removedUsers.empty()) {
                std::lock_guard<std::mutex> lock(m_LobbyMutex);
                for (auto& msg : m_LobbyMessages) {
                    if (std::ranges::find(removedUsers, msg.Who)
                        != removedUsers.end())
                        msg.AvatarTex = {};
                }
            }
        },

        [&](const CA::ClientConnectedEvent& a) {
            if (a.Client.Username != state.Username) {
                m_Console.AddItalicMessageWithColor(
                    a.Client.Color, "Welcome {}!", a.Client.Username);
                UploadPeerAvatarTexture(
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
            m_PeerAvatars.erase(a.Client.Username);
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

// ═════════════════════════════════════════════════════════════════════════════
// AddLobbyMessage
// ═════════════════════════════════════════════════════════════════════════════

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

    if (who == state.Username && m_AvatarTexture) {
        m_LobbyMessages.back().AvatarTex = m_AvatarTexture;
    } else if (auto it = m_PeerAvatars.find(who);
               it != m_PeerAvatars.end()) {
        m_LobbyMessages.back().AvatarTex = it->second.Tex;
    }

    if (m_ActiveConvoIdx == 0)
        m_ChatPanel.RequestScrollToBottom();
}

// ═════════════════════════════════════════════════════════════════════════════
// RebuildConversationList
// ═════════════════════════════════════════════════════════════════════════════

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

        ImTextureID peerTex = {};
        if (auto it = m_PeerAvatars.find(peer); it != m_PeerAvatars.end())
            peerTex = it->second.Tex;

        m_ConversationList.push_back({
            .Title     = peer,
            .Preview   = entries->empty()
                             ? "..."
                             : entries->back().Text.substr(0, 36),
            .TimeLabel = session->IsConnected() ? "online" : "",
            .Messages  = entries,
            .HasUnread = false,
            .AvatarTex = peerTex,
        });
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// LoadAvatarImage
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::LoadAvatarImage(const std::string& filepath) {
    if (filepath.empty()) return;

    auto raw = Safira::LoadImageFromFile(filepath);
    if (!raw.Valid()) {
        spdlog::warn("Failed to load avatar image: {}", filepath);
        return;
    }

    m_AvatarImagePath = filepath;

    if (raw.NeedsCrop()) {
        m_CropSrcWidth  = raw.Width;
        m_CropSrcHeight = raw.Height;
        m_CropRect      = Safira::DefaultCenterCrop(raw);

        m_CropPreviewImage = std::make_shared<Safira::Image>(
            static_cast<uint32_t>(raw.Width),
            static_cast<uint32_t>(raw.Height),
            Safira::ImageFormat::RGBA,
            raw.Pixels.get());
        m_CropPreviewTex = (ImTextureID)m_CropPreviewImage->GetDescriptorSet();

        m_ShowCropModal = true;
        spdlog::info("Avatar {}x{} is not square — opening crop UI",
                     raw.Width, raw.Height);
    } else {
        auto crop = Safira::DefaultCenterCrop(raw);
        auto rgba = Safira::ProcessAvatarImage(filepath, crop);
        if (!rgba) {
            spdlog::warn("Avatar processing failed for {}", filepath);
            return;
        }
        m_ProcessedAvatarBytes = std::move(*rgba);

        m_AvatarImage = std::make_shared<Safira::Image>(
            static_cast<uint32_t>(raw.Width),
            static_cast<uint32_t>(raw.Height),
            Safira::ImageFormat::RGBA,
            raw.Pixels.get());
        m_AvatarTexture = (ImTextureID)m_AvatarImage->GetDescriptorSet();
        spdlog::info("Avatar loaded: {}x{} from {} ({} bytes raw RGBA)",
                     raw.Width, raw.Height, filepath,
                     m_ProcessedAvatarBytes.size());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UploadPeerAvatarTexture
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UploadPeerAvatarTexture(const std::string& username,
                                          const std::vector<uint8_t>& avatarData) {
    if (avatarData.empty()) {
        m_PeerAvatars.erase(username);
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        for (auto& msg : m_LobbyMessages) {
            if (msg.Who == username)
                msg.AvatarTex = {};
        }
        return;
    }

    const size_t hash = HashAvatarData(avatarData);

    if (auto it = m_PeerAvatars.find(username); it != m_PeerAvatars.end()) {
        if (it->second.DataHash == hash)
            return;
    }

    const uint32_t side = Safira::kAvatarPixelSize;
    const size_t expectedSize = side * side * 4;
    if (avatarData.size() != expectedSize) {
        spdlog::warn("Avatar for {} has unexpected size {} (expected {})",
                     username, avatarData.size(), expectedSize);
        m_PeerAvatars.erase(username);
        std::lock_guard<std::mutex> lock(m_LobbyMutex);
        for (auto& msg : m_LobbyMessages) {
            if (msg.Who == username)
                msg.AvatarTex = {};
        }
        return;
    }

    auto img = std::make_shared<Safira::Image>(
        side, side, Safira::ImageFormat::RGBA, avatarData.data());

    PeerAvatarCache cache;
    cache.Image    = std::move(img);
    cache.Tex      = (ImTextureID)cache.Image->GetDescriptorSet();
    cache.DataHash = hash;
    m_PeerAvatars[username] = std::move(cache);

    std::lock_guard<std::mutex> lock(m_LobbyMutex);
    for (auto& msg : m_LobbyMessages) {
        if (msg.Who == username)
            msg.AvatarTex = m_PeerAvatars[username].Tex;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Logout
// ═════════════════════════════════════════════════════════════════════════════

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
    m_PeerAvatars.clear();

    auto& app = Safira::ApplicationGUI::Get();
    app.m_TitlebarUserName.clear();
    app.m_TitlebarConnected  = false;
    app.m_TitlebarAvatarTex   = ImTextureID{};
    app.m_UserManualAway      = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// LeavePrivateChat
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::LeavePrivateChat(const std::string& peerUsername) {
    Dispatch(Safira::ClientAction::LeavePrivateChat{ peerUsername });
    m_ActiveConvoIdx = 0;

    AddLobbyMessage("System",
        std::format("Left private chat with {}.", peerUsername),
        Theme::Get().TextSystem, Safira::MessageRole::System);
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_ConnectionModal
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_ConnectionModal() {
    const auto clientStatus = m_Client->GetConnectionStatus();

    if (!m_ConnectionModalOpen &&
        clientStatus != Safira::ConnectionStatus::Connected &&
        !m_ShowCropModal &&
        !ImGui::IsPopupOpen("Crop Avatar##CropModal"))
        ImGui::OpenPopup("Connection");

    const auto& t = Theme::Get();
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       t.BgPopup);
    ImGui::PushStyleColor(ImGuiCol_Border,         t.ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        t.BgPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  t.BgPanel);
    ImGui::PushStyleColor(ImGuiCol_Text,           t.TextPrimary);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        t.BgFrame);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, t.BgFrameHovered);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  t.BgFrameActive);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

    const ImVec2 vpSize = ImGui::GetMainViewport()->WorkSize;
    const float modalW = std::clamp(vpSize.x * 0.52f, 500.0f, 640.0f);
    const float modalH = std::clamp(vpSize.y * 0.46f, 310.0f, 380.0f);
    ImGui::SetNextWindowSize({ modalW, modalH }, ImGuiCond_Always);
    m_ConnectionModalOpen = ImGui::BeginPopupModal(
        "Connection", nullptr, ImGuiWindowFlags_NoResize);

    if (!m_ConnectionModalOpen) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(8);
        return;
    }

    // Local input fields — synced from store once
    static std::string s_Username;
    static std::string s_ServerIP = "127.0.0.1";
    static bool s_Initialized = false;

    if (!s_Initialized && m_Store) {
        const auto& state = m_Store->GetState();
        if (!state.Username.empty()) s_Username = state.Username;
        if (!state.ServerAddress.empty()) s_ServerIP = state.ServerAddress;
        s_Initialized = true;
    }

    const std::string debugMessage = m_Client->GetConnectionDebugMessage();
    float statusReserve = 0.0f;
    if (clientStatus == Safira::ConnectionStatus::FailedToConnect) {
        statusReserve = debugMessage.empty() ? 24.0f : 42.0f;
    } else if (clientStatus == Safira::ConnectionStatus::Connecting) {
        statusReserve = 24.0f;
    }
    float bodyH = ImGui::GetContentRegionAvail().y - statusReserve - 6.0f;
    if (bodyH < 210.0f) bodyH = 210.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.BgPopupAlt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("##ConnectBody", { 0.0f, bodyH }, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float bodyW = ImGui::GetContentRegionAvail().x;
        const float leftW = std::clamp(bodyW * 0.34f, 160.0f, 220.0f);
        const float panelGap = 12.0f;
        const float panelH = ImGui::GetContentRegionAvail().y;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.BgFrame);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);

        // Left panel: avatar
        ImGui::BeginChild("##ConnectAvatarPanel", { leftW, panelH }, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImDrawList* panelDl = ImGui::GetWindowDrawList();
            const float avatarDiameter = 106.0f;
            const float avatarRadius = avatarDiameter * 0.5f;
            const float availW = ImGui::GetContentRegionAvail().x;
            const float avatarIndent =
                std::max(0.0f, (availW - avatarDiameter) * 0.5f);

            ImGui::Dummy({ 0.0f, 6.0f });
            ImGui::Indent(avatarIndent);
            ImGui::InvisibleButton("##AvatarPreview",
                                   { avatarDiameter, avatarDiameter });
            ImGui::Unindent(avatarIndent);

            const ImVec2 avatarMin = ImGui::GetItemRectMin();
            const ImVec2 avatarMax = ImGui::GetItemRectMax();
            const ImVec2 center = {
                (avatarMin.x + avatarMax.x) * 0.5f,
                (avatarMin.y + avatarMax.y) * 0.5f
            };

            panelDl->AddCircleFilled(center, avatarRadius + 8.0f,
                                     IM_COL32(44, 46, 62, 255), 48);
            panelDl->AddCircle(center, avatarRadius + 8.0f,
                               t.InputBorder, 48, 1.2f);

            if (m_AvatarTexture) {
                panelDl->AddImageRounded(m_AvatarTexture,
                    { center.x - avatarRadius, center.y - avatarRadius },
                    { center.x + avatarRadius, center.y + avatarRadius },
                    { 0, 0 }, { 1, 1 },
                    t.AvatarImageTint, avatarRadius);
            } else {
                panelDl->AddCircleFilled(center, avatarRadius,
                                         IM_COL32(84, 84, 96, 255), 48);
                char letter = s_Username.empty() ? '?'
                    : static_cast<char>(toupper(s_Username[0]));
                char buf[2] = { letter, '\0' };
                ImVec2 lsz = ImGui::CalcTextSize(buf);
                panelDl->AddText(
                    { center.x - lsz.x * 0.5f, center.y - lsz.y * 0.5f },
                    IM_COL32(236, 236, 242, 255), buf);
            }

            const float actionH = 36.0f;
            const float footerY = ImGui::GetWindowHeight()
                - ImGui::GetStyle().WindowPadding.y - actionH;
            if (ImGui::GetCursorPosY() < footerY)
                ImGui::SetCursorPosY(footerY);

            float browseW = ImGui::GetContentRegionAvail().x;
            if (m_AvatarTexture)
                browseW = std::max(96.0f, browseW - 66.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        t.SendBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.SendBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  t.SendBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          t.SendBtnText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 10.0f, 7.0f });
            if (ImGui::Button("Browse Image", { browseW, actionH })) {
                auto path = Safira::FileDialog::OpenImage();
                if (path) {
                    LoadAvatarImage(*path);
                    if (m_ShowCropModal)
                        ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            if (m_AvatarTexture) {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.LogoutBtnHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.LogoutBtnActive);
                ImGui::PushStyleColor(ImGuiCol_Text, t.LogoutIcon);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 8.0f, 7.0f });
                if (ImGui::Button("Clear", { 60.0f, actionH })) {
                    m_AvatarTexture   = {};
                    m_AvatarImage     = nullptr;
                    m_ProcessedAvatarBytes.clear();
                    m_AvatarImagePath.clear();
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(4);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelGap);

        // Right panel: connection form
        ImGui::BeginChild("##ConnectFormPanel", { 0.0f, panelH }, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImGui::TextColored(U32ToVec4(t.TextSecondary), "Username");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##username", &s_Username);

            ImGui::Dummy({ 0.0f, 10.0f });
            ImGui::TextColored(U32ToVec4(t.TextSecondary), "Server Address");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##address", &s_ServerIP);

            const float actionH = 36.0f;
            const float footerY = ImGui::GetWindowHeight()
                - ImGui::GetStyle().WindowPadding.y - actionH;
            if (ImGui::GetCursorPosY() < footerY)
                ImGui::SetCursorPosY(footerY);

            const float actionW =
                (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;

            ImGui::PushStyleColor(ImGuiCol_Button,        t.SendBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.SendBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  t.SendBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          t.SendBtnText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 12.0f, 8.0f });
            if (ImGui::Button("Connect", { actionW, actionH })) {
                std::string addr = s_ServerIP;
                if (addr.rfind(':') == std::string::npos)
                    addr += ":8192";

                Dispatch(Safira::ClientAction::ConnectRequested{
                    addr, s_Username, m_ProcessedAvatarBytes });

                auto& a = Safira::ApplicationGUI::Get();
                a.m_TitlebarUserName   = s_Username;
                a.m_TitlebarAvatarTex  = m_AvatarTexture;
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            ImGui::SameLine(0.0f, 12.0f);

            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.LogoutBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.LogoutBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text, t.LogoutIcon);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 12.0f, 8.0f });
            if (ImGui::Button("Quit", { actionW, actionH }))
                Safira::ApplicationGUI::Get().Close();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(1);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(1);

    // -- Connection status feedback -------------------------------------------
    if (clientStatus == Safira::ConnectionStatus::Connected
        && m_Store->GetState().Status == Safira::ConnectionStatus::Connected)
    {
        ImGui::CloseCurrentPopup();
    } else if (clientStatus == Safira::ConnectionStatus::FailedToConnect) {
        ImGui::TextColored({ 0.9f, 0.2f, 0.1f, 1.0f }, "Connection failed.");
        const auto msg = m_Client->GetConnectionDebugMessage();
        if (!msg.empty())
            ImGui::TextColored({ 0.9f, 0.2f, 0.1f, 1.0f }, "%s", msg.c_str());
    } else if (clientStatus == Safira::ConnectionStatus::Connecting) {
        ImGui::TextColored({ 0.8f, 0.8f, 0.8f, 1.0f }, "Connecting...");
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(8);
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_CropModal
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_CropModal() {
    if (m_ShowCropModal) {
        ImGui::OpenPopup("Crop Avatar##CropModal");
        m_ShowCropModal = false;
    }

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Get().BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Border,   Theme::Get().ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,     Theme::Get().TextPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal("Crop Avatar##CropModal", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {

        ImFont* bold = SidebarBoldFont();
        if (bold) ImGui::PushFont(bold);
        ImGui::Text("Crop to Square");
        if (bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        m_CropRect = Safira::DrawCropWidget(
            m_CropPreviewTex, m_CropSrcWidth, m_CropSrcHeight,
            m_CropRect, 300.0f);

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Get().AccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Get().AccentText);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Apply Crop", { 120, 0 })) {
            auto rgba = Safira::ProcessAvatarImage(m_AvatarImagePath,
                                                    m_CropRect);
            if (rgba) {
                m_ProcessedAvatarBytes = std::move(*rgba);

                auto raw = Safira::LoadImageFromFile(m_AvatarImagePath);
                if (raw.Valid()) {
                    auto cropped = Safira::CropSquare(raw, m_CropRect);
                    auto resized = Safira::ResizeSquare(cropped.data(),
                        m_CropRect.Size, Safira::kAvatarPixelSize);
                    m_AvatarImage = std::make_shared<Safira::Image>(
                        Safira::kAvatarPixelSize, Safira::kAvatarPixelSize,
                        Safira::ImageFormat::RGBA, resized.data());
                    m_AvatarTexture =
                        (ImTextureID)m_AvatarImage->GetDescriptorSet();
                }
                spdlog::info(
                    "Avatar cropped and processed ({} bytes raw RGBA)",
                    m_ProcessedAvatarBytes.size());
            } else {
                spdlog::warn("Avatar crop/compress failed");
            }

            m_CropPreviewImage = nullptr;
            m_CropPreviewTex   = {};
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Cancel", { 80, 0 })) {
            m_CropPreviewImage = nullptr;
            m_CropPreviewTex   = {};
            m_AvatarImagePath.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    if (!open) {
        m_CropPreviewImage = nullptr;
        m_CropPreviewTex   = {};
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_UserListSection
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_UserListSection(float) {
    ImFont* bold = SidebarBoldFont();
    const auto& state = m_Store->GetState();

    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored(U32ToVec4(Theme::Get().TextPrimary), "Online (%d)",
                       static_cast<int>(state.ConnectedClients.size()));
    if (bold) ImGui::PopFont();

    ImGui::Spacing();

    constexpr float kIconSize = 20.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Deferred action: Dispatch calls during iteration would invalidate
    // the state reference and map iterators, so we collect and dispatch after.
    std::string deferredInviteTarget;

    for (const auto& [username, info] : state.ConnectedClients) {
        if (username.empty()) continue;

        const bool isOurs = (username == state.Username);
        constexpr float itemPad = 14.0f;
        ImGui::SetCursorPosX(itemPad);
        const ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float  radius = kIconSize * 0.45f;
        const ImVec2 center = { pos.x + kIconSize * 0.5f,
                                pos.y + kIconSize * 0.5f };

        ImTextureID tex = {};
        if (isOurs && m_AvatarTexture) {
            tex = m_AvatarTexture;
        } else if (auto it = m_PeerAvatars.find(username);
                   it != m_PeerAvatars.end()) {
            tex = it->second.Tex;
        }

        DrawAvatarCircle(dl, center, radius, info.Color, username, tex);
        ImGui::Dummy({ kIconSize, kIconSize });

        if (!isOurs) {
            const std::string popupId = "##UserCtx_" + username;
            if (ImGui::IsItemHovered()
                && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                m_ContextMenuTarget = username;
                ImGui::OpenPopup(popupId.c_str());
            }

            ImGui::PushStyleColor(ImGuiCol_PopupBg,      Theme::Get().BgPopupAlt);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Get().BgFrameHovered);
            ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Get().TextPrimary);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);

            if (ImGui::BeginPopup(popupId.c_str())) {
                const bool alreadyInChat = m_PrivateChats.contains(username);
                const bool alreadyInvited =
                    state.PendingOutgoingInvites.contains(username);

                if (alreadyInChat) {
                    ImGui::TextDisabled("Already in private chat");
                } else if (alreadyInvited) {
                    ImGui::TextDisabled("Invite pending...");
                } else {
                    if (ImGui::Selectable("Invite to private chat")) {
                        deferredInviteTarget = username;
                    }
                }

                ImGui::Separator();

                if (ImGui::Selectable("Report user to server")) {
                    m_ReportTarget = username;
                    m_ReportReasonBuf[0] = '\0';
                    m_ReportModalOpen = true;
                }

                ImGui::EndPopup();
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImColor(Theme::Get().TextPrimary).Value);
        ImGui::TextUnformatted(username.c_str());
        ImGui::PopStyleColor();

        if (m_PrivateChats.contains(username)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(private)");
        } else if (state.PendingOutgoingInvites.contains(username)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(invited)");
        }
    }

    // Dispatch deferred actions now that iteration is complete.
    if (!deferredInviteTarget.empty()) {
        Dispatch(Safira::ClientAction::SendPrivateChatInvite{
            deferredInviteTarget });
        AddLobbyMessage("System",
            std::format("Invited {} to a private chat.",
                        deferredInviteTarget),
            Theme::Get().TextSystem,
            Safira::MessageRole::System);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_ReportModal
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_ReportModal() {
    if (m_ReportModalOpen) {
        ImGui::OpenPopup("Report User##ReportModal");
        m_ReportModalOpen = false;
    }

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Get().BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Border,   Theme::Get().ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,     Theme::Get().TextPrimary);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,  Theme::Get().BgFrame);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal("Report User##ReportModal", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Report %s", m_ReportTarget.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(U32ToVec4(Theme::Get().TextSecondary), "Reason:");
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextMultiline("##ReportReason", m_ReportReasonBuf,
                                   sizeof(m_ReportReasonBuf),
                                   { 300, 80 });

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Get().AccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Get().AccentText);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Submit Report", { 130, 0 })) {
            std::string reason(m_ReportReasonBuf);
            if (!reason.empty()) {
                std::string reportMsg = std::format(
                    "/report {} {}", m_ReportTarget, reason);
                Dispatch(Safira::ClientAction::SendMessage{ reportMsg });
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Cancel", { 80, 0 })) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_IncomingInvites
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_IncomingInvites() {
    const auto& state = m_Store->GetState();
    if (state.IncomingInvites.empty()) return;

    const std::string fromUser = state.IncomingInvites.front().FromUsername;
    const std::string popupId  = "Private Chat Request##" + fromUser;

    ImGui::OpenPopup(popupId.c_str());

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Get().BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Text,    Theme::Get().TextPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal(popupId.c_str(), &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s wants to chat with you privately.",
                    fromUser.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        constexpr float btnW = 120.0f;
        constexpr float btnGap = 8.0f;
        const float totalW = btnW * 2.0f + btnGap;
        const float availW = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() + (availW - totalW) * 0.5f);

        ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Get().SendBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  Theme::Get().SendBtnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   Theme::Get().SendBtnActive);
        ImGui::PushStyleColor(ImGuiCol_Text,           Theme::Get().SendBtnText);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Accept", { btnW, 0 })) {
            Dispatch(Safira::ClientAction::RespondToPrivateChatInvite{
                fromUser, true });
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine(0, btnGap);

        ImGui::PushStyleColor(ImGuiCol_Button,       Theme::Get().DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Get().DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Decline", { btnW, 0 })) {
            Dispatch(Safira::ClientAction::RespondToPrivateChatInvite{
                fromUser, false });
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (!open) {
        Dispatch(Safira::ClientAction::RespondToPrivateChatInvite{
            fromUser, false });
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UI_UnifiedChatWindow
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::UI_UnifiedChatWindow() {
    if (!Safira::ApplicationGUI::Get().IsChatPanelVisible())
        return;

    const auto& state = m_Store->GetState();
    const ImVec2 outerAvail = ImGui::GetContentRegionAvail();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Get().PanelBgVec4());
    ImGui::BeginChild("##ChatPanel", outerAvail, false,
                       ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    RebuildConversationList();

    if (m_ActiveConvoIdx >= static_cast<int>(m_ConversationList.size()))
        m_ActiveConvoIdx = std::max(0,
            static_cast<int>(m_ConversationList.size()) - 1);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sideW = 260.0f;
    const float chatW = avail.x - sideW;

    // -- Left sidebar --------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          Theme::Get().PanelBgVec4());
    ImGui::BeginChild("##Sidebar", { sideW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    {
        const float pad = 14.0f;

        const float userListH = std::min(
            static_cast<float>(state.ConnectedClients.size()) * 26.0f + 36.0f,
            avail.y * 0.35f);

        ImGui::SetCursorPos({ 0, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::BeginChild("##UserSection", { sideW, userListH }, false);
        ImGui::PopStyleVar();
        ImGui::SetCursorPos({ pad, 8.0f });
        UI_UserListSection(sideW);
        ImGui::EndChild();

        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine({ p.x + pad, p.y },
                        { p.x + sideW - pad, p.y },
                        Theme::Get().Divider, 1.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        }

        ImGui::BeginChild("##ConvoList", { 0, 0 }, false);

        ImFont* bold = SidebarBoldFont();
        ImFont* body = SidebarBodyFont();
        if (body) ImGui::PushFont(body);

        for (int i = 0; i < static_cast<int>(m_ConversationList.size()); ++i) {
            const auto& c = m_ConversationList[i];
            ImGui::PushID(i);

            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImVec2 itemSz = { sideW - 4.0f, 54.0f };
            const bool hovered  = ImGui::IsMouseHoveringRect(
                cursor, { cursor.x + itemSz.x, cursor.y + itemSz.y });

            ImU32 bg = 0;
            if (i == m_ActiveConvoIdx) bg = Theme::Get().BgItemSelected;
            else if (hovered)          bg = Theme::Get().BgItemHovered;

            if (bg)
                ImGui::GetWindowDrawList()->AddRectFilled(
                    cursor,
                    { cursor.x + itemSz.x, cursor.y + itemSz.y },
                    bg, 6.0f);

            if (ImGui::InvisibleButton("##c", itemSz))
                m_ActiveConvoIdx = i;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            constexpr float kR   = 14.0f;
            constexpr float kPad = 14.0f;
            const float tx = cursor.x + kPad;
            const float ax = tx + kR;
            const float ay = cursor.y + itemSz.y * 0.5f;

            uint32_t avatarCol = Theme::Get().Accent;
            if (i == 0) {
                avatarCol = Theme::Get().LobbyAvatar;
            } else {
                const auto& title = c.Title;
                if (auto it = state.ConnectedClients.find(title);
                    it != state.ConnectedClients.end())
                    avatarCol = it->second.Color;
            }

            DrawAvatarCircle(dl, { ax, ay }, kR, avatarCol,
                             c.Title, c.AvatarTex);

            const float textX = tx + kR * 2.0f + 10.0f;
            const float rightEdge = cursor.x + sideW - kPad - 4.0f;

            float titleMaxW = rightEdge - textX;
            if (!c.TimeLabel.empty()) {
                ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
                titleMaxW -= (tSz.x + 8.0f);
            }
            SidebarDrawTextTruncated(bold, { textX, cursor.y + 8.0f },
                              Theme::Get().ConvoTitleCol,
                              c.Title.c_str(), titleMaxW);

            const float previewMaxW = rightEdge - textX;
            SidebarDrawTextTruncated(body, { textX, cursor.y + 28.0f },
                              Theme::Get().ConvoPreviewCol,
                              c.Preview.c_str(), previewMaxW);

            if (!c.TimeLabel.empty()) {
                ImVec2 tSz = ImGui::CalcTextSize(c.TimeLabel.c_str());
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { cursor.x + sideW - kPad - tSz.x - 4.0f,
                      cursor.y + 10.0f },
                    Theme::Get().ConvoTimeCol,
                    c.TimeLabel.c_str());
            }

            ImGui::PopID();
        }

        if (body) ImGui::PopFont();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 sidebarEnd = ImGui::GetCursorScreenPos();
        float panelTop = sidebarEnd.y - avail.y;
        dl->AddLine({ sidebarEnd.x, panelTop },
                    { sidebarEnd.x, panelTop + avail.y },
                    Theme::Get().Separator, 1.0f);
    }

    ImGui::PopStyleColor();

    // -- Right chat area -----------------------------------------------------
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          Theme::Get().PanelBgVec4());
    ImGui::BeginChild("##ChatArea", { chatW, avail.y }, false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    if (m_ActiveConvoIdx >= 0
        && m_ActiveConvoIdx < static_cast<int>(m_ConversationList.size())
        && m_ConversationList[m_ActiveConvoIdx].Messages)
    {
        auto& convo = *m_ConversationList[m_ActiveConvoIdx].Messages;
        const std::string& title =
            m_ConversationList[m_ActiveConvoIdx].Title;

        bool connected   = true;
        bool handshaking = false;
        const bool isPrivate = (m_ActiveConvoIdx > 0);

        std::string peerName;

        if (m_ActiveConvoIdx == 0) {
            connected = IsConnected();
        } else {
            int sessionIdx = 0;
            for (auto& [peer, session] : m_PrivateChats) {
                if (sessionIdx == m_ActiveConvoIdx - 1) {
                    connected   = session->IsConnected();
                    handshaking = session->IsRunning() && !connected;
                    peerName    = peer;
                    break;
                }
                sessionIdx++;
            }
        }

        m_ChatPanel.StatusProtocol = (m_ActiveConvoIdx == 0)
            ? "DTLS 1.3 | ML-KEM-512"
            : "TLS 1.3 | X25519/ML-KEM-768";

        m_ChatPanel.SetPrivateChatMode(isPrivate);

        if (isPrivate) {
            m_ChatPanel.SetOwnAvatar(m_AvatarTexture);

            ImTextureID peerTex = {};
            if (auto it = m_PeerAvatars.find(peerName);
                it != m_PeerAvatars.end())
                peerTex = it->second.Tex;
            m_ChatPanel.SetPeerAvatar(peerTex);

            m_ChatPanel.SetOnLeaveCallback([this, peerName]() {
                LeavePrivateChat(peerName);
            });
        } else {
            m_ChatPanel.SetOwnAvatar(ImTextureID{});
            m_ChatPanel.SetPeerAvatar(ImTextureID{});
            m_ChatPanel.SetOnLeaveCallback(nullptr);
        }

        ImGui::SetCursorPos({ 14.0f, 8.0f });

        m_ChatPanel.RenderChatArea(convo, state.Username, title,
                                   connected, handshaking);

        if (auto msg = m_ChatPanel.ConsumePendingMessage()) {
            if (m_ActiveConvoIdx == 0) {
                Dispatch(Safira::ClientAction::SendMessage{ *msg });
            } else {
                int sessionIdx = 0;
                for (auto& [peer, session] : m_PrivateChats) {
                    if (sessionIdx == m_ActiveConvoIdx - 1) {
                        session->Send(*msg);
                        session->AppendMessage(
                            state.Username, *msg, 0xFFFFFFFF);
                        break;
                    }
                    sessionIdx++;
                }
            }
        }
    } else {
        const char* sub = "Select a conversation or start a new one.";
        ImVec2 sSz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPos({ (chatW - sSz.x) * 0.5f, avail.y * 0.45f });
        ImGui::TextColored({ 0.45f, 0.45f, 0.45f, 1.0f }, "%s", sub);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // -- Separator overlay ---------------------------------------------------
    const bool anyModalOpen = !IsConnected()
        || !state.IncomingInvites.empty()
        || ImGui::IsPopupOpen("Report User##ReportModal");
    if (!anyModalOpen) {
        ImVec2 origin = ImGui::GetWindowPos();
        const ImU32 lineCol = Theme::Get().Separator;

        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize({ avail.x, avail.y });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

        ImGuiWindowFlags lineFlags =
              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoDocking    | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("##SeparatorOverlay", nullptr, lineFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(origin, { origin.x + avail.x, origin.y },
                        lineCol, 1.0f);
            dl->AddLine({ origin.x + sideW, origin.y },
                        { origin.x + sideW, origin.y + avail.y },
                        lineCol, 1.0f);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    ImGui::EndChild();
}

// ═════════════════════════════════════════════════════════════════════════════
// Persistence
// ═════════════════════════════════════════════════════════════════════════════

void ClientLayer::SaveConnectionDetails(
    const std::filesystem::path& filepath)
{
    const auto& state = m_Store->GetState();

    YAML::Emitter out;
    out << YAML::BeginMap
        << YAML::Key << "ConnectionDetails" << YAML::Value << YAML::BeginMap
            << YAML::Key << "Username"        << YAML::Value << state.Username
            << YAML::Key << "ServerIP"        << YAML::Value << state.ServerAddress
            << YAML::Key << "AvatarImagePath" << YAML::Value << m_AvatarImagePath
        << YAML::EndMap
        << YAML::EndMap;

    std::ofstream fout(filepath);
    fout << out.c_str();
}

bool ClientLayer::LoadConnectionDetails(
    const std::filesystem::path& filepath)
{
    // Handled inline in OnAttach (before store creation)
    (void)filepath;
    return true;
}
