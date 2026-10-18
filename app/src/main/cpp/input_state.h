// Renderer-independent native touch/controller intent mapper.
//
// Android UI supplies semantic actions only. This layer owns the conversion to
// the legacy PlayerFootSync input fields; it never accepts PC keycodes, scripts,
// or executable input plugins.
#pragma once

#include <cstdint>

namespace samp::input {

// Stable Android-native action bits used across JNI and the touch pad. They are
// deliberately not the raw SA-MP key mask; OnFootInputMapper::wireInput() is
// the single audited conversion point to protocol values.
enum HeldAction : uint16_t {
  kHeldUse = 1u << 0,
  kHeldCrouch = 1u << 1,
  kHeldFire = 1u << 2,
  kHeldSprint = 1u << 3,
  kHeldSecondary = 1u << 4,
  kHeldJump = 1u << 5,
  kHeldAim = 1u << 6,
  kHeldWalk = 1u << 7,
};

constexpr uint16_t kKnownHeldActions =
    kHeldUse | kHeldCrouch | kHeldFire | kHeldSprint | kHeldSecondary | kHeldJump | kHeldAim |
    kHeldWalk;

// One bounded semantic input sample. moveY is positive when the user pushes
// forward/up on the virtual stick. lookDeltaDegrees is relative, accumulated
// into the local preview heading only; it is not an absolute camera transform.
struct TouchIntent {
  float moveX = 0.0f;
  float moveY = 0.0f;
  float lookDeltaDegrees = 0.0f;
  uint16_t heldActions = 0;
};

// Exact values that will populate the client-originated PlayerFootSync fields
// once the renderer/world loop is ready to transmit. headingDegrees is kept
// separately so the local bridge can build GTA's W/X/Y/Z quaternion safely.
struct WireInput {
  bool headingKnown = false;
  int16_t leftRight = 0;
  int16_t upDown = 0;
  uint16_t keys = 0;
  float headingDegrees = 0.0f;
};

// Validates finiteness and known action bits, then clamps untrusted Android
// numeric samples into the documented safe range.
bool sanitizeTouchIntent(const TouchIntent& input, TouchIntent& sanitized);

float normalizeDegrees(float degrees);

// Keeps held semantic state plus a locally previewed facing angle. The caller
// pushes authoritative server-facing corrections through synchronizeHeading().
class OnFootInputMapper {
public:
  bool update(const TouchIntent& input);
  void synchronizeHeading(float authoritativeHeadingDegrees);
  void reset();

  bool hasInput() const { return hasInput_; }
  bool headingKnown() const { return headingKnown_; }
  const TouchIntent& intent() const { return intent_; }
  WireInput wireInput() const;

private:
  bool hasInput_ = false;
  bool headingKnown_ = false;
  TouchIntent intent_;
  float headingDegrees_ = 0.0f;
};

}  // namespace samp::input
