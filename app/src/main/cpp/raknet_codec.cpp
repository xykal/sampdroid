#include "raknet_codec.h"

#include <algorithm>
#include <utility>

namespace samp::raknet {

void BitWriter::writeBit(bool value) {
  if (bitPosition / 8 >= buffer.size()) buffer.push_back(0);
  if (value) buffer[bitPosition / 8] |= static_cast<uint8_t>(0x80u >> (bitPosition % 8));
  ++bitPosition;
}

void BitWriter::writeBits(uint32_t value, int count) {
  for (int byte = 0; byte < (count + 7) / 8; ++byte) {
    const int bitsThisByte = std::min(8, count - 8 * byte);
    const uint8_t current = static_cast<uint8_t>((value >> (8 * byte)) & 0xFFu);
    for (int bit = 0; bit < bitsThisByte; ++bit) {
      writeBit((current >> (bitsThisByte - 1 - bit)) & 1u);
    }
  }
}

void BitWriter::align() {
  while (bitPosition % 8) writeBit(false);
}

void BitWriter::writeBytes(const uint8_t* data, size_t count) {
  if (count == 0) return;
  if (bitPosition % 8 == 0) {
    buffer.insert(buffer.end(), data, data + count);
    bitPosition += count * 8;
    return;
  }
  for (size_t i = 0; i < count; ++i) writeBits(data[i], 8);
}

void BitWriter::writeAlignedBytes(const uint8_t* data, size_t count) {
  align();
  writeBytes(data, count);
}

void BitWriter::writeCompressedU16(uint16_t value) {
  const uint8_t high = static_cast<uint8_t>(value >> 8);
  const uint8_t low = static_cast<uint8_t>(value & 0xFFu);
  if (high == 0) {
    writeBit(true);
    if ((low & 0xF0u) == 0) {
      writeBit(true);
      writeBits(low & 0x0Fu, 4);
    } else {
      writeBit(false);
      writeBits(low, 8);
    }
  } else {
    writeBit(false);
    writeBits(value, 16);
  }
}

void BitWriter::writeCompressedU32(uint32_t value) {
  int byteIndex = 3;
  while (byteIndex > 0) {
    const uint8_t current = static_cast<uint8_t>((value >> (byteIndex * 8)) & 0xFFu);
    if (current == 0) {
      writeBit(true);
      --byteIndex;
    } else {
      writeBit(false);
      writeBits(value, (byteIndex + 1) * 8);
      return;
    }
  }
  const uint8_t low = static_cast<uint8_t>(value & 0xFFu);
  if ((low & 0xF0u) == 0) {
    writeBit(true);
    writeBits(low & 0x0Fu, 4);
  } else {
    writeBit(false);
    writeBits(low, 8);
  }
}

BitReader::BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

bool BitReader::readBit() {
  if (bitPosition_ / 8 >= size_) {
    ok_ = false;
    return false;
  }
  const bool value = (data_[bitPosition_ / 8] >> (7 - (bitPosition_ % 8))) & 1u;
  ++bitPosition_;
  return value;
}

uint32_t BitReader::readBits(int count) {
  uint32_t value = 0;
  for (int byte = 0; byte < (count + 7) / 8 && ok_; ++byte) {
    const int bitsThisByte = std::min(8, count - 8 * byte);
    uint32_t current = 0;
    for (int bit = 0; bit < bitsThisByte; ++bit) {
      current = (current << 1) | (readBit() ? 1u : 0u);
    }
    value |= current << (8 * byte);
  }
  return value;
}

void BitReader::align() {
  while (bitPosition_ % 8 && ok_) readBit();
}

std::vector<uint8_t> BitReader::readBytes(size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t value = static_cast<uint8_t>(readBits(8));
    if (!ok_) return {};
    out.push_back(value);
  }
  return out;
}

std::vector<uint8_t> BitReader::readAlignedBytes(size_t count) {
  align();
  if (!ok_) return {};
  if (bitPosition_ / 8 + count > size_) {
    ok_ = false;
    return {};
  }
  std::vector<uint8_t> out(data_ + bitPosition_ / 8, data_ + bitPosition_ / 8 + count);
  bitPosition_ += count * 8;
  return out;
}

uint16_t BitReader::readCompressedU16() {
  int byteIndex = 1;
  while (byteIndex > 0) {
    if (readBit()) {
      --byteIndex;
    } else {
      return static_cast<uint16_t>(readBits((byteIndex + 1) * 8));
    }
  }
  if (readBit()) return static_cast<uint16_t>(readBits(4));
  return static_cast<uint16_t>(readBits(8));
}

uint32_t BitReader::readCompressedU32() {
  int byteIndex = 3;
  while (byteIndex > 0) {
    if (readBit()) {
      --byteIndex;
    } else {
      return readBits((byteIndex + 1) * 8);
    }
  }
  if (readBit()) return readBits(4);
  return readBits(8);
}

std::vector<uint8_t> buildReliable(uint16_t messageNumber, const std::vector<uint8_t>& payload) {
  BitWriter writer;
  writer.writeBit(false);  // hasAcks
  writer.writeBits(messageNumber, 16);
  writer.writeBits(kReliabilityReliable, 4);
  writer.writeBit(false);  // isSplit
  writer.writeCompressedU16(static_cast<uint16_t>(payload.size() * 8));
  writer.writeAlignedBytes(payload.data(), payload.size());
  return writer.buffer;
}

std::vector<uint8_t> buildAck(const std::vector<uint16_t>& messageNumbers) {
  BitWriter writer;
  writer.writeBit(true);  // hasAcks
  writer.writeCompressedU16(static_cast<uint16_t>(messageNumbers.size()));
  for (uint16_t message : messageNumbers) {
    writer.writeBit(true);  // max == min
    writer.writeBits(message, 16);
  }
  writer.align();
  return writer.buffer;
}

Datagram decodeDatagram(const std::vector<uint8_t>& datagram) {
  Datagram out;
  BitReader reader(datagram.data(), datagram.size());
  const bool hasAcks = reader.readBit();
  if (!reader.ok()) return out;

  if (hasAcks) {
    const uint16_t count = reader.readCompressedU16();
    if (count > 64 || !reader.ok()) return out;
    for (uint16_t i = 0; i < count && reader.ok(); ++i) {
      const bool isSingle = reader.readBit();
      reader.readBits(16);
      if (!isSingle) reader.readBits(16);
    }
  }

  while (reader.ok() && reader.bitPosition() < datagram.size() * 8) {
    // RakNet inserts one marker bit before every internal packet when the
    // datagram begins with an ACK block.
    if (hasAcks) reader.readBit();
    if (!reader.ok()) break;

    IncomingPacket packet;
    packet.messageNumber = static_cast<uint16_t>(reader.readBits(16));
    packet.reliability = static_cast<uint8_t>(reader.readBits(4));
    if (packet.reliability == 7 || packet.reliability == kReliabilityReliableOrdered ||
        packet.reliability == kReliabilityReliableSequenced) {
      reader.readBits(5);   // ordering channel
      reader.readBits(16);  // ordering index
    }

    packet.split = reader.readBit();
    if (packet.split) {
      packet.splitId = static_cast<uint16_t>(reader.readBits(16));
      // RakNet's SplitPacketIndexType is uint32_t and uses WriteCompressed.
      packet.splitIndex = reader.readCompressedU32();
      packet.splitCount = reader.readCompressedU32();
    }

    const uint16_t bitLength = reader.readCompressedU16();
    if (!reader.ok() || bitLength == 0) break;
    packet.payload = reader.readAlignedBytes((bitLength + 7) / 8);
    if (!reader.ok() || packet.payload.empty()) break;

    if (packet.reliability == kReliabilityReliable ||
        packet.reliability == kReliabilityReliableOrdered ||
        packet.reliability == kReliabilityReliableSequenced) {
      out.ackMessageNumbers.push_back(packet.messageNumber);
    }
    out.packets.push_back(std::move(packet));
  }

  out.ok = !out.packets.empty();
  return out;
}

std::vector<IncomingPacket> SplitReassembler::push(IncomingPacket packet) {
  if (!packet.split) return {std::move(packet)};

  // MTU-sized SA-MP traffic should be tiny; this bound prevents an untrusted
  // peer from making a phone reserve arbitrary memory through a malformed header.
  constexpr uint32_t kMaxSplitParts = 512;
  if (packet.splitCount == 0 || packet.splitCount > kMaxSplitParts ||
      packet.splitIndex >= packet.splitCount) {
    return {};
  }

  constexpr size_t kMaxPendingSequences = 64;
  auto it = pending_.find(packet.splitId);
  if (it == pending_.end() && pending_.size() >= kMaxPendingSequences) {
    return {};
  }
  if (it == pending_.end() || it->second.expectedCount != packet.splitCount) {
    PendingSplit fresh;
    fresh.expectedCount = packet.splitCount;
    fresh.reliability = packet.reliability;
    fresh.parts.resize(packet.splitCount);
    fresh.received.assign(packet.splitCount, false);
    it = pending_.insert_or_assign(packet.splitId, std::move(fresh)).first;
  }

  PendingSplit& pending = it->second;
  if (!pending.received[packet.splitIndex]) {
    pending.parts[packet.splitIndex] = std::move(packet.payload);
    pending.received[packet.splitIndex] = true;
    ++pending.receivedCount;
  }

  if (pending.receivedCount != pending.expectedCount) return {};

  IncomingPacket assembled = std::move(packet);
  assembled.split = false;
  assembled.reassembled = true;
  assembled.payload.clear();
  size_t total = 0;
  for (const auto& part : pending.parts) total += part.size();
  assembled.payload.reserve(total);
  for (const auto& part : pending.parts) {
    assembled.payload.insert(assembled.payload.end(), part.begin(), part.end());
  }
  pending_.erase(it);
  return {std::move(assembled)};
}

void SplitReassembler::clear() {
  pending_.clear();
}

}  // namespace samp::raknet
