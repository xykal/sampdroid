#pragma once
#include <cstdint>
#include <string>

namespace samp {

struct ProbeResult {
  bool openConnection = false;   // OPEN_CONNECTION_REPLY diterima
  bool cookieOk = false;         // cookie challenge lolos
  bool accepted = false;         // CONNECTION_REQUEST_ACCEPTED diterima
  std::string variant;           // "samp" / "omp" / ""
  std::string log;               // log langkah demi langkah
  std::string error;
};

ProbeResult raknet_probe(const std::string& ip, uint16_t port, int timeoutMs = 1500);

}  // namespace samp
