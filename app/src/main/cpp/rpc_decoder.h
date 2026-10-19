// Read-only decoders for a small, documented subset of SA-MP/open.mp RPCs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace samp::rpc {

struct PlayerInitData {
  bool valid = false;
  uint16_t playerId = 0;
  uint32_t spawnClassCount = 0;
  uint8_t worldTime = 0;
  uint8_t weather = 0;
  float gravity = 0.0f;
  // PlayerInit drives native name-tag presentation policy. Per-player hides
  // still arrive later in ShowPlayerNameTagForPlayer (80).
  float nameTagDrawDistance = 0.0f;
  bool disableNameTagLos = false;
  bool showNameTags = false;
  // Server-advertised sync cadence values in milliseconds. PlayerInit carries
  // these as raw u32 fields; retain them for future local-player transmit gating.
  uint32_t onFootSyncRateMs = 0;
  uint32_t inCarSyncRateMs = 0;
  uint32_t weaponSyncRateMs = 0;
  uint32_t syncMultiplier = 0;
  uint32_t lagCompensation = 0;
  std::string serverName;
};

const char* name(uint8_t rpcId);
bool decodePlayerInit(const std::vector<uint8_t>& rpcPayload, PlayerInitData& out);
std::string summarize(const std::vector<uint8_t>& rpcPayload);

}  // namespace samp::rpc
