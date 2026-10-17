// Minimal RakNet 2.x wire codec used by SampDroid.
// This is an independent implementation; it does not link against RakNet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace samp::raknet {

constexpr uint8_t kReliabilityReliable = 8;
constexpr uint8_t kReliabilityReliableOrdered = 9;
constexpr uint8_t kReliabilityReliableSequenced = 10;

// RakNet uses MSB-first bit order inside each byte. Multi-byte primitive values
// are emitted little-endian, one byte at a time.
class BitWriter {
public:
  std::vector<uint8_t> buffer;
  size_t bitPosition = 0;

  void writeBit(bool value);
  void writeBits(uint32_t value, int count);
  void align();

  // Equivalent to NetworkBitStream::Write(char*, n): works even when the
  // current offset is not byte aligned.
  void writeBytes(const uint8_t* data, size_t count);
  // Equivalent to WriteAlignedBytes: aligns first, then writes bytes.
  void writeAlignedBytes(const uint8_t* data, size_t count);
  void writeCompressedU16(uint16_t value);
  void writeCompressedU32(uint32_t value);
};

class BitReader {
public:
  BitReader(const uint8_t* data, size_t size);

  bool readBit();
  uint32_t readBits(int count);
  void align();

  // Equivalent to NetworkBitStream::Read(char*, n), without implicit align.
  std::vector<uint8_t> readBytes(size_t count);
  std::vector<uint8_t> readAlignedBytes(size_t count);
  uint16_t readCompressedU16();
  uint32_t readCompressedU32();

  bool ok() const { return ok_; }
  size_t bitPosition() const { return bitPosition_; }

private:
  const uint8_t* data_;
  size_t size_;
  size_t bitPosition_ = 0;
  bool ok_ = true;
};

struct IncomingPacket {
  uint16_t messageNumber = 0;
  uint8_t reliability = 0;
  bool split = false;
  uint16_t splitId = 0;
  uint32_t splitIndex = 0;
  uint32_t splitCount = 0;
  bool reassembled = false;
  std::vector<uint8_t> payload;
};

struct Datagram {
  bool ok = false;
  std::vector<uint16_t> ackMessageNumbers;
  std::vector<IncomingPacket> packets;
};

std::vector<uint8_t> buildReliable(uint16_t messageNumber, const std::vector<uint8_t>& payload);
std::vector<uint8_t> buildAck(const std::vector<uint16_t>& messageNumbers);
Datagram decodeDatagram(const std::vector<uint8_t>& datagram);

// Split packets must be acknowledged fragment-by-fragment, then reassembled
// before the higher-level RPC decoder sees them.
class SplitReassembler {
public:
  std::vector<IncomingPacket> push(IncomingPacket packet);
  void clear();
  size_t pendingCount() const { return pending_.size(); }

private:
  struct PendingSplit {
    uint32_t expectedCount = 0;
    uint8_t reliability = 0;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<bool> received;
    size_t receivedCount = 0;
  };

  std::map<uint16_t, PendingSplit> pending_;
};

}  // namespace samp::raknet
