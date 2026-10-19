// Independent, bounded GTA SA IMG v2 directory reader.
#include "gta_img_archive.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace samp::gta::img {
namespace {

constexpr size_t kHeaderBytes = 8;
constexpr size_t kDirectoryEntryBytes = 32;
constexpr size_t kNameBytes = 24;

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

bool rangeInside(uint64_t offset, uint64_t count, uint64_t total) {
  return offset <= total && count <= total - offset;
}

bool multiplySectors(uint64_t sectors, uint64_t& bytes) {
  if (sectors > std::numeric_limits<uint64_t>::max() / kSectorBytes) return false;
  bytes = sectors * kSectorBytes;
  return true;
}

bool normalizeName(std::string_view raw, std::string& normalized) {
  if (raw.empty() || raw.size() > kNameBytes) return false;
  normalized.clear();
  normalized.reserve(raw.size());
  for (const unsigned char byte : raw) {
    // IMG members are filenames, not paths. Restrict the index to printable
    // filename bytes so it cannot later be misused as a filesystem path.
    if (byte < 0x21u || byte > 0x7Eu || byte == '/' || byte == '\\' || byte == ':') {
      normalized.clear();
      return false;
    }
    char value = static_cast<char>(byte);
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    normalized.push_back(value);
  }
  return normalized.find("..") == std::string::npos;
}

bool decodeDirectoryName(const uint8_t* bytes, std::string& normalized) {
  size_t length = 0;
  while (length < kNameBytes && bytes[length] != 0) ++length;
  // A v2 directory member is documented as NUL-terminated inside 24 bytes.
  // Refuse an unterminated name rather than indexing an ambiguous member.
  if (length == kNameBytes) return false;
  return normalizeName(std::string_view(reinterpret_cast<const char*>(bytes), length), normalized);
}

void fail(std::string& error, const char* message) {
  error = message;
}

}  // namespace

uint64_t MemoryByteSource::size() const {
  return static_cast<uint64_t>(bytes_.size());
}

bool MemoryByteSource::readAt(uint64_t offset, uint8_t* destination, size_t count) const {
  if ((count != 0 && destination == nullptr) ||
      !rangeInside(offset, static_cast<uint64_t>(count), size())) {
    return false;
  }
  if (count != 0) {
    std::memcpy(destination, bytes_.data() + static_cast<size_t>(offset), count);
  }
  return true;
}

const Entry* ArchiveIndex::find(std::string_view name) const {
  std::string normalized;
  if (!normalizeName(name, normalized)) return nullptr;
  const auto it = byName_.find(normalized);
  return it == byName_.end() ? nullptr : &entries_[it->second];
}

bool parseV2(ByteSource& source, ArchiveIndex& out, std::string& error, size_t maxEntries) {
  out.entries_.clear();
  out.byName_.clear();
  error.clear();

  if (maxEntries == 0 || maxEntries > kMaxArchiveEntries) maxEntries = kMaxArchiveEntries;
  if (source.size() < kHeaderBytes) {
    fail(error, "IMG terlalu pendek untuk header VER2");
    return false;
  }

  uint8_t header[kHeaderBytes]{};
  if (!source.readAt(0, header, sizeof(header))) {
    fail(error, "gagal membaca header IMG");
    return false;
  }
  if (std::memcmp(header, "VER2", 4) != 0) {
    fail(error, "bukan archive GTA SA IMG v2 (VER2)");
    return false;
  }

  const uint32_t declaredEntries = readLe32(header + 4);
  if (declaredEntries > maxEntries) {
    fail(error, "jumlah entry IMG melebihi batas aman");
    return false;
  }
  if (declaredEntries >
      (std::numeric_limits<uint64_t>::max() - kHeaderBytes) / kDirectoryEntryBytes) {
    fail(error, "ukuran directory IMG overflow");
    return false;
  }
  const uint64_t directoryBytes = static_cast<uint64_t>(declaredEntries) * kDirectoryEntryBytes;
  const uint64_t directoryEnd = kHeaderBytes + directoryBytes;
  if (!rangeInside(kHeaderBytes, directoryBytes, source.size())) {
    fail(error, "directory IMG terpotong");
    return false;
  }
  if (directoryBytes > std::numeric_limits<size_t>::max()) {
    fail(error, "directory IMG terlalu besar untuk perangkat ini");
    return false;
  }

  std::vector<uint8_t> directory(static_cast<size_t>(directoryBytes));
  if (!directory.empty() && !source.readAt(kHeaderBytes, directory.data(), directory.size())) {
    fail(error, "gagal membaca directory IMG");
    return false;
  }

  ArchiveIndex candidate;
  candidate.entries_.reserve(declaredEntries);
  candidate.byName_.reserve(declaredEntries);
  for (uint32_t index = 0; index < declaredEntries; ++index) {
    const uint8_t* raw = directory.data() + static_cast<size_t>(index) * kDirectoryEntryBytes;
    const uint64_t sectorOffset = readLe32(raw);
    const uint64_t sectorCount = readLe16(raw + 4);
    uint64_t offsetBytes = 0;
    uint64_t sizeBytes = 0;
    if (sectorCount == 0 || !multiplySectors(sectorOffset, offsetBytes) ||
        !multiplySectors(sectorCount, sizeBytes) || offsetBytes < directoryEnd ||
        !rangeInside(offsetBytes, sizeBytes, source.size())) {
      fail(error, "entry IMG memiliki rentang sektor tidak valid");
      return false;
    }

    std::string name;
    if (!decodeDirectoryName(raw + 8, name)) {
      fail(error, "entry IMG memiliki nama tidak valid");
      return false;
    }
    if (candidate.byName_.find(name) != candidate.byName_.end()) {
      fail(error, "entry IMG duplikat");
      return false;
    }

    const size_t entryIndex = candidate.entries_.size();
    candidate.entries_.push_back({std::move(name), offsetBytes, sizeBytes});
    candidate.byName_.emplace(candidate.entries_.back().name, entryIndex);
  }

  out = std::move(candidate);
  return true;
}

bool readEntry(ByteSource& source, const Entry& entry, size_t maxBytes,
               std::vector<uint8_t>& out, std::string& error) {
  out.clear();
  error.clear();
  if (entry.sizeBytes == 0 || entry.sizeBytes > maxBytes ||
      entry.sizeBytes > std::numeric_limits<size_t>::max() ||
      !rangeInside(entry.offsetBytes, entry.sizeBytes, source.size())) {
    error = "entry IMG melebihi batas atau rentangnya tidak valid";
    return false;
  }

  out.resize(static_cast<size_t>(entry.sizeBytes));
  if (!source.readAt(entry.offsetBytes, out.data(), out.size())) {
    out.clear();
    error = "gagal membaca entry IMG";
    return false;
  }
  return true;
}

}  // namespace samp::gta::img
