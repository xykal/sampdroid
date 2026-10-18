#include "client_rpc.h"
#include "client_sync.h"
#include "input_state.h"
#include "local_player_sync.h"
#include "raknet_codec.h"
#include "raknet_session.h"
#include "raknet_session_policy.h"
#include "sync_policy.h"
#include "sync_decoder.h"
#include "rpc_decoder.h"
#include "rpc_state.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

std::vector<uint8_t> makePlayerJoin(uint16_t id, uint32_t rgba, bool npc,
                                    const std::string& name) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 137};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(rgba, 32);
  writer.writeBits(npc ? 1u : 0u, 8);
  writer.writeBits(static_cast<uint32_t>(name.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  return writer.buffer;
}

std::vector<uint8_t> makePlayerColor(uint16_t id, uint32_t rgba) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 72};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(rgba, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeChatMessage(uint32_t rgba, const std::string& text) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 93};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(rgba, 32);
  writer.writeBits(static_cast<uint32_t>(text.size()), 32);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  return writer.buffer;
}

std::vector<uint8_t> makePlayerName(uint16_t id, const std::string& name) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 11};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(static_cast<uint32_t>(name.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  writer.writeBits(1, 8);  // Success
  return writer.buffer;
}

std::vector<uint8_t> makePlayerQuit(uint16_t id, uint8_t reason) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 138};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(reason, 8);
  return writer.buffer;
}

std::vector<uint8_t> makePlayerStreamIn(uint16_t id, uint8_t team, uint32_t model,
                                        float x, float y, float z, float angle, uint32_t rgba,
                                        uint8_t fightingStyle) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 32};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(team, 8);
  writer.writeBits(model, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, angle);
  writer.writeBits(rgba, 32);
  writer.writeBits(fightingStyle, 8);
  for (int i = 0; i < 11; ++i) writer.writeBits(static_cast<uint32_t>(i), 16);
  return writer.buffer;
}

std::vector<uint8_t> makePlayerStreamOut(uint16_t id) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 163};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  return writer.buffer;
}

void writeCompressedUnitFloat(samp::raknet::BitWriter& writer, float value) {
  // Matches NetworkBitStream::WriteCompressed(float)'s unsigned-short cast.
  const auto raw = static_cast<uint16_t>((value + 1.0f) * 32767.5f);
  writer.writeBits(raw, 16);
}

std::vector<uint8_t> makeMinimalRemoteOnFootBroadcast(uint16_t id, float x) {
  samp::raknet::BitWriter writer;
  writer.writeBits(207, 8);
  writer.writeBits(id, 16);
  writer.writeBit(false);  // LeftRight omitted
  writer.writeBit(false);  // UpDown omitted
  writer.writeBits(0, 16);
  writeFloat(writer, x);
  writeFloat(writer, 0.0f);
  writeFloat(writer, 3.0f);
  writer.writeBit(false);  // W sign
  writer.writeBit(false);  // X sign
  writer.writeBit(false);  // Y sign
  writer.writeBit(false);  // Z sign
  writer.writeBits(0, 16);
  writer.writeBits(0, 16);
  writer.writeBits(0, 16);
  writer.writeBits(0xF0, 8);
  writer.writeBits(0, 8);
  writer.writeBits(0, 8);
  writeFloat(writer, 0.0f);
  writer.writeBit(false);  // no surfing
  writer.writeBit(false);  // no animation
  return writer.buffer;
}

std::vector<uint8_t> makeRemoteOnFootBroadcast(uint16_t id) {
  samp::raknet::BitWriter writer;
  writer.writeBits(207, 8);
  writer.writeBits(id, 16);
  writer.writeBit(true);   // LeftRight present
  writer.writeBits(static_cast<uint16_t>(-12), 16);
  writer.writeBit(false);  // UpDown omitted
  writer.writeBits(0x1234, 16);
  writeFloat(writer, 100.5f);
  writeFloat(writer, -200.25f);
  writeFloat(writer, 18.0f);
  writer.writeBit(false);  // quaternion W sign
  writer.writeBit(false);  // X sign
  writer.writeBit(true);   // Y sign
  writer.writeBit(false);  // Z sign
  writer.writeBits(static_cast<uint32_t>(0.5f * 65535.0f), 16);
  writer.writeBits(static_cast<uint32_t>(0.5f * 65535.0f), 16);
  writer.writeBits(0, 16);
  writer.writeBits(0xF8, 8);  // health 100, armour ceil(50 / 7) = 8 => 56 decoded
  writer.writeBits(0x98, 8);
  writer.writeBits(3, 8);
  writeFloat(writer, 5.0f);
  writeCompressedUnitFloat(writer, 0.6f);
  writeCompressedUnitFloat(writer, 0.8f);
  writeCompressedUnitFloat(writer, 0.0f);
  writer.writeBit(true);  // surfing
  writer.writeBits(123, 16);
  writeFloat(writer, 8.0f);
  writeFloat(writer, -9.0f);
  writeFloat(writer, 10.0f);
  writer.writeBit(true);  // animation
  writer.writeBits(456, 16);
  writer.writeBits(789, 16);
  return writer.buffer;
}

std::vector<uint8_t> makeClassSelection(bool selectable, uint8_t team, uint32_t model) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 128};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(selectable ? 1u : 0u, 8);
  writer.writeBits(team, 8);
  writer.writeBits(model, 32);
  writer.writeBits(0, 8);  // Unknown1
  writeFloat(writer, 10.0f);
  writeFloat(writer, 20.0f);
  writeFloat(writer, 30.0f);
  writeFloat(writer, 180.0f);
  writer.writeBits(24, 32);
  writer.writeBits(25, 32);
  writer.writeBits(0, 32);
  writer.writeBits(100, 32);
  writer.writeBits(50, 32);
  writer.writeBits(0, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeSpawnResponse(bool allowed, bool legacyByte = false) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 129};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(allowed ? 1u : 0u, legacyByte ? 8 : 32);
  return writer.buffer;
}

std::vector<uint8_t> makeSetSpawnInfo(uint8_t team, uint32_t model, float x, float y, float z,
                                      float angle) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 68};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(team, 8);
  writer.writeBits(model, 32);
  writer.writeBits(7, 8);  // Unknown1
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, angle);
  writer.writeBits(24, 32);
  writer.writeBits(25, 32);
  writer.writeBits(0, 32);
  writer.writeBits(100, 32);
  writer.writeBits(50, 32);
  writer.writeBits(0, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeVec3Rpc(uint8_t rpcId, float x, float y, float z) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  return writer.buffer;
}

std::vector<uint8_t> makeFloatRpc(uint8_t rpcId, float value) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writeFloat(writer, value);
  return writer.buffer;
}

std::vector<uint8_t> makeByteRpc(uint8_t rpcId, uint8_t value) {
  return {20, rpcId, value};
}

std::vector<uint8_t> makeCheckpoint(float x, float y, float z, float size) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 107};
  writer.writeBytes(header, sizeof(header));
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, size);
  return writer.buffer;
}

std::vector<uint8_t> makeRaceCheckpoint(uint8_t type, float x, float y, float z,
                                        float nextX, float nextY, float nextZ, float size) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 38};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(type, 8);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, nextX);
  writeFloat(writer, nextY);
  writeFloat(writer, nextZ);
  writeFloat(writer, size);
  return writer.buffer;
}

std::vector<uint8_t> makeEmptyRpc(uint8_t rpcId) {
  return {20, rpcId};
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
  expect(decoded.packets[0].payload == std::vector<uint8_t>({0x0B}) &&
         decoded.packets[0].payloadBitLength == 8,
         "wrong reliable payload/bit length");
  expect(decoded.ackMessageNumbers == std::vector<uint16_t>({1}), "ACK list missing");

  // RakNet 2.52 transport keepalive is a one-byte reliable frame, not an
  // application RPC. Its ACK behavior must stay distinct from connected pong.
  const auto keepalive = samp::raknet::buildReliable(23, {23});
  const auto decodedKeepalive = samp::raknet::decodeDatagram(keepalive);
  expect(decodedKeepalive.ok && decodedKeepalive.packets.size() == 1,
         "keepalive frame did not decode");
  expect(decodedKeepalive.packets[0].messageNumber == 23 &&
         decodedKeepalive.packets[0].reliability == samp::raknet::kReliabilityReliable &&
         decodedKeepalive.packets[0].payload == std::vector<uint8_t>({23}),
         "keepalive wire semantics changed");
  expect(samp::sessionpolicy::shouldSendReliableKeepalive(5000, 0),
         "keepalive must begin at five-second reliable idle");
  expect(!samp::sessionpolicy::shouldSendReliableKeepalive(4999, 0) &&
         !samp::sessionpolicy::shouldSendReliableKeepalive(5000, 1),
         "keepalive policy ignored interval or pending ACK");

  const auto pong = samp::raknet::buildUnreliable(24, {9, 1, 2, 3, 4, 5, 6, 7, 8});
  const auto decodedPong = samp::raknet::decodeDatagram(pong);
  expect(decodedPong.ok && decodedPong.packets.size() == 1 &&
         decodedPong.packets[0].messageNumber == 24 &&
         decodedPong.packets[0].reliability == samp::raknet::kReliabilityUnreliable &&
         decodedPong.packets[0].payload == std::vector<uint8_t>({9, 1, 2, 3, 4, 5, 6, 7, 8}) &&
         decodedPong.ackMessageNumbers.empty(),
         "connected pong must remain unacked/unreliable");

  const auto ack = samp::raknet::buildAck({1});
  expect(ack == std::vector<uint8_t>({0xE3, 0x01, 0x00}), "ACK wire vector changed");
  const auto decodedAck = samp::raknet::decodeDatagram(ack);
  expect(decodedAck.receivedAckMessageNumbers == std::vector<uint16_t>({1}),
         "remote ACK message number missing");

  samp::raknet::BitWriter ackRangeWriter;
  ackRangeWriter.writeBit(true);  // hasAcks
  ackRangeWriter.writeCompressedU16(1);
  ackRangeWriter.writeBit(false); // range rather than singleton
  ackRangeWriter.writeBits(10, 16);
  ackRangeWriter.writeBits(12, 16);
  ackRangeWriter.align();
  const auto decodedRange = samp::raknet::decodeDatagram(ackRangeWriter.buffer);
  expect(decodedRange.receivedAckMessageNumbers == std::vector<uint16_t>({10, 11, 12}),
         "remote ACK range changed");

  const auto ordered = samp::raknet::buildReliableOrdered(2, 2, 17, {20, 128, 3, 0});
  const auto orderedDecoded = samp::raknet::decodeDatagram(ordered);
  expect(orderedDecoded.ok && orderedDecoded.packets.size() == 1,
         "reliable ordered packet did not decode");
  const auto& packet = orderedDecoded.packets[0];
  expect(packet.reliability == samp::raknet::kReliabilityReliableOrdered,
         "wrong ordered reliability code");
  expect(packet.orderingChannel == 2 && packet.orderingIndex == 17,
         "ordered channel/index changed");
  expect(packet.payload == std::vector<uint8_t>({20, 128, 3, 0}),
         "ordered payload changed");
}

void testUnreliableSequencedTransport() {
  // This vector has a deliberately non-byte-aligned five-bit payload. It
  // locks the packet bit length, channel-1 header, and alignment before the
  // payload; a byte-length-only builder would silently encode it differently.
  const auto sequenced = samp::raknet::buildUnreliableSequenced(
      0x1234, 1, 0x00A5, {0xA8}, 5);
  const std::vector<uint8_t> expected = {0x1A, 0x09, 0x38, 0x69, 0x40, 0x1A, 0x80, 0xA8};
  expect(sequenced == expected, "unreliable sequenced wire vector changed");

  samp::raknet::BitReader header(sequenced.data(), sequenced.size());
  expect(!header.readBit(), "sequenced packet unexpectedly has ACKs");
  expect(header.readBits(16) == 0x1234, "sequenced message number changed");
  expect(header.readBits(4) == samp::raknet::kReliabilityUnreliableSequenced,
         "sequenced reliability changed");
  expect(header.readBits(5) == 1 && header.readBits(16) == 0x00A5,
         "sequenced channel/index changed");
  expect(!header.readBit() && header.readCompressedU16() == 5,
         "sequenced payload bit length changed");
  expect(header.readAlignedBytes(1) == std::vector<uint8_t>({0xA8}) && header.ok(),
         "sequenced payload alignment changed");

  const auto decoded = samp::raknet::decodeDatagram(sequenced);
  expect(decoded.ok && decoded.ackMessageNumbers.empty() && decoded.packets.size() == 1,
         "unreliable sequenced packet decode/ACK behavior changed");
  expect(decoded.packets[0].reliability == samp::raknet::kReliabilityUnreliableSequenced &&
         decoded.packets[0].orderingChannel == 1 && decoded.packets[0].orderingIndex == 0x00A5 &&
         decoded.packets[0].payload == std::vector<uint8_t>({0xA8}) &&
         decoded.packets[0].payloadBitLength == 5,
         "unreliable sequenced semantics changed");
  samp::raknet::BitReader exactPayload(decoded.packets[0].payload.data(),
                                       decoded.packets[0].payload.size(),
                                       decoded.packets[0].payloadBitLength);
  expect(exactPayload.readBits(5) == 0x15 && exactPayload.ok(),
         "exact payload bit reader changed");
  exactPayload.readBit();
  expect(!exactPayload.ok(), "payload padding bit must not be readable");

  expect(samp::raknet::buildUnreliableSequenced(1, 1, 0, {0x00}, 0).empty(),
         "zero-bit sequenced packet must be rejected");
  expect(samp::raknet::buildUnreliableSequenced(1, 1, 0, {0x00}, 9).empty(),
         "oversized sequenced bit length must be rejected");

  samp::raknet::SequencedPacketGate gate;
  expect(gate.accept(1, 65534) && gate.accept(1, 65535) && gate.accept(1, 0),
         "sequenced ordering wrap must move forward");
  expect(!gate.accept(1, 0) && !gate.accept(1, 65535) && !gate.accept(32, 1),
         "duplicate, stale, or invalid sequenced packet must be rejected");
  gate.clear();
  expect(gate.accept(1, 12), "cleared sequenced gate must accept a fresh index");
}

void testPlayerFootSyncBuilder() {
  samp::sync::OnFootState idle;
  const auto idlePayload = samp::sync::buildPlayerFootSync(idle);
  expect(idlePayload.valid() && idlePayload.bitLength == 552 && idlePayload.bytes.size() == 69,
         "idle PlayerFootSync fixed layout changed");

  std::vector<uint8_t> expectedIdle(69, 0);
  expectedIdle[0] = 207;
  // Raw W component of the legacy quaternion is one; this distinguishes the
  // client-to-server reader layout from the compressed broadcast writer.
  expectedIdle[21] = 0x80;
  expectedIdle[22] = 0x3F;
  expectedIdle[35] = 100;
  expect(idlePayload.bytes == expectedIdle, "idle PlayerFootSync wire vector changed");

  samp::sync::OnFootState state;
  state.leftRight = 0xFFFF;
  state.upDown = 7;
  state.keys = 0x1234;
  state.positionX = 1.5f;
  state.positionY = -2.25f;
  state.positionZ = 3.75f;
  state.quaternionW = -0.5f;
  state.quaternionX = 0.5f;
  state.quaternionY = -0.5f;
  state.quaternionZ = 0.5f;
  state.health = 99;
  state.armour = 50;
  state.weaponAdditionalKey = 0x98;
  state.specialAction = 3;
  state.velocityX = 3.0f;
  state.velocityY = 4.0f;
  state.velocityZ = 0.0f;
  state.surfingOffsetX = 8.0f;
  state.surfingOffsetY = -9.0f;
  state.surfingOffsetZ = 10.0f;
  state.surfingId = 123;
  state.animationId = 456;
  state.animationFlags = 789;
  const auto payload = samp::sync::buildPlayerFootSync(state);
  const std::vector<uint8_t> expected = {
      0xCF, 0xFF, 0xFF, 0x07, 0x00, 0x34, 0x12, 0x00, 0x00, 0xC0, 0x3F,
      0x00, 0x00, 0x10, 0xC0, 0x00, 0x00, 0x70, 0x40, 0x00, 0x00, 0x00,
      0xBF, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0xBF, 0x00, 0x00,
      0x00, 0x3F, 0x63, 0x32, 0x98, 0x03, 0x00, 0x00, 0x40, 0x40, 0x00,
      0x00, 0x80, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
      0x00, 0x00, 0x10, 0xC1, 0x00, 0x00, 0x20, 0x41, 0x7B, 0x00, 0xC8,
      0x01, 0x15, 0x03};
  expect(payload.valid() && payload.bytes == expected,
         "PlayerFootSync client reader-order vector changed");

  state.positionX = 20000.0f;
  expect(!samp::sync::isValidOnFootState(state) && !samp::sync::buildPlayerFootSync(state).valid(),
         "out-of-range PlayerFootSync position must be rejected");
  state.positionX = 1.5f;
  state.velocityX = 101.0f;
  expect(!samp::sync::isValidOnFootState(state),
         "out-of-range PlayerFootSync velocity must be rejected");

  samp::syncpolicy::OrderingCounters ordering;
  expect(ordering.takeRpc() == 0 && ordering.takeRpc() == 1 && ordering.takeSync() == 0 &&
         ordering.takeRpc() == 2 && ordering.takeSync() == 1,
         "RPC and sync ordering counters must remain independent");

  expect(samp::syncpolicy::boundedOnFootRateMs(0) == 30 &&
         samp::syncpolicy::boundedOnFootRateMs(1) == 20 &&
         samp::syncpolicy::boundedOnFootRateMs(5000) == 1000,
         "on-foot rate bounds changed");
  expect(samp::syncpolicy::onFootSendDue(100, 0, 30) &&
         !samp::syncpolicy::onFootSendDue(129, 100, 30) &&
         samp::syncpolicy::onFootSendDue(130, 100, 30) &&
         !samp::syncpolicy::onFootSendDue(99, 100, 30),
         "on-foot rate gate changed");
  expect(samp::syncpolicy::onFootSessionReady(true, true, true, true, true) &&
         !samp::syncpolicy::onFootSessionReady(false, true, true, true, true) &&
         !samp::syncpolicy::onFootSessionReady(true, false, true, true, true) &&
         !samp::syncpolicy::onFootSessionReady(true, true, false, true, true) &&
         !samp::syncpolicy::onFootSessionReady(true, true, true, false, true) &&
         !samp::syncpolicy::onFootSessionReady(true, true, true, true, false),
         "on-foot spawn/ACK gate changed");
}

void testRemoteOnFootBroadcastDecoder() {
  const auto payload = makeRemoteOnFootBroadcast(7);
  // Captures the open.mp broadcast writer layout, including fields beginning
  // mid-byte after the two directional presence bits. This must stay separate
  // from the 69-byte client-to-server OnFootSync vector above.
  const std::vector<uint8_t> expected = {
      0xCF, 0x07, 0x00, 0xFA, 0x7F, 0x8D, 0x04, 0x80, 0x00, 0x32, 0x50, 0x80,
      0x10, 0x12, 0x30, 0xC0, 0x00, 0x24, 0x10, 0x4B, 0xFD, 0xFF, 0xFD, 0xFC,
      0x00, 0x03, 0xE2, 0x60, 0x0C, 0x00, 0x02, 0x81, 0x03, 0x33, 0x31, 0x97,
      0x9B, 0xFD, 0xFE, 0xF6, 0x00, 0x00, 0x00, 0x00, 0x82, 0x00, 0x00, 0x21,
      0x82, 0x00, 0x00, 0x40, 0x83, 0xC8, 0x01, 0x15, 0x03};
  expect(payload == expected, "remote PlayerFootSync broadcast wire vector changed");
  samp::syncdecode::RemoteOnFootSync remote;
  expect(samp::syncdecode::decodeRemoteOnFootBroadcast(payload, remote),
         "remote PlayerFootSync broadcast did not decode");
  expect(remote.playerId == 7 && remote.leftRight == -12 && remote.upDown == 0 &&
         remote.keys == 0x1234 && std::fabs(remote.positionX - 100.5f) < 0.001f &&
         std::fabs(remote.positionY + 200.25f) < 0.001f && std::fabs(remote.positionZ - 18.0f) < 0.001f,
         "remote PlayerFootSync base fields changed");
  expect(std::fabs(remote.quaternionX - 0.5f) < 0.0001f &&
         std::fabs(remote.quaternionY + 0.5f) < 0.0001f &&
         std::fabs(remote.quaternionZ) < 0.0001f && std::fabs(remote.health - 100.0f) < 0.001f &&
         std::fabs(remote.armour - 56.0f) < 0.001f && remote.weaponAdditionalKey == 0x98 &&
         remote.specialAction == 3,
         "remote PlayerFootSync compressed quaternion/percent fields changed");
  expect(std::fabs(remote.velocityX - 3.0f) < 0.01f && std::fabs(remote.velocityY - 4.0f) < 0.01f &&
         std::fabs(remote.velocityZ) < 0.01f && remote.surfing && remote.surfingId == 123 &&
         std::fabs(remote.surfingOffsetY + 9.0f) < 0.001f && remote.animationKnown &&
         remote.animationId == 456 && remote.animationFlags == 789,
         "remote PlayerFootSync optional field alignment changed");
  expect(samp::syncdecode::summarize(remote).find("#7") != std::string::npos,
         "remote PlayerFootSync summary missing player ID");

  auto truncated = payload;
  truncated.pop_back();
  expect(!samp::syncdecode::decodeRemoteOnFootBroadcast(truncated, remote),
         "truncated remote PlayerFootSync must be rejected");
  expect(!samp::syncdecode::decodeRemoteOnFootBroadcast(payload, remote,
                                                         payload.size() * 8 - 1),
         "declared-short remote PlayerFootSync must not consume padding");
  // A deliberately too-short packet must never be interpreted as a complete
  // broadcast frame, even when its first byte is unrelated to Packet 207.
  auto badPosition = std::vector<uint8_t>(4, 0);
  expect(!samp::syncdecode::decodeRemoteOnFootBroadcast(badPosition, remote),
         "malformed remote PlayerFootSync must be rejected");
  const auto minimal = makeMinimalRemoteOnFootBroadcast(8, 1.0f);
  expect(samp::syncdecode::decodeRemoteOnFootBroadcast(minimal, remote) && remote.playerId == 8 &&
         remote.leftRight == 0 && remote.upDown == 0 && !remote.surfing && !remote.animationKnown &&
         std::fabs(remote.velocityX) < 0.001f && std::fabs(remote.quaternionW - 1.0f) < 0.001f,
         "minimal remote PlayerFootSync optional branches changed");
  expect(!samp::syncdecode::decodeRemoteOnFootBroadcast(
             makeMinimalRemoteOnFootBroadcast(7, 20000.0f), remote),
         "out-of-range remote PlayerFootSync position must be rejected");
}

void testNativeInputAndLocalPreview() {
  samp::input::TouchIntent raw;
  raw.moveX = 0.5f;
  raw.moveY = 1.0f;
  raw.lookDeltaDegrees = 90.0f;
  raw.heldActions = samp::input::kHeldUse | samp::input::kHeldFire |
                    samp::input::kHeldSprint | samp::input::kHeldJump |
                    samp::input::kHeldAim;

  samp::input::TouchIntent sanitized;
  expect(samp::input::sanitizeTouchIntent(raw, sanitized) &&
         std::fabs(sanitized.moveX - 0.5f) < 0.001f &&
         sanitized.heldActions == raw.heldActions,
         "native touch input sanitization changed");
  raw.moveX = std::numeric_limits<float>::quiet_NaN();
  expect(!samp::input::sanitizeTouchIntent(raw, sanitized),
         "non-finite touch axis must be rejected");
  raw.moveX = 0.5f;
  raw.heldActions = 0x8000;
  expect(!samp::input::sanitizeTouchIntent(raw, sanitized),
         "unknown touch action bit must be rejected");

  raw.heldActions = samp::input::kHeldUse | samp::input::kHeldFire |
                    samp::input::kHeldSprint | samp::input::kHeldJump |
                    samp::input::kHeldAim;
  samp::input::OnFootInputMapper mapper;
  expect(!mapper.wireInput().headingKnown, "input mapper must require server heading");
  mapper.synchronizeHeading(0.0f);
  expect(mapper.update(raw), "native touch input mapper rejected valid sample");
  const auto wire = mapper.wireInput();
  expect(wire.headingKnown && wire.leftRight == 64 && wire.upDown == -128 &&
         wire.keys == static_cast<uint16_t>(1 | 4 | 8 | 32 | 128) &&
         std::fabs(wire.headingDegrees - 90.0f) < 0.001f,
         "semantic touch-to-SA-MP input mapping changed");

  raw.moveX = 4.0f;
  raw.moveY = -4.0f;
  raw.lookDeltaDegrees = -180.0f;
  raw.heldActions = 0;
  expect(mapper.update(raw), "bounded touch input should be accepted");
  const auto bounded = mapper.wireInput();
  expect(bounded.leftRight == 128 && bounded.upDown == 128 && bounded.keys == 0 &&
         std::fabs(bounded.headingDegrees - 0.0f) < 0.001f,
         "touch axis/look bounds changed");

  samp::state::LocalPlayer local;
  local.positionKnown = true;
  local.facingKnown = true;
  local.x = 10.0f;
  local.y = 20.0f;
  local.z = 30.0f;
  local.healthKnown = true;
  local.health = 87.5f;
  local.armourKnown = true;
  local.armour = 25.0f;
  local.specialActionKnown = true;
  local.specialAction = 3;
  local.velocityKnown = true;
  local.velocityX = 3.0f;
  local.velocityY = 4.0f;
  local.velocityZ = 0.0f;

  const auto preview = samp::bridge::buildOnFootPreview(local, wire);
  expect(preview.ready() && preview.state.leftRight == 64 && preview.state.upDown == 0xFF80 &&
         preview.state.keys == static_cast<uint16_t>(1 | 4 | 8 | 32 | 128) &&
         std::fabs(preview.state.positionX - 10.0f) < 0.001f &&
         std::fabs(preview.state.quaternionW - 0.7071067f) < 0.0001f &&
         std::fabs(preview.state.quaternionZ + 0.7071067f) < 0.0001f &&
         preview.state.health == 88 && preview.state.armour == 25 &&
         preview.state.specialAction == 3 && preview.state.velocityY == 4.0f &&
         samp::sync::buildPlayerFootSync(preview.state).valid(),
         "local player OnFoot preview bridge changed");

  local.positionKnown = false;
  expect(samp::bridge::buildOnFootPreview(local, wire).status ==
             samp::bridge::OnFootPreviewStatus::MissingPosition,
         "preview must not invent a local position");
  local.positionKnown = true;
  local.health = 101.0f;
  expect(samp::bridge::buildOnFootPreview(local, wire).status ==
             samp::bridge::OnFootPreviewStatus::InvalidVitals,
         "preview must not silently clamp out-of-range game vitals");
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
  expect(result[0].payload == std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04}) &&
         result[0].payloadBitLength == 32,
         "split order/bit length changed");

  samp::raknet::IncomingPacket ambiguous;
  ambiguous.split = true;
  ambiguous.splitId = 78;
  ambiguous.splitIndex = 0;
  ambiguous.splitCount = 2;
  ambiguous.payload = {0xA8};
  ambiguous.payloadBitLength = 5;
  expect(reassembler.push(ambiguous).empty() && reassembler.pendingCount() == 0,
         "non-byte-aligned non-final split must be rejected");
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
  expect(init.onFootSyncRateMs == 30 && init.inCarSyncRateMs == 30 &&
         init.weaponSyncRateMs == 30 && init.syncMultiplier == 1 && init.lagCompensation == 1,
         "PlayerInit sync rates were not retained");
  expect(samp::rpc::summarize(payload).find("onfoot=30ms") != std::string::npos,
         "PlayerInit on-foot rate missing from summary");
  expect(samp::rpc::summarize(payload).find("PlayerInit") != std::string::npos,
         "PlayerInit summary has no RPC name");
}

void testClientRpcBuilders() {
  expect(samp::clientrpc::buildRequestClass(3) == std::vector<uint8_t>({20, 128, 3, 0}),
         "request class payload changed");
  expect(samp::clientrpc::buildRequestSpawn() == std::vector<uint8_t>({20, 129}),
         "request spawn payload changed");
  expect(samp::clientrpc::buildPlayerSpawn() == std::vector<uint8_t>({20, 52}),
         "player spawn confirmation payload changed");
}

void testSessionSafetyWithoutSocket() {
  const auto status = samp::raknet_session_status();
  expect(!status.active && !status.error.empty(), "idle session status must be safe");
  const auto classRequest = samp::raknet_session_request_class(0);
  expect(!classRequest.active && !classRequest.error.empty(),
         "class request without session must be rejected");
  const auto spawnRequest = samp::raknet_session_request_spawn();
  expect(!spawnRequest.active && !spawnRequest.error.empty(),
         "spawn request without session must be rejected");
  const auto controls = samp::raknet_session_update_controls(samp::input::TouchIntent{});
  expect(!controls.active && !controls.error.empty(),
         "touch input without session must be rejected");
  const auto onFoot = samp::raknet_session_send_onfoot(samp::sync::OnFootState{});
  expect(!onFoot.active && !onFoot.error.empty(),
         "on-foot send without session must be rejected");
  const auto closed = samp::raknet_session_disconnect();
  expect(!closed.active && !closed.stateSummary.empty(), "idle session close must be safe");
}

void testWorldStateReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;

  expect(world.applyRpc(makePlayerInit(), event), "WorldState rejected PlayerInit");
  expect(world.initialized() && world.playerInit().playerId == 42,
         "WorldState did not retain PlayerInit");

  expect(world.applyRpc(makeSetSpawnInfo(4, 411, 100.0f, -25.5f, 8.0f, -90.0f), event),
         "WorldState rejected SetSpawnInfo");
  const auto& localSpawn = world.localPlayer();
  expect(event.kind == samp::state::RpcEventKind::SpawnInfoChanged && event.localFacingUpdated &&
         localSpawn.spawnInfoKnown && localSpawn.positionKnown && localSpawn.facingKnown &&
         localSpawn.team == 4 && localSpawn.model == 411 &&
         std::fabs(localSpawn.x - 100.0f) < 0.001f &&
         std::fabs(localSpawn.y + 25.5f) < 0.001f &&
         std::fabs(localSpawn.z - 8.0f) < 0.001f &&
         std::fabs(localSpawn.angle - 270.0f) < 0.001f &&
         localSpawn.spawnWeapons[0] == 24 && localSpawn.spawnAmmo[1] == 50,
         "SetSpawnInfo local state wrong");

  expect(world.applyRpc(makeVec3Rpc(12, 1.0f, 2.0f, 3.0f), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged &&
         world.localPlayer().positionKnown && !world.localPlayer().positionFindZ &&
         std::fabs(world.localPlayer().x - 1.0f) < 0.001f,
         "SetPlayerPosition local state wrong");
  expect(world.applyRpc(makeVec3Rpc(13, 4.0f, 5.0f, 6.0f), event) &&
         world.localPlayer().positionFindZ && std::fabs(world.localPlayer().z - 6.0f) < 0.001f,
         "SetPlayerPositionFindZ local state wrong");
  expect(world.applyRpc(makeFloatRpc(19, 450.0f), event) && event.localFacingUpdated &&
         world.localPlayer().facingKnown && std::fabs(world.localPlayer().angle - 90.0f) < 0.001f,
         "SetPlayerFacingAngle local state wrong");
  expect(world.applyRpc(makeFloatRpc(14, 87.5f), event) && world.localPlayer().healthKnown &&
         std::fabs(world.localPlayer().health - 87.5f) < 0.001f,
         "SetPlayerHealth local state wrong");
  expect(world.applyRpc(makeFloatRpc(66, 25.0f), event) && world.localPlayer().armourKnown &&
         std::fabs(world.localPlayer().armour - 25.0f) < 0.001f,
         "SetPlayerArmour local state wrong");
  expect(world.applyRpc(makeByteRpc(88, 3), event) && world.localPlayer().specialActionKnown &&
         world.localPlayer().specialAction == 3,
         "SetPlayerSpecialAction local state wrong");
  expect(world.applyRpc(makeVec3Rpc(90, 3.0f, 4.0f, 0.0f), event) &&
         world.localPlayer().velocityKnown && std::fabs(world.localPlayer().velocityY - 4.0f) < 0.001f,
         "SetPlayerVelocity local state wrong");
  expect(!world.applyRpc(makeVec3Rpc(12, 20000.0f, 0.0f, 0.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "out-of-range local position must be rejected");
  expect(!world.applyRpc(makeFloatRpc(14, std::numeric_limits<float>::quiet_NaN()), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "non-finite local health must be rejected");

  expect(world.applyRpc(makeClassSelection(true, 3, 7), event),
         "WorldState rejected class selection");
  expect(event.kind == samp::state::RpcEventKind::ClassSelection,
         "wrong class selection event kind");
  expect(world.classSelection().valid && world.classSelection().selectable &&
         world.classSelection().team == 3 && world.classSelection().model == 7 &&
         std::fabs(world.classSelection().angle - 180.0f) < 0.001f,
         "class selection state wrong");

  world.beginSpawnRequest();
  expect(!world.spawnResponseKnown() && !world.spawnAllowed(),
         "new spawn request must clear stale permission");
  expect(world.applyRpc(makeSpawnResponse(true, true), event),
         "WorldState rejected legacy-byte spawn response");
  expect(event.kind == samp::state::RpcEventKind::SpawnResponse &&
         world.spawnResponseKnown() && world.spawnAllowed(),
         "legacy-byte spawn response state wrong");
  world.beginSpawnRequest();
  expect(world.applyRpc(makeSpawnResponse(true), event),
         "WorldState rejected u32 spawn response");
  expect(event.kind == samp::state::RpcEventKind::SpawnResponse &&
         world.spawnResponseKnown() && world.spawnAllowed(),
         "u32 spawn response state wrong");

  expect(world.applyRpc(makeCheckpoint(100.0f, -25.5f, 8.0f, 3.0f), event),
         "WorldState rejected SetCheckpoint");
  expect(event.kind == samp::state::RpcEventKind::CheckpointChanged &&
         world.checkpoint().active && std::fabs(world.checkpoint().x - 100.0f) < 0.001f &&
         std::fabs(world.checkpoint().y + 25.5f) < 0.001f &&
         std::fabs(world.checkpoint().size - 3.0f) < 0.001f,
         "checkpoint state wrong");
  expect(world.applyRpc(makeEmptyRpc(37), event) && !world.checkpoint().active,
         "DisableCheckpoint state wrong");
  expect(!world.applyRpc(makeCheckpoint(0.0f, 0.0f, 0.0f, -1.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "negative checkpoint size must be rejected");

  expect(world.applyRpc(makeRaceCheckpoint(2, 1.0f, 2.0f, 3.0f,
                                            4.0f, 5.0f, 6.0f, 7.5f), event),
         "WorldState rejected SetRaceCheckpoint");
  expect(event.kind == samp::state::RpcEventKind::RaceCheckpointChanged &&
         world.raceCheckpoint().active && world.raceCheckpoint().type == 2 &&
         std::fabs(world.raceCheckpoint().nextZ - 6.0f) < 0.001f &&
         std::fabs(world.raceCheckpoint().size - 7.5f) < 0.001f,
         "race checkpoint state wrong");
  expect(world.applyRpc(makeEmptyRpc(39), event) && !world.raceCheckpoint().active,
         "DisableRaceCheckpoint state wrong");

  expect(world.applyRpc(makeChatMessage(0x33CCFFFFu, "Selamat datang"), event),
         "WorldState rejected SendClientMessage");
  expect(event.kind == samp::state::RpcEventKind::ChatMessage && world.chatCount() == 1,
         "chat event/state wrong");
  const auto* latestChat = world.latestChat();
  expect(latestChat && latestChat->rgba == 0x33CCFFFFu && latestChat->text == "Selamat datang",
         "chat payload wrong");
  for (int i = 0; i < 101; ++i) {
    expect(world.applyRpc(makeChatMessage(0xFFFFFFFFu, "x"), event),
           "WorldState rejected bounded chat line");
  }
  expect(world.chatCount() == 100, "chat history cap changed");
  expect(!world.applyRpc(makeChatMessage(0xFFFFFFFFu, std::string(145, 'x')), event),
         "oversized chat line must be rejected");
  expect(event.kind == samp::state::RpcEventKind::Malformed,
         "oversized chat must report malformed state");

  expect(!world.applyRpc(makePlayerStreamIn(42, 0, 0, 0.0f, 0.0f, 3.0f, 0.0f,
                                             0, 0), event),
         "WorldState must ignore a StreamIn claiming local player ID");
  expect(world.applyRpc(makePlayerJoin(7, 0xAABBCCDDu, false, "Darius"), event),
         "WorldState rejected PlayerJoin");
  expect(event.kind == samp::state::RpcEventKind::PlayerJoined, "wrong join event kind");
  expect(world.playerCount() == 1, "join did not create player");
  const auto* player = world.findPlayer(7);
  expect(player && player->name == "Darius" && player->rgba == 0xAABBCCDDu,
         "join player data wrong");

  expect(world.applyRpc(makePlayerStreamIn(7, 9, 411, 90.0f, -80.0f, 12.0f, 450.0f,
                                            0x11223344u, 5), event),
         "WorldState rejected PlayerStreamIn");
  expect(event.kind == samp::state::RpcEventKind::PlayerStreamedIn && world.streamedPlayerCount() == 1 &&
         world.remoteOnFootCount() == 0 && world.findPlayer(7)->streamedIn &&
         world.findPlayer(7)->model == 411 && world.findPlayer(7)->team == 9 &&
         std::fabs(world.findPlayer(7)->x - 90.0f) < 0.001f &&
         std::fabs(world.findPlayer(7)->angle - 90.0f) < 0.001f,
         "PlayerStreamIn actor state wrong");
  samp::syncdecode::RemoteOnFootSync remote;
  expect(samp::syncdecode::decodeRemoteOnFootBroadcast(makeRemoteOnFootBroadcast(7), remote) &&
         world.applyRemoteOnFoot(remote, event),
         "WorldState rejected decoded remote PlayerFootSync");
  expect(event.kind == samp::state::RpcEventKind::RemotePlayerSync && world.remoteOnFootCount() == 1 &&
         world.findPlayer(7)->remoteOnFootKnown && std::fabs(world.findPlayer(7)->x - 100.5f) < 0.001f &&
         std::fabs(world.findPlayer(7)->velocityY - 4.0f) < 0.01f,
         "remote PlayerFootSync actor state wrong");
  const auto validRemote = remote;
  remote.positionX = 20000.0f;
  expect(!world.applyRemoteOnFoot(remote, event) &&
         std::fabs(world.findPlayer(7)->x - 100.5f) < 0.001f,
         "remote state reducer must reject invalid direct state");
  remote = validRemote;
  expect(world.applyRpc(makePlayerStreamOut(7), event) &&
         event.kind == samp::state::RpcEventKind::PlayerStreamedOut && world.streamedPlayerCount() == 0 &&
         world.remoteOnFootCount() == 0 && !world.findPlayer(7)->streamedIn,
         "PlayerStreamOut actor state wrong");
  // Packet 207 must not invent an entity after the stream lifecycle says it
  // is out; a later StreamIn will explicitly reactivate it.
  expect(!world.applyRemoteOnFoot(remote, event),
         "orphan remote PlayerFootSync must be ignored");

  expect(world.applyRpc(makePlayerColor(7, 0x10203040u), event),
         "WorldState rejected SetPlayerColor");
  expect(world.findPlayer(7)->rgba == 0x10203040u, "color did not update");

  expect(world.applyRpc(makePlayerName(7, "Darius KG"), event),
         "WorldState rejected SetPlayerName");
  expect(world.findPlayer(7)->name == "Darius KG", "name did not update");

  expect(world.applyRpc(makePlayerQuit(7, 1), event), "WorldState rejected PlayerQuit");
  expect(event.kind == samp::state::RpcEventKind::PlayerQuit, "wrong quit event kind");
  expect(world.playerCount() == 0 && world.findPlayer(7) == nullptr,
         "quit did not remove player");
}

}  // namespace

int main() {
  testReliableAndAck();
  testUnreliableSequencedTransport();
  testPlayerFootSyncBuilder();
  testRemoteOnFootBroadcastDecoder();
  testNativeInputAndLocalPreview();
  testCompressedU32();
  testSplitReassembly();
  testPlayerInitDecoder();
  testClientRpcBuilders();
  testSessionSafetyWithoutSocket();
  testWorldStateReducer();
  std::puts("raknet codec/state regression tests: OK");
  return 0;
}
