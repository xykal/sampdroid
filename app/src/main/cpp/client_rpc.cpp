#include "client_rpc.h"

#include "raknet_codec.h"

namespace samp::clientrpc {
namespace {

constexpr uint8_t kIdRpc = 20;
constexpr uint8_t kPlayerSpawn = 52;
constexpr uint8_t kPlayerRequestClass = 128;
constexpr uint8_t kPlayerRequestSpawn = 129;

}  // namespace

std::vector<uint8_t> buildRequestClass(uint16_t classId) {
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kPlayerRequestClass, 8);
  writer.writeBits(classId, 16);
  return writer.buffer;
}

std::vector<uint8_t> buildRequestSpawn() {
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kPlayerRequestSpawn, 8);
  return writer.buffer;
}

std::vector<uint8_t> buildPlayerSpawn() {
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kPlayerSpawn, 8);
  return writer.buffer;
}

}  // namespace samp::clientrpc
