#pragma once
#include <cstdint>
#include <string>

namespace samp {

struct ProbeResult {
  bool openConnection = false;   // OPEN_CONNECTION_REPLY diterima
  bool cookieOk = false;         // cookie challenge lolos
  bool authenticated = false;    // challenge AUTH_KEY berhasil dijawab
  bool accepted = false;         // CONNECTION_REQUEST_ACCEPTED diterima
  bool joined = false;           // ClientJoin diproses server (ada stream balik)
  bool playerInit = false;       // RPC 139 berhasil di-reassemble + di-decode
  uint16_t playerId = 0;
  int receivedPackets = 0;
  int receivedRpcs = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  int trackedPlayers = 0;        // PlayerJoin/Quit/Name/Color reducer
  std::string serverName;        // dari PlayerInit, bila server mengirimnya
  std::string rpcSummary;        // ringkasan RPC yang terlihat selama probe
  std::string stateSummary;      // event state terakhir yang berhasil direduce
  std::string variant;           // "samp" / "omp" / ""
  std::string log;               // log langkah demi langkah
  std::string error;
};

ProbeResult raknet_probe(const std::string& ip, uint16_t port, int timeoutMs = 1500);

}  // namespace samp
