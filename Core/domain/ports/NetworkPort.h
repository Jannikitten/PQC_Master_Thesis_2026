#ifndef SAFIRA_DOMAIN_PORTS_NETWORKPORT_H
#define SAFIRA_DOMAIN_PORTS_NETWORKPORT_H

// ═════════════════════════════════════════════════════════════════════════════
// Defines what the application layer needs from the network without
// prescribing HOW (wolfSSL, Botan, etc.).  Infrastructure adapters
// satisfy these concepts.
// ═════════════════════════════════════════════════════════════════════════════

#include "ClientID.h"
#include "Types.h"

#include <concepts>
#include <functional>
#include <string_view>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // ServerNetworkPort — sending data from server to connected clients
    // ─────────────────────────────────────────────────────────────────────────────
    template <typename T>
    concept ServerNetworkPort = requires(T& net, ClientID id, ByteSpan data) {
        { net.SendToClient(id, data) }       -> std::same_as<void>;
        { net.SendToAllClients(data) }       -> std::same_as<void>;
        { net.SendToAllClients(data, id) }   -> std::same_as<void>;
        { net.KickClient(id) }              -> std::same_as<void>;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // ClientNetworkPort — sending data from client to server
    // ─────────────────────────────────────────────────────────────────────────────
    template <typename T>
    concept ClientNetworkPort = requires(T& net, ByteSpan data) {
        { net.Send(data) } -> std::same_as<void>;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Callback signatures for network events
    // ─────────────────────────────────────────────────────────────────────────────
    using OnDataReceivedFn     = std::function<void(ClientID, ByteSpan)>;
    using OnClientConnectedFn  = std::function<void(ClientID, std::string_view address)>;
    using OnClientDisconnectedFn = std::function<void(ClientID, std::string_view address)>;
    using OnClientDataFn       = std::function<void(ByteSpan)>;
    using OnConnectionStatusFn = std::function<void(bool connected, std::string_view detail)>;

} // namespace Safira

#endif // SAFIRA_DOMAIN_PORTS_NETWORKPORT_H
