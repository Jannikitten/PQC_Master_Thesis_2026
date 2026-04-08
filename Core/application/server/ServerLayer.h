#ifndef PQC_MASTER_THESIS_2026_SERVER_LAYER_H
#define PQC_MASTER_THESIS_2026_SERVER_LAYER_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerLayer.h — Wired server application layer
//
// Thin coordinator that owns infrastructure (Server, Console, persistence)
// and wires them to a Redux-style Store.  All business logic lives in
// the pure reducer (ServerReducer.h) and effects are executed by the
// effect middleware (ServerEffects.h).
//
// Responsibilities:
//   - Create and configure the DTLS server
//   - Wire server callbacks → Store.Dispatch(action)
//   - Parse console commands → Store.Dispatch(action)
//   - Own the event queue for cross-thread safety
// ═════════════════════════════════════════════════════════════════════════════

#include "DtlsServer.h"             // Server, ServerConfig, ClientInfo, ClientID
#include "PacketSerialize.h"       // packet types (for console command parsing)
#include "Layer.h"              // Safira::Layer base class
#include "ServerConsoleView.h"
#include "Store.h"
#include "ServerState.h"
#include "ServerAction.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// ServerLayer
// ─────────────────────────────────────────────────────────────────────────────
class ServerLayer : public Safira::Layer {
public:
    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float ts) override;
    void OnUIRender() override;

    void Quit();

private:
    // ── Console command handling ──
    void OnCommand(std::string_view command);
    void SendChatMessage(std::string_view message);

    // ── Thread-safe event queue ──
    void EnqueueEvent(std::function<void()>&& fn);
    void DrainQueuedEvents();

    // ── Persistence ──
    void SaveMessageHistory(const std::vector<Safira::ChatMessage>& history);
    void LoadMessageHistory();

    // ── Infrastructure ──
    std::unique_ptr<Safira::Server> m_Server;
    Console                         m_Console;
    std::filesystem::path           m_MessageHistoryFilePath;

    // ── Store (single source of truth) ──
    std::unique_ptr<Safira::Store<Safira::ServerState, Safira::ServerAction>>
        m_Store;

    // ── Event queue (cross-thread safety) ──
    std::mutex                       m_EventMutex;
    std::queue<std::function<void()>> m_PendingEvents;
};

#endif // PQC_MASTER_THESIS_2026_SERVER_LAYER_H
