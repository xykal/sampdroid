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
  std::string variant;           // "samp" / "omp" / ""
  std::string log;               // log langkah demi langkah
  std::string error;
};

ProbeResult raknet_probe(const std::string& ip, uint16_t port, int timeoutMs = 1500);

}  // namespace samp
