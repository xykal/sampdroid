// Minimal bounded RenderWare DFF geometry reader for user-owned GTA SA models.
// It decodes portable CPU vertex/triangle/material references plus bounded
// FrameList/Atomic static placement transforms; skins, native console geometry,
// animation, and GPU asset distribution remain separate stages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace samp::gta::dff {

struct Vec2 {
  float u = 0.0f;
  float v = 0.0f;
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Rgba {
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;
};

struct Material {
  // Diffuse/modulation color from RpMaterial. `textureName` is lowercased and
  // empty for an untextured material; it is a lookup key, never a filesystem path.
  Rgba color;
  std::string textureName;
};

struct Mesh {
  std::vector<Vec3> vertices;
  std::vector<uint32_t> indices;
  // One entry per triangle, matching the DFF material field. Empty only for
  // legacy/minimal geometry that did not carry a Material List.
  std::vector<uint16_t> triangleMaterials;
  std::vector<Material> materials;
  // Present only when the DFF carries portable prelit/UV data.
  std::vector<Rgba> colors;
  std::vector<Vec2> texCoords;
};

constexpr size_t kMaxMeshes = 2048;
constexpr size_t kMaxVerticesPerMesh = 200000;
constexpr size_t kMaxTrianglesPerMesh = 400000;
constexpr size_t kMaxVerticesTotal = 1000000;
constexpr size_t kMaxTrianglesTotal = 2000000;

// Parses portable RpGeometry chunks inside an SA RenderWare DFF/geometry-list
// transactionally. Native/platform geometry, malformed hierarchy, unbounded
// counts, unsupported morph targets, or invalid indices fail closed.
bool parsePortableMeshes(const std::vector<uint8_t>& bytes, std::vector<Mesh>& out,
                         std::string& error, size_t maxMeshes = kMaxMeshes,
                         size_t maxVertices = kMaxVerticesTotal,
                         size_t maxTriangles = kMaxTrianglesTotal);

}  // namespace samp::gta::dff
