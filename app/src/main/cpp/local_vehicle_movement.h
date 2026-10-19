// Conservative collision-grounded vehicle motion pilot.
//
// This owns no network or render state. It is intentionally a bounded
// kinematic fallback while the native GTA vehicle physics stack is still being
// built: it only moves a server-authorized driver over user-owned COL ground
// and refuses absent ground or blocked geometry instead of inventing a route.
#pragma once

#include "gta_collision_world.h"
#include "input_state.h"

#include <cstdint>

namespace samp::vehiclemotion {

struct LocalVehicleMotionState {
  bool initialized = false;
  bool grounded = false;
  uint16_t vehicleId = 0;
  gta::col::Vec3 position;
  float headingDegrees = 0.0f;
  // Signed forward metres per second. Positive follows the GTA/scene heading;
  // negative is bounded reverse motion.
  float speed = 0.0f;
  int32_t interior = 0;
};

enum class StepStatus : uint8_t {
  Applied,
  MissingGround,
  Blocked,
  InvalidState,
  InvalidInput,
};

struct StepResult {
  StepStatus status = StepStatus::InvalidState;
  bool moved = false;
  bool blocked = false;
  bool grounded = false;
  float speed = 0.0f;
};

// Starts/reconciles vehicle prediction only from a known streamed vehicle
// transform that the server has assigned to the local driver.
bool resetFromAuthoritative(uint16_t vehicleId, const gta::col::Vec3& position,
                            float headingDegrees, int32_t interior,
                            LocalVehicleMotionState& out);

// Advances bounded throttle/steering over the collision world. `moveY` is
// throttle/reverse, `moveX` is steering, and JUMP is a conservative brake.
// It has no authority to enter a vehicle, mutate a WorldState vehicle, or send
// a packet; its caller must still construct/validate Packet 200 separately.
StepResult advance(const gta::world::CollisionWorld& collisionWorld,
                   const input::TouchIntent& intent, float deltaSeconds,
                   LocalVehicleMotionState& state);

const char* stepStatusName(StepStatus status);

}  // namespace samp::vehiclemotion
