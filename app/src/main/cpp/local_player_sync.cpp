#include "local_player_sync.h"

#include <cmath>
#include <cstdint>

namespace samp::bridge {
namespace {

constexpr float kPi = 3.14159265358979323846f;

bool validSyncVital(float value) {
  // The fixed PlayerFootSync client field is a byte representing a game
  // percent. Do not silently turn an unusual scripted value into a different
  // player state; wait for the world bridge to model it correctly instead.
  return std::isfinite(value) && value >= 0.0f && value <= 100.0f;
}

uint8_t percentByte(float value) {
  return static_cast<uint8_t>(std::lround(value));
}

}  // namespace

const char* onFootPreviewStatusName(OnFootPreviewStatus status) {
  switch (status) {
    case OnFootPreviewStatus::Ready:
      return "ready";
    case OnFootPreviewStatus::MissingPosition:
      return "missing-position";
    case OnFootPreviewStatus::MissingHeading:
      return "missing-heading";
    case OnFootPreviewStatus::InvalidVitals:
      return "invalid-vitals";
    case OnFootPreviewStatus::InvalidState:
      return "invalid-state";
  }
  return "invalid-state";
}

OnFootPreview buildOnFootPreview(const state::LocalPlayer& local,
                                 const input::WireInput& controls) {
  OnFootPreview preview;
  if (!local.positionKnown) return preview;
  if (!controls.headingKnown || !std::isfinite(controls.headingDegrees)) {
    preview.status = OnFootPreviewStatus::MissingHeading;
    return preview;
  }

  const float health = local.healthKnown ? local.health : 100.0f;
  const float armour = local.armourKnown ? local.armour : 0.0f;
  if (!validSyncVital(health) || !validSyncVital(armour)) {
    preview.status = OnFootPreviewStatus::InvalidVitals;
    return preview;
  }

  sync::OnFootState next;
  // PlayerFootSync stores the signed analog fields as raw u16 bits. Conversion
  // through uint16_t deliberately preserves the legacy two's-complement wire
  // representation for left/up values.
  next.leftRight = static_cast<uint16_t>(controls.leftRight);
  next.upDown = static_cast<uint16_t>(controls.upDown);
  next.keys = controls.keys;
  next.positionX = local.x;
  next.positionY = local.y;
  next.positionZ = local.z;

  // open.mp's GTAQuat(Vector3 degrees) constructor applies negative half-angle
  // trigonometry. A pedestrian facing angle is a Z-axis rotation, hence W/Z
  // below (raw on-wire order remains W, X, Y, Z).
  const float halfRadians = -input::normalizeDegrees(controls.headingDegrees) * kPi / 360.0f;
  next.quaternionW = std::cos(halfRadians);
  next.quaternionX = 0.0f;
  next.quaternionY = 0.0f;
  next.quaternionZ = std::sin(halfRadians);

  next.health = percentByte(health);
  next.armour = percentByte(armour);
  // The reducer knows spawn weapons but not the actively selected weapon. Do
  // not guess a weapon byte until equipment/UI state owns that decision.
  next.weaponAdditionalKey = 0;
  next.specialAction = local.specialActionKnown ? local.specialAction : 0;
  if (local.velocityKnown) {
    next.velocityX = local.velocityX;
    next.velocityY = local.velocityY;
    next.velocityZ = local.velocityZ;
  }

  // No local world bridge exists yet, so surfing and animation remain neutral.
  // In particular, never derive a movement position from touch intent alone.
  if (!sync::isValidOnFootState(next)) {
    preview.status = OnFootPreviewStatus::InvalidState;
    return preview;
  }

  preview.status = OnFootPreviewStatus::Ready;
  preview.state = next;
  return preview;
}

}  // namespace samp::bridge
