// Independent bounded reader for GTA SA COL2/COL3 collision archives.
#include "gta_collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace samp::gta::col {
namespace {

constexpr size_t kCommonPrefixBytes = 72;
constexpr size_t kCol2HeaderBytes = 108;
constexpr size_t kCol3HeaderBytes = 120;
constexpr size_t kSphereBytes = 20;
constexpr size_t kBoxBytes = 28;
constexpr size_t kCompressedVertexBytes = 6;
constexpr size_t kFaceBytes = 8;
constexpr float kVertexScale = 1.0f / 128.0f;
constexpr float kFloorEpsilon = 0.0001f;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 |
         static_cast<uint32_t>(bytes[3]) << 24;
}

int16_t readLeI16(const uint8_t* bytes) {
  return static_cast<int16_t>(readLe16(bytes));
}

float readFloat(const uint8_t* bytes) {
  const uint32_t raw = readLe32(bytes);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(raw), "COL uses 32-bit IEEE floats");
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

Vec3 readVec3(const uint8_t* bytes) {
  return {readFloat(bytes), readFloat(bytes + 4), readFloat(bytes + 8)};
}

bool finiteVec3(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validBounds(const Bounds& bounds) {
  return finiteVec3(bounds.minimum) && finiteVec3(bounds.maximum) &&
         finiteVec3(bounds.sphereCenter) && std::isfinite(bounds.sphereRadius) &&
         bounds.sphereRadius >= 0.0f && bounds.minimum.x <= bounds.maximum.x &&
         bounds.minimum.y <= bounds.maximum.y && bounds.minimum.z <= bounds.maximum.z;
}

bool rangeInside(size_t offset, size_t count, size_t total) {
  return offset <= total && count <= total - offset;
}

bool multiplySize(size_t left, size_t right, size_t& out) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left) return false;
  out = left * right;
  return true;
}

bool resolveRelativeRange(size_t modelStart, size_t modelEnd, uint32_t relativeOffset,
                          size_t count, size_t itemBytes, size_t& absoluteOffset) {
  size_t byteCount = 0;
  if (!multiplySize(count, itemBytes, byteCount)) return false;
  const size_t base = modelStart + 4;  // COL2/COL3 offsets are after FourCC.
  if (relativeOffset == 0 || relativeOffset > modelEnd - base) return false;
  absoluteOffset = base + static_cast<size_t>(relativeOffset);
  return rangeInside(absoluteOffset, byteCount, modelEnd);
}

bool decodeName(const uint8_t* bytes, std::string& out) {
  constexpr size_t kNameBytes = 22;
  size_t length = 0;
  while (length < kNameBytes && bytes[length] != 0) ++length;
  if (length == 0) return false;
  out.clear();
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const uint8_t value = bytes[i];
    if (value < 0x21u || value > 0x7Eu) return false;
    char character = static_cast<char>(value);
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
    out.push_back(character);
  }
  return true;
}

bool zeroTail(const std::vector<uint8_t>& bytes, size_t offset) {
  for (size_t i = offset; i < bytes.size(); ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

bool parseModel(const std::vector<uint8_t>& bytes, size_t modelStart, size_t modelEnd,
                Version version, size_t maxTrianglesPerArchive, size_t& accumulatedTriangles,
                Model& out, std::string& error) {
  const uint8_t* header = bytes.data() + modelStart;
  const size_t headerBytes = version == Version::Col2 ? kCol2HeaderBytes : kCol3HeaderBytes;
  if (modelEnd - modelStart < headerBytes) {
    error = "header COL terpotong";
    return false;
  }

  Model candidate;
  candidate.version = version;
  if (!decodeName(header + 8, candidate.name)) {
    error = "nama model COL tidak valid";
    return false;
  }
  candidate.bounds.minimum = readVec3(header + 32);
  candidate.bounds.maximum = readVec3(header + 44);
  candidate.bounds.sphereCenter = readVec3(header + 56);
  candidate.bounds.sphereRadius = readFloat(header + 68);
  if (!validBounds(candidate.bounds)) {
    error = "bounds model COL tidak valid";
    return false;
  }

  const size_t sphereCount = readLe16(header + kCommonPrefixBytes);
  const size_t boxCount = readLe16(header + kCommonPrefixBytes + 2);
  const size_t faceCount = version == Version::Col2
                               ? readLe16(header + kCommonPrefixBytes + 4)
                               : readLe32(header + kCommonPrefixBytes + 4);
  if (sphereCount > kMaxPrimitivesPerModel || boxCount > kMaxPrimitivesPerModel ||
      faceCount > kMaxTrianglesPerModel || faceCount > maxTrianglesPerArchive - accumulatedTriangles) {
    error = "jumlah primitive atau triangle COL melebihi batas aman";
    return false;
  }

  const uint32_t sphereRelative = readLe32(header + 84);
  const uint32_t boxRelative = readLe32(header + 88);
  const uint32_t vertexRelative = readLe32(header + 96);
  const uint32_t faceRelative = readLe32(header + 100);

  if (sphereCount != 0) {
    size_t sphereOffset = 0;
    if (!resolveRelativeRange(modelStart, modelEnd, sphereRelative, sphereCount, kSphereBytes,
                              sphereOffset)) {
      error = "rentang sphere COL tidak valid";
      return false;
    }
    candidate.spheres.reserve(sphereCount);
    for (size_t i = 0; i < sphereCount; ++i) {
      const uint8_t* raw = bytes.data() + sphereOffset + i * kSphereBytes;
      Sphere sphere;
      sphere.center = readVec3(raw);
      sphere.radius = readFloat(raw + 12);
      sphere.material = raw[16];
      if (!finiteVec3(sphere.center) || !std::isfinite(sphere.radius) || sphere.radius < 0.0f) {
        error = "sphere COL tidak valid";
        return false;
      }
      candidate.spheres.push_back(sphere);
    }
  }

  if (boxCount != 0) {
    size_t boxOffset = 0;
    if (!resolveRelativeRange(modelStart, modelEnd, boxRelative, boxCount, kBoxBytes, boxOffset)) {
      error = "rentang box COL tidak valid";
      return false;
    }
    candidate.boxes.reserve(boxCount);
    for (size_t i = 0; i < boxCount; ++i) {
      const uint8_t* raw = bytes.data() + boxOffset + i * kBoxBytes;
      Box box;
      box.minimum = readVec3(raw);
      box.maximum = readVec3(raw + 12);
      box.material = raw[24];
      if (!finiteVec3(box.minimum) || !finiteVec3(box.maximum) ||
          box.minimum.x > box.maximum.x || box.minimum.y > box.maximum.y ||
          box.minimum.z > box.maximum.z) {
        error = "box COL tidak valid";
        return false;
      }
      candidate.boxes.push_back(box);
    }
  }

  if (faceCount != 0) {
    size_t faceOffset = 0;
    if (!resolveRelativeRange(modelStart, modelEnd, faceRelative, faceCount, kFaceBytes, faceOffset)) {
      error = "rentang face COL tidak valid";
      return false;
    }

    candidate.triangles.reserve(faceCount);
    uint16_t greatestVertex = 0;
    for (size_t i = 0; i < faceCount; ++i) {
      const uint8_t* raw = bytes.data() + faceOffset + i * kFaceBytes;
      Triangle triangle;
      triangle.a = readLe16(raw);
      triangle.b = readLe16(raw + 2);
      triangle.c = readLe16(raw + 4);
      triangle.material = raw[6];
      triangle.light = raw[7];
      greatestVertex = std::max(greatestVertex, std::max(triangle.a, std::max(triangle.b, triangle.c)));
      candidate.triangles.push_back(triangle);
    }

    const size_t vertexCount = static_cast<size_t>(greatestVertex) + 1;
    size_t vertexOffset = 0;
    if (!resolveRelativeRange(modelStart, modelEnd, vertexRelative, vertexCount,
                              kCompressedVertexBytes, vertexOffset)) {
      error = "rentang vertex COL tidak valid";
      return false;
    }
    candidate.vertices.reserve(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
      const uint8_t* raw = bytes.data() + vertexOffset + i * kCompressedVertexBytes;
      candidate.vertices.push_back({
          static_cast<float>(readLeI16(raw)) * kVertexScale,
          static_cast<float>(readLeI16(raw + 2)) * kVertexScale,
          static_cast<float>(readLeI16(raw + 4)) * kVertexScale,
      });
    }
  }

  accumulatedTriangles += candidate.triangles.size();
  out = std::move(candidate);
  return true;
}

void considerFloor(float z, Vec3 normal, uint8_t material, float minZ, float maxZ, FloorHit& hit) {
  if (!std::isfinite(z) || z < minZ - kFloorEpsilon || z > maxZ + kFloorEpsilon ||
      (hit.hit && z <= hit.z + kFloorEpsilon)) {
    return;
  }
  const float normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
  if (!std::isfinite(normalLength) || normalLength <= kFloorEpsilon) return;
  normal.x /= normalLength;
  normal.y /= normalLength;
  normal.z /= normalLength;
  if (normal.z < 0.0f) {
    normal.x = -normal.x;
    normal.y = -normal.y;
    normal.z = -normal.z;
  }
  hit.hit = true;
  hit.z = z;
  hit.normal = normal;
  hit.material = material;
}

}  // namespace

bool parseArchive(const std::vector<uint8_t>& bytes, std::vector<Model>& out,
                  std::string& error, size_t maxModels, size_t maxTriangles) {
  out.clear();
  error.clear();
  if (maxModels == 0 || maxModels > kMaxModelsPerArchive) maxModels = kMaxModelsPerArchive;
  if (maxTriangles == 0 || maxTriangles > kMaxTrianglesPerArchive) {
    maxTriangles = kMaxTrianglesPerArchive;
  }

  std::vector<Model> candidate;
  candidate.reserve(std::min(maxModels, static_cast<size_t>(64)));
  size_t cursor = 0;
  size_t accumulatedTriangles = 0;
  while (cursor < bytes.size()) {
    if (zeroTail(bytes, cursor)) break;  // IMG sector padding after the final model.
    if (!rangeInside(cursor, 8, bytes.size())) {
      error = "prefix COL terpotong";
      return false;
    }

    const uint8_t* prefix = bytes.data() + cursor;
    Version version;
    if (std::memcmp(prefix, "COL2", 4) == 0) {
      version = Version::Col2;
    } else if (std::memcmp(prefix, "COL3", 4) == 0) {
      version = Version::Col3;
    } else if (std::memcmp(prefix, "COLL", 4) == 0 || std::memcmp(prefix, "COL4", 4) == 0) {
      error = "versi COL ini belum didukung; tidak akan ditebak";
      return false;
    } else {
      error = "magic COL tidak dikenal";
      return false;
    }
    if (candidate.size() >= maxModels) {
      error = "jumlah model COL melebihi batas aman";
      return false;
    }

    const uint32_t declaredAfterSize = readLe32(prefix + 4);
    if (declaredAfterSize > bytes.size() - cursor - 8) {
      error = "ukuran model COL terpotong atau overflow";
      return false;
    }
    const size_t modelEnd = cursor + 8 + static_cast<size_t>(declaredAfterSize);
    Model model;
    if (!parseModel(bytes, cursor, modelEnd, version, maxTriangles, accumulatedTriangles, model, error)) {
      return false;
    }
    candidate.push_back(std::move(model));
    cursor = modelEnd;
  }

  if (candidate.empty()) {
    error = "archive COL tidak berisi model COL2/COL3";
    return false;
  }
  out = std::move(candidate);
  return true;
}

bool findHighestFloor(const Model& model, float x, float y, float minZ, float maxZ,
                      FloorHit& out) {
  out = {};
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(minZ) || !std::isfinite(maxZ) ||
      minZ > maxZ) {
    return false;
  }

  for (const Box& box : model.boxes) {
    if (x >= box.minimum.x - kFloorEpsilon && x <= box.maximum.x + kFloorEpsilon &&
        y >= box.minimum.y - kFloorEpsilon && y <= box.maximum.y + kFloorEpsilon) {
      considerFloor(box.maximum.z, {0.0f, 0.0f, 1.0f}, box.material, minZ, maxZ, out);
    }
  }

  for (const Sphere& sphere : model.spheres) {
    const float dx = x - sphere.center.x;
    const float dy = y - sphere.center.y;
    const float horizontalSquared = dx * dx + dy * dy;
    const float radiusSquared = sphere.radius * sphere.radius;
    if (std::isfinite(horizontalSquared) && horizontalSquared <= radiusSquared) {
      const float dz = std::sqrt(std::max(0.0f, radiusSquared - horizontalSquared));
      considerFloor(sphere.center.z + dz, {dx, dy, dz}, sphere.material, minZ, maxZ, out);
    }
  }

  for (const Triangle& triangle : model.triangles) {
    if (triangle.a >= model.vertices.size() || triangle.b >= model.vertices.size() ||
        triangle.c >= model.vertices.size()) {
      continue;
    }
    const Vec3& a = model.vertices[triangle.a];
    const Vec3& b = model.vertices[triangle.b];
    const Vec3& c = model.vertices[triangle.c];
    const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (!std::isfinite(denominator) || std::fabs(denominator) <= kFloorEpsilon) continue;
    const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / denominator;
    const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / denominator;
    const float w = 1.0f - u - v;
    if (u < -kFloorEpsilon || v < -kFloorEpsilon || w < -kFloorEpsilon) continue;

    const float z = u * a.z + v * b.z + w * c.z;
    const Vec3 edgeOne{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 edgeTwo{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 normal{
        edgeOne.y * edgeTwo.z - edgeOne.z * edgeTwo.y,
        edgeOne.z * edgeTwo.x - edgeOne.x * edgeTwo.z,
        edgeOne.x * edgeTwo.y - edgeOne.y * edgeTwo.x,
    };
    // A near-vertical mesh face is a wall, not an on-foot floor sample.
    if (std::fabs(normal.z) <= kFloorEpsilon) continue;
    considerFloor(z, normal, triangle.material, minZ, maxZ, out);
  }

  return out.hit;
}

}  // namespace samp::gta::col
