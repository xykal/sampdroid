// Deterministic local grounding pilot; no socket or packet serialization here.
#include "local_movement.h"

#include <algorithm>
#include <cmath>

namespace samp::movement {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxStepSeconds = 0.05f;
constexpr float kWalkSpeed = 1.8f;
constexpr float kRunSpeed = 3.6f;
constexpr float kSprintSpeed = 5.4f;
constexpr float kCrouchSpeed = 1.25f;
// This pilot's default vertical acceleration is tuned to the SA-MP default
// gravity 0.008; server gravity scales it only after an explicit safe gate.
constexpr float kBaseGravityAcceleration = 15.0f;
constexpr float kJumpVelocity = 5.5f;
constexpr float kStepUp = 0.60f;
constexpr float kMaxGroundDrop = 2.50f;
// Conservative upright capsule proxy for collision-world side walls. Floor and
// vertical motion remain queried independently so this never invents a Z move.
constexpr float kPlayerCollisionRadius = 0.36f;
constexpr float kPlayerCollisionHeight = 1.55f;
constexpr float kEpsilon = 0.0001f;
constexpr float kMaxCoordinate = 30000.0f;

bool finitePosition(const gta::col::Vec3& position) {
  return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
         std::fabs(position.x) < kMaxCoordinate && std::fabs(position.y) < kMaxCoordinate &&
         std::fabs(position.z) < kMaxCoordinate;
}

float speedFor(uint16_t actions) {
  if ((actions & input::kHeldCrouch) != 0) return kCrouchSpeed;
  if ((actions & input::kHeldWalk) != 0) return kWalkSpeed;
  if ((actions & input::kHeldSprint) != 0) return kSprintSpeed;
  return kRunSpeed;
}

bool floorNear(const gta::world::CollisionWorld& collisionWorld, float x, float y, float centerZ,
               int32_t interior, gta::col::FloorHit& floor,
               const std::vector<gta::removal::Directive>* removals) {
  return collisionWorld.findHighestFloor(x, y, centerZ - kMaxGroundDrop, centerZ + kStepUp,
                                        interior, floor, removals);
}

}  // namespace

bool isSupportedServerGravity(float gravity) {
  return std::isfinite(gravity) && gravity >= kMinSupportedServerGravity &&
         gravity <= kMaxSupportedServerGravity;
}

const char* stepStatusName(StepStatus status) {
  switch (status) {
    case StepStatus::Applied:
      return "applied";
    case StepStatus::MissingGround:
      return "missing-ground";
    case StepStatus::InvalidState:
      return "invalid-state";
    case StepStatus::InvalidInput:
      return "invalid-input";
  }
  return "invalid-state";
}

bool resetFromAuthoritative(const gta::col::Vec3& position, float headingDegrees, int32_t interior,
                            LocalMotionState& out) {
  if (!finitePosition(position) || !std::isfinite(headingDegrees)) return false;
  out = {};
  out.initialized = true;
  out.position = position;
  out.headingDegrees = input::normalizeDegrees(headingDegrees);
  out.interior = interior;
  return true;
}

StepResult advance(const gta::world::CollisionWorld& collisionWorld, const input::TouchIntent& intent,
                   float headingDegrees, float deltaSeconds, LocalMotionState& state,
                   float serverGravity, const std::vector<gta::removal::Directive>* removals) {
  StepResult result;
  if (!state.initialized || !finitePosition(state.position) || !std::isfinite(state.headingDegrees) ||
      !std::isfinite(state.verticalVelocity) || !isSupportedServerGravity(serverGravity)) {
    result.status = StepStatus::InvalidState;
    return result;
  }
  input::TouchIntent sanitized;
  if (!input::sanitizeTouchIntent(intent, sanitized) || !std::isfinite(headingDegrees) ||
      !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
    result.status = StepStatus::InvalidInput;
    return result;
  }

  const float stepSeconds = std::min(deltaSeconds, kMaxStepSeconds);
  const float gravityAcceleration = kBaseGravityAcceleration *
                                    (serverGravity / kDefaultServerGravity);
  if (!std::isfinite(gravityAcceleration) || gravityAcceleration <= 0.0f) {
    result.status = StepStatus::InvalidState;
    return result;
  }
  LocalMotionState next = state;
  next.headingDegrees = input::normalizeDegrees(headingDegrees);

  gta::col::FloorHit currentFloor;
  const bool hasCurrentFloor = floorNear(collisionWorld, next.position.x, next.position.y,
                                         next.position.z, next.interior, currentFloor, removals);
  if (next.grounded || std::fabs(next.verticalVelocity) <= kEpsilon) {
    if (!hasCurrentFloor) {
      result.status = StepStatus::MissingGround;
      return result;
    }
    next.position.z = currentFloor.z;
    next.verticalVelocity = 0.0f;
    next.grounded = true;
  }

  const bool jumping = (sanitized.heldActions & input::kHeldJump) != 0 && next.grounded;
  if (jumping) {
    next.verticalVelocity = kJumpVelocity;
    next.grounded = false;
    result.jumped = true;
  }

  float moveX = sanitized.moveX;
  float moveY = sanitized.moveY;
  const float inputLength = std::sqrt(moveX * moveX + moveY * moveY);
  if (inputLength > 1.0f) {
    moveX /= inputLength;
    moveY /= inputLength;
  }
  const float headingRadians = next.headingDegrees * kPi / 180.0f;
  // GTA-facing convention used by the existing OnFoot bridge: heading zero is
  // positive Y; positive heading turns toward positive X.
  const float forwardX = std::sin(headingRadians);
  const float forwardY = std::cos(headingRadians);
  const float rightX = std::cos(headingRadians);
  const float rightY = -std::sin(headingRadians);
  const float distance = speedFor(sanitized.heldActions) * stepSeconds;
  float targetX = next.position.x + (rightX * moveX + forwardX * moveY) * distance;
  float targetY = next.position.y + (rightY * moveX + forwardY * moveY) * distance;
  const gta::world::HorizontalMoveResult horizontal = collisionWorld.resolveHorizontalMove(
      next.position.x, next.position.y, targetX, targetY, next.position.z, kPlayerCollisionRadius,
      kPlayerCollisionHeight, next.interior, removals);
  if (!horizontal.valid) {
    result.status = StepStatus::InvalidState;
    return result;
  }
  targetX = horizontal.position.x;
  targetY = horizontal.position.y;

  if (next.grounded) {
    gta::col::FloorHit targetFloor;
    if (!floorNear(collisionWorld, targetX, targetY, next.position.z, next.interior, targetFloor,
                   removals)) {
      // Do not turn an absent map/collision member into client-authored free
      // coordinates. Streaming must load a valid floor before movement resumes.
      result.status = StepStatus::MissingGround;
      return result;
    }
    next.position.x = targetX;
    next.position.y = targetY;
    next.position.z = targetFloor.z;
  } else {
    next.position.x = targetX;
    next.position.y = targetY;
    const float previousZ = next.position.z;
    next.verticalVelocity -= gravityAcceleration * stepSeconds;
    const float targetZ = previousZ + next.verticalVelocity * stepSeconds;
    gta::col::FloorHit landingFloor;
    const bool hasLandingFloor = floorNear(collisionWorld, targetX, targetY,
                                           std::max(previousZ, targetZ), next.interior, landingFloor,
                                           removals);
    if (next.verticalVelocity <= 0.0f && hasLandingFloor &&
        landingFloor.z <= previousZ + kStepUp && landingFloor.z >= targetZ - kEpsilon) {
      next.position.z = landingFloor.z;
      next.verticalVelocity = 0.0f;
      next.grounded = true;
    } else if (hasLandingFloor) {
      next.position.z = targetZ;
    } else {
      // A collision world that has no support within the bounded look-down
      // range is incomplete. Preserve the authoritative/safe state instead of
      // allowing unrestricted local falling or syncing fabricated positions.
      result.status = StepStatus::MissingGround;
      return result;
    }
  }

  if (!finitePosition(next.position)) {
    result.status = StepStatus::InvalidState;
    return result;
  }
  result.moved = std::fabs(next.position.x - state.position.x) > kEpsilon ||
                 std::fabs(next.position.y - state.position.y) > kEpsilon ||
                 std::fabs(next.position.z - state.position.z) > kEpsilon ||
                 std::fabs(next.headingDegrees - state.headingDegrees) > kEpsilon;
  state = next;
  result.status = StepStatus::Applied;
  result.grounded = state.grounded;
  return result;
}

}  // namespace samp::movement
