#ifndef SAFIRA_APPLICATION_SERVER_SERVERDESERIALIZE_H
#define SAFIRA_APPLICATION_SERVER_SERVERDESERIALIZE_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerDeserialize.h — Deserialization middleware for the server store
//
// Intercepts Action::DataReceived (raw bytes from a client) and converts
// it into a typed action (MessageReceived, ConnectionRequested, etc.)
// using DeserializeServerPacket.
//
// The typed action is then forwarded through the rest of the middleware
// chain.  The original DataReceived is not forwarded (the reducer's
// handler for DataReceived is a no-op anyway).
// ═════════════════════════════════════════════════════════════════════════════

#include "Store.h"
#include "ServerState.h"
#include "ServerAction.h"
#include "PacketSerialize.h"       // DeserializeServerPacket, BufferReader

#include <spdlog/spdlog.h>

namespace Safira::Middleware {

    // ─────────────────────────────────────────────────────────────────────────────
    // MakeServerDeserializeMiddleware
    //
    // Returns a Store middleware that:
    //   - On DataReceived: parses the payload into a typed packet, converts
    //     it to the corresponding typed Action, and forwards through next()
    //   - On all other actions: passes through unchanged
    // ─────────────────────────────────────────────────────────────────────────────
    inline auto MakeServerDeserializeMiddleware() {
        return [](
            const Store<ServerState, ServerAction>& store,
            const ServerAction& action,
            Store<ServerState, ServerAction>::DispatchFn next)
        {
            // Only intercept DataReceived
            auto* data = std::get_if<Action::DataReceived>(&action);
            if (!data) {
                next(action);
                return;
            }

            // Deserialize the raw payload
            BufferReader reader(ByteSpan(data->Payload.data(), data->Payload.size()));
            auto packet = DeserializeServerPacket(reader);
            if (!packet) {
                spdlog::warn("[server] packet parse error from client {}: {}",
                             data->ID.Value, Describe(packet.error()));
                return;
            }

            // Convert packet variant → typed action and forward
            std::visit(Overloaded{
                [&](const MessagePacket& pkt) {
                    next(ServerAction{
                        Action::MessageReceived{ data->ID, pkt.Message }});
                },

                [&](const ConnectionRequestPacket& pkt) {
                    // Look up the client's transport address from state
                    std::string address;
                    const auto& addrs = store.GetState().ClientAddresses;
                    if (auto it = addrs.find(data->ID); it != addrs.end())
                        address = it->second;

                    next(ServerAction{
                        Action::ConnectionRequested{ data->ID, address, pkt }});
                },

                [&](const PrivateChatInvitePacket& pkt) {
                    next(ServerAction{
                        Action::PrivateChatInviteReceived{
                            data->ID, pkt.Username }});
                },

                [&](const PrivateChatResponsePacket& pkt) {
                    next(ServerAction{
                        Action::PrivateChatResponseReceived{
                            data->ID, pkt }});
                },
            }, *packet);
        };
    }

} // namespace Safira::Middleware

#endif // SAFIRA_APPLICATION_SERVER_SERVERDESERIALIZE_H
