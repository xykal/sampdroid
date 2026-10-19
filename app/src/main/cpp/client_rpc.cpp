#include "client_rpc.h"

#include "raknet_codec.h"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace samp::clientrpc {
namespace {

constexpr uint8_t kIdRpc = 20;
constexpr uint8_t kOnPlayerSelectObject = 27;
constexpr uint8_t kOnPlayerEditAttachedObject = 116;
constexpr uint8_t kOnPlayerEditObject = 117;
constexpr uint8_t kPlayerRequestCommandMessage = 50;
constexpr uint8_t kEnterVehicle = 26;
constexpr uint8_t kOnPlayerDialogResponse = 62;
constexpr uint8_t kPlayerSpawn = 52;
constexpr uint8_t kOnPlayerSelectedMenuRow = 132;
constexpr uint8_t kOnPlayerExitedMenu = 140;
constexpr uint8_t kSelectTextDraw = 83;
constexpr uint8_t kPlayerRequestChatMessage = 101;
constexpr uint8_t kPlayerRequestClass = 128;
constexpr uint8_t kPlayerRequestSpawn = 129;
constexpr uint8_t kPickedUpPickup = 131;
constexpr uint8_t kExitVehicle = 154;
constexpr uint8_t kRequestScoresAndPings = 155;
constexpr size_t kMaxChatBytes = 128;
constexpr size_t kMaxCommandBytes = 256;
constexpr size_t kMaxDialogResponseBytes = 255;

void writeFloat(raknet::BitWriter& writer, float value) {
  uint32_t raw = 0;
  static_assert(sizeof(raw) == sizeof(value), "unexpected float size");
  std::memcpy(&raw, &value, sizeof(raw));
  writer.writeBits(raw, 32);
}

bool validObjectSelectPosition(float x, float y, float z) {
  // Match the compatibility envelope applied to server-world object snapshots.
  // The caller gets these values only from the native reducer, but retain this
  // builder guard so a future use cannot encode NaN/Inf or an unbounded point.
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         x > -20000.0f && x < 20000.0f && y > -20000.0f && y < 20000.0f &&
         z > -1000.0f && z < 200000.0f;
}

bool validObjectEditRotation(float x, float y, float z) {
  // Keep the same broad angle envelope used by the reducer before normalizing
  // renderer transforms. SA-MP scripts can use more than one turn, but never
  // need values large enough to poison downstream matrix arithmetic.
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         std::fabs(x) <= 360000.0f && std::fabs(y) <= 360000.0f &&
         std::fabs(z) <= 360000.0f;
}

bool validAttachedObjectOffset(float x, float y, float z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         std::fabs(x) <= 10000.0f && std::fabs(y) <= 10000.0f &&
         std::fabs(z) <= 10000.0f;
}

bool validAttachedObjectScale(float x, float y, float z) {
  const auto validAxis = [](float value) {
    return std::isfinite(value) && std::fabs(value) >= 0.001f && std::fabs(value) <= 100.0f;
  };
  return validAxis(x) && validAxis(y) && validAxis(z);
}

bool hasUnsafeChatControlByte(const std::string& text) {
  for (const unsigned char value : text) {
    // Allow normal UTF-8 bytes but reject line framing/control characters.
    // Rendering/server formatting remains responsible for its own markup.
    if (value < 0x20u || value == 0x7Fu) return true;
  }
  return false;
}

bool hasUnsafeDialogControlByte(const std::string& text) {
  for (const unsigned char value : text) {
    // List and TablistHeaders dialogs conventionally return the selected
    // tab-separated row as inputText. Preserve a tab for that interoperable
    // case, while still rejecting NUL/newline and the remaining controls.
    if ((value < 0x20u && value != '\t') || value == 0x7Fu) return true;
  }
  return false;
}

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

std::vector<uint8_t> buildChatMessage(const std::string& message) {
  if (message.empty() || message.size() > kMaxChatBytes || hasUnsafeChatControlByte(message)) return {};
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kPlayerRequestChatMessage, 8);
  writer.writeBits(static_cast<uint32_t>(message.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(message.data()), message.size());
  return writer.buffer;
}

std::vector<uint8_t> buildCommandMessage(const std::string& command) {
  // The server's legacy handler expects the slash-prefixed command itself and
  // a u32 byte length; do not silently reclassify malformed chat as a command.
  if (command.size() < 2 || command.size() > kMaxCommandBytes || command.front() != '/' ||
      hasUnsafeChatControlByte(command)) {
    return {};
  }
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kPlayerRequestCommandMessage, 8);
  writer.writeBits(static_cast<uint32_t>(command.size()), 32);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(command.data()), command.size());
  return writer.buffer;
}

std::vector<uint8_t> buildDialogResponse(int16_t dialogId, bool response, int16_t listItem,
                                         const std::string& text) {
  if (text.size() > kMaxDialogResponseBytes || hasUnsafeDialogControlByte(text)) return {};
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kOnPlayerDialogResponse, 8);
  writer.writeBits(static_cast<uint16_t>(dialogId), 16);
  writer.writeBits(response ? 1u : 0u, 8);
  writer.writeBits(static_cast<uint16_t>(listItem), 16);
  writer.writeBits(static_cast<uint32_t>(text.size()), 8);
  if (!text.empty()) {
    writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }
  return writer.buffer;
}

std::vector<uint8_t> buildMenuSelect(uint8_t row) {
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kOnPlayerSelectedMenuRow, 8);
  writer.writeBits(row, 8);
  return writer.buffer;
}

std::vector<uint8_t> buildMenuExit() {
  return {kIdRpc, kOnPlayerExitedMenu};
}

std::vector<uint8_t> buildPickedUpPickup(int32_t pickupId) {
  if (pickupId < 0) return {};
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kPickedUpPickup, 8);
  writer.writeBits(static_cast<uint32_t>(pickupId), 32);
  return writer.buffer;
}

std::vector<uint8_t> buildEnterVehicle(uint16_t vehicleId, bool passenger) {
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kEnterVehicle, 8);
  writer.writeBits(vehicleId, 16);
  writer.writeBits(passenger ? 1u : 0u, 8);
  return writer.buffer;
}

std::vector<uint8_t> buildExitVehicle(uint16_t vehicleId) {
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kExitVehicle, 8);
  writer.writeBits(vehicleId, 16);
  return writer.buffer;
}

std::vector<uint8_t> buildSelectTextDraw(uint16_t wireId) {
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kSelectTextDraw, 8);
  writer.writeBits(wireId, 16);
  return writer.buffer;
}

std::vector<uint8_t> buildSelectObject(uint16_t objectId, uint32_t model,
                                       float hitX, float hitY, float hitZ) {
  // Current open.mp client object pools reserve ID zero; reject it along with
  // the wire's 0xFFFF sentinel rather than forging a selection callback.
  if (objectId == 0u || objectId == 0xFFFFu || model == 0u || model > 65535u ||
      !validObjectSelectPosition(hitX, hitY, hitZ)) {
    return {};
  }
  // Source audit: current open.mp's OnPlayerSelectObject::read consumes these
  // fields exactly. `SelectType` is retained for legacy compatibility; current
  // object-component handling resolves the shared object-ID pool by ID/model,
  // so Android emits the neutral `None` value rather than inventing a type.
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kOnPlayerSelectObject, 8);
  writer.writeBits(0u, 32);  // SelectType
  writer.writeBits(objectId, 16);
  writer.writeBits(model, 32);
  writeFloat(writer, hitX);
  writeFloat(writer, hitY);
  writeFloat(writer, hitZ);
  return writer.buffer;
}

std::vector<uint8_t> buildObjectEditResponse(bool playerObject, uint16_t objectId,
                                             uint32_t response, float x, float y, float z,
                                             float rotationX, float rotationY, float rotationZ) {
  // ObjectEditResponse values are Cancel=0, Final=1, Update=2 in the current
  // SDK. The session gates these against an active server-open edit tuple.
  if (objectId == 0u || objectId == 0xFFFFu || response > 2u ||
      !validObjectSelectPosition(x, y, z) ||
      !validObjectEditRotation(rotationX, rotationY, rotationZ)) {
    return {};
  }
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kOnPlayerEditObject, 8);
  writer.writeBit(playerObject);
  writer.writeBits(objectId, 16);
  writer.writeBits(response, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, rotationX);
  writeFloat(writer, rotationY);
  writeFloat(writer, rotationZ);
  return writer.buffer;
}

std::vector<uint8_t> buildAttachedObjectEditResponse(
    uint32_t response, uint32_t slot, uint32_t model, uint32_t bone,
    float offsetX, float offsetY, float offsetZ,
    float rotationX, float rotationY, float rotationZ,
    float scaleX, float scaleY, float scaleZ,
    uint32_t colour1Argb, uint32_t colour2Argb) {
  // The current object-component callback exposes this response as a boolean
  // `saved`, so preserve only its source-compatible cancel/save values.
  if (response > 1u || slot >= 10u || model == 0u || model > 65535u || bone > 18u ||
      !validAttachedObjectOffset(offsetX, offsetY, offsetZ) ||
      !validObjectEditRotation(rotationX, rotationY, rotationZ) ||
      !validAttachedObjectScale(scaleX, scaleY, scaleZ)) {
    return {};
  }
  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kOnPlayerEditAttachedObject, 8);
  writer.writeBits(response, 32);
  writer.writeBits(slot, 32);
  writer.writeBits(model, 32);
  writer.writeBits(bone, 32);
  writeFloat(writer, offsetX);
  writeFloat(writer, offsetY);
  writeFloat(writer, offsetZ);
  writeFloat(writer, rotationX);
  writeFloat(writer, rotationY);
  writeFloat(writer, rotationZ);
  writeFloat(writer, scaleX);
  writeFloat(writer, scaleY);
  writeFloat(writer, scaleZ);
  // Unlike SetPlayerAttachedObject's server->client ABGR fields, the current
  // OnPlayerEditAttachedObject reader calls Colour::FromARGB for both values.
  writer.writeBits(colour1Argb, 32);
  writer.writeBits(colour2Argb, 32);
  return writer.buffer;
}

std::vector<uint8_t> buildRequestScoresAndPings() {
  return {kIdRpc, kRequestScoresAndPings};
}

}  // namespace samp::clientrpc
