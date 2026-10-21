#pragma once
#include <cstdint>
#include <string>

namespace samp {

struct ProbeResult {
  bool openConnection = false;   // OPEN_CONNECTION_REPLY diterima
  bool cookieOk = false;         // cookie challenge lolos
  bool authenticated = false;    // challenge AUTH_KEY berhasil dijawab
  bool accepted = false;         // CONNECTION_REQUEST_ACCEPTED diterima
  // These are RakNet transport receipts only. They say the peer ACKed the
  // exact source-style post-accept records, never that SA-MP semantically
  // admitted ClientJoin or will send PlayerInit/class/spawn.
  bool newIncomingTransportAcked = false;
  bool staticDataTransportAcked = false;
  bool clientJoinTransportAcked = false;
  // Kept semantic: only a decoded PlayerInit marks game admission here; a
  // broadcast or transport ACK after ClientJoin cannot set it.
  bool joined = false;
  bool playerInit = false;       // RPC 139 berhasil di-reassemble + di-decode
  uint16_t playerId = 0;
  int receivedPackets = 0;
  int receivedRpcs = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  int trackedPlayers = 0;        // PlayerJoin/Quit/Name/Color reducer
  std::string clientProtocol;    // exact selected ClientJoin wire identity
  std::string serverName;        // dari PlayerInit, bila server mengirimnya
  std::string rpcSummary;        // ringkasan RPC yang terlihat selama probe
  std::string stateSummary;      // event state terakhir yang berhasil direduce
  // Transport branch only: never game admission or an official OMP identity.
  std::string variant;           // "samp" / "omp" / "cache" / ""
  bool openMpServerEnvelope = false;  // exact access record enabled inbound unwrap
  std::string log;               // log langkah demi langkah
  std::string error;
};

// Probe uses the same fixed ClientJoin profile and query-gated cookie transport
// selection as a retained session; blank/unknown advertisement remains baseline.
ProbeResult raknet_probe(const std::string& ip, uint16_t port, int timeoutMs = 1500,
                         const std::string& advertisedServerVersion = "");

}  // namespace samp
