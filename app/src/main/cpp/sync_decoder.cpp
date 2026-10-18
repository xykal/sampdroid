// Independent implementation based on open.mp's public
// Shared/NetCode/core.hpp PlayerFootSync::write and Shared/Network bitstream
// primitives. No upstream implementation code is linked or copied here.
#include "sync_decoder.h"

#include "raknet_codec.h"

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

}  // namespace

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

std::string summarize(const RemoteOnFootSync& sync) {
  std::ostringstream out;
  out << "PlayerFootSync #" << sync.playerId << " @ " << sync.positionX << "," << sync.positionY
      << "," << sync.positionZ;
  return out.str();
}

}  // namespace samp::syncdecode
