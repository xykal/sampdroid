// Minimal RakNet 2.x wire codec used by SampDroid.
// This is an independent implementation; it does not link against RakNet.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace samp::raknet {

constexpr uint8_t kReliabilityUnreliable = 6;
constexpr uint8_t kReliabilityUnreliableSequenced = 7;
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
  // `bitLength == 0` means all bytes are readable. Supplying the exact
  // RakNet frame bit length keeps padding bits from being mistaken as payload.
  BitReader(const uint8_t* data, size_t size, size_t bitLength = 0);

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
  size_t bitLength_;
  size_t bitPosition_ = 0;
  bool ok_ = true;
};

struct IncomingPacket {
  uint16_t messageNumber = 0;
  uint8_t reliability = 0;
  uint8_t orderingChannel = 0;
  uint16_t orderingIndex = 0;
  bool split = false;
  uint16_t splitId = 0;
  uint32_t splitIndex = 0;
  uint32_t splitCount = 0;
  bool reassembled = false;
  // Exact app-payload bit count carried in the RakNet frame. It can be less
  // than payload.size() * 8 when the final byte contains transport padding.
  uint16_t payloadBitLength = 0;
  std::vector<uint8_t> payload;
};

struct Datagram {
  bool ok = false;
  // Reliable packets decoded from this datagram; caller must acknowledge these.
  std::vector<uint16_t> ackMessageNumbers;
  // ACK records carried by the remote peer for our own reliable message numbers.
  std::vector<uint16_t> receivedAckMessageNumbers;
  std::vector<IncomingPacket> packets;
};

// RakNet UNRELIABLE_SEQUENCED packets must not let an older datagram overwrite
// newer state. The 16-bit index wraps; half the sequence space is deliberately
// treated as ambiguous/stale.
class SequencedPacketGate {
public:
  bool accept(uint8_t orderingChannel, uint16_t orderingIndex);
  void clear();

private:
  std::array<bool, 32> seen_{};
  std::array<uint16_t, 32> latest_{};
};

// RakNet acknowledges every reliable transport message, including duplicates,
// but only delivers each message number once to higher layers. Keep a bounded
// sliding receive window before split reassembly so duplicate RELIABLE frames
// cannot repeat a control action or consume fragment resources.
class ReliableMessageGate {
public:
  static constexpr uint16_t kHistoryWindow = 4096;

  bool accept(uint16_t messageNumber);
  void clear();
  size_t seenCount() const { return seen_.size(); }

private:
  bool initialized_ = false;
  uint16_t newest_ = 0;
  std::set<uint16_t> seen_;
};

// RELIABLE_ORDERED is reliable at the transport layer, but a later UDP
// datagram can still arrive before an earlier one. Retain bounded packets per
// channel until the missing ordering index arrives, then release only the
// contiguous application sequence. Overflow is terminal for a session rather
// than silently ACKing-and-losing an ordered RPC that can never be recovered.
class ReliableOrderedGate {
public:
  static constexpr size_t kMaxBufferedPacketsPerChannel = 64;
  static constexpr size_t kMaxBufferedPacketsTotal = 128;
  static constexpr size_t kMaxBufferedBytesTotal = 2u * 1024u * 1024u;

  std::vector<IncomingPacket> push(IncomingPacket packet);
  void clear();
  size_t pendingCount() const { return pendingCount_; }
  size_t pendingBytes() const { return pendingBytes_; }
  bool overflowed() const { return overflowed_; }

private:
  struct ChannelState {
    uint16_t nextExpected = 0;
    std::map<uint16_t, IncomingPacket> pending;
  };

  std::array<ChannelState, 32> channels_{};
  size_t pendingCount_ = 0;
  size_t pendingBytes_ = 0;
  bool overflowed_ = false;
};

std::vector<uint8_t> buildUnreliable(uint16_t messageNumber, const std::vector<uint8_t>& payload);

// UNRELIABLE_SEQUENCED uses its own ordering stream. `payloadBitLength` is
// explicit because RakNet frames encode a bit count, not merely a byte count.
// It must be nonzero and no larger than payload.size() * 8.
std::vector<uint8_t> buildUnreliableSequenced(uint16_t messageNumber, uint8_t orderingChannel,
                                              uint16_t orderingIndex,
                                              const std::vector<uint8_t>& payload,
                                              uint16_t payloadBitLength);

std::vector<uint8_t> buildReliable(uint16_t messageNumber, const std::vector<uint8_t>& payload);
std::vector<uint8_t> buildReliableOrdered(uint16_t messageNumber, uint8_t orderingChannel,
                                          uint16_t orderingIndex,
                                          const std::vector<uint8_t>& payload);
// Sorts/deduplicates and coalesces non-wrapping contiguous u16 message IDs into
// bounded RakNet ACK ranges. Wrap-around IDs are intentionally separate records.
std::vector<uint8_t> buildAck(const std::vector<uint16_t>& messageNumbers);
Datagram decodeDatagram(const std::vector<uint8_t>& datagram);

// Split packets must be acknowledged fragment-by-fragment, then reassembled
// before the higher-level RPC decoder sees them.
class SplitReassembler {
public:
  static constexpr uint32_t kMaxSplitParts = 512;
  static constexpr size_t kMaxPendingSequences = 64;
  // Reassembled frames carry a u16 bit length, so retaining more than this
  // cannot produce a representable IncomingPacket anyway.
  static constexpr size_t kMaxReassembledPayloadBytes =
      (static_cast<size_t>(UINT16_MAX) + 7u) / 8u;
  static constexpr size_t kMaxPendingBytes = 256u * 1024u;

  std::vector<IncomingPacket> push(IncomingPacket packet);
  void clear();
  size_t pendingCount() const { return pending_.size(); }
  size_t pendingBytes() const { return pendingBytes_; }
  bool overflowed() const { return overflowed_; }

private:
  struct PendingSplit {
    uint32_t expectedCount = 0;
    // Every fragment produced by legacy RakNet copies the original transport
    // metadata (only its message number changes). Keep that identity so a
    // recycled split ID cannot splice unrelated packet bodies together.
    uint8_t reliability = 0;
    uint8_t orderingChannel = 0;
    uint16_t orderingIndex = 0;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<uint16_t> partBitLengths;
    std::vector<bool> received;
    size_t receivedCount = 0;
    size_t receivedBytes = 0;
    size_t receivedBits = 0;
  };

  std::map<uint16_t, PendingSplit> pending_;
  size_t pendingBytes_ = 0;
  bool overflowed_ = false;
};

}  // namespace samp::raknet
