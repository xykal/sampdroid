// Bounded readers for user-owned GTA SA world manifests (gta.dat/IDE/text+binary IPL).
// They only turn already-selected data into neutral descriptors; no asset is
// bundled, copied, executed, or downloaded by SampDroid.
#pragma once

#include "gta_collision.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace samp::gta::map {

constexpr size_t kMaxManifestPaths = 4096;
constexpr size_t kMaxIdeObjects = 50000;
constexpr size_t kMaxIplInstances = 200000;

struct AssetManifest {
  std::vector<std::string> idePaths;
  std::vector<std::string> iplPaths;
  std::vector<std::string> colPaths;
  std::vector<std::string> imgPaths;
};

// Parses a textual GTA SA gta.dat manifest. Paths are normalized to lowercase
// forward-slash relative paths; traversal/absolute paths are rejected.
bool parseGtaDat(std::string_view text, AssetManifest& out, std::string& error,
                 size_t maxPaths = kMaxManifestPaths);

struct IdeObject {
  uint32_t modelId = 0;
  // Lowercase RenderWare/IMG base name shared with the matching COL model.
  std::string modelName;
  std::string textureDictionary;
  // Normalized source `cars` handling ID. It is empty for static objects and
  // peds, and merely joins an already-selected user-owned handling.cfg profile
  // for the conservative local-driver bridge.
  std::string handlingName;
  float drawDistance = 0.0f;
  uint32_t flags = 0;
};

// A `txdp` row is a bounded dictionary fallback relationship: if a material is
// absent from `child`, the renderer may search `parent`. It is never a path or
// an asset-loading directive on its own.
struct TxdParentRelation {
  std::string child;
  std::string parent;
};

struct IdeParseResult {
  std::vector<IdeObject> objects;
  std::vector<TxdParentRelation> txdParents;
};

// Reads GTA SA placement-capable `objs`, `tobj`, and `anim` IDE definitions,
// plus `cars` and `peds` records needed to resolve streamed SA-MP vehicle/player
// model IDs to user-owned DFF/TXD members. Multiple historical OBJS/TOBJ
// mesh-count layouts are validated explicitly; dynamic records retain only
// ID/model/TXD/type mapping plus the `cars` handling lookup token. `txdp`
// relationships are parsed separately for a
// later bounded texture fallback lookup. Other sections are ignored until their
// explicit `end`; duplicate IDs or conflicting txdp children fail closed.
bool parseIdeFile(std::string_view text, IdeParseResult& out, std::string& error,
                  size_t maxObjects = kMaxIdeObjects);

// Compatibility wrapper for consumers interested only in model definitions.
bool parseIdeObjects(std::string_view text, std::vector<IdeObject>& out, std::string& error,
                     size_t maxObjects = kMaxIdeObjects);

struct Quaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct Instance {
  uint32_t modelId = 0;
  std::string modelName;
  int32_t interior = 0;
  col::Vec3 position;
  Quaternion rotation;
  int32_t lodIndex = -1;
};

// Reads the text `inst` section of an IPL. Placement is retained for later map
// streaming/collision transforms; it does not move the local player or emit
// network sync.
bool parseIplInstances(std::string_view text, std::vector<Instance>& out, std::string& error,
                       size_t maxInstances = kMaxIplInstances);

// Strictly reads the San Andreas binary IPL `bnry` header and its 40-byte INST
// records. Binary records identify a model only by ID, so their `modelName`
// stays empty until GameDataRuntime resolves it through an already loaded IDE.
// Unknown binary sections are never interpreted. Malformed offsets, counts,
// flags, floats, or quaternions fail closed.
bool parseBinaryIplInstances(const std::vector<uint8_t>& bytes, std::vector<Instance>& out,
                             std::string& error, size_t maxInstances = kMaxIplInstances);

}  // namespace samp::gta::map
