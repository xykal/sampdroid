#include "input_state.h"

#include <algorithm>
#include <cmath>

namespace samp::input {
namespace {

constexpr float kMaxAxis = 1.0f;
constexpr float kAxisDeadZone = 0.08f;
constexpr float kMaxLookDeltaDegrees = 90.0f;
constexpr float kMaxAimPitchDegrees = 75.0f;
constexpr float kProtocolAxisMagnitude = 128.0f;

// open.mp SDK/include/values.hpp Key enum. These are the legacy on-foot Keys
// bits, not the private HeldAction enum exposed by the Android control pad.
constexpr uint16_t kKeyAction = 1;
constexpr uint16_t kKeyCrouch = 2;
constexpr uint16_t kKeyFire = 4;
constexpr uint16_t kKeySprint = 8;
constexpr uint16_t kKeySecondaryAttack = 16;
constexpr uint16_t kKeyJump = 32;
constexpr uint16_t kKeyAim = 128;
constexpr uint16_t kKeyWalk = 1024;

int16_t quantizeAxis(float value) {
  if (std::fabs(value) < kAxisDeadZone) return 0;
  const long rounded = std::lround(std::clamp(value, -kMaxAxis, kMaxAxis) *
                                   kProtocolAxisMagnitude);
  return static_cast<int16_t>(std::clamp(rounded, -128l, 128l));
}

}  // namespace

float normalizeDegrees(float degrees) {
  if (!std::isfinite(degrees)) return 0.0f;
  float normalized = std::fmod(degrees, 360.0f);
  if (normalized < 0.0f) normalized += 360.0f;
  return normalized;
}

bool sanitizeTouchIntent(const TouchIntent& input, TouchIntent& sanitized) {
  if (!std::isfinite(input.moveX) || !std::isfinite(input.moveY) ||
      !std::isfinite(input.lookDeltaDegrees) || !std::isfinite(input.lookPitchDeltaDegrees) ||
      (input.heldActions & ~kKnownHeldActions) != 0) {
    return false;
  }

  sanitized.moveX = std::clamp(input.moveX, -kMaxAxis, kMaxAxis);
  sanitized.moveY = std::clamp(input.moveY, -kMaxAxis, kMaxAxis);
  sanitized.lookDeltaDegrees =
      std::clamp(input.lookDeltaDegrees, -kMaxLookDeltaDegrees, kMaxLookDeltaDegrees);
  sanitized.lookPitchDeltaDegrees =
      std::clamp(input.lookPitchDeltaDegrees, -kMaxLookDeltaDegrees, kMaxLookDeltaDegrees);
  sanitized.heldActions = input.heldActions;
  return true;
}

bool OnFootInputMapper::update(const TouchIntent& input) {
  TouchIntent sanitized;
  if (!sanitizeTouchIntent(input, sanitized)) return false;
  intent_ = sanitized;
  hasInput_ = true;
  if (headingKnown_) {
    headingDegrees_ = normalizeDegrees(headingDegrees_ + intent_.lookDeltaDegrees);
  }
  // Pitch has no server-side heading equivalent in this limited native bridge.
  // Preserve it locally for camera/AimSync only, bounded far below an inverted
  // vertical camera; it never alters the on-foot movement quaternion.
  aimPitchDegrees_ = std::clamp(aimPitchDegrees_ + intent_.lookPitchDeltaDegrees,
                                -kMaxAimPitchDegrees, kMaxAimPitchDegrees);
  return true;
}

void OnFootInputMapper::synchronizeHeading(float authoritativeHeadingDegrees) {
  if (!std::isfinite(authoritativeHeadingDegrees)) return;
  headingDegrees_ = normalizeDegrees(authoritativeHeadingDegrees);
  headingKnown_ = true;
}

void OnFootInputMapper::reset() {
  hasInput_ = false;
  headingKnown_ = false;
  intent_ = {};
  headingDegrees_ = 0.0f;
  aimPitchDegrees_ = 0.0f;
}

WireInput OnFootInputMapper::wireInput() const {
  WireInput output;
  output.headingKnown = headingKnown_;
  output.headingDegrees = headingDegrees_;
  output.aimPitchDegrees = aimPitchDegrees_;
  output.leftRight = quantizeAxis(intent_.moveX);
  // SA-MP/open.mp uses negative signed up/down values for forward input
  // (SDK Key::UP = -128, Key::DOWN = 128).
  output.upDown = quantizeAxis(-intent_.moveY);

  uint16_t keys = 0;
  if ((intent_.heldActions & kHeldUse) != 0) keys |= kKeyAction;
  if ((intent_.heldActions & kHeldCrouch) != 0) keys |= kKeyCrouch;
  if ((intent_.heldActions & kHeldFire) != 0) keys |= kKeyFire;
  if ((intent_.heldActions & kHeldSprint) != 0) keys |= kKeySprint;
  if ((intent_.heldActions & kHeldSecondary) != 0) keys |= kKeySecondaryAttack;
  if ((intent_.heldActions & kHeldJump) != 0) keys |= kKeyJump;
  if ((intent_.heldActions & kHeldAim) != 0) keys |= kKeyAim;
  if ((intent_.heldActions & kHeldWalk) != 0) keys |= kKeyWalk;
  output.keys = keys;
  return output;
}

}  // namespace samp::input
