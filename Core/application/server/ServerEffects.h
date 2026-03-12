#ifndef SAFIRA_APPLICATION_SERVER_SERVEREFFECTS_H
#define SAFIRA_APPLICATION_SERVER_SERVEREFFECTS_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerEffects.h — Effect-executing middleware for the server store
//
// This middleware intercepts every action, runs it through the full
// ServerReduce (which returns state + effects), then executes each
// effect by calling into infrastructure adapters.
//
// Protocol effects are serialized here using SerializePacket before
// being sent via the network adapter.  This file sits at the boundary
// between the application layer and infrastructure.
// ═════════════════════════════════════════════════════════════════════════════

#include "Store.h"
#include "Effect.h"
#include "ServerState.h"
#include "ServerAction.h"
#include "ServerReducer.h"
#include "PacketSerialize.h"       // SerializePacket, packet types

#include <functional>
#include <ranges>
#include <spdlog/spdlog.h>

namespace Safira::Middleware {

// ─────────────────────────────────────────────────────────────────────────────
// ServerEffectHandler — adapter interface injected at wiring time
//
// Each callback maps to an infrastructure capability (network, persistence,
// logging).  All are std::function so they can bind to lambdas capturing
// the concrete adapters (DtlsServer, YamlMessageStore, Console, etc.).
// ─────────────────────────────────────────────────────────────────────────────
struct ServerEffectHandler {
    // Network
    std::function<void(ClientID, ByteSpan)>  SendToClient;
    std::function<void(ByteSpan, ClientID)>  SendToAllClients;   // exclude optional
    std::function<void(ClientID)>            KickClient;

    // Persistence
    std::function<void(const std::vector<ChatMessage>&)> SaveHistory;
    std::function<void()>                                LoadHistory;

    // Logging / Console
    std::function<void(const std::string&)>                              LogInfo;
    std::function<void(const std::string&)>                              LogItalic;
    std::function<void(const std::string&, const std::string&, uint32_t)> LogTagged;
};

// ─────────────────────────────────────────────────────────────────────────────
// ExecuteEffect — interprets a single ServerEffect value
//
// Protocol effects are serialized into scratch and sent via the handler.
// Infrastructure effects are dispatched directly to the handler.
// ─────────────────────────────────────────────────────────────────────────────
inline void ExecuteEffect(const Effect::ServerEffect& effect,
                          const ServerEffectHandler& handler,
                          const ServerState& stateAfter,
                          ByteBuffer& scratch)
{
    namespace FX = Effect;

    std::visit(Overloaded{
        [](const FX::None&) {},

        // ── Protocol effects (serialize + send) ─────────────────────────

        [&](const FX::SendConnectionResponse& fx) {
            scratch.resize(64);
            BufferWriter w(scratch);
            SerializePacket(w, ConnectionResponsePacket{ fx.Accepted });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        [&](const FX::BroadcastClientConnect& fx) {
            if (!stateAfter.ConnectedClients.contains(fx.NewClient)) return;
            scratch.resize(512);
            BufferWriter w(scratch);
            SerializePacket(w, ClientConnectPacket{
                stateAfter.ConnectedClients.at(fx.NewClient) });
            if (handler.SendToAllClients) handler.SendToAllClients(w.Written(), {});
        },

        [&](const FX::SendClientListTo& fx) {
            auto values = stateAfter.ConnectedClients | std::views::values;
            std::vector<UserInfo> list(values.begin(), values.end());
            scratch.resize(4096);
            BufferWriter w(scratch);
            SerializePacket(w, ClientListPacket{ std::move(list) });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        [&](const FX::SendClientListToAll&) {
            auto values = stateAfter.ConnectedClients | std::views::values;
            std::vector<UserInfo> list(values.begin(), values.end());
            scratch.resize(4096);
            BufferWriter w(scratch);
            SerializePacket(w, ClientListPacket{ std::move(list) });
            if (handler.SendToAllClients) handler.SendToAllClients(w.Written(), {});
        },

        [&](const FX::SendMessageHistoryTo& fx) {
            const auto& history = stateAfter.MessageHistory;
            const size_t start = (history.size() > stateAfter.HistorySyncLimit)
                ? (history.size() - stateAfter.HistorySyncLimit) : 0;
            std::vector<ChatMessage> recent(
                history.begin() + static_cast<std::ptrdiff_t>(start),
                history.end());
            scratch.resize(32768);
            BufferWriter w(scratch);
            SerializePacket(w, MessageHistoryPacket{ std::move(recent) });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        [&](const FX::BroadcastServerMessage& fx) {
            scratch.resize(8192);
            BufferWriter w(scratch);
            SerializePacket(w, ServerMessagePacket{ fx.From, fx.Message });
            if (handler.SendToAllClients)
                handler.SendToAllClients(w.Written(), fx.Exclude);
        },

        [&](const FX::BroadcastClientDisconnect& fx) {
            scratch.resize(512);
            BufferWriter w(scratch);
            SerializePacket(w, ClientDisconnectPacket{ fx.Client });
            if (handler.SendToAllClients)
                handler.SendToAllClients(w.Written(), fx.Exclude);
        },

        [&](const FX::SendServerShutdownToAll&) {
            scratch.resize(64);
            BufferWriter w(scratch);
            SerializePacket(w, ServerShutdownPacket{});
            if (handler.SendToAllClients) handler.SendToAllClients(w.Written(), {});
        },

        [&](const FX::SendKickNotification& fx) {
            scratch.resize(512);
            BufferWriter w(scratch);
            SerializePacket(w, ClientKickPacket{ fx.Reason });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        [&](const FX::ForwardPrivateChatInvite& fx) {
            scratch.resize(256);
            BufferWriter w(scratch);
            SerializePacket(w, PrivateChatInvitePacket{ fx.FromUsername });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        [&](const FX::ForwardPrivateChatConnectTo& fx) {
            scratch.resize(512);
            BufferWriter w(scratch);
            SerializePacket(w, PrivateChatConnectToPacket{
                fx.PeerUsername, fx.Address });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        [&](const FX::ForwardPrivateChatDeclined& fx) {
            scratch.resize(256);
            BufferWriter w(scratch);
            SerializePacket(w, PrivateChatDeclinedPacket{ fx.PeerUsername });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        [&](const FX::SendMotdTo& fx) {
            if (stateAfter.Motd.empty()) return;
            scratch.resize(4096);
            BufferWriter w(scratch);
            SerializePacket(w, ServerMessagePacket{ "SERVER", stateAfter.Motd });
            if (handler.SendToClient) handler.SendToClient(fx.Target, w.Written());
        },

        // ── Infrastructure effects ──────────────────────────────────────

        [&](const FX::KickClient& fx) {
            if (handler.KickClient) handler.KickClient(fx.Target);
        },

        [&](const FX::SaveHistory&) {
            if (handler.SaveHistory) handler.SaveHistory(stateAfter.MessageHistory);
        },

        [&](const FX::LoadHistory&) {
            if (handler.LoadHistory) handler.LoadHistory();
        },

        [&](const FX::LogMessage& fx) {
            switch (fx.Type) {
                case FX::LogMessage::Level::Info:
                    if (handler.LogInfo) handler.LogInfo(fx.Message);
                    break;
                case FX::LogMessage::Level::Italic:
                    if (handler.LogItalic) handler.LogItalic(fx.Message);
                    break;
                case FX::LogMessage::Level::Tagged:
                    if (handler.LogTagged) handler.LogTagged(fx.Message, fx.Tag, fx.Color);
                    break;
            }
        },
    }, effect);
}

// ─────────────────────────────────────────────────────────────────────────────
// MakeServerEffectMiddleware
//
// Factory that returns a Store middleware.  On every action:
//   1. Runs ServerReduce(currentState, action) to get effects
//   2. Calls next(action) so the store's reducer updates state
//   3. Executes each effect against the handler
//
// Step (1) runs the reducer an extra time, but the reducer is pure and
// cheap.  This keeps effects perfectly synchronised with the new state.
// ─────────────────────────────────────────────────────────────────────────────
inline auto MakeServerEffectMiddleware(ServerEffectHandler handler) {
    return [h = std::move(handler),
            scratch = std::make_shared<ByteBuffer>(8192)](
        const Store<ServerState, ServerAction>& store,
        const ServerAction& action,
        Store<ServerState, ServerAction>::DispatchFn next)
    {
        // 1. Compute effects from the CURRENT state + this action
        auto result = ServerReduce(store.GetState(), action);

        // 2. Let the reducer in the store also update state
        next(action);

        // 3. Execute each effect (state in store is now updated)
        for (const auto& fx : result.Effects) {
            ExecuteEffect(fx, h, result.State, *scratch);
        }
    };
}

} // namespace Safira::Middleware

#endif // SAFIRA_APPLICATION_SERVER_SERVEREFFECTS_H
