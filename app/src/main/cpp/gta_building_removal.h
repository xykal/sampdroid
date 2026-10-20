// Bounded server-directed removal of static GTA IPL placements.
//
// SA-MP RPC 43 (RemoveBuildingForPlayer) removes stock world models only for
// the receiving player.  This neutral representation is shared by the RPC
// reducer, static map renderer, and COL collision queries so a removed model
// cannot remain visible or solid on Android merely because those paths use
// separate user-owned GTA data structures.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace samp::gta::removal {

// The legacy writer sends the Pawn value -1 through a uint32 field. It means
// every static model in the radius, not a model ID 4294967295.
constexpr uint32_t kAnyModel = 0xFFFFFFFFu;
// The legacy client/server path is commonly limited to about 1000 remove
// records. Retain that interoperable upper bound before any renderer or COL
// query sees the data, rather than allowing a peer to grow a session forever.
constexpr size_t kMaxDirectives = 1000u;
constexpr float kMaxCoordinate = 50000.0f;
constexpr float kMaxRadius = 10000.0f;

struct Directive {
  uint32_t modelId = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float radius = 0.0f;
};

inline bool valid(const Directive& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
         std::isfinite(value.radius) && std::fabs(value.x) <= kMaxCoordinate &&
         std::fabs(value.y) <= kMaxCoordinate && std::fabs(value.z) <= kMaxCoordinate &&
         value.radius >= 0.0f && value.radius <= kMaxRadius;
}

inline bool same(const Directive& left, const Directive& right) {
  // Values are validated finite at ingress, so exact wire-value equality is
  // deterministic and safely deduplicates retransmitted directives without
  // widening a server-selected removal radius.
  return left.modelId == right.modelId && left.x == right.x && left.y == right.y &&
         left.z == right.z && left.radius == right.radius;
}

inline bool removes(const Directive& directive, uint32_t placementModelId,
                    float placementX, float placementY, float placementZ) {
  if (!valid(directive) || !std::isfinite(placementX) || !std::isfinite(placementY) ||
      !std::isfinite(placementZ) ||
      (directive.modelId != kAnyModel && directive.modelId != placementModelId)) {
    return false;
  }
  // The client API's "within a range" applies to the placed GTA model origin.
  // Use an exact 3-D radius so an exterior directive cannot silently erase an
  // unrelated copy only sharing X/Y in a distant vertical layer.
  const double dx = static_cast<double>(placementX) - directive.x;
  const double dy = static_cast<double>(placementY) - directive.y;
  const double dz = static_cast<double>(placementZ) - directive.z;
  const double radius = directive.radius;
  return dx * dx + dy * dy + dz * dz <= radius * radius;
}

inline bool removesAny(const std::vector<Directive>* directives, uint32_t placementModelId,
                       float placementX, float placementY, float placementZ) {
  if (directives == nullptr) return false;
  // Callers receive the vector from a reducer that enforces kMaxDirectives.
  // Keep a defensive loop cap anyway because this header is also used by host
  // tests and user-data render paths.
  const size_t count = directives->size() > kMaxDirectives ? kMaxDirectives : directives->size();
  for (size_t index = 0; index < count; ++index) {
    if (removes((*directives)[index], placementModelId, placementX, placementY, placementZ)) return true;
  }
  return false;
}

}  // namespace samp::gta::removal
