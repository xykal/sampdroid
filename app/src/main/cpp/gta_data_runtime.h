// Stateful, read-only assembly of user-selected GTA SA map data.
// This layer owns only duplicated Android SAF descriptors and parsed metadata;
// it never writes/repackages/downloads game data or invokes Windows binaries.
#pragma once

#include "gta_collision_catalog.h"
#include "gta_collision_world.h"
#include "gta_dff_mesh.h"
#include "gta_file_source.h"
#include "gta_map_manifest.h"
#include "gta_txd_texture.h"
#include "gta_vehicle_handling.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace samp::gta::runtime {

constexpr size_t kMaxMapTextBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxRuntimeDffModels = 4096;
constexpr size_t kMaxRuntimeDffMeshes = 8192;
constexpr size_t kMaxRuntimeDffVertices = 1500000;
constexpr size_t kMaxRuntimeDffIndices = 4500000;
constexpr size_t kMaxRuntimeTxdDictionaries = 2048;
constexpr size_t kMaxRuntimeTextures = 8192;
constexpr size_t kMaxRuntimeTexturePixels = 16u * 1024u * 1024u;
// `txdp` is metadata only, but bound its retained graph and each lookup chain
// independently so a modded IDE cannot turn texture fallback into unbounded
// map traversal or IMG reads.
constexpr size_t kMaxRuntimeTxdParentRelations = 4096;
constexpr size_t kMaxRuntimeTxdParentLookupDepth = 16;
constexpr size_t kMaxNearbyWorldTxdLoadsPerBatch = 4;
constexpr size_t kMaxDffPreviewVertices = 120000;
constexpr size_t kDffPreviewFloatsPerVertex = 7;
constexpr size_t kMaxDffTexturedPreviewPixels = 1024u * 1024u;
// Screen-space SA-MP sprite textdraws receive one independent copied TXD
// texture. Cap it below the generic TXD catalog ceiling so a server's text
// string cannot promote an oversized user asset into a HUD allocation.
constexpr size_t kMaxTextDrawSpritePixels = 512u * 512u;
constexpr size_t kDffTexturedPreviewFloatsPerVertex = 9;
constexpr size_t kMaxDffPreviewTextureNames = 4096;
// One GLES refresh can overlay a few distinct nearby materials while retaining
// the former one-texture aggregate RGBA budget for ARMv7 devices.
constexpr size_t kMaxNearbyWorldTexturedRenderBatches = 4;
constexpr size_t kMaxNearbyWorldTexturedTotalPixels = 1024u * 1024u;
// A copied nearby-world batch is intentionally lower than the catalog limits:
// it is refreshed from the Android UI and must stay comfortable on 32-bit RAM.
constexpr size_t kMaxNearbyWorldRenderVertices = 120000;
constexpr size_t kMaxNearbyWorldRenderInstances = 128;
constexpr size_t kMaxNearbyWorldRenderCandidates = 4096;
constexpr size_t kMaxNearbyWorldModelLoadsPerBatch = 4;
constexpr size_t kMaxServerObjectRenderInputs = 128;
constexpr size_t kMaxRuntimeImgStreamingIpls = 512;
// The primary GTA3 IMG remains the map/collision authority. A small bounded
// auxiliary set (for stock `models/cutscene.img`, for example) can supply only
// DFF/TXD member lookup without copying it into app storage.
constexpr size_t kMaxRuntimeAdditionalImgArchives = 4;
constexpr size_t kMaxRuntimeMountedImgEntries = 50000;
// Explicit full-catalog sync is opt-in from the user's SAF-selected tree. The
// cap covers stock GTA SA's 216 IMG COL chunks while keeping a malformed/modded
// archive from turning a one-time Android import into unbounded RAM/I/O.
constexpr size_t kMaxRuntimeImgCollisionMembers = 256;
constexpr uint64_t kMaxRuntimeImgCollisionBytes = 32u * 1024u * 1024u;
constexpr float kMaxNearbyWorldRenderRadius = 650.0f;

struct TexturedDffPreview {
  // Triangle-expanded x, y, z, r, g, b, a, u, v data and one matching decoded
  // RGBA dictionary texture. Both vectors are caller-owned copies.
  std::vector<float> vertices;
  std::vector<uint8_t> textureRgba;
  uint16_t textureWidth = 0;
  uint16_t textureHeight = 0;
};

// One immutable TXD level-zero copy for a server textdraw font-4 sprite. It
// contains no filesystem path and no RenderWare object; the Android HUD only
// receives dimensions plus RGBA bytes after a bounded dictionary/key lookup.
struct TexturePreview {
  std::vector<uint8_t> rgba;
  uint16_t width = 0;
  uint16_t height = 0;
};

// Triangle-expanded, model-placement-transformed x, y, z, r, g, b, a data.
// Positions remain GTA-space relative to the requested anchor; JNI/Kotlin copies
// them before GLES consumes them. One unavailable or non-portable DFF never
// invalidates an otherwise usable nearby batch.
struct NearbyWorldRenderBatch {
  std::vector<float> vertices;
  size_t consideredPlacements = 0;
  size_t renderedPlacements = 0;
  size_t renderedModels = 0;
  size_t skippedModels = 0;
};

// One safe texture/material subset overlay for the nearby color batch. GLES 2
// draws this as a separate texture because ES2 has no portable texture-array
// path; future batching may return more than one dictionary/material group.
struct NearbyWorldTexturedRenderBatch {
  std::vector<float> vertices;
  std::vector<uint8_t> textureRgba;
  uint16_t textureWidth = 0;
  uint16_t textureHeight = 0;
};

// A small ordered collection of optional texture/material overlays. Distinct
// materials within one portable DFF can occupy separate items, while repeated
// instances of one immutable texture coalesce into one item. Every item owns
// copied vertex/RGBA data; no object aliases the mounted user source or a GLES
// resource.
struct NearbyWorldTexturedRenderBatches {
  std::vector<NearbyWorldTexturedRenderBatch> batches;
};

struct ServerObjectMaterialOverrideInput {
  bool active = false;
  // Retained verbatim from the u16 source writer. 0xFFFF is API model -1:
  // color-only/no replacement texture and may carry paired empty lookup names;
  // other model values remain available for future source-model semantics
  // without retaining a Java object.
  uint16_t sourceModel = 0;
  // Source ObjectMaterial uses an ABGR wire colour. Zero preserves the DFF
  // material colour; a nonzero value replaces only the flattened DFF slot's
  // base modulation and suppresses its vertex-lighting multiplication. Names
  // are lookup keys, never paths.
  uint32_t colourAbgr = 0xFFFFFFFFu;
  std::string textureDictionary;
  std::string textureName;

  // Source Text=2 material data. It is generated into a bounded immutable RGBA
  // overlay inside the generic runtime (not an Android font object), so the
  // GLES batch remains copied/native and testable off-device. Text takes the
  // same flattened DFF slot as a Default record and has no TXD lookup.
  bool textActive = false;
  uint8_t textMaterialSize = 0;
  uint8_t textFontSize = 0;
  bool textBold = false;
  uint32_t textColourArgb = 0;
  uint32_t textBackgroundColourArgb = 0;
  uint8_t textAlignment = 0;
  std::string text;
};

// Validated dynamic-model transform passed as a copied renderer input. The
// existing JNI name is retained for compatibility, but callers may supply a
// frame-resolved server object (including a bounded attachment/MoveObject
// sample) or a streamed vehicle resolved through IDE `cars`. It does not own
// raw network state or attachment lifecycle.
struct ServerObjectRenderInput {
  uint32_t modelId = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float drawDistance = 0.0f;
  // Source SetPlayerAttachedObject supports object-local scale. Keep it as a
  // copied bounded render transform; it never changes collision or networking.
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float scaleZ = 1.0f;
  std::array<ServerObjectMaterialOverrideInput, 16> materialOverrides{};
};

struct Snapshot {
  bool imgMounted = false;
  uint64_t imgBytes = 0;
  size_t imgEntries = 0;
  size_t additionalImgArchives = 0;
  uint64_t additionalImgBytes = 0;
  size_t additionalImgEntries = 0;
  size_t collisionModels = 0;
  size_t dffModels = 0;
  size_t dffMeshes = 0;
  size_t dffVertices = 0;
  size_t txdDictionaries = 0;
  size_t txdParentRelations = 0;
  size_t textures = 0;
  size_t texturePixels = 0;
  size_t manifestIdePaths = 0;
  size_t manifestIplPaths = 0;
  size_t manifestColPaths = 0;
  size_t manifestImgPaths = 0;
  size_t ideObjects = 0;
  size_t handlingProfiles = 0;
  size_t iplPlacements = 0;
  size_t collisionInstances = 0;
  size_t missingCollisionPlacements = 0;
};

// A runtime mount is intentionally assembled in explicit pieces because SAF
// exposes user-approved files rather than a trusted filesystem path. All load
// methods are transactional: a malformed next file leaves the prior runtime
// intact. Keep it behind the JNI runtime mutex when shared across threads.
class GameDataRuntime {
public:
  void clear();
  // Clears COL/IDE/IPL-derived world assembly while preserving the mounted IMG,
  // parsed gta.dat manifest, and explicitly loaded DFF/TXD preview catalogs.
  void clearMapAssembly();
  Snapshot snapshot() const;

  // Mounts a selected IMG v2 by duplicating the caller's descriptor. Replacing
  // an IMG resets dependent COL/IDE/IPL/manifest state atomically.
  bool mountImgFileDescriptor(int fileDescriptor, std::string& error);

  // Adds a bounded set of read-only auxiliary IMG v2 descriptors without
  // replacing the primary GTA3 IMG or resetting assembled map state. DFF/TXD
  // lookup checks the primary first, then these archives in supplied order.
  // A later explicit full COL sync also scans their `.col` members under one
  // shared cap (primary entries retain duplicate-model precedence); streaming
  // parent IPL discovery remains primary-only because its manifest relation is
  // intentionally rooted in the selected GTA3 archive.
  bool mountAdditionalImgFileDescriptors(const std::vector<int>& fileDescriptors,
                                         std::string& error);

  // Loads one named `.col` member already indexed in the mounted IMG.
  bool loadImgCollisionMember(std::string_view memberName, std::string& error);

  // Explicitly discovers every bounded `.col` member in the mounted IMG,
  // parses them into one transactional catalog, and rebuilds the collision
  // world once. This makes a stock PC GTA SA root (whose gta.dat has no
  // COLFILE directives) usable without guessing paths or copying its assets.
  bool loadAllImgCollisionMembers(std::string& error);

  // Loads a selected portable `.dff` member from the mounted IMG into the
  // bounded mesh catalog, including material texture-name references.
  bool loadImgDffMember(std::string_view memberName, std::string& error);

  // Loads a selected PC TXD member from the mounted IMG into a bounded decoded
  // RGBA texture dictionary. GPU upload/batching remains a renderer stage.
  bool loadImgTxdMember(std::string_view memberName, std::string& error);

  // Copies the most recently committed portable DFF as bounded, triangle-expanded
  // GLES-ready position/RGBA vertices. This is a read-only preview bridge; it
  // does not place the model into an IPL world or mutate game state.
  bool copyLatestDffPreview(size_t maxVertices, std::vector<float>& out,
                            std::string& error) const;

  // Builds one bounded textured triangle batch when a DFF material's texture
  // name resolves unambiguously in the loaded TXD catalog. This preview does
  // not infer IPL placement or silently select an ambiguous dictionary.
  bool copyLatestTexturedDffPreview(size_t maxVertices, size_t maxTexturePixels,
                                    TexturedDffPreview& out, std::string& error) const;

  // Resolves one lowercased TXD dictionary/key pair for a font-4 SA-MP sprite
  // textdraw. Names are lookup tokens rather than paths; at most two TXD lazy
  // loads (child + parent) and a fixed copied-pixel budget are permitted per request.
  bool copyTexturePreview(std::string_view dictionaryName, std::string_view textureName,
                          size_t maxPixels, TexturePreview& out, std::string& error);

  // Finds IPL placements for the requested authoritative interior nearest a
  // GTA-space anchor, lazily decodes a bounded number of matching user-owned
  // IMG DFF members, applies each IPL transform, and returns a copied
  // untextured/prelit GLES batch. Interior defaults to exterior (0) for
  // callers that have no server-authoritative interior fact. This is a
  // map-rendering bridge, not a GTA scene simulation or asset repackager.
  bool copyNearbyWorldRenderBatch(float anchorX, float anchorY, float anchorZ,
                                  float radius, size_t maxInstances,
                                  size_t maxVertices, NearbyWorldRenderBatch& out,
                                  std::string& error, int32_t interior = 0,
                                  const std::vector<removal::Directive>* removals = nullptr);

  // Resolves a bounded copied set of static server-object or streamed-vehicle
  // model IDs through loaded IDE/IMG data, applies their Euler transforms, and
  // returns a prelit color batch. It never turns unknown IDs or
  // attached/moving state into guessed geometry.
  bool copyServerObjectRenderBatch(float anchorX, float anchorY, float anchorZ,
                                   float radius,
                                   const std::vector<ServerObjectRenderInput>& objects,
                                   size_t maxVertices, NearbyWorldRenderBatch& out,
                                   std::string& error);

  // Builds up to four texture/material overlays for frame-resolved server
  // objects, streamed vehicles, or actor skins using the same copied transforms.
  // Repeated instances of one resolved immutable texture share one overlay, so
  // the count remains a material/GLES-slot cap rather than an entity cap. A
  // bounded default object-material override may replace a flattened portable
  // DFF material slot; text materials remain deliberately excluded.
  bool copyServerObjectTexturedRenderBatches(
      float anchorX, float anchorY, float anchorZ, float radius,
      const std::vector<ServerObjectRenderInput>& objects, size_t maxBatches,
      size_t maxVertices, size_t maxTexturePixels,
      NearbyWorldTexturedRenderBatches& out, std::string& error);

  // Builds one nearest texture/material overlay from already loaded nearby DFF
  // models and their IDE TXD dictionary. It may lazily read a bounded child →
  // parent (`txdp`) TXD chain from the mounted IMG, but never retains Java/GLES
  // memory or guesses an ambiguous texture name.
  bool copyNearbyWorldTexturedRenderBatch(float anchorX, float anchorY, float anchorZ,
                                          float radius, size_t maxVertices,
                                          size_t maxTexturePixels,
                                          NearbyWorldTexturedRenderBatch& out,
                                          std::string& error, int32_t interior = 0,
                                          const std::vector<removal::Directive>* removals = nullptr);

  // Same bounded nearby-material path, but returns up to four independently
  // textured overlays ordered by nearest placement in the requested interior.
  // Placements sharing one resolved immutable texture append into one overlay.
  // Vertex and RGBA budgets are shared across the complete result; this is
  // still not full material binding.
  bool copyNearbyWorldTexturedRenderBatches(float anchorX, float anchorY, float anchorZ,
                                            float radius, size_t maxBatches,
                                            size_t maxVertices, size_t maxTexturePixels,
                                            NearbyWorldTexturedRenderBatches& out,
                                            std::string& error, int32_t interior = 0,
                                            const std::vector<removal::Directive>* removals = nullptr);

  // Parses a user-selected text gta.dat and retains only its normalized,
  // relative source manifest. The SAF tree resolver chooses/open files later.
  bool loadGtaDatFileDescriptor(int fileDescriptor, std::string& error);
  const map::AssetManifest& assetManifest() const { return assetManifest_; }

  // Loads one or more standalone GTA COLFILE descriptors transactionally.
  bool loadExternalCollisionFileDescriptor(int fileDescriptor, std::string& error);
  bool loadExternalCollisionFileDescriptors(const std::vector<int>& fileDescriptors,
                                            std::string& error);

  // Parses and merges text IDE plus text or strict SA `bnry` IPL descriptors
  // transactionally in batches. Binary INST records require a loaded IDE model
  // mapping; unsupported binary sections are never guessed.
  bool loadIdeFileDescriptor(int fileDescriptor, std::string& error);
  bool loadIdeFileDescriptors(const std::vector<int>& fileDescriptors, std::string& error);

  // Parses the user-selected stock-compatible `data/handling.cfg` separately
  // from map assembly. Its model join becomes usable only after cars IDE data
  // has also been loaded, and it never writes/copies the selected SD-card tree.
  bool loadHandlingFileDescriptor(int fileDescriptor, std::string& error);
  bool vehicleMotionTuning(uint32_t modelId,
                           vehiclemotion::VehicleMotionTuning& out) const;

  bool loadIplFileDescriptor(int fileDescriptor, std::string& error);
  bool loadIplFileDescriptors(const std::vector<int>& fileDescriptors, std::string& error);

  // Searches the mounted IMG for strict `parent_streamN.ipl` binary IPLs whose
  // parent base name appears in the already parsed gta.dat manifest. This is
  // how stock SA stores streaming placements; it never treats arbitrary IMG
  // members as map data and commits the whole discovered set transactionally.
  bool loadImgStreamingIpls(std::string& error);

  const world::CollisionWorld& collisionWorld() const { return collisionWorld_; }

private:
  bool loadImgDffMemberInternal(std::string_view memberName, bool selectLatest,
                                std::string& error);
  bool findMountedImgMember(std::string_view memberName,
                            img::FileDescriptorByteSource*& source,
                            const img::Entry*& entry);
  // Resolves one material name from the named dictionary, then only through an
  // explicitly parsed txdp parent chain. A missing/unsupported TXD is an
  // optional-rendering miss; malformed topology and cap violations fail.
  bool resolveTextureWithParents(const std::string& dictionaryName,
                                 const std::string& textureName,
                                 size_t& lazyLoads,
                                 const txd::Texture*& out,
                                 std::string& error);
  bool readDescriptorBytes(int fileDescriptor, size_t maxBytes, std::vector<uint8_t>& out,
                           std::string& error) const;
  bool readDescriptorText(int fileDescriptor, std::string& out, std::string& error) const;
  bool rebuildWorld(const world::CollisionCatalog& catalog,
                    const std::vector<map::Instance>& placements,
                    world::CollisionWorld& out, world::CollisionWorldBuildReport& report,
                    std::string& error) const;

  struct AuxiliaryImgArchive {
    std::unique_ptr<img::FileDescriptorByteSource> source;
    img::ArchiveIndex index;
  };
  std::unique_ptr<img::FileDescriptorByteSource> imageSource_;
  img::ArchiveIndex imageIndex_;
  std::vector<AuxiliaryImgArchive> auxiliaryImages_;
  // Heap ownership keeps model addresses stable when a fully validated
  // replacement catalog/world is committed atomically.
  std::unique_ptr<world::CollisionCatalog> collisionCatalog_;
  std::unordered_map<std::string, std::vector<dff::Mesh>> dffModels_;
  // A missing or unsupported member is remembered per IMG mount so a frame
  // refresh cannot repeatedly parse the same rejected user-owned asset.
  std::unordered_set<std::string> unavailableDffModels_;
  std::string latestDffModel_;
  size_t dffMeshes_ = 0;
  size_t dffVertices_ = 0;
  size_t dffIndices_ = 0;
  std::unordered_map<std::string, std::vector<txd::Texture>> txdDictionaries_;
  std::unordered_set<std::string> unavailableTxdDictionaries_;
  size_t textures_ = 0;
  size_t texturePixels_ = 0;
  map::AssetManifest assetManifest_;
  // Lowercase handling ID → parsed read-only source coefficients. It survives
  // map-source rebuilds, but clear()/IMG replacement drops it with the selected
  // GTA tree so stale user data cannot drive a later mount.
  std::unordered_map<std::string, handling::Profile> handlingProfiles_;
  std::unordered_map<uint32_t, map::IdeObject> ideObjects_;
  // Lowercase child dictionary name → explicit lowercase fallback parent.
  // Retained only from validated `txdp` IDE sections; never user-supplied paths.
  std::unordered_map<std::string, std::string> txdParentByChild_;
  std::vector<map::Instance> placements_;
  world::CollisionWorld collisionWorld_;
  world::CollisionWorldBuildReport worldReport_;
};

}  // namespace samp::gta::runtime
