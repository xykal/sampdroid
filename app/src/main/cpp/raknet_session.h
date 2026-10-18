// Persistent native RakNet session API for the Android client shell.
// This owns no renderer and never executes a Windows/x86 client.
#pragma once

#include <cstdint>
#include <string>

namespace samp {

struct SessionSnapshot {
  bool active = false;
  bool accepted = false;
  bool joined = false;
  bool playerInit = false;
  bool classSelectionKnown = false;
  bool classSelectable = false;
  bool spawnResponseKnown = false;
  bool spawnAllowed = false;
  uint16_t playerId = 0;
  uint32_t classModel = 0;
  int receivedPackets = 0;
  int receivedRpcs = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  int trackedPlayers = 0;
  int chatMessages = 0;
  std::string serverName;
  std::string rpcSummary;
  std::string stateSummary;
  std::string error;
};

// Establishes and retains one explicit, user-initiated Android-native session.
// Handshake retries can require multiple timeout windows; always call off the UI thread.
SessionSnapshot raknet_session_connect(const std::string& ip, uint16_t port,
                                      int timeoutMs = 1000);

// Receives at most one datagram, ACKs reliable packets, then reduces RPC state.
// Call from an IO coroutine while the user has chosen to keep the session alive.
SessionSnapshot raknet_session_poll(int timeoutMs = 200);

// Sends a RELIABLE_ORDERED channel-2 PlayerRequestClass RPC. The caller must
// subsequently poll for the server reply before reading classSelectionKnown.
SessionSnapshot raknet_session_request_class(uint16_t classId);

// Prepared for the next lifecycle step; it only sends after an explicit caller action.
SessionSnapshot raknet_session_request_spawn();

// Sends a best-effort disconnect notification and releases the UDP socket.
SessionSnapshot raknet_session_disconnect();

// Returns the last retained session state without reading from the network.
SessionSnapshot raknet_session_status();

}  // namespace samp
