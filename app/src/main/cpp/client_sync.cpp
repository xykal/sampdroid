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

bool isFinite(float value) {
  return std::isfinite(value);
}

void writeFloat(raknet::BitWriter& writer, float value) {
  uint32_t raw = 0;
  static_assert(sizeof(raw) == sizeof(value), "unexpected float size");
  std::memcpy(&raw, &value, sizeof(raw));
  writer.writeBits(raw, 32);
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

}  // namespace samp::sync
