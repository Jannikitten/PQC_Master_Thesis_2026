#ifndef SAFIRA_APPLICATION_CLIENT_CLIENTDESERIALIZE_H
#define SAFIRA_APPLICATION_CLIENT_CLIENTDESERIALIZE_H

// ═════════════════════════════════════════════════════════════════════════════
// ClientDeserialize.h — Deserialization middleware for the client store
//
// Intercepts ClientAction::DataReceived (raw bytes from the server) and
// converts it into a typed action (MessageReceived, Connected, etc.)
// using DeserializeClientPacket.
//
// The typed action is forwarded through the rest of the middleware chain.
// ═════════════════════════════════════════════════════════════════════════════

#include "Store.h"
#include "ClientState.h"
#include "ClientAction.h"
#include "PacketSerialize.h"       // DeserializeClientPacket, BufferReader

#include <spdlog/spdlog.h>

namespace Safira::Middleware {

// ─────────────────────────────────────────────────────────────────────────────
// MakeClientDeserializeMiddleware
//
// Returns a Store middleware that:
//   - On DataReceived: parses the payload into a typed packet, converts
//     it to the corresponding ClientAction, and forwards through next()
//   - On all other actions: passes through unchanged
// ─────────────────────────────────────────────────────────────────────────────
inline auto MakeClientDeserializeMiddleware() {
    return [](
        const Store<ClientState, ClientActionVariant>& /*store*/,
        const ClientActionVariant& action,
        Store<ClientState, ClientActionVariant>::DispatchFn next)
    {
        namespace CA = ClientAction;

        // Only intercept DataReceived
        auto* data = std::get_if<CA::DataReceived>(&action);
        if (!data) {
            next(action);
            return;
        }

        // Deserialize the raw payload
        BufferReader reader(ByteSpan(data->Payload.data(), data->Payload.size()));
        auto packet = DeserializeClientPacket(reader);
        if (!packet) {
            spdlog::warn("[client] packet parse error: {}",
                         Describe(packet.error()));
            return;
        }

        // Convert packet variant → typed client action and forward
        std::visit(Overloaded{
            [&](const ServerMessagePacket& pkt) {
                next(ClientActionVariant{
                    CA::MessageReceived{ pkt.From, pkt.Message }});
            },

            [&](const ConnectionResponsePacket& pkt) {
                if (pkt.Accepted) {
                    next(ClientActionVariant{
                        CA::Connected{ 0xFFFFFFFF }});
                } else {
                    next(ClientActionVariant{
                        CA::ConnectionFailed{ "Server rejected connection" }});
                }
            },

            [&](const ClientListPacket& pkt) {
                next(ClientActionVariant{
                    CA::ClientListReceived{ pkt.Clients }});
            },

            [&](const ClientConnectPacket& pkt) {
                next(ClientActionVariant{
                    CA::ClientConnectedEvent{ pkt.Client }});
            },

            [&](const ClientDisconnectPacket& pkt) {
                next(ClientActionVariant{
                    CA::ClientDisconnectedEvent{ pkt.Client }});
            },

            [&](const MessageHistoryPacket& pkt) {
                next(ClientActionVariant{
                    CA::MessageHistoryReceived{ pkt.Messages }});
            },

            [&](const ServerShutdownPacket&) {
                next(ClientActionVariant{ CA::ServerShutdownReceived{} });
            },

            [&](const ClientKickPacket& pkt) {
                next(ClientActionVariant{
                    CA::Kicked{ pkt.Reason }});
            },

            [&](const PrivateChatInvitePacket& pkt) {
                next(ClientActionVariant{
                    CA::PrivateChatInviteReceived{ pkt.Username }});
            },

            [&](const PrivateChatConnectToPacket& pkt) {
                next(ClientActionVariant{
                    CA::PrivateChatEstablished{
                        pkt.PeerUsername, pkt.Address }});
            },

            [&](const PrivateChatDeclinedPacket& pkt) {
                next(ClientActionVariant{
                    CA::PrivateChatDeclined{ pkt.PeerUsername }});
            },
        }, *packet);
    };
}

} // namespace Safira::Middleware

#endif // SAFIRA_APPLICATION_CLIENT_CLIENTDESERIALIZE_H
