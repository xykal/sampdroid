// Bounded IMG-to-COL catalog bridge for user-owned GTA SA data.
#include "gta_collision_catalog.h"

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace samp::gta::world {
namespace {

bool normalizeModelName(std::string_view raw, std::string& out) {
  if (raw.empty() || raw.size() > 22) return false;
  out.clear();
  out.reserve(raw.size());
  for (const unsigned char byte : raw) {
    if (byte < 0x21u || byte > 0x7Eu || byte == '/' || byte == '\\' || byte == ':') {
      out.clear();
      return false;
    }
    char character = static_cast<char>(byte);
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    out.push_back(character);
  }
  return true;
}

bool isColMemberName(std::string_view name) {
  return name.size() >= 4 && name.compare(name.size() - 4, 4, ".col") == 0;
}

}  // namespace

const col::Model* CollisionCatalog::find(std::string_view modelName) const {
  std::string normalized;
  if (!normalizeModelName(modelName, normalized)) return nullptr;
  const auto it = models_.find(normalized);
  return it == models_.end() ? nullptr : &it->second;
}

bool loadCollisionArchiveBytes(const std::vector<uint8_t>& bytes, CollisionCatalog& out,
                               std::string& error, size_t maxCatalogModels) {
  error.clear();
  if (maxCatalogModels == 0 || maxCatalogModels > kMaxCollisionModelsInCatalog) {
    maxCatalogModels = kMaxCollisionModelsInCatalog;
  }

  std::vector<col::Model> parsed;
  if (!col::parseArchive(bytes, parsed, error)) return false;

  size_t newModels = 0;
  std::unordered_set<std::string> namesInArchive;
  namesInArchive.reserve(parsed.size());
  for (const col::Model& model : parsed) {
    if (!namesInArchive.insert(model.name).second) {
      error = "nama model collision duplikat dalam archive";
      return false;
    }
    if (out.models_.find(model.name) == out.models_.end()) ++newModels;
  }
  if (out.models_.size() > maxCatalogModels ||
      newModels > maxCatalogModels - out.models_.size()) {
    error = "catalog collision melebihi batas model aman";
    return false;
  }

  // All failure-prone parsing/validation finished above. Apply this archive as
  // one deterministic commit; a later explicit source may replace the same
  // model name, matching ordered source selection without ambiguous state.
  out.models_.reserve(out.models_.size() + newModels);
  for (col::Model& model : parsed) {
    const auto existing = out.models_.find(model.name);
    if (existing == out.models_.end()) {
      std::string key = model.name;
      out.models_.emplace(std::move(key), std::move(model));
    } else {
      existing->second = std::move(model);
    }
  }
  return true;
}

bool loadCollisionImgMember(img::ByteSource& source, const img::ArchiveIndex& index,
                            std::string_view memberName, CollisionCatalog& out,
                            std::string& error, size_t maxExtractBytes,
                            size_t maxCatalogModels) {
  error.clear();
  if (maxExtractBytes == 0 || maxExtractBytes > img::kMaxExtractBytes) {
    maxExtractBytes = img::kMaxExtractBytes;
  }
  const img::Entry* entry = index.find(memberName);
  if (!entry) {
    error = "member collision IMG tidak ditemukan";
    return false;
  }
  if (!isColMemberName(entry->name)) {
    error = "member IMG yang dipilih bukan file .col";
    return false;
  }

  std::vector<uint8_t> bytes;
  if (!img::readEntry(source, *entry, maxExtractBytes, bytes, error)) return false;
  return loadCollisionArchiveBytes(bytes, out, error, maxCatalogModels);
}

}  // namespace samp::gta::world
