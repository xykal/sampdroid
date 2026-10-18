// Persistent native RakNet session API for the Android client shell.
// This owns no renderer and never executes a Windows/x86 client.
#pragma once

#include "client_sync.h"
#include "input_state.h"

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
  bool spawnConfirmationSent = false;
  bool spawnConfirmationAcked = false;
  bool localSpawnInfoKnown = false;
  bool localPositionKnown = false;
  bool nativeControlsKnown = false;
  bool onFootPreviewReady = false;
  bool checkpointActive = false;
  bool raceCheckpointActive = false;
  uint16_t playerId = 0;
  uint32_t classModel = 0;
  uint32_t localModel = 0;
  float localX = 0.0f;
  float localY = 0.0f;
  float localZ = 0.0f;
  float localAngle = 0.0f;
  float controlMoveX = 0.0f;
  float controlMoveY = 0.0f;
  float controlHeading = 0.0f;
  int controlLeftRight = 0;
  int controlUpDown = 0;
  uint16_t controlKeys = 0;
  // Raw OnFootRate retained from PlayerInit, plus the safe client-side
  // interval after malformed-rate bounds are applied.
  uint32_t onFootSyncRateMs = 0;
  uint32_t effectiveOnFootSyncRateMs = 0;
  int receivedPackets = 0;
  int receivedRpcs = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  int trackedPlayers = 0;
  int streamedPlayers = 0;
  int remoteOnFootPlayers = 0;
  int remoteOnFootPackets = 0;
  int chatMessages = 0;
  int reliableKeepalives = 0;
  int connectedPongs = 0;
  int outboundOnFootPackets = 0;
  bool localOnFootStateKnown = false;
  std::string serverName;
  std::string rpcSummary;
  std::string syncSummary;
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

// Starts the explicit spawn handshake. A nonzero server response automatically
// queues the required empty PlayerSpawn RPC; movement remains locked until its
// reliable transport ACK arrives.
SessionSnapshot raknet_session_request_spawn();

// Updates bounded Android-native semantic touch/controller input and rebuilds
// a local-only prospective OnFootSync preview. This function never transmits a
// gameplay packet; position/collision integration remains the world bridge's
// responsibility.
SessionSnapshot raknet_session_update_controls(const input::TouchIntent& input);

// Emits exactly one source-validated client PlayerFootSync packet only after
// the active session has PlayerInit, a server-approved spawn response, and a
// transport-ACKed PlayerSpawn confirmation. It uses UNRELIABLE_SEQUENCED/
// channel 1, its own ordering counter, and the
// server-advertised OnFootRate. Callers decide when to invoke it; this API does
// not start an automatic movement loop.
SessionSnapshot raknet_session_send_onfoot(const sync::OnFootState& state);

// Sends a best-effort disconnect notification and releases the UDP socket.
SessionSnapshot raknet_session_disconnect();

// Returns the last retained session state without reading from the network.
SessionSnapshot raknet_session_status();

}  // namespace samp
