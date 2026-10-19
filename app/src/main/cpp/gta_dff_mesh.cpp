// Bounded portable RenderWare DFF geometry reader.
#include "gta_dff_mesh.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace samp::gta::dff {
namespace {

constexpr uint32_t kRwStruct = 0x01;
constexpr uint32_t kRwString = 0x02;
constexpr uint32_t kRwTexture = 0x06;
constexpr uint32_t kRwMaterial = 0x07;
constexpr uint32_t kRwMaterialList = 0x08;
constexpr uint32_t kRwGeometry = 0x0F;
constexpr uint32_t kRwClump = 0x10;
constexpr uint32_t kRwFrameList = 0x0E;
constexpr uint32_t kRwAtomic = 0x14;
constexpr uint32_t kRwGeometryList = 0x1A;
constexpr uint32_t kGeometryPositions = 0x00000002u;
constexpr uint32_t kGeometryTextured = 0x00000004u;
constexpr uint32_t kGeometryPrelit = 0x00000008u;
constexpr uint32_t kGeometryTextured2 = 0x00000080u;
constexpr uint32_t kGeometryNative = 0x01000000u;
constexpr size_t kChunkHeaderBytes = 12;
constexpr size_t kGeometryHeaderBytes = 16;
constexpr size_t kLegacySurfaceBytes = 12;
constexpr size_t kMorphHeaderBytes = 24;  // sphere + hasVertices + hasNormals
constexpr size_t kTriangleBytes = 8;
constexpr size_t kVec2Bytes = 8;
constexpr size_t kVec3Bytes = 12;
constexpr size_t kMaxMaterialsPerMesh = 4096;
constexpr size_t kMaxTextureNameBytes = 64;
constexpr size_t kMaxFramesPerClump = 4096;
constexpr size_t kFrameRecordBytes = 56;
constexpr size_t kAtomicStructBytes = 16;

struct Chunk {
  uint32_t type = 0;
  size_t payloadOffset = 0;
  size_t payloadEnd = 0;
  uint32_t version = 0;
};

// RenderWare frame matrices encode the local right/up/at basis as columns,
// followed by local translation, parent frame index, and flags. Only the
// transform necessary to place portable static geometry is retained; skeleton
// animation/skin plugins remain outside this reader.
struct FrameTransform {
  Vec3 right{1.0f, 0.0f, 0.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
  Vec3 at{0.0f, 0.0f, 1.0f};
  Vec3 position{};
  int32_t parent = -1;
};

struct AtomicBinding {
  uint32_t frameIndex = 0;
  uint32_t geometryIndex = 0;
};

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

float readFloat(const uint8_t* bytes) {
  const uint32_t raw = readLe32(bytes);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(raw), "RenderWare portable floats are 32-bit");
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

bool rangeInside(size_t offset, size_t count, size_t total) {
  return offset <= total && count <= total - offset;
}

bool multiplySize(size_t left, size_t right, size_t& out) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left) return false;
  out = left * right;
  return true;
}

bool readChunk(const std::vector<uint8_t>& bytes, size_t& cursor, size_t end, Chunk& out) {
  if (!rangeInside(cursor, kChunkHeaderBytes, end)) return false;
  const uint8_t* header = bytes.data() + cursor;
  const uint32_t length = readLe32(header + 4);
  const size_t payloadOffset = cursor + kChunkHeaderBytes;
  if (!rangeInside(payloadOffset, static_cast<size_t>(length), end)) return false;
  out.type = readLe32(header);
  out.payloadOffset = payloadOffset;
  out.payloadEnd = payloadOffset + static_cast<size_t>(length);
  out.version = readLe32(header + 8);
  cursor = out.payloadEnd;
  return true;
}

// IMG v2 members are sector-aligned. A valid DFF can therefore be followed by
// zero-fill up to the sector boundary; treat only an all-zero *top-level*
// suffix as padding, never a malformed chunk embedded in a RenderWare parent.
bool zeroFilledSuffix(const std::vector<uint8_t>& bytes, size_t offset) {
  return offset <= bytes.size() &&
         std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end(),
                     [](uint8_t value) { return value == 0; });
}

bool finite(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

int32_t decodeSigned32(uint32_t raw) {
  if (raw <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
    return static_cast<int32_t>(raw);
  }
  if (raw == 0x80000000u) return std::numeric_limits<int32_t>::min();
  return -static_cast<int32_t>((~raw) + 1u);
}

Vec3 transformVector(const FrameTransform& transform, const Vec3& value) {
  return {
      transform.right.x * value.x + transform.up.x * value.y + transform.at.x * value.z,
      transform.right.y * value.x + transform.up.y * value.y + transform.at.y * value.z,
      transform.right.z * value.x + transform.up.z * value.y + transform.at.z * value.z,
  };
}

Vec3 transformPoint(const FrameTransform& transform, const Vec3& value) {
  Vec3 result = transformVector(transform, value);
  result.x += transform.position.x;
  result.y += transform.position.y;
  result.z += transform.position.z;
  return result;
}

bool normalizeTextureName(const std::vector<uint8_t>& bytes, const Chunk& stringChunk,
                          std::string& out) {
  const size_t size = stringChunk.payloadEnd - stringChunk.payloadOffset;
  if (size == 0 || size > kMaxTextureNameBytes + 1) return false;
  const uint8_t* raw = bytes.data() + stringChunk.payloadOffset;
  size_t length = 0;
  while (length < size && raw[length] != 0) ++length;
  if (length == 0 || length == size || length > kMaxTextureNameBytes) return false;
  out.clear();
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const unsigned char ch = raw[i];
    // A texture reference is a dictionary key, never a path. Stock GTA SA
    // dictionaries legitimately use literal spaces (for example "new road"),
    // so retain printable ASCII from space onward while refusing separators
    // and controls before lowercasing deterministically.
    if (ch < 0x20 || ch > 0x7E || ch == '/' || ch == '\\' || ch == ':') return false;
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return true;
}

bool parseTexture(const std::vector<uint8_t>& bytes, const Chunk& texture, std::string& name) {
  size_t cursor = texture.payloadOffset;
  bool structFound = false;
  bool nameFound = false;
  Chunk child;
  while (cursor < texture.payloadEnd) {
    if (!readChunk(bytes, cursor, texture.payloadEnd, child)) return false;
    if (child.type == kRwStruct) {
      if (structFound || child.payloadEnd - child.payloadOffset < 4) return false;
      structFound = true;
    } else if (child.type == kRwString && !nameFound) {
      if (!normalizeTextureName(bytes, child, name)) return false;
      nameFound = true;
    }
  }
  return structFound && nameFound;
}

bool parseMaterial(const std::vector<uint8_t>& bytes, const Chunk& material, Material& out) {
  size_t cursor = material.payloadOffset;
  bool structFound = false;
  bool textureFound = false;
  bool textured = false;
  Material candidate;
  Chunk child;
  while (cursor < material.payloadEnd) {
    if (!readChunk(bytes, cursor, material.payloadEnd, child)) return false;
    if (child.type == kRwStruct) {
      if (structFound || child.payloadEnd - child.payloadOffset < 16) return false;
      const uint8_t* raw = bytes.data() + child.payloadOffset;
      candidate.color = {raw[4], raw[5], raw[6], raw[7]};
      textured = readLe32(raw + 12) != 0;
      structFound = true;
    } else if (child.type == kRwTexture) {
      if (textureFound || !parseTexture(bytes, child, candidate.textureName)) return false;
      textureFound = true;
    }
  }
  if (!structFound || textured != textureFound) return false;
  out = std::move(candidate);
  return true;
}

bool parseMaterialList(const std::vector<uint8_t>& bytes, const Chunk& materialList,
                       std::vector<Material>& out) {
  size_t cursor = materialList.payloadOffset;
  Chunk header;
  if (!readChunk(bytes, cursor, materialList.payloadEnd, header) || header.type != kRwStruct ||
      header.payloadEnd - header.payloadOffset < 4) {
    return false;
  }
  const size_t count = readLe32(bytes.data() + header.payloadOffset);
  size_t indexBytes = 0;
  if (count == 0 || count > kMaxMaterialsPerMesh || !multiplySize(count, 4, indexBytes) ||
      !rangeInside(header.payloadOffset + 4, indexBytes, header.payloadEnd)) {
    return false;
  }

  std::vector<int32_t> references;
  references.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t rawReference = readLe32(bytes.data() + header.payloadOffset + 4 + i * 4);
    if (rawReference == 0xFFFFFFFFu) {
      references.push_back(-1);
    } else if (rawReference <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
      references.push_back(static_cast<int32_t>(rawReference));
    } else {
      return false;
    }
  }
  std::vector<Material> candidate;
  candidate.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const int32_t reference = references[i];
    if (reference >= 0) {
      if (static_cast<size_t>(reference) >= i) return false;
      candidate.push_back(candidate[static_cast<size_t>(reference)]);
      continue;
    }
    if (reference != -1) return false;
    Chunk material;
    if (!readChunk(bytes, cursor, materialList.payloadEnd, material) || material.type != kRwMaterial) {
      return false;
    }
    Material parsed;
    if (!parseMaterial(bytes, material, parsed)) return false;
    candidate.push_back(std::move(parsed));
  }
  // Material-list extensions are not interpreted yet, but their chunk bounds
  // still must be structurally valid rather than becoming an unchecked tail.
  while (cursor < materialList.payloadEnd) {
    if (!readChunk(bytes, cursor, materialList.payloadEnd, header)) return false;
  }
  out = std::move(candidate);
  return true;
}

bool parseGeometryStruct(const std::vector<uint8_t>& bytes, size_t begin, size_t end,
                         size_t surfaceBytes, Mesh& out) {
  if (!rangeInside(begin, kGeometryHeaderBytes + surfaceBytes, end)) return false;
  size_t cursor = begin;
  const uint8_t* header = bytes.data() + cursor;
  const uint32_t format = readLe32(header);
  const size_t triangleCount = readLe32(header + 4);
  const size_t vertexCount = readLe32(header + 8);
  const uint32_t morphTargets = readLe32(header + 12);
  cursor += kGeometryHeaderBytes + surfaceBytes;

  if ((format & kGeometryNative) != 0 || (format & kGeometryPositions) == 0 ||
      morphTargets != 1 || vertexCount == 0 ||
      vertexCount > kMaxVerticesPerMesh || triangleCount > kMaxTrianglesPerMesh) {
    return false;
  }
  size_t texSetCount = (format >> 16u) & 0xFFu;
  if (texSetCount == 0) {
    if ((format & kGeometryTextured2) != 0) {
      texSetCount = 2;
    } else if ((format & kGeometryTextured) != 0) {
      texSetCount = 1;
    }
  }
  if (texSetCount > 8) return false;

  Mesh candidate;
  candidate.vertices.reserve(vertexCount);
  candidate.indices.reserve(triangleCount * 3u);
  candidate.triangleMaterials.reserve(triangleCount);
  if ((format & kGeometryPrelit) != 0) {
    size_t colorBytes = 0;
    if (!multiplySize(vertexCount, 4, colorBytes) || !rangeInside(cursor, colorBytes, end)) return false;
    candidate.colors.reserve(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
      const uint8_t* raw = bytes.data() + cursor + i * 4;
      candidate.colors.push_back({raw[0], raw[1], raw[2], raw[3]});
    }
    cursor += colorBytes;
  }
  for (size_t set = 0; set < texSetCount; ++set) {
    size_t texBytes = 0;
    if (!multiplySize(vertexCount, kVec2Bytes, texBytes) || !rangeInside(cursor, texBytes, end)) {
      return false;
    }
    if (set == 0) {
      candidate.texCoords.reserve(vertexCount);
      for (size_t i = 0; i < vertexCount; ++i) {
        const uint8_t* raw = bytes.data() + cursor + i * kVec2Bytes;
      Vec2 coordinate{readFloat(raw), readFloat(raw + 4)};
      // A handful of stock PC DFFs contain NaN/Inf UVs even though their
      // vertex positions and triangle indices are otherwise portable. Never
      // expose those values to GLES: canonicalize only the optional UV to the
      // texture origin, while positions/normals still fail closed below.
      if (!std::isfinite(coordinate.u) || !std::isfinite(coordinate.v)) coordinate = {};
      candidate.texCoords.push_back(coordinate);
      }
    }
    cursor += texBytes;
  }

  // `rpGEOMETRYTRISTRIP` may be set by stock GTA SA models to describe the
  // renderer's BinMesh extension. The serialized RpGeometry still carries this
  // bounded canonical RpTriangle array, so consume those triangle records
  // rather than rejecting an otherwise portable CPU geometry. The extension is
  // never used as an unchecked alternate index stream.
  size_t triangleBytes = 0;
  if (!multiplySize(triangleCount, kTriangleBytes, triangleBytes) ||
      !rangeInside(cursor, triangleBytes, end)) {
    return false;
  }
  for (size_t i = 0; i < triangleCount; ++i) {
    const uint8_t* raw = bytes.data() + cursor + i * kTriangleBytes;
    const uint16_t vertex2 = readLe16(raw);
    const uint16_t vertex1 = readLe16(raw + 2);
    const uint16_t material = readLe16(raw + 4);
    const uint16_t vertex3 = readLe16(raw + 6);
    if (vertex1 >= vertexCount || vertex2 >= vertexCount || vertex3 >= vertexCount) return false;
    // The on-disk order is v2, v1, material, v3. Preserve a consistent list
    // winding for GLES and retain the material slot for later texture batching.
    candidate.indices.push_back(vertex1);
    candidate.indices.push_back(vertex2);
    candidate.indices.push_back(vertex3);
    candidate.triangleMaterials.push_back(material);
  }
  cursor += triangleBytes;

  if (!rangeInside(cursor, kMorphHeaderBytes, end)) return false;
  // Bounding sphere is consumed for layout validation; renderer bounds will be
  // derived from finite decoded vertices to avoid trusting an unused field.
  const uint32_t hasVertices = readLe32(bytes.data() + cursor + 16);
  const uint32_t hasNormals = readLe32(bytes.data() + cursor + 20);
  if (hasVertices != 1 || hasNormals > 1) return false;
  cursor += kMorphHeaderBytes;
  size_t vertexBytes = 0;
  if (!multiplySize(vertexCount, kVec3Bytes, vertexBytes) || !rangeInside(cursor, vertexBytes, end)) {
    return false;
  }
  for (size_t i = 0; i < vertexCount; ++i) {
    const uint8_t* raw = bytes.data() + cursor + i * kVec3Bytes;
    const Vec3 vertex{readFloat(raw), readFloat(raw + 4), readFloat(raw + 8)};
    if (!finite(vertex)) return false;
    candidate.vertices.push_back(vertex);
  }
  cursor += vertexBytes;
  if (hasNormals != 0) {
    size_t normalBytes = 0;
    if (!multiplySize(vertexCount, kVec3Bytes, normalBytes) || !rangeInside(cursor, normalBytes, end)) {
      return false;
    }
    // Normals are currently not exposed, but require finite values so a bad
    // source cannot hide malformed geometry behind an ignored stream.
    for (size_t i = 0; i < vertexCount; ++i) {
      const uint8_t* raw = bytes.data() + cursor + i * kVec3Bytes;
      if (!finite({readFloat(raw), readFloat(raw + 4), readFloat(raw + 8)})) return false;
    }
    cursor += normalBytes;
  }
  if (cursor != end) return false;
  out = std::move(candidate);
  return true;
}

bool parseGeometry(const std::vector<uint8_t>& bytes, const Chunk& geometry, Mesh& out) {
  size_t cursor = geometry.payloadOffset;
  bool geometryStructFound = false;
  bool materialListFound = false;
  Mesh candidate;
  Chunk child;
  while (cursor < geometry.payloadEnd) {
    if (!readChunk(bytes, cursor, geometry.payloadEnd, child)) return false;
    if (child.type == kRwStruct) {
      if (geometryStructFound) return false;
      // SA DFF uses the 16-byte header. Older layouts carry three surface floats
      // after it; accept that layout only when the full bounded decode succeeds.
      if (!parseGeometryStruct(bytes, child.payloadOffset, child.payloadEnd, 0, candidate) &&
          !parseGeometryStruct(bytes, child.payloadOffset, child.payloadEnd, kLegacySurfaceBytes,
                               candidate)) {
        return false;
      }
      geometryStructFound = true;
    } else if (child.type == kRwMaterialList) {
      if (!geometryStructFound || materialListFound ||
          !parseMaterialList(bytes, child, candidate.materials)) {
        return false;
      }
      materialListFound = true;
    }
  }
  if (!geometryStructFound) return false;
  if (materialListFound) {
    for (const uint16_t material : candidate.triangleMaterials) {
      if (material >= candidate.materials.size()) return false;
    }
  } else {
    // Keep a legacy/minimal test DFF readable, but never pretend that its
    // on-disk material slot resolves to a real dictionary material.
    candidate.triangleMaterials.clear();
  }
  out = std::move(candidate);
  return true;
}

bool parseGeometryList(const std::vector<uint8_t>& bytes, const Chunk& geometryList,
                       std::vector<Mesh>& meshes, size_t maxMeshes, size_t maxVertices,
                       size_t maxTriangles, size_t& totalVertices, size_t& totalTriangles) {
  size_t cursor = geometryList.payloadOffset;
  Chunk child;
  if (!readChunk(bytes, cursor, geometryList.payloadEnd, child) || child.type != kRwStruct ||
      child.payloadEnd - child.payloadOffset < 4) {
    return false;
  }
  const size_t declaredGeometries = readLe32(bytes.data() + child.payloadOffset);
  if (declaredGeometries == 0 || declaredGeometries > maxMeshes) return false;

  size_t parsedGeometries = 0;
  while (cursor < geometryList.payloadEnd) {
    if (!readChunk(bytes, cursor, geometryList.payloadEnd, child)) return false;
    if (child.type != kRwGeometry) continue;
    if (meshes.size() >= maxMeshes) return false;
    Mesh mesh;
    if (!parseGeometry(bytes, child, mesh) || mesh.vertices.size() > maxVertices - totalVertices ||
        mesh.indices.size() / 3u > maxTriangles - totalTriangles) {
      return false;
    }
    totalVertices += mesh.vertices.size();
    totalTriangles += mesh.indices.size() / 3u;
    meshes.push_back(std::move(mesh));
    ++parsedGeometries;
  }
  return parsedGeometries == declaredGeometries;
}

bool parseFrameList(const std::vector<uint8_t>& bytes, const Chunk& frameList,
                    std::vector<FrameTransform>& out) {
  out.clear();
  size_t cursor = frameList.payloadOffset;
  Chunk child;
  if (!readChunk(bytes, cursor, frameList.payloadEnd, child) || child.type != kRwStruct ||
      child.payloadEnd - child.payloadOffset < 4) {
    return false;
  }
  const size_t frameCount = readLe32(bytes.data() + child.payloadOffset);
  size_t frameBytes = 0;
  if (frameCount == 0 || frameCount > kMaxFramesPerClump ||
      !multiplySize(frameCount, kFrameRecordBytes, frameBytes) ||
      child.payloadEnd - child.payloadOffset != 4u + frameBytes) {
    return false;
  }

  std::vector<FrameTransform> frames;
  frames.reserve(frameCount);
  size_t record = child.payloadOffset + 4u;
  for (size_t index = 0; index < frameCount; ++index, record += kFrameRecordBytes) {
    const uint8_t* raw = bytes.data() + record;
    FrameTransform frame;
    frame.right = {readFloat(raw), readFloat(raw + 4), readFloat(raw + 8)};
    frame.up = {readFloat(raw + 12), readFloat(raw + 16), readFloat(raw + 20)};
    frame.at = {readFloat(raw + 24), readFloat(raw + 28), readFloat(raw + 32)};
    frame.position = {readFloat(raw + 36), readFloat(raw + 40), readFloat(raw + 44)};
    frame.parent = decodeSigned32(readLe32(raw + 48));
    if (!finite(frame.right) || !finite(frame.up) || !finite(frame.at) || !finite(frame.position) ||
        frame.parent < -1 || frame.parent >= static_cast<int32_t>(frameCount)) {
      return false;
    }
    frames.push_back(frame);
  }
  // Frame extensions (HAnim/name/etc.) are deliberately opaque, but every
  // chunk still has to be bounded before geometry can use the frame matrix.
  while (cursor < frameList.payloadEnd) {
    if (!readChunk(bytes, cursor, frameList.payloadEnd, child)) return false;
  }

  // Resolve arbitrary parent order without recursive traversal. A cycle or an
  // invalid ancestor is rejected rather than applying a guessed transform.
  std::vector<uint8_t> state(frameCount, 0u);
  std::vector<size_t> chain;
  chain.reserve(frameCount);
  for (size_t start = 0; start < frameCount; ++start) {
    if (state[start] == 2u) continue;
    chain.clear();
    size_t current = start;
    while (state[current] != 2u) {
      if (state[current] == 1u) return false;
      state[current] = 1u;
      chain.push_back(current);
      const int32_t parent = frames[current].parent;
      if (parent < 0) break;
      current = static_cast<size_t>(parent);
    }
    while (!chain.empty()) {
      const size_t index = chain.back();
      chain.pop_back();
      const int32_t parent = frames[index].parent;
      if (parent >= 0) {
        const FrameTransform parentWorld = frames[static_cast<size_t>(parent)];
        FrameTransform& local = frames[index];
        local.right = transformVector(parentWorld, local.right);
        local.up = transformVector(parentWorld, local.up);
        local.at = transformVector(parentWorld, local.at);
        local.position = transformPoint(parentWorld, local.position);
        if (!finite(local.right) || !finite(local.up) || !finite(local.at) || !finite(local.position)) {
          return false;
        }
      }
      state[index] = 2u;
    }
  }
  out = std::move(frames);
  return true;
}

bool parseAtomicBinding(const std::vector<uint8_t>& bytes, const Chunk& atomic,
                        AtomicBinding& out) {
  size_t cursor = atomic.payloadOffset;
  bool structFound = false;
  AtomicBinding candidate;
  Chunk child;
  while (cursor < atomic.payloadEnd) {
    if (!readChunk(bytes, cursor, atomic.payloadEnd, child)) return false;
    if (child.type != kRwStruct) continue;
    if (structFound || child.payloadEnd - child.payloadOffset < kAtomicStructBytes) return false;
    const uint8_t* raw = bytes.data() + child.payloadOffset;
    candidate.frameIndex = readLe32(raw);
    candidate.geometryIndex = readLe32(raw + 4);
    structFound = true;
  }
  if (!structFound) return false;
  out = candidate;
  return true;
}

bool appendTransformedMesh(const Mesh& source, const FrameTransform* transform,
                           std::vector<Mesh>& meshes, size_t maxMeshes, size_t maxVertices,
                           size_t maxTriangles, size_t& totalVertices, size_t& totalTriangles) {
  const size_t triangles = source.indices.size() / 3u;
  if (source.indices.empty() || source.indices.size() % 3u != 0u || meshes.size() >= maxMeshes ||
      source.vertices.size() > maxVertices - totalVertices ||
      triangles > maxTriangles - totalTriangles) {
    return false;
  }
  Mesh candidate = source;
  if (transform) {
    for (Vec3& vertex : candidate.vertices) {
      vertex = transformPoint(*transform, vertex);
      if (!finite(vertex)) return false;
    }
  }
  totalVertices += candidate.vertices.size();
  totalTriangles += triangles;
  meshes.push_back(std::move(candidate));
  return true;
}

bool parseClump(const std::vector<uint8_t>& bytes, const Chunk& clump, std::vector<Mesh>& meshes,
                size_t maxMeshes, size_t maxVertices, size_t maxTriangles, size_t& totalVertices,
                size_t& totalTriangles) {
  size_t cursor = clump.payloadOffset;
  Chunk child;
  bool geometryListFound = false;
  bool frameListFound = false;
  std::vector<Mesh> sourceMeshes;
  std::vector<FrameTransform> frames;
  std::vector<AtomicBinding> atomics;
  while (cursor < clump.payloadEnd) {
    if (!readChunk(bytes, cursor, clump.payloadEnd, child)) return false;
    if (child.type == kRwGeometryList) {
      if (geometryListFound) return false;
      size_t sourceVertices = 0;
      size_t sourceTriangles = 0;
      if (!parseGeometryList(bytes, child, sourceMeshes, maxMeshes, maxVertices, maxTriangles,
                             sourceVertices, sourceTriangles)) {
        return false;
      }
      geometryListFound = true;
    } else if (child.type == kRwFrameList) {
      if (frameListFound || !parseFrameList(bytes, child, frames)) return false;
      frameListFound = true;
    } else if (child.type == kRwAtomic) {
      if (atomics.size() >= maxMeshes) return false;
      AtomicBinding binding;
      if (!parseAtomicBinding(bytes, child, binding)) return false;
      atomics.push_back(binding);
    }
  }
  if (!geometryListFound || sourceMeshes.empty()) return false;

  if (atomics.empty()) {
    for (const Mesh& mesh : sourceMeshes) {
      if (!appendTransformedMesh(mesh, nullptr, meshes, maxMeshes, maxVertices, maxTriangles,
                                 totalVertices, totalTriangles)) {
        return false;
      }
    }
    return true;
  }
  if (!frameListFound || frames.empty()) return false;
  for (const AtomicBinding& binding : atomics) {
    if (binding.frameIndex >= frames.size() || binding.geometryIndex >= sourceMeshes.size() ||
        !appendTransformedMesh(sourceMeshes[binding.geometryIndex], &frames[binding.frameIndex], meshes,
                               maxMeshes, maxVertices, maxTriangles, totalVertices, totalTriangles)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool parsePortableMeshes(const std::vector<uint8_t>& bytes, std::vector<Mesh>& out,
                         std::string& error, size_t maxMeshes, size_t maxVertices,
                         size_t maxTriangles) {
  out.clear();
  error.clear();
  if (maxMeshes == 0 || maxMeshes > kMaxMeshes) maxMeshes = kMaxMeshes;
  if (maxVertices == 0 || maxVertices > kMaxVerticesTotal) maxVertices = kMaxVerticesTotal;
  if (maxTriangles == 0 || maxTriangles > kMaxTrianglesTotal) maxTriangles = kMaxTrianglesTotal;
  if (bytes.size() < kChunkHeaderBytes) {
    error = "DFF terlalu pendek untuk header RenderWare";
    return false;
  }

  std::vector<Mesh> candidate;
  size_t totalVertices = 0;
  size_t totalTriangles = 0;
  size_t cursor = 0;
  Chunk top;
  while (cursor < bytes.size()) {
    // Avoid repeatedly scanning a crafted zero-chunk stream: a zero byte is
    // legal here only when every remaining byte is IMG sector padding.
    if (bytes[cursor] == 0u &&
        (bytes.size() - cursor < 4u || readLe32(bytes.data() + cursor) == 0u)) {
      if (zeroFilledSuffix(bytes, cursor)) break;
      error = "chunk nol non-padding setelah stream RenderWare DFF";
      return false;
    }
    if (!readChunk(bytes, cursor, bytes.size(), top)) {
      error = "chunk RenderWare DFF terpotong atau melampaui batas";
      return false;
    }
    bool accepted = false;
    if (top.type == kRwClump) {
      accepted = parseClump(bytes, top, candidate, maxMeshes, maxVertices, maxTriangles,
                            totalVertices, totalTriangles);
    } else if (top.type == kRwGeometryList) {
      accepted = parseGeometryList(bytes, top, candidate, maxMeshes, maxVertices, maxTriangles,
                                   totalVertices, totalTriangles);
    }
    if ((top.type == kRwClump || top.type == kRwGeometryList) && !accepted) {
      error = "hierarki atau geometry portable DFF tidak didukung";
      return false;
    }
  }
  if (candidate.empty()) {
    error = "DFF tidak memiliki mesh portable yang didukung";
    return false;
  }
  out = std::move(candidate);
  return true;
}

}  // namespace samp::gta::dff
