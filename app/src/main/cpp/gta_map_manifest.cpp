// Independent, bounded readers for GTA SA text manifests and strict binary IPL.
#include "gta_map_manifest.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace samp::gta::map {
namespace {

constexpr size_t kMaxLineBytes = 1024;
constexpr size_t kMaxCsvFields = 16;
constexpr size_t kBinaryIplHeaderBytes = 76;
constexpr size_t kBinaryIplInstanceBytes = 40;
constexpr float kMinWorldXy = -20000.0f;
constexpr float kMaxWorldXy = 20000.0f;
constexpr float kMinWorldZ = -1000.0f;
constexpr float kMaxWorldZ = 200000.0f;

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

std::string_view stripComment(std::string_view value) {
  const size_t comment = value.find('#');
  return trim(comment == std::string_view::npos ? value : value.substr(0, comment));
}

bool nextLine(std::string_view& input, std::string_view& line) {
  if (input.empty()) return false;
  const size_t end = input.find('\n');
  line = end == std::string_view::npos ? input : input.substr(0, end);
  input = end == std::string_view::npos ? std::string_view{} : input.substr(end + 1);
  line = stripComment(line);
  return true;
}

std::string lowerAscii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const unsigned char byte : value) {
    char character = static_cast<char>(byte);
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    out.push_back(character);
  }
  return out;
}

std::string_view firstWord(std::string_view& value) {
  value = trim(value);
  size_t length = 0;
  while (length < value.size() && value[length] != ' ' && value[length] != '\t') ++length;
  const std::string_view word = value.substr(0, length);
  value.remove_prefix(length);
  value = trim(value);
  return word;
}

bool parseUnsigned(std::string_view text, uint32_t& out) {
  text = trim(text);
  if (text.empty()) return false;
  uint64_t value = 0;
  for (const unsigned char byte : text) {
    if (byte < '0' || byte > '9') return false;
    value = value * 10u + static_cast<uint64_t>(byte - '0');
    if (value > std::numeric_limits<uint32_t>::max()) return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

bool parseSigned(std::string_view text, int32_t& out) {
  text = trim(text);
  if (text.empty()) return false;
  bool negative = false;
  if (text.front() == '-' || text.front() == '+') {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  uint32_t magnitude = 0;
  if (!parseUnsigned(text, magnitude)) return false;
  if ((!negative && magnitude > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) ||
      (negative && magnitude > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1u)) {
    return false;
  }
  if (negative && magnitude == static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1u) {
    out = std::numeric_limits<int32_t>::min();
  } else {
    out = negative ? -static_cast<int32_t>(magnitude) : static_cast<int32_t>(magnitude);
  }
  return true;
}

bool parseFloat(std::string_view text, float& out) {
  text = trim(text);
  if (text.empty() || text.size() > 64) return false;
  std::string copy(text);
  char* end = nullptr;
  const float value = std::strtof(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return false;
  out = value;
  return true;
}

bool normalizeRelativePath(std::string_view raw, std::string& out) {
  raw = trim(raw);
  if (raw.empty() || raw.size() > 260 || raw.front() == '/' || raw.front() == '\\') return false;
  out.clear();
  out.reserve(raw.size());
  std::string component;
  const auto flushComponent = [&]() {
    if (component.empty() || component == "." || component == "..") return false;
    if (!out.empty()) out.push_back('/');
    out += component;
    component.clear();
    return true;
  };
  for (const unsigned char byte : raw) {
    if (byte == '/' || byte == '\\') {
      if (!flushComponent()) {
        out.clear();
        return false;
      }
      continue;
    }
    if (byte < 0x21u || byte > 0x7Eu || byte == ':') {
      out.clear();
      return false;
    }
    char character = static_cast<char>(byte);
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    component.push_back(character);
  }
  if (!flushComponent()) {
    out.clear();
    return false;
  }
  return true;
}

bool normalizeModelName(std::string_view raw, std::string& out) {
  raw = trim(raw);
  if (raw.empty() || raw.size() > 32) return false;
  out.clear();
  out.reserve(raw.size());
  for (const unsigned char byte : raw) {
    const bool alpha = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
    const bool digit = byte >= '0' && byte <= '9';
    // Stock GTA SA uses a small handful of punctuation characters in actual
    // DFF/TXD identifiers (for example `kb_planter+bush` and
    // `CJ_FIREBIN_(L0)`). They are still plain bounded names, never paths:
    // slashes, backslashes, dots, whitespace, controls, and every other
    // character remain rejected.
    const bool stockPunctuation = byte == '_' || byte == '-' || byte == '+' ||
                                  byte == '(' || byte == ')';
    if (!alpha && !digit && !stockPunctuation) {
      out.clear();
      return false;
    }
    char character = static_cast<char>(byte);
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    out.push_back(character);
  }
  return true;
}

bool splitCsv(std::string_view line, std::vector<std::string_view>& fields) {
  fields.clear();
  while (true) {
    if (fields.size() >= kMaxCsvFields) return false;
    const size_t comma = line.find(',');
    const std::string_view field = trim(comma == std::string_view::npos ? line : line.substr(0, comma));
    if (field.empty()) return false;
    fields.push_back(field);
    if (comma == std::string_view::npos) return true;
    line.remove_prefix(comma + 1);
  }
}

// `cars`/`peds` carry a large, version-specific tail that this renderer never
// interprets. Read exactly the safe resolver prefix, so a harmless empty or
// legacy field later in a stock vehicle line cannot reject the entire table.
bool splitCsvPrefix(std::string_view line, size_t requiredFields,
                    std::vector<std::string_view>& fields) {
  fields.clear();
  if (requiredFields == 0 || requiredFields > kMaxCsvFields) return false;
  while (fields.size() < requiredFields) {
    const size_t comma = line.find(',');
    const std::string_view field = trim(comma == std::string_view::npos ? line : line.substr(0, comma));
    if (field.empty()) return false;
    fields.push_back(field);
    if (fields.size() == requiredFields) return true;
    if (comma == std::string_view::npos) return false;
    line.remove_prefix(comma + 1);
  }
  return true;
}

// A few stock vehicles.ide lines accidentally merge identical DFF/TXD names
// with whitespace instead of a comma (e.g. `emperor emperor`). Accept only
// that exact duplicate-token repair. Any other extra token remains rejected.
bool normalizeDynamicModelName(std::string_view raw, std::string& out,
                               bool& duplicatedDictionaryName) {
  duplicatedDictionaryName = false;
  const std::string_view modelToken = firstWord(raw);
  if (!normalizeModelName(modelToken, out)) return false;
  if (raw.empty()) return true;
  std::string duplicate;
  const std::string_view duplicateToken = firstWord(raw);
  if (!normalizeModelName(duplicateToken, duplicate) || !raw.empty() || duplicate != out) {
    out.clear();
    return false;
  }
  duplicatedDictionaryName = true;
  return true;
}

bool validPosition(const col::Vec3& position) {
  return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
         position.x > kMinWorldXy && position.x < kMaxWorldXy &&
         position.y > kMinWorldXy && position.y < kMaxWorldXy &&
         position.z > kMinWorldZ && position.z < kMaxWorldZ;
}

bool normalizeQuaternion(Quaternion& rotation) {
  const float squared = rotation.x * rotation.x + rotation.y * rotation.y +
                        rotation.z * rotation.z + rotation.w * rotation.w;
  if (!std::isfinite(squared) || squared < 0.25f || squared > 2.25f) return false;
  const float inverseLength = 1.0f / std::sqrt(squared);
  rotation.x *= inverseLength;
  rotation.y *= inverseLength;
  rotation.z *= inverseLength;
  rotation.w *= inverseLength;
  return std::isfinite(rotation.x) && std::isfinite(rotation.y) &&
         std::isfinite(rotation.z) && std::isfinite(rotation.w);
}

bool readLe32(const std::vector<uint8_t>& bytes, size_t offset, uint32_t& out) {
  if (offset > bytes.size() || bytes.size() - offset < 4u) return false;
  out = static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
  return true;
}

bool readLeFloat(const std::vector<uint8_t>& bytes, size_t offset, float& out) {
  uint32_t raw = 0;
  if (!readLe32(bytes, offset, raw)) return false;
  static_assert(sizeof(raw) == sizeof(out), "unexpected float size");
  std::memcpy(&out, &raw, sizeof(out));
  return std::isfinite(out);
}

template <typename Collection>
bool appendUnique(Collection& collection, std::string&& value, size_t maxPaths) {
  for (const std::string& existing : collection) {
    if (existing == value) return true;
  }
  if (collection.size() >= maxPaths) return false;
  collection.push_back(std::move(value));
  return true;
}

size_t totalPathCount(const AssetManifest& manifest) {
  return manifest.idePaths.size() + manifest.iplPaths.size() + manifest.colPaths.size() +
         manifest.imgPaths.size();
}

}  // namespace

bool parseGtaDat(std::string_view text, AssetManifest& out, std::string& error, size_t maxPaths) {
  out = {};
  error.clear();
  if (maxPaths == 0 || maxPaths > kMaxManifestPaths) maxPaths = kMaxManifestPaths;

  AssetManifest candidate;
  std::string_view line;
  while (nextLine(text, line)) {
    if (line.empty()) continue;
    if (line.size() > kMaxLineBytes) {
      error = "baris gta.dat melebihi batas aman";
      return false;
    }
    std::string_view remainder = line;
    const std::string key = lowerAscii(firstWord(remainder));
    std::vector<std::string>* target = nullptr;
    if (key == "ide") {
      target = &candidate.idePaths;
    } else if (key == "ipl") {
      target = &candidate.iplPaths;
    } else if (key == "img") {
      target = &candidate.imgPaths;
    } else if (key == "colfile") {
      uint32_t zone = 0;
      const std::string_view zoneToken = firstWord(remainder);
      if (!parseUnsigned(zoneToken, zone) || zone > 3) {
        error = "zone COLFILE gta.dat tidak valid";
        return false;
      }
      target = &candidate.colPaths;
    } else {
      continue;  // Other gta.dat directives are outside this map-data stage.
    }

    std::string path;
    if (!normalizeRelativePath(remainder, path)) {
      error = "path gta.dat tidak aman atau tidak valid";
      return false;
    }
    if (!appendUnique(*target, std::move(path), maxPaths) || totalPathCount(candidate) > maxPaths) {
      error = "jumlah path gta.dat melebihi batas aman";
      return false;
    }
  }

  if (totalPathCount(candidate) == 0) {
    error = "gta.dat tidak memuat IDE/IPL/COLFILE/IMG yang didukung";
    return false;
  }
  out = std::move(candidate);
  return true;
}

bool parseIdeFile(std::string_view text, IdeParseResult& out, std::string& error,
                  size_t maxObjects) {
  out = {};
  error.clear();
  if (maxObjects == 0 || maxObjects > kMaxIdeObjects) maxObjects = kMaxIdeObjects;

  enum class Section {
    None,
    Objects,
    TimedObjects,
    AnimatedObjects,
    Vehicles,
    Pedestrians,
    TextureParents,
    Other,
  };
  Section section = Section::None;
  // `txdp` is a real stock-SA dictionary-parent table. Keep its bounded name
  // relationships so the runtime can attempt one safe parent texture lookup;
  // a table-only file remains valid and contributes no placement/model itself.
  bool sawTextureParents = false;
  std::vector<IdeObject> candidate;
  std::vector<TxdParentRelation> candidateParents;
  std::unordered_map<std::string, std::string> parentByChild;
  std::unordered_set<uint32_t> modelIds;
  std::vector<std::string_view> fields;
  std::string_view line;
  while (nextLine(text, line)) {
    if (line.empty()) continue;
    if (line.size() > kMaxLineBytes) {
      error = "baris IDE melebihi batas aman";
      return false;
    }
    if (line.find(',') == std::string_view::npos) {
      const std::string directive = lowerAscii(trim(line));
      if (directive == "objs") {
        section = Section::Objects;
      } else if (directive == "tobj") {
        section = Section::TimedObjects;
      } else if (directive == "anim") {
        section = Section::AnimatedObjects;
      } else if (directive == "cars") {
        // SA-MP StreamInVehicle IDs refer to this stock IDE namespace. The
        // remaining handling/vehicle metadata is not gameplay authority here;
        // retain only the safe DFF/TXD resolver mapping below.
        section = Section::Vehicles;
      } else if (directive == "peds") {
        // Streamed player skins use the same global model-ID namespace. Keep
        // only model/TXD/type mapping; ped animation/voice metadata is not a
        // replacement for GTA simulation or a server authority source.
        section = Section::Pedestrians;
      } else if (directive == "txdp") {
        sawTextureParents = true;
        section = Section::TextureParents;
      } else if (directive == "end") {
        section = Section::None;
      } else {
        section = Section::Other;
      }
      continue;
    }
    if (section == Section::None || section == Section::Other) continue;
    const bool dynamicSection = section == Section::Vehicles || section == Section::Pedestrians;
    if (dynamicSection ? !splitCsvPrefix(line, 4u, fields) : !splitCsv(line, fields)) {
      error = "record IDE tidak valid";
      return false;
    }
    if (section == Section::TextureParents) {
      // Retain only normalized stock `child,parent` names; runtime graph
      // validation later rejects conflicts/cycles before a render lookup can
      // use them. This section never carries a path or direct asset payload.
      std::string child;
      std::string parent;
      if (fields.size() != 2u || !normalizeModelName(fields[0], child) ||
          !normalizeModelName(fields[1], parent)) {
        error = "record txdp IDE tidak valid";
        return false;
      }
      const auto existingParent = parentByChild.find(child);
      if (existingParent != parentByChild.end()) {
        if (existingParent->second != parent) {
          error = "parent dictionary txdp bertentangan";
          return false;
        }
        continue;  // Duplicate identical relation is a harmless stock no-op.
      }
      if (candidateParents.size() >= maxObjects) {
        error = "jumlah parent dictionary txdp melebihi batas aman";
        return false;
      }
      parentByChild.emplace(child, parent);
      candidateParents.push_back({std::move(child), std::move(parent)});
      continue;
    }
    if (candidate.size() >= maxObjects) {
      error = "record IDE terlalu banyak";
      return false;
    }

    if (section == Section::Vehicles || section == Section::Pedestrians) {
      // GTA SA `cars`/`peds` have additional handling/class/animation fields
      // which are intentionally not treated as physics or gameplay sources.
      // Their first four fields safely bind a server model ID to user-owned
      // DFF/TXD data without guessing any remaining metadata.
      if (fields.size() < 4u) {
        error = section == Section::Vehicles ? "layout cars IDE tidak didukung"
                                             : "layout peds IDE tidak didukung";
        return false;
      }
      IdeObject dynamicModel;
      std::string modelType;
      bool duplicatedDictionaryName = false;
      if (!parseUnsigned(fields[0], dynamicModel.modelId) ||
          !normalizeDynamicModelName(fields[1], dynamicModel.modelName, duplicatedDictionaryName) ||
          !(duplicatedDictionaryName
                ? (normalizeModelName(dynamicModel.modelName, dynamicModel.textureDictionary) &&
                   normalizeModelName(fields[2], modelType))
                : (normalizeModelName(fields[2], dynamicModel.textureDictionary) &&
                   normalizeModelName(fields[3], modelType))) ||
          !modelIds.insert(dynamicModel.modelId).second) {
        error = section == Section::Vehicles
                    ? "nilai record cars IDE tidak valid atau model ID duplikat"
                    : "nilai record peds IDE tidak valid atau model ID duplikat";
        return false;
      }
      // Stock peds.ide denotes CJ (skin/model ID zero) as `null`, while the
      // actual user-owned IMG member is `player.dff`. Preserve custom model-0
      // names verbatim, but normalize this documented stock sentinel so a
      // valid SA-MP skin zero never resolves to a fictional null.dff.
      if (section == Section::Pedestrians && dynamicModel.modelId == 0u &&
          dynamicModel.modelName == "null") {
        dynamicModel.modelName = "player";
      }
      // Draw distance/flags apply to map objects, not this renderer-only
      // streamed vehicle/ped mapping. Leave deterministic zero defaults.
      candidate.push_back(std::move(dynamicModel));
      continue;
    }

    if (fields.size() < 5u) {
      error = "record object IDE tidak valid";
      return false;
    }

    size_t drawIndex = 0;
    size_t flagsIndex = 0;
    if (section == Section::Objects) {
      // SA supports the compact type 4 (draw, flags) and type 1-3 layouts
      // (mesh count, one draw distance per mesh, flags).
      if (fields.size() == 5u) {
        drawIndex = 3u;
        flagsIndex = 4u;
      } else {
        uint32_t meshCount = 0;
        if (!parseUnsigned(fields[3], meshCount) || meshCount == 0u || meshCount > 3u ||
            fields.size() != static_cast<size_t>(5u + meshCount)) {
          error = "layout objs IDE tidak didukung";
          return false;
        }
        drawIndex = 4u;
        flagsIndex = fields.size() - 1u;
        for (size_t index = drawIndex; index < flagsIndex; ++index) {
          float drawDistance = 0.0f;
          if (!parseFloat(fields[index], drawDistance) || drawDistance < 0.0f) {
            error = "draw distance objs IDE tidak valid";
            return false;
          }
        }
      }
    } else if (section == Section::TimedObjects) {
      // TOBJ adds timeOn/timeOff. Its first five fields mirror compact OBJS,
      // while older layouts carry a mesh count plus 1-3 draw distances.
      size_t timeOnIndex = 0;
      size_t timeOffIndex = 0;
      if (fields.size() == 7u) {
        drawIndex = 3u;
        flagsIndex = 4u;
        timeOnIndex = 5u;
        timeOffIndex = 6u;
      } else {
        uint32_t meshCount = 0;
        if (!parseUnsigned(fields[3], meshCount) || meshCount == 0u || meshCount > 3u ||
            fields.size() != static_cast<size_t>(7u + meshCount)) {
          error = "layout tobj IDE tidak didukung";
          return false;
        }
        drawIndex = 4u;
        flagsIndex = drawIndex + static_cast<size_t>(meshCount);
        timeOnIndex = flagsIndex + 1u;
        timeOffIndex = flagsIndex + 2u;
        for (size_t index = drawIndex; index < flagsIndex; ++index) {
          float drawDistance = 0.0f;
          if (!parseFloat(fields[index], drawDistance) || drawDistance < 0.0f) {
            error = "draw distance tobj IDE tidak valid";
            return false;
          }
        }
      }
      int32_t timeOn = 0;
      int32_t timeOff = 0;
      if (!parseSigned(fields[timeOnIndex], timeOn) || !parseSigned(fields[timeOffIndex], timeOff) ||
          timeOn < 0 || timeOn > 24 || timeOff < 0 || timeOff > 24) {
        error = "rentang waktu tobj IDE tidak valid";
        return false;
      }
    } else {  // ANIM: ID, model, TXD, animation name, draw, flags.
      if (fields.size() != 6u) {
        error = "layout anim IDE tidak didukung";
        return false;
      }
      std::string animationName;
      if (!normalizeModelName(fields[3], animationName)) {
        error = "nama animasi IDE tidak valid";
        return false;
      }
      drawIndex = 4u;
      flagsIndex = 5u;
    }

    IdeObject object;
    int32_t signedFlags = 0;
    if (!parseUnsigned(fields[0], object.modelId) || !normalizeModelName(fields[1], object.modelName) ||
        !normalizeModelName(fields[2], object.textureDictionary) ||
        !parseFloat(fields[drawIndex], object.drawDistance) || !std::isfinite(object.drawDistance) ||
        object.drawDistance < 0.0f || !parseSigned(fields[flagsIndex], signedFlags) ||
        !modelIds.insert(object.modelId).second) {
      error = "nilai record object IDE tidak valid atau model ID duplikat";
      return false;
    }
    object.flags = static_cast<uint32_t>(signedFlags);
    candidate.push_back(std::move(object));
  }

  if (candidate.empty() && !sawTextureParents) {
    error = "IDE tidak memiliki section objs/tobj/anim/cars/peds/txdp yang didukung";
    return false;
  }
  out.objects = std::move(candidate);
  out.txdParents = std::move(candidateParents);
  return true;
}

bool parseIdeObjects(std::string_view text, std::vector<IdeObject>& out, std::string& error,
                     size_t maxObjects) {
  IdeParseResult parsed;
  if (!parseIdeFile(text, parsed, error, maxObjects)) {
    out.clear();
    return false;
  }
  out = std::move(parsed.objects);
  return true;
}

bool parseIplInstances(std::string_view text, std::vector<Instance>& out, std::string& error,
                       size_t maxInstances) {
  out.clear();
  error.clear();
  if (maxInstances == 0 || maxInstances > kMaxIplInstances) maxInstances = kMaxIplInstances;

  enum class Section { None, Instances, IgnoredStock, Other };
  Section section = Section::None;
  // Stock gta.dat explicitly lists zoning and occlusion IPL-like text files.
  // They carry no render/collision instances in this runtime, but need a
  // known-safe empty result instead of making a valid extracted root fail.
  bool sawKnownNonInstanceSection = false;
  bool sawInstanceSection = false;
  std::vector<Instance> candidate;
  std::vector<std::string_view> fields;
  std::string_view line;
  while (nextLine(text, line)) {
    if (line.empty()) continue;
    if (line.size() > kMaxLineBytes) {
      error = "baris IPL melebihi batas aman";
      return false;
    }
    if (line.find(',') == std::string_view::npos) {
      const std::string directive = lowerAscii(trim(line));
      if (directive == "inst") {
        sawInstanceSection = true;
        section = Section::Instances;
      } else if (directive == "zone" || directive == "occl" || directive == "cull" ||
                 directive == "auzo" || directive == "enex" || directive == "grge" ||
                 directive == "jump" || directive == "mult" || directive == "path" ||
                 directive == "pick" || directive == "tcyc" || directive == "cars") {
        // These established GTA SA IPL sections are intentionally not turned
        // into gameplay authority or guessed as placement records. Their
        // contents remain a bounded read-only no-op for now.
        sawKnownNonInstanceSection = true;
        section = Section::IgnoredStock;
      } else if (directive == "end") {
        section = Section::None;
      } else {
        section = Section::Other;
      }
      continue;
    }
    if (section != Section::Instances) continue;
    if (!splitCsv(line, fields) || fields.size() < 10 || candidate.size() >= maxInstances) {
      error = "record inst IPL tidak valid atau terlalu banyak";
      return false;
    }

    Instance instance;
    if (!parseUnsigned(fields[0], instance.modelId) || !normalizeModelName(fields[1], instance.modelName) ||
        !parseSigned(fields[2], instance.interior) ||
        !parseFloat(fields[3], instance.position.x) || !parseFloat(fields[4], instance.position.y) ||
        !parseFloat(fields[5], instance.position.z) || !parseFloat(fields[6], instance.rotation.x) ||
        !parseFloat(fields[7], instance.rotation.y) || !parseFloat(fields[8], instance.rotation.z) ||
        !parseFloat(fields[9], instance.rotation.w)) {
      error = "nilai record inst IPL tidak valid";
      return false;
    }
    if (fields.size() >= 11 && !parseSigned(fields[10], instance.lodIndex)) {
      error = "LOD record inst IPL tidak valid";
      return false;
    }
    if (!validPosition(instance.position)) {
      error = "posisi record inst IPL di luar batas aman";
      return false;
    }
    if (!normalizeQuaternion(instance.rotation)) {
      error = "quaternion record inst IPL tidak valid";
      return false;
    }
    candidate.push_back(std::move(instance));
  }

  // Empty `inst` sections occur in stock interior manifests. An empty result
  // is safe only when the file declared an established GTA section; arbitrary
  // unknown-only text remains rejected rather than masquerading as world data.
  if (candidate.empty() && !sawInstanceSection && !sawKnownNonInstanceSection) {
    error = "IPL tidak memiliki section inst text yang didukung";
    return false;
  }
  out = std::move(candidate);
  return true;
}

bool parseBinaryIplInstances(const std::vector<uint8_t>& bytes, std::vector<Instance>& out,
                             std::string& error, size_t maxInstances) {
  out.clear();
  error.clear();
  if (maxInstances == 0 || maxInstances > kMaxIplInstances) maxInstances = kMaxIplInstances;
  if (bytes.size() < kBinaryIplHeaderBytes || bytes[0] != static_cast<uint8_t>('b') ||
      bytes[1] != static_cast<uint8_t>('n') || bytes[2] != static_cast<uint8_t>('r') ||
      bytes[3] != static_cast<uint8_t>('y')) {
    error = "header binary IPL bnry tidak valid";
    return false;
  }

  // SA stores six count fields, followed by six offset/size pairs. This
  // renderer currently understands only INST; every other section stays opaque
  // rather than being guessed as a different record type.
  uint32_t instanceCount = 0;
  uint32_t instanceOffsetRaw = 0;
  uint32_t instanceSizeRaw = 0;
  if (!readLe32(bytes, 4u, instanceCount) || !readLe32(bytes, 28u, instanceOffsetRaw) ||
      !readLe32(bytes, 32u, instanceSizeRaw)) {
    error = "header binary IPL terpotong";
    return false;
  }
  if (static_cast<size_t>(instanceCount) > maxInstances) {
    error = "jumlah INST binary IPL melebihi batas aman";
    return false;
  }
  // A valid streaming file may contain only CARS or another opaque section.
  // It contributes no object placement to this bridge but must not make the
  // entire stock IMG synchronization fail.
  if (instanceCount == 0u) return true;
  const size_t instanceOffset = static_cast<size_t>(instanceOffsetRaw);
  if (instanceOffset < kBinaryIplHeaderBytes || instanceOffset > bytes.size() ||
      static_cast<size_t>(instanceCount) >
          (bytes.size() - instanceOffset) / kBinaryIplInstanceBytes) {
    error = "offset atau panjang INST binary IPL tidak valid";
    return false;
  }
  const size_t requiredBytes = static_cast<size_t>(instanceCount) * kBinaryIplInstanceBytes;
  if (instanceSizeRaw != 0u &&
      (static_cast<size_t>(instanceSizeRaw) < requiredBytes ||
       static_cast<size_t>(instanceSizeRaw) > bytes.size() - instanceOffset)) {
    error = "ukuran INST binary IPL tidak valid";
    return false;
  }

  const auto decodeSigned32 = [](uint32_t raw) -> int32_t {
    if (raw <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
      return static_cast<int32_t>(raw);
    }
    if (raw == 0x80000000u) return std::numeric_limits<int32_t>::min();
    return -static_cast<int32_t>((~raw) + 1u);
  };

  std::vector<Instance> candidate;
  candidate.reserve(static_cast<size_t>(instanceCount));
  for (uint32_t index = 0; index < instanceCount; ++index) {
    const size_t base = instanceOffset + static_cast<size_t>(index) * kBinaryIplInstanceBytes;
    Instance instance;
    uint32_t flags = 0;
    uint32_t lodRaw = 0;
    if (!readLeFloat(bytes, base + 0u, instance.position.x) ||
        !readLeFloat(bytes, base + 4u, instance.position.y) ||
        !readLeFloat(bytes, base + 8u, instance.position.z) ||
        !readLeFloat(bytes, base + 12u, instance.rotation.x) ||
        !readLeFloat(bytes, base + 16u, instance.rotation.y) ||
        !readLeFloat(bytes, base + 20u, instance.rotation.z) ||
        !readLeFloat(bytes, base + 24u, instance.rotation.w) ||
        !readLe32(bytes, base + 28u, instance.modelId) || !readLe32(bytes, base + 32u, flags) ||
        !readLe32(bytes, base + 36u, lodRaw) || !validPosition(instance.position) ||
        !normalizeQuaternion(instance.rotation)) {
      error = "record INST binary IPL tidak valid";
      return false;
    }
    // SA binary IPL stores instance flags here; its low byte is the render
    // interior. Higher bits are retained by the source game for placement
    // behavior but are intentionally not interpreted by this basic bridge.
    instance.interior = static_cast<int32_t>(flags & 0xFFu);
    instance.lodIndex = decodeSigned32(lodRaw);
    candidate.push_back(std::move(instance));
  }

  out = std::move(candidate);
  return true;
}

}  // namespace samp::gta::map
