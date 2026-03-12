#include "ServerLayer.h"
#include "SafiraAssert.h"
#include "StringUtils.h"
#include "ServerWiring.h"

// ═════════════════════════════════════════════════════════════════════════════
// ServerLayer.cpp — Wired server application layer
//
// All application state lives in the Store.  Server callbacks dispatch
// actions; the reducer computes new state + effects; the effect middleware
// serializes packets and calls into infrastructure.
//
// This file is a thin coordinator: it owns the infrastructure objects,
// wires them to the Store, and handles console command parsing.
// ═════════════════════════════════════════════════════════════════════════════

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <fstream>
#include <print>
#include <ranges>
#include <utility>

#include <spdlog/spdlog.h>

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::OnAttach() {
    // ── Create and start the DTLS server ────────────────────────────────
    Safira::ServerConfig config;
    config.Port                    = 8192;
    config.MaxClients              = 64;
    config.HandshakeTimeoutSeconds = 10.0f;

    m_Server = std::make_unique<Safira::Server>(config);

    // ── Build the effect handler (binds infrastructure) ─────────────────
    Safira::Middleware::ServerEffectHandler effectHandler;

    effectHandler.SendToClient = [this](Safira::ClientID id, Safira::ByteSpan buf) {
        m_Server->SendToClient(id, buf);
    };
    effectHandler.SendToAllClients = [this](Safira::ByteSpan buf, Safira::ClientID exclude) {
        m_Server->SendToAllClients(buf, exclude);
    };
    effectHandler.KickClient = [this](Safira::ClientID id) {
        m_Server->KickClient(id);
    };
    effectHandler.SaveHistory = [this](const std::vector<Safira::ChatMessage>& history) {
        SaveMessageHistory(history);
    };
    effectHandler.LoadHistory = [this]() {
        LoadMessageHistory();
    };
    effectHandler.LogInfo = [this](const std::string& msg) {
        m_Console.AddMessage("{}", msg);
    };
    effectHandler.LogItalic = [this](const std::string& msg) {
        m_Console.AddItalicMessage("{}", msg);
    };
    effectHandler.LogTagged = [this](const std::string& msg,
                                     const std::string& tag,
                                     uint32_t color) {
        if (color != 0)
            m_Console.AddTaggedMessageWithColor(color, tag, "{}", msg);
        else
            m_Console.AddTaggedMessage(tag, "{}", msg);
    };

    // ── Create the wired store ──────────────────────────────────────────
    Safira::ServerState initialState;
    initialState.Port       = config.Port;
    initialState.MaxClients = config.MaxClients;

    m_Store = Safira::CreateServerStore(
        std::move(initialState), std::move(effectHandler));

    // ── Wire server callbacks → Store dispatch ──────────────────────────
    m_Server->OnClientConnected([this](Safira::ClientInfo& c) {
        const Safira::ClientID id = c.ID;
        const std::string addr = c.AddressStr;
        EnqueueEvent([this, id, addr]() {
            m_Store->Dispatch(Safira::Action::ClientConnected{ id, addr });
        });
    });

    m_Server->OnClientDisconnected([this](Safira::ClientInfo& c) {
        const Safira::ClientID id = c.ID;
        EnqueueEvent([this, id]() {
            m_Store->Dispatch(Safira::Action::ClientDisconnected{ id });
        });
    });

    m_Server->OnDataReceived([this](Safira::ClientInfo& c, Safira::ByteSpan d) {
        const Safira::ClientID id = c.ID;
        std::vector<uint8_t> payload(d.begin(), d.end());
        EnqueueEvent([this, id, payload = std::move(payload)]() {
            m_Store->Dispatch(Safira::Action::DataReceived{ id, payload });
        });
    });

    m_Server->Start();

    // ── Load message history ────────────────────────────────────────────
    m_MessageHistoryFilePath = "MessageHistory.yaml";
    m_Console.AddTaggedMessage("Info", "Loading message history...");
    LoadMessageHistory();

    // Display loaded history in console
    const auto& history = m_Store->GetState().MessageHistory;
    for (const auto& msg : history)
        m_Console.AddTaggedMessage(msg.Username, "{}", msg.Message);

    m_Console.AddTaggedMessage("Info", "Started server on port {} (max {} clients)",
                               config.Port, config.MaxClients);

    // Display available commands on startup
    m_Console.AddItalicMessage("");
    m_Console.AddItalicMessage("Available server commands:");
    m_Console.AddItalicMessage("  /kick <user> [reason]  — disconnect a user");
    m_Console.AddItalicMessage("  /mute <user>           — silence a user (broadcast to all)");
    m_Console.AddItalicMessage("  /unmute <user>         — restore a user's voice");
    m_Console.AddItalicMessage("  /list                  — show connected clients");
    m_Console.AddItalicMessage("  /stats                 — server-wide statistics");
    m_Console.AddItalicMessage("  /broadcast <msg>       — send server announcement");
    m_Console.AddItalicMessage("  /motd [msg]            — set/clear message of the day");
    m_Console.AddItalicMessage("  /help                  — show this list again");
    m_Console.AddItalicMessage("");

    m_Console.SetMessageSendCallback([this](std::string_view msg) { SendChatMessage(msg); });
}

void ServerLayer::OnDetach() {
    m_Store->Dispatch(Safira::Action::Shutdown{});
    m_Server->Stop();
}

void ServerLayer::OnUpdate(float ts) {
    DrainQueuedEvents();
    m_Store->Dispatch(Safira::Action::Tick{ ts });
}

void ServerLayer::OnUIRender() { m_Console.OnUIRender(); }

// ═════════════════════════════════════════════════════════════════════════════
// Console command handling
//
// Commands that modify state dispatch actions to the Store.
// Read-only commands (/list, /stats, /help) read directly from state.
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::SendChatMessage(std::string_view message) {
    if (!message.empty() && message[0] == '/') {
        OnCommand(message);
        return;
    }
    m_Store->Dispatch(Safira::Action::SendChatMessage{ std::string(message) });
}

void ServerLayer::Quit() {
    m_Store->Dispatch(Safira::Action::Shutdown{});
    m_Server->Stop();
}

void ServerLayer::OnCommand(std::string_view command) {
    if (command.size() < 2 || command[0] != '/') return;

    auto commandStr = command.substr(1);
    auto tokens = Safira::Utils::SplitString(commandStr, ' ');
    if (tokens.empty()) return;

    const auto& cmd = tokens[0];

    // ── /kick <username> [reason] ───────────────────────────────────────
    if (cmd == "kick") {
        if (tokens.size() >= 2) {
            std::string reason;
            for (size_t i = 2; i < tokens.size(); ++i) {
                if (!reason.empty()) reason += ' ';
                reason += std::string(tokens[i]);
            }
            m_Store->Dispatch(Safira::Action::KickCommand{
                std::string(tokens[1]), reason });
        } else {
            m_Console.AddItalicMessage("Usage: /kick <username> [reason]");
        }
    }
    // ── /list — show connected users (read-only) ────────────────────────
    else if (cmd == "list") {
        const auto& state = m_Store->GetState();
        m_Console.AddItalicMessage("Connected clients ({}/{}):",
                                   state.ConnectedClients.size(),
                                   state.MaxClients);
        for (const auto& [id, info] : state.ConnectedClients) {
            (void)id;
            m_Console.AddItalicMessage("  {}", info.Username);
        }
    }
    // ── /stats — server statistics (read-only) ──────────────────────────
    else if (cmd == "stats") {
        const auto& state = m_Store->GetState();
        m_Console.AddItalicMessage("Server stats:");
        m_Console.AddItalicMessage("  Clients:  {}/{}",
                                   state.ConnectedClients.size(),
                                   state.MaxClients);
        m_Console.AddItalicMessage("  Messages: {} in history",
                                   state.MessageHistory.size());
        m_Console.AddItalicMessage("  Muted:    {}", state.MutedUsers.size());
    }
    // ── /broadcast <message> — server announcement ──────────────────────
    else if (cmd == "broadcast") {
        if (tokens.size() >= 2) {
            std::string msg;
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (!msg.empty()) msg += ' ';
                msg += std::string(tokens[i]);
            }
            m_Store->Dispatch(Safira::Action::BroadcastCommand{ msg });
        } else {
            m_Console.AddItalicMessage("Usage: /broadcast <message>");
        }
    }
    // ── /motd [message] — set or clear the message of the day ───────────
    else if (cmd == "motd") {
        if (tokens.size() >= 2) {
            std::string motd;
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (!motd.empty()) motd += ' ';
                motd += std::string(tokens[i]);
            }
            m_Store->Dispatch(Safira::Action::SetMotdCommand{ motd });
        } else {
            m_Store->Dispatch(Safira::Action::SetMotdCommand{ "" });
        }
    }
    // ── /mute <username> ────────────────────────────────────────────────
    else if (cmd == "mute") {
        if (tokens.size() == 2) {
            m_Store->Dispatch(Safira::Action::MuteCommand{
                std::string(tokens[1]) });
        } else {
            m_Console.AddItalicMessage("Usage: /mute <username>");
        }
    }
    // ── /unmute <username> ──────────────────────────────────────────────
    else if (cmd == "unmute") {
        if (tokens.size() == 2) {
            m_Store->Dispatch(Safira::Action::UnmuteCommand{
                std::string(tokens[1]) });
        } else {
            m_Console.AddItalicMessage("Usage: /unmute <username>");
        }
    }
    // ── /help ───────────────────────────────────────────────────────────
    else if (cmd == "help") {
        m_Console.AddItalicMessage("Available commands:");
        m_Console.AddItalicMessage("  /kick <user> [reason]  — disconnect a user");
        m_Console.AddItalicMessage("  /mute <user>           — silence a user");
        m_Console.AddItalicMessage("  /unmute <user>         — restore a user's voice");
        m_Console.AddItalicMessage("  /list                  — show connected clients");
        m_Console.AddItalicMessage("  /stats                 — server statistics");
        m_Console.AddItalicMessage("  /broadcast <msg>       — send server announcement");
        m_Console.AddItalicMessage("  /motd [msg]            — set/clear message of the day");
        m_Console.AddItalicMessage("  /help                  — this message");
    }
    // ── Unknown ─────────────────────────────────────────────────────────
    else {
        m_Console.AddItalicMessage("Unknown command: /{}. Type /help for a list.", cmd);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Persistence
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::SaveMessageHistory(const std::vector<Safira::ChatMessage>& history) {
    YAML::Emitter out;
    out << YAML::BeginMap
        << YAML::Key << "MessageHistory"
        << YAML::Value << YAML::BeginSeq;

    for (const auto& msg : history) {
        out << YAML::BeginMap
            << YAML::Key << "User"    << YAML::Value << msg.Username
            << YAML::Key << "Message" << YAML::Value << msg.Message
            << YAML::EndMap;
    }

    out << YAML::EndSeq << YAML::EndMap;
    std::ofstream fout(m_MessageHistoryFilePath);
    fout << out.c_str();
}

void ServerLayer::LoadMessageHistory() {
    if (!std::filesystem::exists(m_MessageHistoryFilePath)) return;

    std::vector<Safira::ChatMessage> messages;

    YAML::Node data;
    try {
        data = YAML::LoadFile(m_MessageHistoryFilePath.string());
    } catch (const YAML::ParserException& e) {
        spdlog::error("failed to parse {}: {}", m_MessageHistoryFilePath.string(), e.what());
        return;
    }

    auto root = data["MessageHistory"];
    if (!root) return;

    messages.reserve(root.size());
    for (const auto& node : root) {
        messages.emplace_back(
            node["User"].as<std::string>(),
            node["Message"].as<std::string>());
    }

    // Dispatch loaded history into the store
    m_Store->Dispatch(Safira::Action::HistoryLoaded{ std::move(messages) });
}

// ═════════════════════════════════════════════════════════════════════════════
// Thread-safe event queue
// ═════════════════════════════════════════════════════════════════════════════

void ServerLayer::EnqueueEvent(std::function<void()>&& fn) {
    std::lock_guard lock(m_EventMutex);
    m_PendingEvents.push(std::move(fn));
}

void ServerLayer::DrainQueuedEvents() {
    std::queue<std::function<void()>> pending;
    {
        std::lock_guard lock(m_EventMutex);
        pending.swap(m_PendingEvents);
    }

    while (!pending.empty()) {
        pending.front()();
        pending.pop();
    }
}
