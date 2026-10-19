// Independent implementation based on open.mp's public Shared/NetCode
// core.hpp PlayerAimSync/PlayerBulletSync/PlayerFootSync::write, vehicle.hpp
// PlayerPassengerSync::write, and Shared/Network bitstream primitives. No
// upstream implementation code is
// linked or copied here.
#include "sync_decoder.h"

#include "raknet_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace samp::syncdecode {
namespace {

constexpr float kMinWorldXy = -20000.0f;
constexpr float kMaxWorldXy = 20000.0f;
constexpr float kMinWorldZ = -1000.0f;
constexpr float kMaxWorldZ = 200000.0f;
constexpr float kMaxVelocitySquared = 100.0f * 100.0f;
constexpr float kMaxSurfingOffset = 10000.0f;
constexpr float kCompressedVecMagnitudeEpsilon = 0.00001f;
constexpr float kMaxAimPitchRadians = 3.14159265358979323846f;

float readFloat(raknet::BitReader& reader, bool& finite) {
  const uint32_t raw = reader.readBits(32);
  float value = 0.0f;
  static_assert(sizeof(raw) == sizeof(value), "SA-MP uses 32-bit float values");
  std::memcpy(&value, &raw, sizeof(value));
  finite = finite && std::isfinite(value);
  return value;
}

float readCompressedUnitFloat(raknet::BitReader& reader) {
  const uint16_t raw = static_cast<uint16_t>(reader.readBits(16));
  // RakNet NetworkBitStream::ReadCompressed(float): raw / 32767.5 - 1.
  return static_cast<float>(raw) / 32767.5f - 1.0f;
}

bool validWorldPosition(float x, float y, float z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         x > kMinWorldXy && x < kMaxWorldXy && y > kMinWorldXy && y < kMaxWorldXy &&
         z > kMinWorldZ && z < kMaxWorldZ;
}

bool validVelocity(float x, float y, float z) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
  const double squared = static_cast<double>(x) * x + static_cast<double>(y) * y +
                         static_cast<double>(z) * z;
  return std::isfinite(squared) && squared <= kMaxVelocitySquared;
}

bool validSurfingOffset(float x, float y, float z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         std::fabs(x) <= kMaxSurfingOffset && std::fabs(y) <= kMaxSurfingOffset &&
         std::fabs(z) <= kMaxSurfingOffset;
}

float unpackPercent(uint8_t nibble) {
  // writeCompressedPercentPair stores ceil(percent / 7), reserving 15 for
  // 100. The client-visible state is consequently quantized in seven-point
  // steps for values below 100.
  return nibble >= 15 ? 100.0f : static_cast<float>(nibble * 7);
}

bool decodeCompressedQuaternion(raknet::BitReader& reader, float& w, float& x, float& y, float& z) {
  const bool wNegative = reader.readBit();
  const bool xNegative = reader.readBit();
  const bool yNegative = reader.readBit();
  const bool zNegative = reader.readBit();
  x = static_cast<float>(reader.readBits(16)) / 65535.0f;
  y = static_cast<float>(reader.readBits(16)) / 65535.0f;
  z = static_cast<float>(reader.readBits(16)) / 65535.0f;
  if (!reader.ok()) return false;
  if (xNegative) x = -x;
  if (yNegative) y = -y;
  if (zNegative) z = -z;
  const float xyzSquared = x * x + y * y + z * z;
  if (!std::isfinite(xyzSquared) || xyzSquared > 1.01f) return false;
  w = std::sqrt(std::max(0.0f, 1.0f - xyzSquared));
  if (wNegative) w = -w;
  return std::isfinite(w);
}

float normalizedDegrees(float value) {
  float result = std::fmod(value, 360.0f);
  if (result < 0.0f) result += 360.0f;
  return result;
}

bool validVehicleHealth(float value) {
  // Stock health is normally near 1000, but compatible scripts may set higher
  // values. Bound it to the Packet 200 u16 envelope instead of rejecting a
  // legitimate server visual update merely because it exceeds stock health.
  return std::isfinite(value) && value >= 0.0f && value <= 65535.0f;
}

// Same compatible camera-mode envelope that the open.mp AimSync handler
// accepts before it normalizes an invalid incoming mode. A broadcast frame
// outside this envelope is not useful renderer data, so reject it at the
// untrusted Android boundary rather than inventing a fallback camera.
bool validAimCameraMode(uint8_t mode) {
  if (mode < 3u || mode > 64u) return false;
  if (mode == 5u || mode == 6u || mode == 17u || mode == 52u || mode == 54u ||
      mode == 60u || mode == 61u) {
    return false;
  }
  return !((mode >= 9u && mode <= 13u) || (mode >= 19u && mode <= 21u) ||
           (mode >= 23u && mode <= 28u) || (mode >= 30u && mode <= 45u) ||
           (mode >= 48u && mode <= 50u));
}

bool validBulletWeapon(uint8_t weaponId) {
  // open.mp WeaponSlotData::shootable() accepts the legacy bullet weapons:
  // Colt-45 through Sniper (22..34) plus Minigun (38). Rocket/spray/melee
  // weapons use different sync semantics and cannot be a Packet-206 visual.
  return (weaponId >= 22u && weaponId <= 34u) || weaponId == 38u;
}

bool finiteVectorWithinSquared(float x, float y, float z, double maximumSquared) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
  const double squared = static_cast<double>(x) * x + static_cast<double>(y) * y +
                         static_cast<double>(z) * z;
  return std::isfinite(squared) && squared <= maximumSquared;
}

}  // namespace

bool decodeRemoteAimBroadcast(const std::vector<uint8_t>& payload, RemoteAimSync& out,
                              size_t payloadBitLength) {
  // NetworkPacketBase<PlayerAimSync>::write serializes exactly:
  // ID:u8 | PlayerID:u16 | CamMode:u8 | CamFront:3*f32 | CamPos:3*f32 |
  // AimZ:f32 | ZoomWepState:u8 | AspectRatio:u8 = 272 bits / 34 bytes.
  constexpr size_t kRemoteAimBits = 272;
  constexpr size_t kRemoteAimBytes = kRemoteAimBits / 8;
  out = {};
  if (payload.size() != kRemoteAimBytes || payload.empty() ||
      payload[0] != kPlayerAimSyncPacketId ||
      (payloadBitLength != 0 && payloadBitLength != kRemoteAimBits)) {
    return false;
  }

  raknet::BitReader reader(payload.data(), payload.size(), payloadBitLength);
  if (reader.readBits(8) != kPlayerAimSyncPacketId) return false;
  out.playerId = static_cast<uint16_t>(reader.readBits(16));
  out.cameraMode = static_cast<uint8_t>(reader.readBits(8));
  bool finite = true;
  out.cameraFrontX = readFloat(reader, finite);
  out.cameraFrontY = readFloat(reader, finite);
  out.cameraFrontZ = readFloat(reader, finite);
  out.cameraPositionX = readFloat(reader, finite);
  out.cameraPositionY = readFloat(reader, finite);
  out.cameraPositionZ = readFloat(reader, finite);
  out.aimZ = readFloat(reader, finite);
  const uint8_t zoomWeaponState = static_cast<uint8_t>(reader.readBits(8));
  out.cameraZoom = static_cast<uint8_t>(zoomWeaponState & 0x3Fu);
  out.weaponState = static_cast<uint8_t>((zoomWeaponState >> 6u) & 0x03u);
  out.aspectRatio = static_cast<uint8_t>(reader.readBits(8));
  if (!reader.ok() || !finite || !validAimCameraMode(out.cameraMode) ||
      !validWorldPosition(out.cameraPositionX, out.cameraPositionY, out.cameraPositionZ) ||
      out.aimZ < -kMaxAimPitchRadians || out.aimZ > kMaxAimPitchRadians) {
    return false;
  }
  const double frontSquared = static_cast<double>(out.cameraFrontX) * out.cameraFrontX +
                              static_cast<double>(out.cameraFrontY) * out.cameraFrontY +
                              static_cast<double>(out.cameraFrontZ) * out.cameraFrontZ;
  // Preserve the server-compatible envelope and raw vector in the snapshot.
  // The renderer only normalizes it for a short direction marker; it never
  // turns it into a target or a hit endpoint.
  return std::isfinite(frontSquared) && frontSquared > 0.0 && frontSquared < 1.5;
}

bool decodeRemoteBulletBroadcast(const std::vector<uint8_t>& payload, RemoteBulletSync& out,
                                 size_t payloadBitLength) {
  // PlayerBulletSync::write has no conditional/compressed fields:
  // ID:u8 | PlayerID:u16 | HitType:u8 | HitID:u16 | Origin/HitPos/Offset:3*f32 |
  // WeaponID:u8 = 344 bits / 43 bytes.
  constexpr size_t kRemoteBulletBits = 344;
  constexpr size_t kRemoteBulletBytes = kRemoteBulletBits / 8;
  out = {};
  if (payload.size() != kRemoteBulletBytes || payload.empty() ||
      payload[0] != kPlayerBulletSyncPacketId ||
      (payloadBitLength != 0 && payloadBitLength != kRemoteBulletBits)) {
    return false;
  }

  raknet::BitReader reader(payload.data(), payload.size(), payloadBitLength);
  if (reader.readBits(8) != kPlayerBulletSyncPacketId) return false;
  out.playerId = static_cast<uint16_t>(reader.readBits(16));
  out.hitType = static_cast<uint8_t>(reader.readBits(8));
  out.hitId = static_cast<uint16_t>(reader.readBits(16));
  bool finite = true;
  out.originX = readFloat(reader, finite);
  out.originY = readFloat(reader, finite);
  out.originZ = readFloat(reader, finite);
  out.hitPositionX = readFloat(reader, finite);
  out.hitPositionY = readFloat(reader, finite);
  out.hitPositionZ = readFloat(reader, finite);
  out.offsetX = readFloat(reader, finite);
  out.offsetY = readFloat(reader, finite);
  out.offsetZ = readFloat(reader, finite);
  out.weaponId = static_cast<uint8_t>(reader.readBits(8));
  if (!reader.ok() || !finite || out.hitType > 4u || !validBulletWeapon(out.weaponId) ||
      !finiteVectorWithinSquared(out.originX, out.originY, out.originZ, 20000.0 * 20000.0) ||
      // HitPos is not used as an Android hit assertion, but it supplies the
      // server-relayed direction cue and needs normal renderer-safe bounds.
      !validWorldPosition(out.hitPositionX, out.hitPositionY, out.hitPositionZ)) {
    return false;
  }
  const double maxOffsetSquared = out.hitType == 0u ? 20000.0 * 20000.0 : 1000.0 * 1000.0;
  return finiteVectorWithinSquared(out.offsetX, out.offsetY, out.offsetZ, maxOffsetSquared);
}

bool decodeRemoteOnFootBroadcast(const std::vector<uint8_t>& payload, RemoteOnFootSync& out,
                                      size_t payloadBitLength) {
  out = {};
  if (payload.size() < 4 || payload[0] != kPlayerFootSyncPacketId) return false;
  if (payloadBitLength != 0 && payloadBitLength > payload.size() * 8) return false;

  raknet::BitReader reader(payload.data(), payload.size(), payloadBitLength);
  if (reader.readBits(8) != kPlayerFootSyncPacketId) return false;
  out.playerId = static_cast<uint16_t>(reader.readBits(16));

  const bool hasLeftRight = reader.readBit();
  if (hasLeftRight) out.leftRight = static_cast<int16_t>(reader.readBits(16));
  const bool hasUpDown = reader.readBit();
  if (hasUpDown) out.upDown = static_cast<int16_t>(reader.readBits(16));
  out.keys = static_cast<uint16_t>(reader.readBits(16));

  bool finite = true;
  out.positionX = readFloat(reader, finite);
  out.positionY = readFloat(reader, finite);
  out.positionZ = readFloat(reader, finite);
  if (!reader.ok() || !finite || !validWorldPosition(out.positionX, out.positionY, out.positionZ)) {
    return false;
  }

  const bool wNegative = reader.readBit();
  const bool xNegative = reader.readBit();
  const bool yNegative = reader.readBit();
  const bool zNegative = reader.readBit();
  out.quaternionX = static_cast<float>(reader.readBits(16)) / 65535.0f;
  out.quaternionY = static_cast<float>(reader.readBits(16)) / 65535.0f;
  out.quaternionZ = static_cast<float>(reader.readBits(16)) / 65535.0f;
  if (xNegative) out.quaternionX = -out.quaternionX;
  if (yNegative) out.quaternionY = -out.quaternionY;
  if (zNegative) out.quaternionZ = -out.quaternionZ;
  const float quaternionXYZ2 = out.quaternionX * out.quaternionX +
                               out.quaternionY * out.quaternionY +
                               out.quaternionZ * out.quaternionZ;
  // The reference reader clamps a minor quantization overshoot to zero. Keep
  // that tolerance, but reject an obviously non-normalized hostile payload.
  if (!std::isfinite(quaternionXYZ2) || quaternionXYZ2 > 1.01f) return false;
  float quaternionW2 = 1.0f - quaternionXYZ2;
  if (quaternionW2 < 0.0f) quaternionW2 = 0.0f;
  out.quaternionW = std::sqrt(quaternionW2);
  if (wNegative) out.quaternionW = -out.quaternionW;

  const uint8_t healthArmour = static_cast<uint8_t>(reader.readBits(8));
  out.health = unpackPercent(static_cast<uint8_t>(healthArmour >> 4));
  out.armour = unpackPercent(static_cast<uint8_t>(healthArmour & 0x0Fu));
  out.weaponAdditionalKey = static_cast<uint8_t>(reader.readBits(8));
  out.specialAction = static_cast<uint8_t>(reader.readBits(8));

  const float velocityMagnitude = readFloat(reader, finite);
  if (!reader.ok() || !finite || velocityMagnitude < 0.0f || velocityMagnitude > 100.0f) {
    return false;
  }
  if (velocityMagnitude > kCompressedVecMagnitudeEpsilon) {
    const float normalizedX = readCompressedUnitFloat(reader);
    const float normalizedY = readCompressedUnitFloat(reader);
    const float normalizedZ = readCompressedUnitFloat(reader);
    if (!reader.ok()) return false;
    out.velocityX = normalizedX * velocityMagnitude;
    out.velocityY = normalizedY * velocityMagnitude;
    out.velocityZ = normalizedZ * velocityMagnitude;
  }
  if (!validVelocity(out.velocityX, out.velocityY, out.velocityZ)) return false;

  out.surfing = reader.readBit();
  if (out.surfing) {
    out.surfingId = static_cast<uint16_t>(reader.readBits(16));
    out.surfingOffsetX = readFloat(reader, finite);
    out.surfingOffsetY = readFloat(reader, finite);
    out.surfingOffsetZ = readFloat(reader, finite);
    if (!reader.ok() || !finite ||
        !validSurfingOffset(out.surfingOffsetX, out.surfingOffsetY, out.surfingOffsetZ)) {
      return false;
    }
  }

  out.animationKnown = reader.readBit();
  if (out.animationKnown) {
    out.animationId = static_cast<uint16_t>(reader.readBits(16));
    out.animationFlags = static_cast<uint16_t>(reader.readBits(16));
  }
  if (!reader.ok() || !std::isfinite(out.quaternionW) || !std::isfinite(out.quaternionX) ||
      !std::isfinite(out.quaternionY) || !std::isfinite(out.quaternionZ)) {
    return false;
  }
  return true;
}

bool decodeRemoteVehicleBroadcast(const std::vector<uint8_t>& payload, RemoteVehicleSync& out,
                                  size_t payloadBitLength) {
  out = {};
  if (payload.size() < 5 ||
      (payload[0] != kPlayerVehicleSyncPacketId && payload[0] != kPlayerUnoccupiedSyncPacketId) ||
      (payloadBitLength != 0 && payloadBitLength > payload.size() * 8)) {
    return false;
  }
  raknet::BitReader reader(payload.data(), payload.size(), payloadBitLength);
  const uint8_t packetId = static_cast<uint8_t>(reader.readBits(8));
  out.playerId = static_cast<uint16_t>(reader.readBits(16));
  out.vehicleId = static_cast<uint16_t>(reader.readBits(16));
  if (!reader.ok() || out.vehicleId == 0) return false;

  bool finite = true;
  if (packetId == kPlayerVehicleSyncPacketId) {
    out.driverSync = true;
    // Directional input is consumed for wire alignment but belongs to the
    // remote driver's state, not this vehicle-only visual reducer.
    reader.readBits(16);
    reader.readBits(16);
    reader.readBits(16);
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    if (!decodeCompressedQuaternion(reader, qw, qx, qy, qz)) return false;
    out.x = readFloat(reader, finite);
    out.y = readFloat(reader, finite);
    out.z = readFloat(reader, finite);
    const float magnitude = readFloat(reader, finite);
    if (!reader.ok() || !finite || magnitude < 0.0f || magnitude > 100.0f ||
        !validWorldPosition(out.x, out.y, out.z)) {
      return false;
    }
    if (magnitude > kCompressedVecMagnitudeEpsilon) {
      out.velocityX = readCompressedUnitFloat(reader) * magnitude;
      out.velocityY = readCompressedUnitFloat(reader) * magnitude;
      out.velocityZ = readCompressedUnitFloat(reader) * magnitude;
    }
    // The compatible Packet 200 writer quantizes vehicle health to u16.
    out.health = static_cast<float>(reader.readBits(16));
    reader.readBits(8);  // Packed driver health/armour.
    reader.readBits(8);  // Additional key/weapon.
    reader.readBit();    // Siren.
    reader.readBit();    // Landing gear.
    const bool hasHydraThrust = reader.readBit();
    if (hasHydraThrust) reader.readBits(32);
    const bool hasTrailer = reader.readBit();
    if (hasTrailer) reader.readBits(16);
    if (!reader.ok() || !validVelocity(out.velocityX, out.velocityY, out.velocityZ) ||
        !validVehicleHealth(out.health)) {
      return false;
    }
    // GTA's horizontal heading is the negative Z-axis quaternion yaw. Keep
    // roll/pitch in the validation path without pretending this proxy renders
    // full vehicle attitude.
    const float yawRadians = std::atan2(2.0f * (qw * qz + qx * qy),
                                        1.0f - 2.0f * (qy * qy + qz * qz));
    out.angle = normalizedDegrees(-yawRadians * 180.0f / 3.14159265358979323846f);
    out.angleKnown = std::isfinite(out.angle);
    return out.angleKnown;
  }

  // Packet 209 contains raw unoccupied rigid-body vectors. Its direction
  // vector is not an Euler heading, so retain the last authoritative angle
  // from StreamInVehicle/driver sync while applying its new position/health.
  reader.readBits(8);  // Seat ID.
  for (int i = 0; i < 3; ++i) readFloat(reader, finite);  // Roll.
  for (int i = 0; i < 3; ++i) readFloat(reader, finite);  // Direction.
  out.x = readFloat(reader, finite);
  out.y = readFloat(reader, finite);
  out.z = readFloat(reader, finite);
  out.velocityX = readFloat(reader, finite);
  out.velocityY = readFloat(reader, finite);
  out.velocityZ = readFloat(reader, finite);
  for (int i = 0; i < 3; ++i) readFloat(reader, finite);  // Angular velocity.
  out.health = readFloat(reader, finite);
  return reader.ok() && finite && validWorldPosition(out.x, out.y, out.z) &&
         validVelocity(out.velocityX, out.velocityY, out.velocityZ) && validVehicleHealth(out.health);
}

bool decodeRemotePassengerBroadcast(const std::vector<uint8_t>& payload, RemotePassengerSync& out,
                                    size_t payloadBitLength) {
  // Packet 211 has no conditional fields in the compatible public writer:
  // 8 ID + 16 player + 16 vehicle + 16 packed seat + two u8 vitals + three
  // u16 input words + raw three-float position = 216 bits / 27 bytes.
  constexpr size_t kPassengerPacketBits = 216;
  constexpr size_t kPassengerPacketBytes = kPassengerPacketBits / 8;
  out = {};
  if (payload.size() != kPassengerPacketBytes || payload.empty() ||
      payload[0] != kPlayerPassengerSyncPacketId ||
      (payloadBitLength != 0 && payloadBitLength != kPassengerPacketBits)) {
    return false;
  }

  raknet::BitReader reader(payload.data(), payload.size(), payloadBitLength);
  if (reader.readBits(8) != kPlayerPassengerSyncPacketId) return false;
  out.playerId = static_cast<uint16_t>(reader.readBits(16));
  out.vehicleId = static_cast<uint16_t>(reader.readBits(16));
  const uint16_t packedSeat = static_cast<uint16_t>(reader.readBits(16));
  out.seatId = static_cast<uint8_t>(packedSeat & 0x3Fu);
  out.driveBy = (packedSeat & 0x40u) != 0u;
  out.cuffed = (packedSeat & 0x80u) != 0u;
  out.weaponAdditionalKey = static_cast<uint8_t>(packedSeat >> 8u);
  out.health = static_cast<float>(reader.readBits(8));
  out.armour = static_cast<float>(reader.readBits(8));
  out.leftRight = static_cast<int16_t>(reader.readBits(16));
  out.upDown = static_cast<int16_t>(reader.readBits(16));
  out.keys = static_cast<uint16_t>(reader.readBits(16));
  bool finite = true;
  out.x = readFloat(reader, finite);
  out.y = readFloat(reader, finite);
  out.z = readFloat(reader, finite);
  return reader.ok() && finite && out.vehicleId != 0 && out.health <= 100.0f &&
         out.armour <= 100.0f && validWorldPosition(out.x, out.y, out.z);
}

std::string summarize(const RemoteAimSync& sync) {
  std::ostringstream out;
  out << "PlayerAimSync #" << sync.playerId << " mode " << static_cast<unsigned>(sync.cameraMode)
      << " front " << sync.cameraFrontX << "," << sync.cameraFrontY << "," << sync.cameraFrontZ;
  return out.str();
}

std::string summarize(const RemoteBulletSync& sync) {
  std::ostringstream out;
  out << "PlayerBulletSync #" << sync.playerId << " weapon " << static_cast<unsigned>(sync.weaponId);
  return out.str();
}

std::string summarize(const RemoteOnFootSync& sync) {
  std::ostringstream out;
  out << "PlayerFootSync #" << sync.playerId << " @ " << sync.positionX << "," << sync.positionY
      << "," << sync.positionZ;
  return out.str();
}

std::string summarize(const RemoteVehicleSync& sync) {
  std::ostringstream out;
  out << (sync.driverSync ? "PlayerVehicleSync" : "PlayerUnoccupiedSync")
      << " vehicle #" << sync.vehicleId << " @ " << sync.x << "," << sync.y << "," << sync.z;
  return out.str();
}

std::string summarize(const RemotePassengerSync& sync) {
  std::ostringstream out;
  out << "PlayerPassengerSync #" << sync.playerId << " vehicle #" << sync.vehicleId
      << " seat " << static_cast<unsigned>(sync.seatId) << " @ " << sync.x << "," << sync.y
      << "," << sync.z;
  return out.str();
}

}  // namespace samp::syncdecode
