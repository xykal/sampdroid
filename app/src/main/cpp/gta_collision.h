// Bounded decoder plus floor/side-query kernel for user-owned GTA SA collision data.
//
// It reads COL2/COL3 archives selected by the player; no GTA asset is embedded
// in SampDroid. The result is renderer-agnostic and will be shared by map
// streaming, local movement, camera, and vehicle grounding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace samp::gta::col {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Bounds {
  Vec3 minimum;
  Vec3 maximum;
  Vec3 sphereCenter;
  float sphereRadius = 0.0f;
};

struct Sphere {
  Vec3 center;
  float radius = 0.0f;
  uint8_t material = 0;
};

struct Box {
  Vec3 minimum;
  Vec3 maximum;
  uint8_t material = 0;
};

struct Triangle {
  uint16_t a = 0;
  uint16_t b = 0;
  uint16_t c = 0;
  uint8_t material = 0;
  uint8_t light = 0;
};

enum class Version : uint8_t {
  Col2,
  Col3,
};

// One COL archive can contain many named collision models. Geometry is local
// to that model; later IPL/world placement supplies its world transform.
struct Model {
  Version version = Version::Col3;
  std::string name;
  Bounds bounds;
  std::vector<Sphere> spheres;
  std::vector<Box> boxes;
  std::vector<Vec3> vertices;
  std::vector<Triangle> triangles;
};

constexpr size_t kMaxModelsPerArchive = 16384;
constexpr size_t kMaxPrimitivesPerModel = 65535;
constexpr size_t kMaxTrianglesPerModel = 200000;
constexpr size_t kMaxTrianglesPerArchive = 1000000;

// Parses a concatenated COL2/COL3 archive transactionally. COL1/COL4 are not
// guessed: callers receive a clear failure rather than a silently wrong world.
bool parseArchive(const std::vector<uint8_t>& bytes, std::vector<Model>& out,
                  std::string& error, size_t maxModels = kMaxModelsPerArchive,
                  size_t maxTriangles = kMaxTrianglesPerArchive);

struct FloorHit {
  bool hit = false;
  float z = 0.0f;
  Vec3 normal{0.0f, 0.0f, 1.0f};
  uint8_t material = 0;
};

// Returns the highest local collision surface between minZ and maxZ at x/y.
// It evaluates COL primitives and triangle mesh without applying a world
// transform; map streaming will transform the query into model-local space.
bool findHighestFloor(const Model& model, float x, float y, float minZ, float maxZ,
                      FloorHit& out);

}  // namespace samp::gta::col
