// Independent wire decoder based on public interoperability documentation.
// PlayerInit layout is documented by open.mp's MPL-2.0 NetCode/core.hpp;
// no upstream implementation code is included here.
#include "rpc_decoder.h"

#include "raknet_codec.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace samp::rpc {
namespace {

constexpr uint8_t kIdRpc = 20;
constexpr uint8_t kPlayerInit = 139;

float readFloat(raknet::BitReader& reader) {
  const uint32_t raw = reader.readBits(32);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(raw), "SA-MP uses IEEE 754 32-bit floats");
  std::memcpy(&value, &raw, sizeof(value));
  if (!std::isfinite(value)) {
    // Keep reader failure distinct from an invalid remote float value.
    return 0.0f;
  }
  return value;
}

std::string readDynString8(raknet::BitReader& reader) {
  const size_t length = static_cast<uint8_t>(reader.readBits(8));
  // PlayerInit's HybridString<64> must never exceed this. A hard cap also
  // means a malformed server datagram cannot force a large allocation.
  if (!reader.ok() || length > 64) return {};
  const auto bytes = reader.readBytes(length);
  if (!reader.ok() || bytes.empty()) return {};
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

const char* name(uint8_t rpcId) {
  switch (rpcId) {
    case 11: return "SetPlayerName";
    case 12: return "SetPlayerPosition";
    case 13: return "SetPlayerPositionFindZ";
    case 14: return "SetPlayerHealth";
    case 19: return "SetPlayerFacingAngle";
    case 25: return "PlayerConnect";
    case 32: return "PlayerStreamIn";
    case 37: return "DisableCheckpoint";
    case 38: return "SetRaceCheckpoint";
    case 39: return "DisableRaceCheckpoint";
    case 52: return "PlayerSpawn";
    case 60: return "SendGameTimeUpdate";
    case 66: return "SetPlayerArmour";
    case 68: return "SetSpawnInfo";
    case 72: return "SetPlayerColor";
    case 88: return "SetPlayerSpecialAction";
    case 90: return "SetPlayerVelocity";
    case 93: return "SendClientMessage";
    case 99: return "MoveObject";
    case 107: return "SetCheckpoint";
    case 128: return "PlayerRequestClass";
    case 129: return "PlayerRequestSpawn";
    case 137: return "PlayerJoin";
    case 138: return "PlayerQuit";
    case 139: return "PlayerInit";
    case 163: return "PlayerStreamOut";
    case 164: return "StreamInVehicle";
    case 165: return "StreamOutVehicle";
    case 166: return "PlayerDeath";
    default: return "UnknownRPC";
  }
}

bool decodePlayerInit(const std::vector<uint8_t>& rpcPayload, PlayerInitData& out) {
  out = {};
  if (rpcPayload.size() < 3 || rpcPayload[0] != kIdRpc || rpcPayload[1] != kPlayerInit) {
    return false;
  }

  raknet::BitReader reader(rpcPayload.data() + 2, rpcPayload.size() - 2);

  reader.readBit();  // EnableZoneNames
  reader.readBit();  // UsePlayerPedAnims
  reader.readBit();  // AllowInteriorWeapons
  reader.readBit();  // UseLimitGlobalChatRadius
  readFloat(reader);
  reader.readBit();  // EnableStuntBonus
  readFloat(reader); // SetNameTagDrawDistance
  reader.readBit();  // DisableInteriorEnterExits
  reader.readBit();  // DisableNameTagLOS
  reader.readBit();  // ManualVehicleEngineAndLights
  out.spawnClassCount = reader.readBits(32);
  out.playerId = static_cast<uint16_t>(reader.readBits(16));
  reader.readBit();  // ShowNameTags
  reader.readBits(32); // ShowPlayerMarkers
  out.worldTime = static_cast<uint8_t>(reader.readBits(8));
  out.weather = static_cast<uint8_t>(reader.readBits(8));
  out.gravity = readFloat(reader);
  reader.readBit(); // LanMode
  reader.readBits(32); // SetDeathDropAmount
  reader.readBit(); // Instagib
  // PlayerInit's cadence fields are milliseconds (the compatible server
  // default for OnFootRate is 30). Preserve them instead of silently
  // discarding the values so outbound local sync can honor server policy.
  out.onFootSyncRateMs = reader.readBits(32);
  out.inCarSyncRateMs = reader.readBits(32);
  out.weaponSyncRateMs = reader.readBits(32);
  out.syncMultiplier = reader.readBits(32);
  out.lagCompensation = reader.readBits(32);
  out.serverName = readDynString8(reader);
  reader.readBytes(212); // VehicleModels
  reader.readBits(32);   // EnableVehicleFriendlyFire

  out.valid = reader.ok();
  return out.valid;
}

std::string summarize(const std::vector<uint8_t>& rpcPayload) {
  if (rpcPayload.size() < 2 || rpcPayload[0] != kIdRpc) return "non-RPC";

  const uint8_t rpcId = rpcPayload[1];
  std::ostringstream out;
  out << static_cast<unsigned>(rpcId) << "(" << name(rpcId) << ")";

  if (rpcId == kPlayerInit) {
    PlayerInitData init;
    if (decodePlayerInit(rpcPayload, init)) {
      out << " id=" << init.playerId
          << " classes=" << init.spawnClassCount
          << " server=\"" << init.serverName << "\""
          << " time=" << static_cast<unsigned>(init.worldTime)
          << " weather=" << static_cast<unsigned>(init.weather)
          << " onfoot=" << init.onFootSyncRateMs << "ms";
    } else {
      out << " [invalid/incomplete]";
    }
  }
  return out.str();
}

}  // namespace samp::rpc
