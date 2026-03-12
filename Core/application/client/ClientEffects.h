#ifndef SAFIRA_APPLICATION_CLIENT_CLIENTEFFECTS_H
#define SAFIRA_APPLICATION_CLIENT_CLIENTEFFECTS_H

// =============================================================================
// ClientEffects.h — Effect-executing middleware for the client store
//
// Same pattern as ServerEffects: intercept every action, compute effects
// via the pure reducer, let the store update, then execute effects
// against injected infrastructure adapters.
//
// Protocol effects (SendChatMessageToServer, etc.) are serialized here
// using SerializePacket before being sent through the handler.
// =============================================================================

#include "Store.h"
#include "Effect.h"
#include "ClientState.h"
#include "ClientAction.h"
#include "ClientReducer.h"
#include "PacketSerialize.h"       // SerializePacket, packet types

#include <functional>
#include <memory>
#include <spdlog/spdlog.h>

namespace Safira::Middleware {

// -----------------------------------------------------------------------------
// ClientEffectHandler — adapter interface injected at wiring time
// -----------------------------------------------------------------------------
struct ClientEffectHandler {
    // Network I/O
    std::function<void(ByteSpan)>         SendToServer;
    std::function<void(const std::string&)> ConnectToServer;
    std::function<void()>                   Disconnect;

    // P2P session management
    std::function<void(const std::string&)>                          StartP2PResponder;
    std::function<void(const std::string&, const std::string&)>      StartP2PInitiator;
    std::function<void(const std::string&)>                          CloseP2P;
    std::function<void()>                                            CloseAllP2P;

    // Persistence
    std::function<void()>                   SaveDetails;

    // Logging
    std::function<void(const std::string&)>                              LogInfo;
    std::function<void(const std::string&)>                              LogItalic;
    std::function<void(const std::string&, const std::string&, uint32_t)> LogTagged;

    // Post-action hook: fired after every action is processed so the
    // presentation layer can sync textures, console, etc.
    std::function<void(const ClientActionVariant&, const ClientState&)> OnActionProcessed;
};

// -----------------------------------------------------------------------------
// ExecuteEffect — interprets a single ClientEffect value
// -----------------------------------------------------------------------------
inline void ExecuteEffect(const Effect::ClientEffect& effect,
                          const ClientEffectHandler& handler,
                          const ClientState& /*stateAfter*/,
                          ByteBuffer& scratch)
{
    namespace FX = Effect;

    std::visit(Overloaded{
        [](const FX::None&) {},

        // -- Protocol effects (serialize + send) ------------------------------

        [&](const FX::SendChatMessageToServer& fx) {
            scratch.resize(256 + fx.Message.size());
            BufferWriter w(scratch);
            SerializePacket(w, MessagePacket{ fx.Message });
            if (handler.SendToServer) handler.SendToServer(w.Written());
        },

        [&](const FX::SendConnectionRequestToServer& fx) {
            scratch.resize(512 + fx.AvatarData.size());
            BufferWriter w(scratch);
            SerializePacket(w, ConnectionRequestPacket{
                .Color      = fx.Color,
                .Username   = fx.Username,
                .AvatarData = fx.AvatarData,
            });
            if (handler.SendToServer) handler.SendToServer(w.Written());
        },

        [&](const FX::SendPrivateChatInviteToServer& fx) {
            scratch.resize(256);
            BufferWriter w(scratch);
            SerializePacket(w, PrivateChatInvitePacket{ fx.TargetUsername });
            if (handler.SendToServer) handler.SendToServer(w.Written());
        },

        [&](const FX::SendPrivateChatResponseToServer& fx) {
            scratch.resize(256);
            BufferWriter w(scratch);
            SerializePacket(w, PrivateChatResponsePacket{
                fx.PeerUsername, fx.Accepted, fx.ListenPort });
            if (handler.SendToServer) handler.SendToServer(w.Written());
        },

        // -- Infrastructure effects -------------------------------------------

        [&](const FX::ConnectToServer& fx) {
            if (handler.ConnectToServer) handler.ConnectToServer(fx.Address);
        },

        [&](const FX::Disconnect&) {
            if (handler.Disconnect) handler.Disconnect();
        },

        [&](const FX::SaveConnectionDetails&) {
            if (handler.SaveDetails) handler.SaveDetails();
        },

        [&](const FX::StartP2PAsResponder& fx) {
            if (handler.StartP2PResponder)
                handler.StartP2PResponder(fx.PeerUsername);
        },

        [&](const FX::StartP2PAsInitiator& fx) {
            if (handler.StartP2PInitiator)
                handler.StartP2PInitiator(fx.PeerUsername, fx.PeerAddress);
        },

        [&](const FX::CloseP2PSession& fx) {
            if (handler.CloseP2P) handler.CloseP2P(fx.PeerUsername);
        },

        [&](const FX::CloseAllP2PSessions&) {
            if (handler.CloseAllP2P) handler.CloseAllP2P();
        },

        // -- Logging ----------------------------------------------------------

        [&](const FX::LogMessage& fx) {
            switch (fx.Type) {
                case FX::LogMessage::Level::Info:
                    if (handler.LogInfo) handler.LogInfo(fx.Message);
                    break;
                case FX::LogMessage::Level::Italic:
                    if (handler.LogItalic) handler.LogItalic(fx.Message);
                    break;
                case FX::LogMessage::Level::Tagged:
                    if (handler.LogTagged)
                        handler.LogTagged(fx.Message, fx.Tag, fx.Color);
                    break;
            }
        },
    }, effect);
}

// -----------------------------------------------------------------------------
// MakeClientEffectMiddleware
//
// Factory returning a Store middleware.  On each dispatch:
//   1. Compute effects from current state + action
//   2. Forward action to store reducer
//   3. Execute effects
//   4. Fire OnActionProcessed for presentation sync
// -----------------------------------------------------------------------------
inline auto MakeClientEffectMiddleware(ClientEffectHandler handler) {
    auto scratch = std::make_shared<ByteBuffer>(8192);

    return [h = std::move(handler), scratch](
        const Store<ClientState, ClientActionVariant>& store,
        const ClientActionVariant& action,
        Store<ClientState, ClientActionVariant>::DispatchFn next)
    {
        // 1. Compute effects from current state
        auto result = ClientReduce(store.GetState(), action);

        // 2. Let the store's reducer update state
        next(action);

        // 3. Execute effects
        for (const auto& fx : result.Effects) {
            ExecuteEffect(fx, h, store.GetState(), *scratch);
        }

        // 4. Fire presentation hook
        if (h.OnActionProcessed) {
            h.OnActionProcessed(action, store.GetState());
        }
    };
}

} // namespace Safira::Middleware

#endif // SAFIRA_APPLICATION_CLIENT_CLIENTEFFECTS_H
