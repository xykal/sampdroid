// Read-only GTA San Andreas IMG v2 archive index.
//
// This is an independent, bounded reader for data selected by the player. It
// never bundles, downloads, writes, or executes game data. The parsed entries
// will feed the native Android world/collision renderer in later stages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace samp::gta::img {

constexpr uint64_t kSectorBytes = 2048;
constexpr size_t kMaxArchiveEntries = 200000;
constexpr size_t kMaxExtractBytes = 16u * 1024u * 1024u;

// Minimal random-access boundary so an Android file descriptor can be used
// later without copying a multi-gigabyte user-owned IMG archive into memory.
class ByteSource {
public:
  virtual ~ByteSource() = default;
  virtual uint64_t size() const = 0;
  virtual bool readAt(uint64_t offset, uint8_t* destination, size_t count) const = 0;
};

// Test-only/in-memory implementation. Production Android file-descriptor I/O
// will implement the same tiny interface rather than changing archive parsing.
class MemoryByteSource final : public ByteSource {
public:
  explicit MemoryByteSource(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  uint64_t size() const override;
  bool readAt(uint64_t offset, uint8_t* destination, size_t count) const override;

private:
  const std::vector<uint8_t>& bytes_;
};

struct Entry {
  // Lowercase, validated IMG member name, e.g. "gta3.col" or "player.dff".
  std::string name;
  uint64_t offsetBytes = 0;
  uint64_t sizeBytes = 0;
};

// Index only: it is deliberately small relative to an IMG archive and owns no
// game asset payload. Entries are kept in on-disk directory order.
class ArchiveIndex {
public:
  const std::vector<Entry>& entries() const { return entries_; }
  const Entry* find(std::string_view name) const;
  bool empty() const { return entries_.empty(); }

private:
  friend bool parseV2(ByteSource&, ArchiveIndex&, std::string&, size_t);
  std::vector<Entry> entries_;
  std::unordered_map<std::string, size_t> byName_;
};

// Reads a GTA SA IMG v2 ("VER2") header + directory from `source`. It checks
// sector arithmetic, directory spans, duplicate names, and every entry's range
// against the source length before publishing a transactional index.
bool parseV2(ByteSource& source, ArchiveIndex& out, std::string& error,
             size_t maxEntries = kMaxArchiveEntries);

// Extracts exactly one padded IMG entry subject to a caller-defined cap. The
// source data remains read-only; `out` is cleared on failure.
bool readEntry(ByteSource& source, const Entry& entry, size_t maxBytes,
               std::vector<uint8_t>& out, std::string& error);

}  // namespace samp::gta::img
