// Bounded bridge from a user-owned IMG member to parsed local COL models.
// The catalog is deliberately renderer- and filesystem-agnostic: Android SAF
// supplies ByteSource, and a later spatial streamer decides which .col member
// to request near the player.
#pragma once

#include "gta_collision.h"
#include "gta_img_archive.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace samp::gta::world {

constexpr size_t kMaxCollisionModelsInCatalog = 50000;

class CollisionCatalog {
public:
  const col::Model* find(std::string_view modelName) const;
  size_t modelCount() const { return models_.size(); }
  bool empty() const { return models_.empty(); }
  void clear() { models_.clear(); }

private:
  friend bool loadCollisionArchiveBytes(const std::vector<uint8_t>&, CollisionCatalog&, std::string&,
                                        size_t);
  friend bool loadCollisionImgMember(img::ByteSource&, const img::ArchiveIndex&, std::string_view,
                                     CollisionCatalog&, std::string&, size_t, size_t);
  std::unordered_map<std::string, col::Model> models_;
};

// Merges one already-bounded COL2/COL3 archive into the catalog. A later
// explicitly loaded archive deterministically replaces an earlier model of the
// same name; duplicate names within one archive fail closed. This also supports
// GTA `COLFILE` entries outside an IMG.
bool loadCollisionArchiveBytes(const std::vector<uint8_t>& bytes, CollisionCatalog& out,
                               std::string& error,
                               size_t maxCatalogModels = kMaxCollisionModelsInCatalog);

// Reads exactly one `.col` member from an indexed IMG and parses its COL2/COL3
// models under the existing per-entry extraction limits. No unrelated IMG data
// is read.
bool loadCollisionImgMember(img::ByteSource& source, const img::ArchiveIndex& index,
                            std::string_view memberName, CollisionCatalog& out,
                            std::string& error,
                            size_t maxExtractBytes = img::kMaxExtractBytes,
                            size_t maxCatalogModels = kMaxCollisionModelsInCatalog);

}  // namespace samp::gta::world
