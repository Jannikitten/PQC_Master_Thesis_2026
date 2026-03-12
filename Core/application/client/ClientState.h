#ifndef SAFIRA_APPLICATION_CLIENT_CLIENTSTATE_H
#define SAFIRA_APPLICATION_CLIENT_CLIENTSTATE_H

// ═════════════════════════════════════════════════════════════════════════════
// ClientState.h — Immutable client-side application state
//
// Single source of truth for everything the client knows.  The UI reads
// from this; the reducer produces new versions of it.
//
// Presentation-only state (textures, crop rects, modal visibility) lives
// in the presentation layer, NOT here.  This struct holds pure domain /
// application data only.
// ═════════════════════════════════════════════════════════════════════════════

#include "User.h"
#include "Message.h"
#include "Connection.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// IncomingInvite — a pending private-chat invitation
// ─────────────────────────────────────────────────────────────────────────────
struct IncomingInvite {
    std::string FromUsername;
};

// ─────────────────────────────────────────────────────────────────────────────
// ClientState
// ─────────────────────────────────────────────────────────────────────────────
struct ClientState {
    // ── Connection ──────────────────────────────────────────────────────
    ConnectionStatus Status = ConnectionStatus::Disconnected;
    std::string      ServerAddress  = "127.0.0.1";
    std::string      Username;
    uint32_t         Color = 0xFFFFFFFF;

    // ── Own avatar (processed bytes for the wire) ───────────────────────
    std::vector<uint8_t> AvatarBytes;

    // ── Connected peer list ─────────────────────────────────────────────
    std::map<std::string, UserInfo> ConnectedClients;

    // ── Lobby chat messages ─────────────────────────────────────────────
    std::vector<ChatMessage> LobbyMessages;

    // ── Private chat state ──────────────────────────────────────────────
    std::vector<IncomingInvite>  IncomingInvites;
    std::set<std::string>        PendingOutgoingInvites;
    std::set<std::string>        ActivePrivateChats;

    // ── Message history (received from server on connect) ───────────────
    std::vector<ChatMessage> MessageHistory;

    // ── Server-assigned MOTD ────────────────────────────────────────────
    std::string Motd;

    // ── Kick reason (if we were kicked) ─────────────────────────────────
    std::string KickReason;
};

} // namespace Safira

#endif // SAFIRA_APPLICATION_CLIENT_CLIENTSTATE_H
