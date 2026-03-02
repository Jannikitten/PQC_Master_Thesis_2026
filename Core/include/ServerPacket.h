#ifndef PQC_MASTER_THESIS_2026_SERVERPACKET_H
#define PQC_MASTER_THESIS_2026_SERVERPACKET_H

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// All packet type identifiers shared between client and server.
// ─────────────────────────────────────────────────────────────────────────────
enum class PacketType : uint32_t {
	// ── Existing ──────────────────────────────────────────────────────────────
	None                  = 0,
	Message               = 1,
	ClientConnectionRequest = 2,
	ConnectionStatus      = 3,
	ClientList            = 4,
	ClientConnect         = 5,
	ClientUpdate          = 6,
	ClientDisconnect      = 7,
	ClientUpdateResponse  = 8,
	MessageHistory        = 9,
	ServerShutdown        = 10,
	ClientKick            = 11,

	// ── Private chat signalling (relayed through main server) ─────────────────
	//
	// Flow:
	//   A → Server : PrivateChatInvite      { target_username }
	//   Server → B : PrivateChatInvite      { from_username }
	//
	//   B → Server : PrivateChatResponse    { from_username, accepted, listen_port }
	//   Server → A : PrivateChatConnectTo   { peer_username, ip_and_port }   (if accepted)
	//   Server → B : PrivateChatDeclined    { peer_username }                (if declined,
	//                                                                          forwarded to A too)
	//
	// After PrivateChatConnectTo the two peers establish a direct DTLS session
	// (PrivateChatSession) without further involvement of the main server.
	PrivateChatInvite     = 20,
	PrivateChatResponse   = 21,
	PrivateChatConnectTo  = 22,
	PrivateChatDeclined   = 23,
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
inline bool IsValidMessage(const std::string& msg) {
	return !msg.empty() && msg.size() <= 2048;
}

#endif //PQC_MASTER_THESIS_2026_SERVERPACKET_H