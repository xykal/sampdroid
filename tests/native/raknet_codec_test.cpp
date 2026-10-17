#include "raknet_codec.h"
#include "rpc_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::exit(1);
}

void expect(bool condition, const char* message) {
  if (!condition) fail(message);
}

void writeFloat(samp::raknet::BitWriter& writer, float value) {
  uint32_t raw = 0;
  static_assert(sizeof(raw) == sizeof(value), "unexpected float size");
  std::memcpy(&raw, &value, sizeof(raw));
  writer.writeBits(raw, 32);
}

std::vector<uint8_t> makePlayerInit() {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 139};
  writer.writeBytes(header, sizeof(header));

  writer.writeBit(true);   // EnableZoneNames
  writer.writeBit(true);   // UsePlayerPedAnims
  writer.writeBit(false);  // AllowInteriorWeapons
  writer.writeBit(true);   // UseLimitGlobalChatRadius
  writeFloat(writer, 42.5f);
  writer.writeBit(false);  // EnableStuntBonus
  writeFloat(writer, 70.0f);
  writer.writeBit(false);  // DisableInteriorEnterExits
  writer.writeBit(true);   // DisableNameTagLOS
  writer.writeBit(false);  // ManualVehicleEngineAndLights
  writer.writeBits(3, 32); // SetSpawnInfoCount
  writer.writeBits(42, 16); // PlayerID
  writer.writeBit(true);    // ShowNameTags
  writer.writeBits(1, 32);  // ShowPlayerMarkers
  writer.writeBits(12, 8);  // SetWorldTime
  writer.writeBits(7, 8);   // SetWeather
  writeFloat(writer, 0.008f);
  writer.writeBit(false);   // LanMode
  writer.writeBits(5000, 32); // SetDeathDropAmount
  writer.writeBit(false);   // Instagib
  writer.writeBits(30, 32); // OnFootRate
  writer.writeBits(30, 32); // InCarRate
  writer.writeBits(30, 32); // WeaponRate
  writer.writeBits(1, 32);  // Multiplier
  writer.writeBits(1, 32);  // LagCompensation

  const std::string name = "SampDroid Test";
  writer.writeBits(static_cast<uint32_t>(name.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());

  std::vector<uint8_t> models(212, 0);
  writer.writeBytes(models.data(), models.size());
  writer.writeBits(1, 32);  // EnableVehicleFriendlyFire
  return writer.buffer;
}

void testReliableAndAck() {
  const auto reliable = samp::raknet::buildReliable(1, {0x0B});
  const std::vector<uint8_t> expectedReliable = {0x00, 0x80, 0x43, 0x80, 0x0B};
  expect(reliable == expectedReliable, "reliable packet wire vector changed");

  const auto decoded = samp::raknet::decodeDatagram(reliable);
  expect(decoded.ok && decoded.packets.size() == 1, "reliable packet did not decode");
  expect(decoded.packets[0].messageNumber == 1, "wrong reliable message number");
  expect(decoded.packets[0].reliability == samp::raknet::kReliabilityReliable,
         "wrong reliability code");
  expect(decoded.packets[0].payload == std::vector<uint8_t>({0x0B}),
         "wrong reliable payload");
  expect(decoded.ackMessageNumbers == std::vector<uint16_t>({1}), "ACK list missing");

  const auto ack = samp::raknet::buildAck({1});
  expect(ack == std::vector<uint8_t>({0xE3, 0x01, 0x00}), "ACK wire vector changed");
}

void testCompressedU32() {
  samp::raknet::BitWriter writer;
  const std::vector<uint32_t> values = {0, 15, 16, 255, 256, 65535, 65536, 0x12345678u};
  for (uint32_t value : values) writer.writeCompressedU32(value);

  samp::raknet::BitReader reader(writer.buffer.data(), writer.buffer.size());
  for (uint32_t expected : values) {
    expect(reader.readCompressedU32() == expected, "compressed u32 round-trip changed");
  }
  expect(reader.ok(), "compressed u32 reader failed");
}

void testSplitReassembly() {
  samp::raknet::SplitReassembler reassembler;
  samp::raknet::IncomingPacket second;
  second.split = true;
  second.splitId = 77;
  second.splitIndex = 1;
  second.splitCount = 2;
  second.payload = {0x03, 0x04};
  expect(reassembler.push(second).empty(), "split completed too early");

  samp::raknet::IncomingPacket first;
  first.split = true;
  first.splitId = 77;
  first.splitIndex = 0;
  first.splitCount = 2;
  first.payload = {0x01, 0x02};
  const auto result = reassembler.push(first);
  expect(result.size() == 1, "split was not reassembled");
  expect(result[0].reassembled, "reassembled flag absent");
  expect(result[0].payload == std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04}),
         "split order changed");
}

void testPlayerInitDecoder() {
  const auto payload = makePlayerInit();
  samp::rpc::PlayerInitData init;
  expect(samp::rpc::decodePlayerInit(payload, init), "PlayerInit decoder rejected valid packet");
  expect(init.valid, "PlayerInit valid flag missing");
  expect(init.playerId == 42, "PlayerInit player ID wrong");
  expect(init.spawnClassCount == 3, "PlayerInit class count wrong");
  expect(init.worldTime == 12 && init.weather == 7, "PlayerInit time/weather wrong");
  expect(init.serverName == "SampDroid Test", "PlayerInit server name wrong");
  expect(std::fabs(init.gravity - 0.008f) < 0.00001f, "PlayerInit gravity wrong");
  expect(samp::rpc::summarize(payload).find("PlayerInit") != std::string::npos,
         "PlayerInit summary has no RPC name");
}

}  // namespace

int main() {
  testReliableAndAck();
  testCompressedU32();
  testSplitReassembly();
  testPlayerInitDecoder();
  std::puts("raknet codec regression tests: OK");
  return 0;
}
