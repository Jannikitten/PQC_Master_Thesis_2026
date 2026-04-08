#ifndef SAFIRA_APPLICATION_SERVER_SERVERSTATE_H
#define SAFIRA_APPLICATION_SERVER_SERVERSTATE_H

// ═════════════════════════════════════════════════════════════════════════════
// This is the single source of truth for the server.  The reducer
// produces new states; it never mutates the existing one.
// ═════════════════════════════════════════════════════════════════════════════

#include "ClientID.h"
#include "User.h"
#include "Message.h"
#include "RateLimit.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Safira {

    struct ServerState {
        // ── Connected clients ──
        std::unordered_map<ClientID, UserInfo>             ConnectedClients;
        std::unordered_map<ClientID, std::string>          ClientAddresses;

        // ── Private chat invites (responder -> initiator) ──
        std::unordered_map<ClientID, ClientID>             PendingInvites;

        // ── Rate limiting (per client) ──
        std::unordered_map<ClientID, Rules::RateLimitState> RateLimits;

        // ── Muted users ──
        std::unordered_set<std::string>                    MutedUsers;

        // ── Message history ──
        std::vector<ChatMessage>                           MessageHistory;

        // ── Server config ──
        std::string  Motd;
        uint16_t     Port         = 8192;
        uint32_t     MaxClients   = 64;
        std::size_t       MaxHistory   = 5000;
        std::size_t       HistorySyncLimit = 500;

        // ── Timers ──
        float ClientListTimer = 0.0f;
        static constexpr float kClientListInterval = 5.0f;
    };

} // namespace Safira

#endif // SAFIRA_APPLICATION_SERVER_SERVERSTATE_H
