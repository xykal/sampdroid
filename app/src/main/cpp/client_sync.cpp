#include "client_sync.h"

#include "raknet_codec.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace samp::sync {
namespace {

constexpr float kMinWorldXy = -20000.0f;
constexpr float kMaxWorldXy = 20000.0f;
constexpr float kMinWorldZ = -1000.0f;
constexpr float kMaxWorldZ = 200000.0f;
constexpr float kMaxVelocitySquared = 100.0f * 100.0f;
constexpr size_t kPlayerFootSyncBits = 69u * 8u;
constexpr size_t kPlayerVehicleSyncBits = 64u * 8u;
constexpr size_t kPlayerAimSyncBits = 32u * 8u;
// Source-verified against open.mp PlayerBulletSync::read (commit
// 3d20f3dc): client Packet-206 omits the server-broadcast PlayerID. Packet ID,
// hit type/ID, three raw VEC3 values, and weapon byte = 41 bytes.
constexpr size_t kPlayerNoTargetBulletSyncBits = 41u * 8u;
constexpr size_t kPlayerPassengerSyncBits = 25u * 8u;

bool isFinite(float value) {
  return std::isfinite(value);
}

void writeFloat(raknet::BitWriter& writer, float value) {
  uint32_t raw = 0;
  static_assert(sizeof(raw) == sizeof(value), "unexpected float size");
  std::memcpy(&raw, &value, sizeof(raw));
  writer.writeBits(raw, 32);
}

bool validWorldPosition(float x, float y, float z) {
  return isFinite(x) && isFinite(y) && isFinite(z) && x > kMinWorldXy && x < kMaxWorldXy &&
         y > kMinWorldXy && y < kMaxWorldXy && z > kMinWorldZ && z < kMaxWorldZ;
}

// Same camera-mode whitelist/rejection policy applied by the compatible
// open.mp PlayerAimSync handler. The local builder currently uses only mode
// 53 (ordinary weapon aim), but retaining the validator avoids emitting a
// packet that server code will immediately coerce or reject.
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

}  // namespace

bool isValidOnFootState(const OnFootState& state) {
  // Matches NetworkBitStream::readPosVEC3: the legacy server uses strict
  // bounds here, so values on the outer edge are rejected as well.
  if (!isFinite(state.positionX) || !isFinite(state.positionY) || !isFinite(state.positionZ) ||
      state.positionX <= kMinWorldXy || state.positionX >= kMaxWorldXy ||
      state.positionY <= kMinWorldXy || state.positionY >= kMaxWorldXy ||
      state.positionZ <= kMinWorldZ || state.positionZ >= kMaxWorldZ) {
    return false;
  }

  // The server's readGTAQuat checks all four raw components for finiteness.
  if (!isFinite(state.quaternionW) || !isFinite(state.quaternionX) ||
      !isFinite(state.quaternionY) || !isFinite(state.quaternionZ)) {
    return false;
  }

  // Matches readVelVEC3. Compute in double precision so the local guard does
  // not overflow before the source-compatible 100-unit velocity bound.
  if (!isFinite(state.velocityX) || !isFinite(state.velocityY) || !isFinite(state.velocityZ)) {
    return false;
  }
  const double velocitySquared = static_cast<double>(state.velocityX) * state.velocityX +
                                 static_cast<double>(state.velocityY) * state.velocityY +
                                 static_cast<double>(state.velocityZ) * state.velocityZ;
  if (!std::isfinite(velocitySquared) || velocitySquared > kMaxVelocitySquared) return false;

  // PlayerFootSync::read consumes a raw surf offset vector even while the
  // surfing ID is zero, so retain its finite-value safety rule as well.
  return isFinite(state.surfingOffsetX) && isFinite(state.surfingOffsetY) &&
         isFinite(state.surfingOffsetZ);
}

EncodedPayload buildPlayerFootSync(const OnFootState& state) {
  if (!isValidOnFootState(state)) return {};

  // Do not use PlayerFootSync::write here. That method serializes the
  // server-to-recipient broadcast representation (player ID, conditional
  // directional inputs, compressed state). The server's inbound read handler
  // consumes this fixed, raw client representation instead.
  raknet::BitWriter writer;
  writer.writeBits(kPlayerFootSyncPacketId, 8);
  writer.writeBits(state.leftRight, 16);
  writer.writeBits(state.upDown, 16);
  writer.writeBits(state.keys, 16);

  writeFloat(writer, state.positionX);
  writeFloat(writer, state.positionY);
  writeFloat(writer, state.positionZ);

  writeFloat(writer, state.quaternionW);
  writeFloat(writer, state.quaternionX);
  writeFloat(writer, state.quaternionY);
  writeFloat(writer, state.quaternionZ);

  writer.writeBits(state.health, 8);
  writer.writeBits(state.armour, 8);
  writer.writeBits(state.weaponAdditionalKey, 8);
  writer.writeBits(state.specialAction, 8);

  writeFloat(writer, state.velocityX);
  writeFloat(writer, state.velocityY);
  writeFloat(writer, state.velocityZ);

  writeFloat(writer, state.surfingOffsetX);
  writeFloat(writer, state.surfingOffsetY);
  writeFloat(writer, state.surfingOffsetZ);
  writer.writeBits(state.surfingId, 16);

  writer.writeBits(state.animationId, 16);
  writer.writeBits(state.animationFlags, 16);

  if (writer.bitPosition != kPlayerFootSyncBits ||
      writer.bitPosition > std::numeric_limits<uint16_t>::max()) {
    return {};
  }
  return {std::move(writer.buffer), static_cast<uint16_t>(writer.bitPosition)};
}

bool isValidInCarState(const InCarState& state) {
  if (state.vehicleId == 0) return false;
  if (!isFinite(state.positionX) || !isFinite(state.positionY) || !isFinite(state.positionZ) ||
      state.positionX <= kMinWorldXy || state.positionX >= kMaxWorldXy ||
      state.positionY <= kMinWorldXy || state.positionY >= kMaxWorldXy ||
      state.positionZ <= kMinWorldZ || state.positionZ >= kMaxWorldZ) {
    return false;
  }
  if (!isFinite(state.quaternionW) || !isFinite(state.quaternionX) ||
      !isFinite(state.quaternionY) || !isFinite(state.quaternionZ)) {
    return false;
  }
  if (!isFinite(state.velocityX) || !isFinite(state.velocityY) || !isFinite(state.velocityZ)) {
    return false;
  }
  const double velocitySquared = static_cast<double>(state.velocityX) * state.velocityX +
                                 static_cast<double>(state.velocityY) * state.velocityY +
                                 static_cast<double>(state.velocityZ) * state.velocityZ;
  if (!std::isfinite(velocitySquared) || velocitySquared > kMaxVelocitySquared) return false;
  // Local vehicle facts come from a source-validated StreamIn/vehicle mutator
  // reducer that uses this same safe envelope. Do not manufacture an arbitrary
  // health value merely because Packet 200 stores it as a float.
  if (!isFinite(state.vehicleHealth) || state.vehicleHealth < 0.0f ||
      state.vehicleHealth > 65535.0f) {
    return false;
  }
  return state.siren <= 1u && state.landingGear <= 1u;
}

EncodedPayload buildPlayerVehicleSync(const InCarState& state) {
  if (!isValidInCarState(state)) return {};

  // This is the client-originated reader order in PlayerVehicleSync::read.
  // Do not reuse the server broadcast writer: that representation has a player
  // ID, compressed vectors/quaternion, optional bits, and u16 vehicle health.
  raknet::BitWriter writer;
  writer.writeBits(kPlayerVehicleSyncPacketId, 8);
  writer.writeBits(state.vehicleId, 16);
  writer.writeBits(state.leftRight, 16);
  writer.writeBits(state.upDown, 16);
  writer.writeBits(state.keys, 16);
  writeFloat(writer, state.quaternionW);
  writeFloat(writer, state.quaternionX);
  writeFloat(writer, state.quaternionY);
  writeFloat(writer, state.quaternionZ);
  writeFloat(writer, state.positionX);
  writeFloat(writer, state.positionY);
  writeFloat(writer, state.positionZ);
  writeFloat(writer, state.velocityX);
  writeFloat(writer, state.velocityY);
  writeFloat(writer, state.velocityZ);
  writeFloat(writer, state.vehicleHealth);
  // `readCompressedPercentPair` on the client-originated reader consumes the
  // two raw legacy percent bytes, just like PlayerFootSync::read.
  writer.writeBits(state.playerHealth, 8);
  writer.writeBits(state.playerArmour, 8);
  writer.writeBits(state.additionalKeyWeapon, 8);
  writer.writeBits(state.siren, 8);
  writer.writeBits(state.landingGear, 8);
  writer.writeBits(state.trailerId, 16);
  writer.writeBits(state.hydraThrustAngle, 32);

  if (writer.bitPosition != kPlayerVehicleSyncBits ||
      writer.bitPosition > std::numeric_limits<uint16_t>::max()) {
    return {};
  }
  return {std::move(writer.buffer), static_cast<uint16_t>(writer.bitPosition)};
}

bool isValidAimState(const AimState& state) {
  if (!validAimCameraMode(state.cameraMode) || state.cameraZoom > 63u || state.weaponState > 3u) {
    return false;
  }
  if (!validWorldPosition(state.cameraPositionX, state.cameraPositionY, state.cameraPositionZ) ||
      !isFinite(state.cameraFrontX) || !isFinite(state.cameraFrontY) ||
      !isFinite(state.cameraFrontZ) || !isFinite(state.aimZ)) {
    return false;
  }
  const double frontSquared = static_cast<double>(state.cameraFrontX) * state.cameraFrontX +
                              static_cast<double>(state.cameraFrontY) * state.cameraFrontY +
                              static_cast<double>(state.cameraFrontZ) * state.cameraFrontZ;
  // open.mp accepts a nonzero camera front vector below 1.5 squared. The
  // native bridge provides a normalized vector, but preserve the server's
  // compatibility envelope here instead of normalizing malformed input.
  if (!std::isfinite(frontSquared) || frontSquared <= 0.0 || frontSquared >= 1.5) return false;
  constexpr float kMaxAimPitchRadians = 3.14159265358979323846f;
  return state.aimZ >= -kMaxAimPitchRadians && state.aimZ <= kMaxAimPitchRadians;
}

EncodedPayload buildPlayerAimSync(const AimState& state) {
  if (!isValidAimState(state)) return {};

  // Source-verified client-originated writer order from open.mp's NPC
  // `sendAimSync`: unlike a server broadcast there is no PlayerID prefix.
  raknet::BitWriter writer;
  writer.writeBits(kPlayerAimSyncPacketId, 8);
  writer.writeBits(state.cameraMode, 8);
  writeFloat(writer, state.cameraFrontX);
  writeFloat(writer, state.cameraFrontY);
  writeFloat(writer, state.cameraFrontZ);
  writeFloat(writer, state.cameraPositionX);
  writeFloat(writer, state.cameraPositionY);
  writeFloat(writer, state.cameraPositionZ);
  writeFloat(writer, state.aimZ);
  const uint8_t zoomWeaponState = static_cast<uint8_t>((state.cameraZoom & 0x3Fu) |
                                                        ((state.weaponState & 0x03u) << 6u));
  writer.writeBits(zoomWeaponState, 8);
  writer.writeBits(state.aspectRatio, 8);
  if (writer.bitPosition != kPlayerAimSyncBits ||
      writer.bitPosition > std::numeric_limits<uint16_t>::max()) {
    return {};
  }
  return {std::move(writer.buffer), static_cast<uint16_t>(writer.bitPosition)};
}

bool isValidNoTargetBulletState(const BulletState& state) {
  // Current Android implementation deliberately refuses all entity/object hit
  // variants. It can never nominate a target ID, an offset on one, or local
  // damage; server code remains the only authority for gameplay outcomes.
  if (state.hitType != 0u || state.hitId != 0u || state.offsetX != 0.0f ||
      state.offsetY != 0.0f || state.offsetZ != 0.0f) {
    return false;
  }
  // For HitType=None, open.mp exposes a real static-world contact as absolute
  // HitPos coordinates and uses all zeroes when nothing was hit. Android may
  // use only its bounded user-data COL ray for that optional world-only point;
  // partial/non-finite values are never permitted onto the wire.
  const bool noImpact = state.hitPositionX == 0.0f && state.hitPositionY == 0.0f &&
                        state.hitPositionZ == 0.0f;
  if (!noImpact && !validWorldPosition(state.hitPositionX, state.hitPositionY,
                                       state.hitPositionZ)) {
    return false;
  }
  // open.mp WeaponSlotData::shootable accepts Colt-45 through Sniper plus
  // Minigun. Other weapon families use separate legacy sync semantics.
  const bool shootableWeapon = (state.weaponId >= 22u && state.weaponId <= 34u) ||
                               state.weaponId == 38u;
  return shootableWeapon && validWorldPosition(state.originX, state.originY, state.originZ);
}

EncodedPayload buildPlayerNoTargetBulletSync(const BulletState& state) {
  if (!isValidNoTargetBulletState(state)) return {};

  // PlayerBulletSync::read consumes the peer identity from RakNet/session
  // context. Do not accidentally reuse the 43-byte server broadcast form,
  // whose PlayerID prefix would shift every client field by two bytes.
  raknet::BitWriter writer;
  writer.writeBits(kPlayerBulletSyncPacketId, 8);
  writer.writeBits(state.hitType, 8);
  writer.writeBits(state.hitId, 16);
  writeFloat(writer, state.originX);
  writeFloat(writer, state.originY);
  writeFloat(writer, state.originZ);
  writeFloat(writer, state.hitPositionX);
  writeFloat(writer, state.hitPositionY);
  writeFloat(writer, state.hitPositionZ);
  writeFloat(writer, state.offsetX);
  writeFloat(writer, state.offsetY);
  writeFloat(writer, state.offsetZ);
  writer.writeBits(state.weaponId, 8);
  if (writer.bitPosition != kPlayerNoTargetBulletSyncBits ||
      writer.bitPosition > std::numeric_limits<uint16_t>::max()) {
    return {};
  }
  return {std::move(writer.buffer), static_cast<uint16_t>(writer.bitPosition)};
}

bool isValidPassengerState(const PassengerState& state) {
  // Server PlayerPassengerSyncHandler explicitly rejects seat zero, then the
  // vehicle component validates its passenger-seat range for the model. The
  // six-bit wire field is bounded here before it reaches a server.
  if (state.vehicleId == 0 || state.seatId == 0 || state.seatId > 63u) return false;
  if (!isFinite(state.positionX) || !isFinite(state.positionY) || !isFinite(state.positionZ) ||
      state.positionX <= kMinWorldXy || state.positionX >= kMaxWorldXy ||
      state.positionY <= kMinWorldXy || state.positionY >= kMaxWorldXy ||
      state.positionZ <= kMinWorldZ || state.positionZ >= kMaxWorldZ) {
    return false;
  }
  // The compatible reader consumes raw legacy u8 values. Reject values beyond
  // normal player vital ranges rather than using the whole byte as a loophole.
  return state.health <= 100u && state.armour <= 100u;
}

EncodedPayload buildPlayerPassengerSync(const PassengerState& state) {
  if (!isValidPassengerState(state)) return {};

  // Source-verified against open.mp's PlayerPassengerSync::read and the NPC
  // component's client-originated generator. Unlike its broadcast writer this
  // does not include PlayerID. Do not serialize the SDK bitfield directly:
  // its allocation order is implementation-defined outside the wire contract.
  const uint16_t packedSeat = static_cast<uint16_t>(state.seatId & 0x3Fu) |
                              (state.driveBy ? 0x40u : 0u) |
                              (state.cuffed ? 0x80u : 0u) |
                              (static_cast<uint16_t>(state.weaponAdditionalKey) << 8u);
  raknet::BitWriter writer;
  writer.writeBits(kPlayerPassengerSyncPacketId, 8);
  writer.writeBits(state.vehicleId, 16);
  writer.writeBits(packedSeat, 16);
  writer.writeBits(state.health, 8);
  writer.writeBits(state.armour, 8);
  writer.writeBits(state.leftRight, 16);
  writer.writeBits(state.upDown, 16);
  writer.writeBits(state.keys, 16);
  writeFloat(writer, state.positionX);
  writeFloat(writer, state.positionY);
  writeFloat(writer, state.positionZ);

  if (writer.bitPosition != kPlayerPassengerSyncBits ||
      writer.bitPosition > std::numeric_limits<uint16_t>::max()) {
    return {};
  }
  return {std::move(writer.buffer), static_cast<uint16_t>(writer.bitPosition)};
}

}  // namespace samp::sync
