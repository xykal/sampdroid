// Deterministic on-foot grounding pilot over the native collision world.
// It intentionally owns no socket and emits no SA-MP packet; a later gameplay
// bridge may consume only its collision-validated state.
#pragma once

#include "gta_collision_world.h"
#include "input_state.h"

#include <cstdint>

namespace samp::movement {

// GTA/SA-MP default gravity from PlayerInit/SetPlayerGravity. The movement pilot
// exposes its supported envelope so the bridge can stop safely instead of
// extrapolating a server physics mode it cannot model faithfully.
constexpr float kDefaultServerGravity = 0.008f;
constexpr float kMinSupportedServerGravity = 0.001f;
constexpr float kMaxSupportedServerGravity = 0.020f;

bool isSupportedServerGravity(float gravity);

struct LocalMotionState {
  bool initialized = false;
  bool grounded = false;
  gta::col::Vec3 position;
  float headingDegrees = 0.0f;
  float verticalVelocity = 0.0f;
  int32_t interior = 0;
};

enum class StepStatus : uint8_t {
  Applied,
  MissingGround,
  InvalidState,
  InvalidInput,
};

struct StepResult {
  StepStatus status = StepStatus::InvalidState;
  bool moved = false;
  bool jumped = false;
  bool grounded = false;
};

// Starts/reconciles the local simulation from an authoritative spawn/teleport.
// It does not extrapolate an unknown server position.
bool resetFromAuthoritative(const gta::col::Vec3& position, float headingDegrees, int32_t interior,
                            LocalMotionState& out);

// Advances bounded walking/running/jumping against an already-built collision
// world. Only a nearby valid floor can keep or land the player; absent collision
// returns MissingGround instead of creating free-flight coordinates. A bounded
// upright-cylinder side sweep blocks/sideslips against nearby COL primitives and
// vertical mesh faces; vehicle, weapon, animation, and network simulation stay
// separate work.
StepResult advance(const gta::world::CollisionWorld& collisionWorld, const input::TouchIntent& intent,
                   float headingDegrees, float deltaSeconds, LocalMotionState& state,
                   float serverGravity = kDefaultServerGravity,
                   const std::vector<gta::removal::Directive>* removals = nullptr);

const char* stepStatusName(StepStatus status);

}  // namespace samp::movement
