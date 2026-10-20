// Spatially indexed, read-only collision placements for the native GTA world.
// It consumes parsed IDE/IPL-style placements and selected COL models; it does
// not perform unrestricted asset I/O or synthesize network movement.
#pragma once

#include "gta_building_removal.h"
#include "gta_collision_catalog.h"
#include "gta_map_manifest.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace samp::gta::world {

constexpr size_t kMaxCollisionWorldInstances = 200000;
constexpr size_t kMaxSpatialReferences = 1000000;

struct CollisionWorldBuildReport {
  size_t consideredPlacements = 0;
  size_t attachedPlacements = 0;
  size_t missingCollisionModels = 0;
  size_t largePlacements = 0;
};

// Result of a bounded upright-player horizontal sweep. `position` is feet
// position in GTA world coordinates; a blocked sweep may slide along one axis
// but never invents vertical motion.
struct HorizontalMoveResult {
  bool valid = false;
  bool blocked = false;
  col::Vec3 position;
};

// Result of a bounded static-world ray query. It never resolves players,
// streamed vehicles, server objects, or damage. `valid` means the fixed query
// work budget completed; `hit` is meaningful only when `valid` is true.
struct RaycastHit {
  bool valid = false;
  bool hit = false;
  float distance = 0.0f;
  col::Vec3 position;
  col::Vec3 normal{0.0f, 0.0f, 1.0f};
  uint8_t material = 0;
};

// A lightweight immutable world index. It stores pointers into the catalog;
// retain the catalog unchanged for this world's lifetime, then rebuild the
// world after changing/reloading collision members.
class CollisionWorld {
public:
  CollisionWorld() = default;

  size_t instanceCount() const { return instances_.size(); }
  bool empty() const { return instances_.empty(); }

  // Finds the highest surface for a vertical world-space query. Placement
  // quaternion, box, sphere, and mesh collision are all transformed into world
  // space. `interior` matches IPL interior exactly.
  bool findHighestFloor(float x, float y, float minZ, float maxZ, int32_t interior,
                        col::FloorHit& out,
                        const std::vector<removal::Directive>* removals = nullptr) const;

  // Resolves an upright player cylinder against nearby COL boxes, spheres, and
  // sufficiently vertical mesh faces. The target is clamped/slid horizontally;
  // callers retain authoritative floor/gravity handling separately.
  HorizontalMoveResult resolveHorizontalMove(float startX, float startY, float targetX,
                                             float targetY, float feetZ, float radius,
                                             float height, int32_t interior,
                                             const std::vector<removal::Directive>* removals = nullptr) const;

  // Casts a normalized-or-normalizable ray through parsed COL boxes, spheres,
  // and mesh faces for one exact IPL interior. Max distance is deliberately
  // capped (256 GTA units) and all candidate/primitive work is budgeted. The
  // result is static-data geometry only; callers must never treat it as a
  // network target/hit/damage authority.
  bool raycastStaticWorld(const col::Vec3& origin, const col::Vec3& direction,
                          float maxDistance, int32_t interior, RaycastHit& out,
                          const std::vector<removal::Directive>* removals = nullptr) const;

private:
  struct Instance {
    const col::Model* model = nullptr;
    uint32_t modelId = 0;
    col::Vec3 position;
    map::Quaternion rotation;
    int32_t interior = 0;
    col::Vec3 worldMinimum;
    col::Vec3 worldMaximum;
  };
  friend bool buildCollisionWorld(const CollisionCatalog&, const std::vector<map::Instance>&,
                                  CollisionWorld&, CollisionWorldBuildReport&, std::string&, size_t,
                                  size_t);

  std::vector<Instance> instances_;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
  std::vector<uint32_t> largeInstances_;
};

// Creates a new index transactionally from IPL placements whose model collision
// is currently available in `catalog`. Missing models are reported/skipped so a
// streaming loader can add their COL member later; malformed transforms or cap
// overflow fail rather than silently building a wrong index.
bool buildCollisionWorld(const CollisionCatalog& catalog, const std::vector<map::Instance>& placements,
                         CollisionWorld& out, CollisionWorldBuildReport& report, std::string& error,
                         size_t maxInstances = kMaxCollisionWorldInstances,
                         size_t maxSpatialReferences = kMaxSpatialReferences);

}  // namespace samp::gta::world
