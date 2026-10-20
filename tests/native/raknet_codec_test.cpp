#include "client_rpc.h"
#include "client_sync.h"
#include "gta_collision.h"
#include "gta_collision_catalog.h"
#include "gta_collision_world.h"
#include "gta_data_runtime.h"
#include "gta_dff_mesh.h"
#include "gta_file_source.h"
#include "gta_img_archive.h"
#include "gta_map_manifest.h"
#include "gta_txd_texture.h"
#include "gta_vehicle_handling.h"
#include "input_state.h"
#include "local_gameplay_bridge.h"
#include "local_movement.h"
#include "local_vehicle_movement.h"
#include "local_player_sync.h"
#include "raknet_codec.h"
#include "raknet_handshake.h"
#include "raknet_string_compressor.h"
#include "raknet_session.h"
#include "raknet_session_policy.h"
#include "samp_query.h"
#include "sync_policy.h"
#include "sync_decoder.h"
#include "rpc_decoder.h"
#include "rpc_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::exit(1);
}

[[noreturn]] void fail(const std::string& message) {
  fail(message.c_str());
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

void writeLe16At(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  if (offset + 2 > bytes.size()) fail("test fixture u16 write overflow");
  bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32At(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  if (offset + 4 > bytes.size()) fail("test fixture u32 write overflow");
  bytes[offset] = static_cast<uint8_t>(value & 0xFFu);
  bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void writeFloatAt(std::vector<uint8_t>& bytes, size_t offset, float value) {
  uint32_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  writeLe32At(bytes, offset, raw);
}

void writeVec3At(std::vector<uint8_t>& bytes, size_t offset, float x, float y, float z) {
  writeFloatAt(bytes, offset, x);
  writeFloatAt(bytes, offset + 4, y);
  writeFloatAt(bytes, offset + 8, z);
}

void writeAsciiAt(std::vector<uint8_t>& bytes, size_t offset, const std::string& value) {
  if (offset + value.size() > bytes.size()) fail("test fixture string write overflow");
  std::memcpy(bytes.data() + offset, value.data(), value.size());
}

std::vector<uint8_t> makeImgV2Fixture() {
  // Header + two 32-byte entries, followed by sector-aligned payloads.
  std::vector<uint8_t> bytes(3 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 2);
  writeLe32At(bytes, 8, 1);       // entry 0 sector offset
  writeLe16At(bytes, 12, 1);      // entry 0 streaming size
  writeAsciiAt(bytes, 16, "TEST.COL");
  writeLe32At(bytes, 40, 2);      // entry 1 sector offset
  writeLe16At(bytes, 44, 1);      // entry 1 streaming size
  writeAsciiAt(bytes, 48, "PLAYER.DFF");
  bytes[2048] = 0xC0;
  bytes[2049] = 0x13;
  bytes[4096] = 0xD0;
  bytes[4097] = 0x0D;
  return bytes;
}

std::vector<uint8_t> makeCol3Fixture() {
  // One named COL3 model: an axis-aligned box at z=1..2 plus one floor
  // triangle at z=0. The file is sector-padded exactly as an IMG entry is.
  constexpr size_t kHeader = 120;
  constexpr size_t kBoxOffset = kHeader;
  constexpr size_t kVertexOffset = kBoxOffset + 28;
  constexpr size_t kFaceOffset = kVertexOffset + 18;
  constexpr size_t kModelBytes = kFaceOffset + 8;
  std::vector<uint8_t> bytes(2048, 0);
  writeAsciiAt(bytes, 0, "COL3");
  writeLe32At(bytes, 4, static_cast<uint32_t>(kModelBytes - 8));
  writeAsciiAt(bytes, 8, "testfloor");
  writeVec3At(bytes, 32, -2.0f, -2.0f, -1.0f);  // bounds min
  writeVec3At(bytes, 44, 2.0f, 2.0f, 3.0f);     // bounds max
  writeVec3At(bytes, 56, 0.0f, 0.0f, 0.0f);     // bound sphere center
  writeFloatAt(bytes, 68, 4.0f);                // bound sphere radius
  writeLe16At(bytes, 72, 0);                    // spheres
  writeLe16At(bytes, 74, 1);                    // boxes
  writeLe32At(bytes, 76, 1);                    // mesh faces
  writeLe32At(bytes, 80, 0);                    // flags
  writeLe32At(bytes, 88, static_cast<uint32_t>(kBoxOffset - 4));
  writeLe32At(bytes, 96, static_cast<uint32_t>(kVertexOffset - 4));
  writeLe32At(bytes, 100, static_cast<uint32_t>(kFaceOffset - 4));
  writeVec3At(bytes, kBoxOffset, -0.25f, -0.25f, 1.0f);
  writeVec3At(bytes, kBoxOffset + 12, 0.25f, 0.25f, 2.0f);
  bytes[kBoxOffset + 24] = 7;  // material
  writeLe16At(bytes, kVertexOffset, static_cast<uint16_t>(-128));
  writeLe16At(bytes, kVertexOffset + 2, static_cast<uint16_t>(-128));
  writeLe16At(bytes, kVertexOffset + 4, 0);
  writeLe16At(bytes, kVertexOffset + 6, 128);
  writeLe16At(bytes, kVertexOffset + 8, static_cast<uint16_t>(-128));
  writeLe16At(bytes, kVertexOffset + 10, 0);
  writeLe16At(bytes, kVertexOffset + 12, static_cast<uint16_t>(-128));
  writeLe16At(bytes, kVertexOffset + 14, 128);
  writeLe16At(bytes, kVertexOffset + 16, 0);
  writeLe16At(bytes, kFaceOffset, 0);
  writeLe16At(bytes, kFaceOffset + 2, 1);
  writeLe16At(bytes, kFaceOffset + 4, 2);
  bytes[kFaceOffset + 6] = 15;  // material
  bytes[kFaceOffset + 7] = 3;   // light
  return bytes;
}

std::vector<uint8_t> makeWallCol3Fixture() {
  std::vector<uint8_t> bytes = makeCol3Fixture();
  // Preserve the small fixture's floor triangle, but turn its collision box
  // into a standing wall for the horizontal player-cylinder regression.
  std::fill(bytes.begin() + 8, bytes.begin() + 30, 0);
  writeAsciiAt(bytes, 8, "wall");
  writeVec3At(bytes, 120, 0.15f, -2.0f, -1.0f);
  writeVec3At(bytes, 132, 0.45f, 2.0f, 3.0f);
  return bytes;
}

std::vector<uint8_t> makeImgWithCol3Fixture() {
  const std::vector<uint8_t> col = makeCol3Fixture();
  std::vector<uint8_t> bytes(2 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 1);
  writeLe32At(bytes, 8, 1);   // first payload sector
  writeLe16At(bytes, 12, 1);  // one padded sector
  writeAsciiAt(bytes, 16, "TEST.COL");
  std::memcpy(bytes.data() + 2048, col.data(), col.size());
  return bytes;
}

std::vector<uint8_t> makeImgWithNamedCol3Fixture(const std::string& memberName,
                                                  const std::string& modelName) {
  if (memberName.empty() || memberName.size() >= 24 || modelName.empty() || modelName.size() >= 22) {
    fail("named IMG COL fixture name overflow");
  }
  std::vector<uint8_t> col = makeCol3Fixture();
  std::fill(col.begin() + 8, col.begin() + 30, 0);
  writeAsciiAt(col, 8, modelName);
  std::vector<uint8_t> bytes(2 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 1);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, memberName);
  std::memcpy(bytes.data() + 2048, col.data(), col.size());
  return bytes;
}

std::vector<uint8_t> makeImgWithColEntryCount(size_t count) {
  if (count == 0 || count > 999u) fail("invalid IMG COL-count fixture size");
  const std::vector<uint8_t> col = makeCol3Fixture();
  const size_t directoryEnd = 8u + count * 32u;
  const size_t firstPayloadSector = (directoryEnd + 2047u) / 2048u;
  std::vector<uint8_t> bytes((firstPayloadSector + count) * 2048u, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, static_cast<uint32_t>(count));
  for (size_t index = 0; index < count; ++index) {
    const size_t entry = 8u + index * 32u;
    writeLe32At(bytes, entry, static_cast<uint32_t>(firstPayloadSector + index));
    writeLe16At(bytes, entry + 4u, 1u);
    char name[24]{};
    const int written = std::snprintf(name, sizeof(name), "C%03zu.COL", index);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(name)) {
      fail("IMG COL-count fixture name overflow");
    }
    writeAsciiAt(bytes, entry + 8u, name);
    std::memcpy(bytes.data() + (firstPayloadSector + index) * 2048u, col.data(), col.size());
  }
  return bytes;
}

std::vector<uint8_t> makeBinaryIplFixture(uint32_t modelId = 1000u,
                                          uint32_t interiorFlags = 3u) {
  // SA binary IPL: "bnry", six u32 counts, six offset/size pairs (76-byte
  // header), then one 40-byte INST. Only INST is intentionally exercised.
  std::vector<uint8_t> bytes(76u + 40u, 0);
  writeAsciiAt(bytes, 0, "bnry");
  writeLe32At(bytes, 4, 1);   // INST count
  writeLe32At(bytes, 28, 76); // INST offset
  writeLe32At(bytes, 32, 0);  // Rockstar files normally leave size at zero
  writeFloatAt(bytes, 76, 10.0f);
  writeFloatAt(bytes, 80, -20.0f);
  writeFloatAt(bytes, 84, 5.0f);
  writeFloatAt(bytes, 88, 0.0f);
  writeFloatAt(bytes, 92, 0.0f);
  writeFloatAt(bytes, 96, 0.0f);
  writeFloatAt(bytes, 100, 1.0f);
  writeLe32At(bytes, 104, modelId);
  writeLe32At(bytes, 108, interiorFlags);
  writeLe32At(bytes, 112, 0xFFFFFFFFu);  // LOD -1
  return bytes;
}

std::vector<uint8_t> makeImgWithBinaryStreamingIplFixture() {
  const std::vector<uint8_t> ipl = makeBinaryIplFixture(1000u, 0u);
  if (ipl.size() > 2048) fail("binary IPL fixture unexpectedly exceeds one IMG sector");
  std::vector<uint8_t> bytes(2 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 1);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, "LAE2_STREAM0.IPL");
  std::memcpy(bytes.data() + 2048, ipl.data(), ipl.size());
  return bytes;
}

void appendLe16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void appendLe32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
  bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

void appendFloat(std::vector<uint8_t>& bytes, float value) {
  uint32_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  appendLe32(bytes, raw);
}

void appendRwChunk(std::vector<uint8_t>& output, uint32_t type,
                   const std::vector<uint8_t>& payload) {
  appendLe32(output, type);
  appendLe32(output, static_cast<uint32_t>(payload.size()));
  appendLe32(output, 0x1803FFFFu);  // common SA-era RenderWare stream version
  output.insert(output.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> makePortableDffFixture(uint32_t geometryFormat = 0x00000006u,
                                            const std::string& textureKey = "TestTex") {
  // One GeometryList → Geometry → Struct with a single portable triangle.
  if (textureKey.empty() || textureKey.size() > 64) fail("portable DFF fixture texture key invalid");
  std::vector<uint8_t> geometryStruct;
  appendLe32(geometryStruct, geometryFormat);  // positions + first UV set
  appendLe32(geometryStruct, 1);             // triangles
  appendLe32(geometryStruct, 3);             // vertices
  appendLe32(geometryStruct, 1);             // morph targets
  if ((geometryFormat & 0x00000084u) != 0u) {
    appendFloat(geometryStruct, 0.0f);       // UV 0
    appendFloat(geometryStruct, 0.0f);
    appendFloat(geometryStruct, 1.0f);       // UV 1
    appendFloat(geometryStruct, 0.0f);
    appendFloat(geometryStruct, 0.0f);       // UV 2
    appendFloat(geometryStruct, 1.0f);
  }
  appendLe16(geometryStruct, 1);             // vertex2
  appendLe16(geometryStruct, 0);             // vertex1
  appendLe16(geometryStruct, 0);             // material
  appendLe16(geometryStruct, 2);             // vertex3
  appendFloat(geometryStruct, 0.0f);         // morph bounding sphere
  appendFloat(geometryStruct, 0.0f);
  appendFloat(geometryStruct, 0.0f);
  appendFloat(geometryStruct, 2.0f);
  appendLe32(geometryStruct, 1);             // has vertices
  appendLe32(geometryStruct, 0);             // has normals
  appendFloat(geometryStruct, -1.0f);
  appendFloat(geometryStruct, -1.0f);
  appendFloat(geometryStruct, 0.0f);
  appendFloat(geometryStruct, 1.0f);
  appendFloat(geometryStruct, -1.0f);
  appendFloat(geometryStruct, 0.0f);
  appendFloat(geometryStruct, -1.0f);
  appendFloat(geometryStruct, 1.0f);
  appendFloat(geometryStruct, 0.0f);

  std::vector<uint8_t> materialStruct;
  appendLe32(materialStruct, 0);  // flags
  materialStruct.insert(materialStruct.end(), {128, 200, 255, 255});
  appendLe32(materialStruct, 0);  // unused
  appendLe32(materialStruct, 1);  // textured
  std::vector<uint8_t> texturePayload;
  std::vector<uint8_t> textureStruct;
  appendLe32(textureStruct, 0);  // filter/addressing
  appendRwChunk(texturePayload, 0x01, textureStruct);
  std::vector<uint8_t> textureName(textureKey.begin(), textureKey.end());
  textureName.push_back(0);
  appendRwChunk(texturePayload, 0x02, textureName);
  std::vector<uint8_t> materialPayload;
  appendRwChunk(materialPayload, 0x01, materialStruct);
  appendRwChunk(materialPayload, 0x06, texturePayload);
  std::vector<uint8_t> materialListStruct;
  appendLe32(materialListStruct, 1);
  appendLe32(materialListStruct, 0xFFFFFFFFu);  // base material, not an instance
  std::vector<uint8_t> materialListPayload;
  appendRwChunk(materialListPayload, 0x01, materialListStruct);
  appendRwChunk(materialListPayload, 0x07, materialPayload);

  std::vector<uint8_t> geometryPayload;
  appendRwChunk(geometryPayload, 0x01, geometryStruct);
  appendRwChunk(geometryPayload, 0x08, materialListPayload);
  std::vector<uint8_t> geometryListPayload;
  std::vector<uint8_t> countPayload;
  appendLe32(countPayload, 1);
  appendRwChunk(geometryListPayload, 0x01, countPayload);
  appendRwChunk(geometryListPayload, 0x0F, geometryPayload);
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x1A, geometryListPayload);
  return file;
}

std::vector<uint8_t> makeMixedPortableDffFixture(const std::string& textureKey = "TestTex") {
  // Two Geometry chunks in one GeometryList: the first is normally textured,
  // the second intentionally has positions but no UVs and an unrelated key.
  // This models stock multi-mesh assets where an untextured detail must not
  // prevent a safe overlay for a separate textured material.
  const std::vector<uint8_t> textured = makePortableDffFixture(0x00000006u, textureKey);
  const std::vector<uint8_t> untextured = makePortableDffFixture(0x00000002u, "UnmatchedTex");
  constexpr size_t kGeometryListHeaderAndCount = 28u;
  if (textured.size() <= kGeometryListHeaderAndCount ||
      untextured.size() <= kGeometryListHeaderAndCount) {
    fail("portable mixed DFF fixture layout invalid");
  }
  std::vector<uint8_t> payload;
  std::vector<uint8_t> count;
  appendLe32(count, 2);
  appendRwChunk(payload, 0x01, count);
  payload.insert(payload.end(), textured.begin() + kGeometryListHeaderAndCount, textured.end());
  payload.insert(payload.end(), untextured.begin() + kGeometryListHeaderAndCount, untextured.end());
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x1A, payload);
  return file;
}

// Defined below; declare it here so the compact multi-texture fixture can reuse
// the exact same portable D3D9 raster encoder.
std::vector<uint8_t> makePortableTxdFixture(const std::string& textureKey);

// Builds one portable GeometryList with two independently textured meshes.
// It exercises material selection across a single DFF, not merely two nearby
// DFF placements that happen to have different dictionaries.
std::vector<uint8_t> makeTwoTexturedPortableDffFixture(const std::string& firstTexture,
                                                        const std::string& secondTexture) {
  const std::vector<uint8_t> first = makePortableDffFixture(0x00000006u, firstTexture);
  const std::vector<uint8_t> second = makePortableDffFixture(0x00000006u, secondTexture);
  constexpr size_t kGeometryListHeaderAndCount = 28u;
  if (first.size() <= kGeometryListHeaderAndCount || second.size() <= kGeometryListHeaderAndCount) {
    fail("two-texture DFF fixture layout invalid");
  }
  std::vector<uint8_t> payload;
  std::vector<uint8_t> count;
  appendLe32(count, 2);
  appendRwChunk(payload, 0x01, count);
  payload.insert(payload.end(), first.begin() + kGeometryListHeaderAndCount, first.end());
  payload.insert(payload.end(), second.begin() + kGeometryListHeaderAndCount, second.end());
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x1A, payload);
  return file;
}

// A TXD dictionary is one outer chunk, one 4-byte count/device struct, and a
// sequence of TextureNative chunks. Reusing the portable one-texture fixtures
// gives a compact exact two-material source vector without a hand-wavy fake.
std::vector<uint8_t> makeTwoTextureTxdFixture(const std::string& firstTexture,
                                               const std::string& secondTexture) {
  const std::vector<uint8_t> first = makePortableTxdFixture(firstTexture);
  const std::vector<uint8_t> second = makePortableTxdFixture(secondTexture);
  constexpr size_t kDictionaryHeaderAndStruct = 28u;
  if (first.size() <= kDictionaryHeaderAndStruct || second.size() <= kDictionaryHeaderAndStruct) {
    fail("two-texture TXD fixture layout invalid");
  }
  std::vector<uint8_t> dictionaryPayload;
  std::vector<uint8_t> dictionaryStruct;
  appendLe16(dictionaryStruct, 2);  // texture count
  appendLe16(dictionaryStruct, 9);  // D3D9 device ID
  appendRwChunk(dictionaryPayload, 0x01, dictionaryStruct);
  dictionaryPayload.insert(dictionaryPayload.end(),
                           first.begin() + kDictionaryHeaderAndStruct, first.end());
  dictionaryPayload.insert(dictionaryPayload.end(),
                           second.begin() + kDictionaryHeaderAndStruct, second.end());
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x16, dictionaryPayload);
  return file;
}

std::vector<uint8_t> makeImgWithMultiMaterialDffAndTxdFixture() {
  const std::vector<uint8_t> dff = makeTwoTexturedPortableDffFixture("OneTex", "TwoTex");
  const std::vector<uint8_t> txd = makeTwoTextureTxdFixture("OneTex", "TwoTex");
  if (dff.size() > 2048 || txd.size() > 2048) {
    fail("multi-material DFF/TXD fixture unexpectedly exceeds IMG entry bounds");
  }
  std::vector<uint8_t> bytes(3 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 2);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, "MULTI.DFF");
  writeLe32At(bytes, 40, 2);
  writeLe16At(bytes, 44, 1);
  writeAsciiAt(bytes, 48, "MULTI.TXD");
  std::memcpy(bytes.data() + 2048, dff.data(), dff.size());
  std::memcpy(bytes.data() + 4096, txd.data(), txd.size());
  return bytes;
}

std::vector<uint8_t> makeFrameAtomicDffFixture(bool selfReferentialFrame = false) {
  const std::vector<uint8_t> geometryList = makePortableDffFixture();
  std::vector<uint8_t> frames;
  appendLe32(frames, 2);  // root + geometry frame
  const auto appendFrame = [](std::vector<uint8_t>& output, float x, float y, float z,
                              uint32_t parent) {
    appendFloat(output, 1.0f); appendFloat(output, 0.0f); appendFloat(output, 0.0f);  // right
    appendFloat(output, 0.0f); appendFloat(output, 1.0f); appendFloat(output, 0.0f);  // up
    appendFloat(output, 0.0f); appendFloat(output, 0.0f); appendFloat(output, 1.0f);  // at
    appendFloat(output, x); appendFloat(output, y); appendFloat(output, z);            // position
    appendLe32(output, parent);
    appendLe32(output, 0);  // frame flags
  };
  appendFrame(frames, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
  appendFrame(frames, 10.0f, 20.0f, 30.0f, selfReferentialFrame ? 1u : 0u);
  std::vector<uint8_t> frameListPayload;
  appendRwChunk(frameListPayload, 0x01, frames);

  std::vector<uint8_t> atomicStruct;
  appendLe32(atomicStruct, 1);  // geometry frame
  appendLe32(atomicStruct, 0);  // GeometryList index
  appendLe32(atomicStruct, 5);  // renderable atomic flags
  appendLe32(atomicStruct, 0);
  std::vector<uint8_t> atomicPayload;
  appendRwChunk(atomicPayload, 0x01, atomicStruct);

  std::vector<uint8_t> clumpStruct;
  appendLe32(clumpStruct, 1);  // atomics
  appendLe32(clumpStruct, 0);  // lights
  appendLe32(clumpStruct, 0);  // cameras
  std::vector<uint8_t> clumpPayload;
  appendRwChunk(clumpPayload, 0x01, clumpStruct);
  appendRwChunk(clumpPayload, 0x0E, frameListPayload);
  // The portable fixture is already one complete GeometryList chunk.
  clumpPayload.insert(clumpPayload.end(), geometryList.begin(), geometryList.end());
  appendRwChunk(clumpPayload, 0x14, atomicPayload);
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x10, clumpPayload);
  return file;
}

std::vector<uint8_t> makePortableTxdFixture(const std::string& textureKey = "TestTex") {
  if (textureKey.empty() || textureKey.size() > 31) fail("portable TXD fixture texture key invalid");
  std::vector<uint8_t> rasterStruct(88, 0);
  writeLe32At(rasterStruct, 0, 9);       // GTA SA PC D3D9 platform
  writeLe32At(rasterStruct, 72, 0x0500); // RASTER_8888
  writeLe32At(rasterStruct, 76, 0);
  writeLe16At(rasterStruct, 80, 2);
  writeLe16At(rasterStruct, 82, 2);
  rasterStruct[84] = 32;  // depth
  rasterStruct[85] = 1;   // mip levels
  writeAsciiAt(rasterStruct, 8, textureKey);
  appendLe32(rasterStruct, 16);  // top mip byte size
  // BGRA texels: red, green, blue, white.
  rasterStruct.insert(rasterStruct.end(), {
      0, 0, 255, 255,
      0, 255, 0, 255,
      255, 0, 0, 255,
      255, 255, 255, 255,
  });

  std::vector<uint8_t> rasterPayload;
  appendRwChunk(rasterPayload, 0x01, rasterStruct);
  std::vector<uint8_t> dictionaryPayload;
  std::vector<uint8_t> dictionaryStruct;
  appendLe16(dictionaryStruct, 1);  // texture count
  appendLe16(dictionaryStruct, 9);  // D3D9 device ID
  appendRwChunk(dictionaryPayload, 0x01, dictionaryStruct);
  appendRwChunk(dictionaryPayload, 0x15, rasterPayload);
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x16, dictionaryPayload);
  return file;
}

std::vector<uint8_t> makeEmptyPortableTxdFixture() {
  std::vector<uint8_t> dictionaryStruct;
  appendLe16(dictionaryStruct, 0);  // valid stock placeholder: no rasters
  appendLe16(dictionaryStruct, 9);  // D3D9 device ID
  std::vector<uint8_t> dictionaryPayload;
  appendRwChunk(dictionaryPayload, 0x01, dictionaryStruct);
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x16, dictionaryPayload);
  return file;
}

std::vector<uint8_t> makePortableDxt1TxdFixture() {
  std::vector<uint8_t> rasterStruct(88, 0);
  writeLe32At(rasterStruct, 0, 9);
  writeLe32At(rasterStruct, 72, 0x0200);      // RASTER_565 / DXT1 opaque convention
  writeLe32At(rasterStruct, 76, 0x31545844);  // DXT1 FOURCC
  writeLe16At(rasterStruct, 80, 4);
  writeLe16At(rasterStruct, 82, 4);
  rasterStruct[84] = 16;
  rasterStruct[85] = 1;
  rasterStruct[87] = 0x08;  // compressed flag
  writeAsciiAt(rasterStruct, 8, "DxtRed");
  appendLe32(rasterStruct, 8);
  appendLe16(rasterStruct, 0xF800);  // endpoint red
  appendLe16(rasterStruct, 0x0000);
  appendLe32(rasterStruct, 0);       // all 16 pixels select endpoint 0

  std::vector<uint8_t> rasterPayload;
  appendRwChunk(rasterPayload, 0x01, rasterStruct);
  std::vector<uint8_t> dictionaryPayload;
  std::vector<uint8_t> dictionaryStruct;
  appendLe16(dictionaryStruct, 1);
  appendLe16(dictionaryStruct, 9);
  appendRwChunk(dictionaryPayload, 0x01, dictionaryStruct);
  appendRwChunk(dictionaryPayload, 0x15, rasterPayload);
  std::vector<uint8_t> file;
  appendRwChunk(file, 0x16, dictionaryPayload);
  return file;
}

std::vector<uint8_t> makeImgWithTxdFixture() {
  const std::vector<uint8_t> txd = makePortableTxdFixture();
  if (txd.size() > 2048) fail("portable TXD fixture unexpectedly exceeds one IMG sector");
  std::vector<uint8_t> bytes(2 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 1);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, "TEST.TXD");
  std::memcpy(bytes.data() + 2048, txd.data(), txd.size());
  return bytes;
}

std::vector<uint8_t> makeImgWithNamedTxdFixture(const std::string& memberName,
                                                   const std::string& textureKey = "TestTex",
                                                   bool empty = false) {
  const std::vector<uint8_t> txd = empty ? makeEmptyPortableTxdFixture()
                                          : makePortableTxdFixture(textureKey);
  if (txd.size() > 2048 || memberName.empty() || memberName.size() > 24) {
    fail("named TXD fixture unexpectedly exceeds IMG entry bounds");
  }
  std::vector<uint8_t> bytes(2 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 1);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, memberName);
  std::memcpy(bytes.data() + 2048, txd.data(), txd.size());
  return bytes;
}

std::vector<uint8_t> makeImgWithDffFixture(const std::string& memberName = "TEST.DFF") {
  const std::vector<uint8_t> dff = makePortableDffFixture();
  if (dff.size() > 2048 || memberName.empty() || memberName.size() > 24) {
    fail("portable DFF fixture unexpectedly exceeds IMG entry bounds");
  }
  std::vector<uint8_t> bytes(2 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 1);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, memberName);
  std::memcpy(bytes.data() + 2048, dff.data(), dff.size());
  return bytes;
}

std::vector<uint8_t> makeImgWithDffAndTxdFixture(const std::string& dffName = "TEST.DFF",
                                                  const std::string& txdName = "TEST.TXD") {
  const std::vector<uint8_t> dff = makePortableDffFixture();
  const std::vector<uint8_t> txd = makePortableTxdFixture();
  if (dff.size() > 2048 || txd.size() > 2048 || dffName.empty() || txdName.empty() ||
      dffName.size() > 24 || txdName.size() > 24) {
    fail("portable DFF/TXD fixture unexpectedly exceeds IMG entry bounds");
  }
  std::vector<uint8_t> bytes(3 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 2);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, dffName);
  writeLe32At(bytes, 40, 2);
  writeLe16At(bytes, 44, 1);
  writeAsciiAt(bytes, 48, txdName);
  std::memcpy(bytes.data() + 2048, dff.data(), dff.size());
  std::memcpy(bytes.data() + 4096, txd.data(), txd.size());
  return bytes;
}

std::vector<uint8_t> makeImgWithTwoDffAndTxdFixture() {
  const std::vector<uint8_t> firstDff = makePortableDffFixture(0x00000006u, "OneTex");
  const std::vector<uint8_t> firstTxd = makePortableTxdFixture("OneTex");
  const std::vector<uint8_t> secondDff = makePortableDffFixture(0x00000006u, "TwoTex");
  const std::vector<uint8_t> secondTxd = makePortableTxdFixture("TwoTex");
  if (firstDff.size() > 2048 || firstTxd.size() > 2048 || secondDff.size() > 2048 ||
      secondTxd.size() > 2048) {
    fail("two-material DFF/TXD fixture unexpectedly exceeds IMG entry bounds");
  }
  std::vector<uint8_t> bytes(5 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 4);
  const std::array<std::pair<const char*, const std::vector<uint8_t>*>, 4> entries = {{
      {"TESTONE.DFF", &firstDff}, {"ONE.TXD", &firstTxd},
      {"TESTTWO.DFF", &secondDff}, {"TWO.TXD", &secondTxd},
  }};
  for (size_t index = 0; index < entries.size(); ++index) {
    const size_t directory = 8u + index * 32u;
    writeLe32At(bytes, directory, static_cast<uint32_t>(index + 1u));
    writeLe16At(bytes, directory + 4u, 1);
    writeAsciiAt(bytes, directory + 8u, entries[index].first);
    std::memcpy(bytes.data() + (index + 1u) * 2048u, entries[index].second->data(),
                entries[index].second->size());
  }
  return bytes;
}

std::vector<uint8_t> makeImgWithDffAndEmptyTxdFixture(
    const std::string& dffName = "TESTFLOOR.DFF", const std::string& txdName = "CHILD.TXD") {
  const std::vector<uint8_t> dff = makePortableDffFixture();
  const std::vector<uint8_t> txd = makeEmptyPortableTxdFixture();
  if (dff.size() > 2048 || txd.size() > 2048 || dffName.empty() || txdName.empty() ||
      dffName.size() > 24 || txdName.size() > 24) {
    fail("DFF/empty-TXD fixture unexpectedly exceeds IMG entry bounds");
  }
  std::vector<uint8_t> bytes(3 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 2);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, dffName);
  writeLe32At(bytes, 40, 2);
  writeLe16At(bytes, 44, 1);
  writeAsciiAt(bytes, 48, txdName);
  std::memcpy(bytes.data() + 2048, dff.data(), dff.size());
  std::memcpy(bytes.data() + 4096, txd.data(), txd.size());
  return bytes;
}

std::vector<uint8_t> makeImgWithTxdParentFixture() {
  const std::vector<uint8_t> dff = makeMixedPortableDffFixture("TestTex");
  const std::vector<uint8_t> child = makeEmptyPortableTxdFixture();
  const std::vector<uint8_t> parent = makePortableTxdFixture("TestTex");
  if (dff.size() > 2048 || child.size() > 2048 || parent.size() > 2048) {
    fail("portable TXD-parent fixture unexpectedly exceeds IMG entry bounds");
  }
  // Directory sector + one sector for the DFF, empty child dictionary, and
  // parent dictionary. The child intentionally lacks TestTex.
  std::vector<uint8_t> bytes(4 * 2048, 0);
  writeAsciiAt(bytes, 0, "VER2");
  writeLe32At(bytes, 4, 3);
  writeLe32At(bytes, 8, 1);
  writeLe16At(bytes, 12, 1);
  writeAsciiAt(bytes, 16, "TESTFLOOR.DFF");
  writeLe32At(bytes, 40, 2);
  writeLe16At(bytes, 44, 1);
  writeAsciiAt(bytes, 48, "CHILD.TXD");
  writeLe32At(bytes, 72, 3);
  writeLe16At(bytes, 76, 1);
  writeAsciiAt(bytes, 80, "PARENT.TXD");
  std::memcpy(bytes.data() + 2048, dff.data(), dff.size());
  std::memcpy(bytes.data() + 4096, child.data(), child.size());
  std::memcpy(bytes.data() + 6144, parent.data(), parent.size());
  return bytes;
}

std::vector<uint8_t> makePlayerInit(bool manualVehicleEngineAndLights = false) {
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
  writer.writeBit(manualVehicleEngineAndLights);  // ManualVehicleEngineAndLights
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

std::vector<uint8_t> makeSetPlayerChatBubble(uint16_t id, uint32_t rgba, float drawDistance,
                                             uint32_t expireTime, const std::string& text) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 59};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(rgba, 32);
  writeFloat(writer, drawDistance);
  writer.writeBits(expireTime, 32);
  writer.writeBits(static_cast<uint32_t>(text.size()), 8);
  if (!text.empty()) {
    writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }
  return writer.buffer;
}

std::vector<uint8_t> makePlayerChatMessage(uint16_t id, const std::string& text) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 101};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(static_cast<uint32_t>(text.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  return writer.buffer;
}

std::vector<uint8_t> makeShowDialog(int16_t id, uint8_t style, const std::string& title,
                                    const std::string& button1, const std::string& button2) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 61};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(static_cast<uint16_t>(id), 16);
  if (id == -1) return writer.buffer;
  writer.writeBits(style, 8);
  const auto writeString8 = [&writer](const std::string& value) {
    writer.writeBits(static_cast<uint32_t>(value.size()), 8);
    if (!value.empty()) {
      writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
  };
  writeString8(title);
  writeString8(button1);
  writeString8(button2);
  // Default RakNet StringCompressor fixture for "Hello" (42 bits total,
  // including the compressed-u16 bit count). Do not byte-align it: the body
  // is an exact bit stream in ShowDialog.
  const std::vector<uint8_t> compressedHello = {0x88, 0x1F, 0x02, 0x52, 0x85, 0x40};
  samp::raknet::BitReader fixture(compressedHello.data(), compressedHello.size());
  for (int bit = 0; bit < 42; ++bit) writer.writeBit(fixture.readBit());
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

// RPC 166 is observer PlayerDeath: its exact server-to-client body is only a
// remote u16 player ID, not a local death reason/killer report.
std::vector<uint8_t> makeRemotePlayerDeath(uint16_t id) {
  return {20, 166, static_cast<uint8_t>(id & 0xFFu), static_cast<uint8_t>(id >> 8u)};
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

// Non-DL SetPlayerSkin is source-verified as PlayerID:u32 | Skin:u32. The
// u32 player ID is intentional and must not be accidentally encoded as the
// u16 used by the nearby StreamIn/StreamOut RPCs.
std::vector<uint8_t> makeSetPlayerSkin(uint32_t id, uint32_t model) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 153};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 32);
  writer.writeBits(model, 32);
  return writer.buffer;
}

// Non-DL ShowActorForPlayer is source-verified as ActorID:u16 | SkinID:u32 |
// VEC3 | Angle:float | Health:float | Invulnerable:u8. It shares neither the
// player pool nor Packet-207 state with a remote player.
std::vector<uint8_t> makeShowActor(uint16_t id, uint32_t model, float x, float y, float z,
                                   float angle, float health, uint8_t invulnerable) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 171};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(model, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, angle);
  writeFloat(writer, health);
  writer.writeBits(invulnerable, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeHideActor(uint16_t id) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 172};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  return writer.buffer;
}

std::vector<uint8_t> makeSetActorAngle(uint16_t id, float angle) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 175};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writeFloat(writer, angle);
  return writer.buffer;
}

std::vector<uint8_t> makeSetActorPosition(uint16_t id, float x, float y, float z) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 176};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  return writer.buffer;
}

std::vector<uint8_t> makeSetActorHealth(uint16_t id, float health) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 178};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writeFloat(writer, health);
  return writer.buffer;
}

// Current open.mp ApplyPlayerAnimation (86) / ApplyActorAnimation (173) are
// entity-id, two DynStr8 tokens, float delta, four packed BITs, then u32 time.
// Do not byte-align after the four flags: the u32 begins at bit offset four.
std::vector<uint8_t> makeApplyAnimation(uint8_t rpcId, uint16_t id, const std::string& library,
                                        const std::string& name, float delta, bool loop, bool lockX,
                                        bool lockY, bool freeze, uint32_t durationMs) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(static_cast<uint32_t>(library.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(library.data()), library.size());
  writer.writeBits(static_cast<uint32_t>(name.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  writeFloat(writer, delta);
  writer.writeBit(loop);
  writer.writeBit(lockX);
  writer.writeBit(lockY);
  writer.writeBit(freeze);
  writer.writeBits(durationMs, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeClearAnimation(uint8_t rpcId, uint16_t id) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  return writer.buffer;
}

std::vector<uint8_t> makeStreamInVehicle(uint16_t id, uint32_t model, float x, float y, float z,
                                         float angle, uint8_t colour1 = 1, uint8_t colour2 = 2,
                                         float health = 1000.0f, uint8_t interior = 0,
                                         uint8_t paintjob = 3) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 164};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(model, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, angle);
  writer.writeBits(colour1, 8);
  writer.writeBits(colour2, 8);
  writeFloat(writer, health);
  writer.writeBits(interior, 8);
  writer.writeBits(0, 32);  // DoorDamage
  writer.writeBits(0, 32);  // PanelDamage
  writer.writeBits(0, 8);   // LightDamage
  writer.writeBits(0, 8);   // TyreDamage
  writer.writeBits(0, 8);   // Siren
  for (int slot = 0; slot < 14; ++slot) writer.writeBits(0, 8);
  writer.writeBits(paintjob, 8);
  writer.writeBits(0, 32);  // BodyColour1
  writer.writeBits(0, 32);  // BodyColour2
  return writer.buffer;
}

std::vector<uint8_t> makeStreamOutVehicle(uint16_t id) {
  return {20, 165, static_cast<uint8_t>(id & 0xFF), static_cast<uint8_t>(id >> 8)};
}

std::vector<uint8_t> makeSetVehicleParams(uint16_t id,
                                          const std::array<uint8_t, 16>& parameters) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 24};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  for (uint8_t parameter : parameters) writer.writeBits(parameter, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeSetVehicleParamsForPlayer(uint16_t id, uint8_t objective,
                                                   uint8_t doorsLocked) {
  return {20u, 161u, static_cast<uint8_t>(id & 0xFFu),
          static_cast<uint8_t>(id >> 8), objective, doorsLocked};
}

void writeCompressedUnitFloat(samp::raknet::BitWriter& writer, float value) {
  // Matches NetworkBitStream::WriteCompressed(float)'s unsigned-short cast.
  const auto raw = static_cast<uint16_t>((value + 1.0f) * 32767.5f);
  writer.writeBits(raw, 16);
}

void writeCompressedQuaternion(samp::raknet::BitWriter& writer, float w, float x, float y, float z) {
  writer.writeBit(w < 0.0f);
  writer.writeBit(x < 0.0f);
  writer.writeBit(y < 0.0f);
  writer.writeBit(z < 0.0f);
  const auto component = [](float value) {
    return static_cast<uint16_t>(std::lround(std::fabs(value) * 65535.0f));
  };
  writer.writeBits(component(x), 16);
  writer.writeBits(component(y), 16);
  writer.writeBits(component(z), 16);
}

std::vector<uint8_t> makeRemoteVehicleDriverBroadcast(uint16_t playerId, uint16_t vehicleId,
                                                        float quaternionW = 0.70710678f,
                                                        float quaternionX = 0.0f,
                                                        float quaternionY = 0.0f,
                                                        float quaternionZ = -0.70710678f) {
  samp::raknet::BitWriter writer;
  writer.writeBits(200, 8);
  writer.writeBits(playerId, 16);
  writer.writeBits(vehicleId, 16);
  writer.writeBits(0, 16);  // LeftRight
  writer.writeBits(0, 16);  // UpDown
  writer.writeBits(0, 16);  // Keys
  // Same negative-Z heading convention used by the local PlayerFoot preview:
  // default is 90° GTA-facing; callers can provide a source packet tilt case.
  writeCompressedQuaternion(writer, quaternionW, quaternionX, quaternionY, quaternionZ);
  writeFloat(writer, 125.0f);
  writeFloat(writer, -75.0f);
  writeFloat(writer, 6.0f);
  writeFloat(writer, 5.0f);  // compressed velocity magnitude
  writeCompressedUnitFloat(writer, 0.6f);
  writeCompressedUnitFloat(writer, 0.8f);
  writeCompressedUnitFloat(writer, 0.0f);
  writer.writeBits(875, 16);  // vehicle health
  writer.writeBits(0xFF, 8);  // driver health/armour packed
  writer.writeBits(0, 8);     // weapon/additional key
  writer.writeBit(false);     // Siren
  writer.writeBit(false);     // Landing gear
  writer.writeBit(false);     // Hydra thrust omitted
  writer.writeBit(false);     // Trailer omitted
  return writer.buffer;
}

std::vector<uint8_t> makeRemotePassengerBroadcast(uint16_t playerId, uint16_t vehicleId,
                                                    uint8_t seatId = 2) {
  samp::raknet::BitWriter writer;
  writer.writeBits(211, 8);
  writer.writeBits(playerId, 16);
  writer.writeBits(vehicleId, 16);
  // SA-MP/open.mp VehiclePassengerSyncPacket: seat low 6 bits, DriveBy/Cuffed
  // then a second byte for weapon/additional-key exactly as source writer.
  const uint16_t packedSeat = static_cast<uint16_t>((seatId & 0x3Fu) | 0x40u | (0x82u << 8u));
  writer.writeBits(packedSeat, 16);
  writer.writeBits(87, 8);  // raw health
  writer.writeBits(31, 8);  // raw armour
  writer.writeBits(static_cast<uint16_t>(-7), 16);
  writer.writeBits(9, 16);
  writer.writeBits(0x4321, 16);
  writeFloat(writer, 121.0f);
  writeFloat(writer, -74.0f);
  writeFloat(writer, 6.5f);
  return writer.buffer;
}

std::vector<uint8_t> makeRemoteUnoccupiedVehicleBroadcast(uint16_t playerId, uint16_t vehicleId) {
  samp::raknet::BitWriter writer;
  writer.writeBits(209, 8);
  writer.writeBits(playerId, 16);
  writer.writeBits(vehicleId, 16);
  writer.writeBits(0, 8);  // Seat
  for (int i = 0; i < 6; ++i) writeFloat(writer, 0.0f);  // roll + direction
  writeFloat(writer, 225.0f);
  writeFloat(writer, -175.0f);
  writeFloat(writer, 8.0f);
  writeFloat(writer, 1.0f);
  writeFloat(writer, 2.0f);
  writeFloat(writer, 0.0f);
  for (int i = 0; i < 3; ++i) writeFloat(writer, 0.0f);  // angular velocity
  writeFloat(writer, 640.0f);
  return writer.buffer;
}

std::vector<uint8_t> makeRemoteTrailerBroadcast(uint16_t playerId, uint16_t vehicleId,
                                                 float quaternionW = 0.70710678f,
                                                 float quaternionX = 0.0f,
                                                 float quaternionY = 0.0f,
                                                 float quaternionZ = -0.70710678f) {
  // Exact public open.mp PlayerTrailerSync writer: raw position, raw Vector4
  // constructed as q.w/q.x/q.y/q.z, then raw linear + turn VEC3 values.
  samp::raknet::BitWriter writer;
  writer.writeBits(210, 8);
  writer.writeBits(playerId, 16);
  writer.writeBits(vehicleId, 16);
  writeFloat(writer, 325.0f);
  writeFloat(writer, -275.0f);
  writeFloat(writer, 7.5f);
  writeFloat(writer, quaternionW);
  writeFloat(writer, quaternionX);
  writeFloat(writer, quaternionY);
  writeFloat(writer, quaternionZ);
  writeFloat(writer, 3.0f);
  writeFloat(writer, 4.0f);
  writeFloat(writer, 0.0f);
  writeFloat(writer, 0.1f);
  writeFloat(writer, -0.2f);
  writeFloat(writer, 0.3f);
  return writer.buffer;
}

std::vector<uint8_t> makeRemoteAimBroadcast(uint16_t playerId) {
  // Exact server-to-recipient Packet-203 layout: unlike the local 32-byte
  // writer, this begins with the broadcast player-ID prefix.
  samp::raknet::BitWriter writer;
  writer.writeBits(203, 8);
  writer.writeBits(playerId, 16);
  writer.writeBits(53, 8);  // compatible ordinary-weapon camera mode
  writeFloat(writer, 0.0f);
  writeFloat(writer, 1.0f);
  writeFloat(writer, 0.0f);
  writeFloat(writer, 1.5f);
  writeFloat(writer, -2.25f);
  writeFloat(writer, 3.75f);
  writeFloat(writer, -0.5f);
  writer.writeBits(0xA5, 8);  // zoom 37, weapon state 2
  writer.writeBits(0x7F, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeRemoteBulletBroadcast(uint16_t playerId) {
  // Exact server-to-recipient PlayerBulletSync layout from the public writer.
  // This fixture is a server-relayed miss (HitType=None), not a locally made
  // hit/damage assertion.
  samp::raknet::BitWriter writer;
  writer.writeBits(206, 8);
  writer.writeBits(playerId, 16);
  writer.writeBits(0, 8);   // PlayerBulletHitType_None
  writer.writeBits(0, 16);  // no target ID interpreted by this client
  writeFloat(writer, 1.5f);
  writeFloat(writer, -2.25f);
  writeFloat(writer, 3.75f);
  writeFloat(writer, 11.5f);
  writeFloat(writer, -2.25f);
  writeFloat(writer, 3.75f);
  writeFloat(writer, 0.0f);
  writeFloat(writer, 0.0f);
  writeFloat(writer, 0.0f);
  writer.writeBits(24, 8);  // Desert Eagle, source-defined bullet weapon
  return writer.buffer;
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

std::vector<uint8_t> makeRemoveBuilding(uint32_t modelId, float x, float y, float z,
                                        float radius) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 43};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(modelId, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, radius);
  return writer.buffer;
}

std::vector<uint8_t> makePutPlayerInVehicle(uint16_t vehicleId, uint8_t seatId) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 70};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(vehicleId, 16);
  writer.writeBits(seatId, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeRemovePlayerFromVehicle() {
  return {20, 71};
}

std::vector<uint8_t> makeVehicleByteRpc(uint8_t rpcId, uint16_t vehicleId, uint8_t value) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(vehicleId, 16);
  writer.writeBits(value, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeVehicleFloatRpc(uint8_t rpcId, uint16_t vehicleId, float value) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(vehicleId, 16);
  writeFloat(writer, value);
  return writer.buffer;
}

// RPC 91 deliberately has no vehicle ID. The recipient-local seat-zero context
// selects its target; fixtures must never smuggle an ID into this wire layout.
std::vector<uint8_t> makeSetVehicleVelocity(uint8_t type, float x, float y, float z) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 91};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(type, 8);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  return writer.buffer;
}

// Global SendDeathMessage is fixed: KillerID:u16 (0xFFFF for none),
// PlayerID:u16, Reason:u8. Unlike RPC-166 it has no position/health payload.
std::vector<uint8_t> makeSendDeathMessage(uint16_t killerId, uint16_t victimId, uint8_t reason) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 55};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(killerId, 16);
  writer.writeBits(victimId, 16);
  writer.writeBits(reason, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeSetPlayerAttachedObject(uint16_t playerId, uint32_t slot, bool create,
                                                   uint32_t model = 18637u, uint32_t bone = 2u,
                                                   float offsetX = 0.0f, float offsetY = 0.0f,
                                                   float offsetZ = 0.0f, float rotationX = 0.0f,
                                                   float rotationY = 0.0f, float rotationZ = 0.0f,
                                                   float scaleX = 1.0f, float scaleY = 1.0f,
                                                   float scaleZ = 1.0f,
                                                   uint32_t colour1Abgr = 0x11223344u,
                                                   uint32_t colour2Abgr = 0x55667788u) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 113};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(playerId, 16);
  writer.writeBits(slot, 32);
  writer.writeBit(create);
  if (create) {
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
    writer.writeBits(colour1Abgr, 32);
    writer.writeBits(colour2Abgr, 32);
  }
  return writer.buffer;
}

std::vector<uint8_t> makeAttachObjectToPlayer(uint16_t objectId, uint16_t playerId,
                                              float offsetX, float offsetY, float offsetZ,
                                              float rotationX, float rotationY, float rotationZ) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 75};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(objectId, 16);
  writer.writeBits(playerId, 16);
  writeFloat(writer, offsetX);
  writeFloat(writer, offsetY);
  writeFloat(writer, offsetZ);
  writeFloat(writer, rotationX);
  writeFloat(writer, rotationY);
  writeFloat(writer, rotationZ);
  return writer.buffer;
}

struct DefaultObjectMaterialWire {
  uint8_t slot = 0;
  uint16_t sourceModel = 0;
  std::string dictionary;
  std::string texture;
  uint32_t colourAbgr = 0xFFFFFFFFu;
};

void writeDynString8(samp::raknet::BitWriter& writer, const std::string& value) {
  if (value.size() > 255u) fail("test DynStr8 fixture exceeds u8");
  writer.writeBits(static_cast<uint32_t>(value.size()), 8);
  if (!value.empty()) {
    writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }
}

void writeDefaultObjectMaterial(samp::raknet::BitWriter& writer,
                                const DefaultObjectMaterialWire& material) {
  writer.writeBits(1u, 8);  // ObjectMaterialData::Type::Default (None=0)
  writer.writeBits(material.slot, 8);
  writer.writeBits(material.sourceModel, 16);
  writeDynString8(writer, material.dictionary);
  writeDynString8(writer, material.texture);
  writer.writeBits(material.colourAbgr, 32);
}

void writeCompressedHello(samp::raknet::BitWriter& writer);

void writeTextObjectMaterial(samp::raknet::BitWriter& writer, uint8_t slot) {
  writer.writeBits(2u, 8);  // ObjectMaterialData::Type::Text (None=0)
  writer.writeBits(slot, 8);
  writer.writeBits(10u, 8);  // ObjectMaterialSize_32x32
  writeDynString8(writer, "Arial");
  writer.writeBits(12u, 8);
  writer.writeBits(0u, 8);
  writer.writeBits(0xFFFFFFFFu, 32);  // FontColour ARGB
  writer.writeBits(0u, 32);           // BackgroundColour ARGB
  writer.writeBits(0u, 8);            // Alignment
  writeCompressedHello(writer);
}

std::vector<uint8_t> makeCreateObject(uint16_t id, uint32_t model, float x, float y, float z,
                                      float rotationX, float rotationY, float rotationZ,
                                      float drawDistance, bool extended = false,
                                      uint16_t attachedVehicle = 0xFFFFu,
                                      uint16_t attachedObject = 0xFFFFu,
                                      float offsetX = 0.0f, float offsetY = 0.0f,
                                      float offsetZ = 0.0f, float attachmentRotationX = 0.0f,
                                      float attachmentRotationY = 0.0f,
                                      float attachmentRotationZ = 0.0f,
                                      uint8_t syncRotation = 1u,
                                      const std::vector<DefaultObjectMaterialWire>& materials = {},
                                      int textMaterialSlot = -1) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 44};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(model, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, rotationX);
  writeFloat(writer, rotationY);
  writeFloat(writer, rotationZ);
  writeFloat(writer, drawDistance);
  if (extended) {
    writer.writeBits(0, 8);  // NoCameraCol
    // Current open.mp writer order: vehicle parent, then object parent.
    writer.writeBits(attachedVehicle, 16);
    writer.writeBits(attachedObject, 16);
    if (attachedVehicle != 0xFFFFu || attachedObject != 0xFFFFu) {
      writeFloat(writer, offsetX);
      writeFloat(writer, offsetY);
      writeFloat(writer, offsetZ);
      writeFloat(writer, attachmentRotationX);
      writeFloat(writer, attachmentRotationY);
      writeFloat(writer, attachmentRotationZ);
      writer.writeBits(syncRotation, 8);
    }
    if (materials.size() > 16u || textMaterialSlot < -1 || textMaterialSlot > 15 ||
        materials.size() + (textMaterialSlot >= 0 ? 1u : 0u) > 16u) {
      fail("test CreateObject material fixture exceeds slots");
    }
    writer.writeBits(static_cast<uint32_t>(materials.size() +
                                           (textMaterialSlot >= 0 ? 1u : 0u)), 8);
    for (const DefaultObjectMaterialWire& material : materials) {
      writeDefaultObjectMaterial(writer, material);
    }
    if (textMaterialSlot >= 0) {
      writeTextObjectMaterial(writer, static_cast<uint8_t>(textMaterialSlot));
    }
  } else if (!materials.empty() || textMaterialSlot >= 0) {
    fail("legacy CreateObject fixture cannot carry a material section");
  }
  return writer.buffer;
}

std::vector<uint8_t> makeSetObjectMaterialDefault(
    uint16_t objectId, const DefaultObjectMaterialWire& material) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 84};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(objectId, 16);
  writeDefaultObjectMaterial(writer, material);
  return writer.buffer;
}

std::vector<uint8_t> makeSetObjectMaterialText(uint16_t objectId, uint8_t slot) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 84};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(objectId, 16);
  writeTextObjectMaterial(writer, slot);
  return writer.buffer;
}

std::vector<uint8_t> makeObjectVec3Rpc(uint8_t rpcId, uint16_t id, float x, float y, float z) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  return writer.buffer;
}

std::vector<uint8_t> makeMoveObject(uint16_t id, float startX, float startY, float startZ,
                                    float targetX, float targetY, float targetZ, float speed,
                                    float rotationX = -1000.0f, float rotationY = -1000.0f,
                                    float rotationZ = -1000.0f, bool includeRotation = true) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 99};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writeFloat(writer, startX);
  writeFloat(writer, startY);
  writeFloat(writer, startZ);
  writeFloat(writer, targetX);
  writeFloat(writer, targetY);
  writeFloat(writer, targetZ);
  writeFloat(writer, speed);
  if (includeRotation) {
    writeFloat(writer, rotationX);
    writeFloat(writer, rotationY);
    writeFloat(writer, rotationZ);
  }
  return writer.buffer;
}

std::vector<uint8_t> makeStopObject(uint16_t id) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 122};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  return writer.buffer;
}

std::vector<uint8_t> makeDestroyObject(uint16_t id) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 47};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  return writer.buffer;
}

std::vector<uint8_t> makeCreatePickup(uint32_t id, uint32_t model, uint32_t spawnType,
                                      float x, float y, float z) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 95};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 32);
  writer.writeBits(model, 32);
  writer.writeBits(spawnType, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  return writer.buffer;
}

std::vector<uint8_t> makeDestroyPickup(uint32_t id) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 63};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeShowTextDraw(uint16_t wireId, const std::string& text,
                                      bool selectable = false, bool useBox = true,
                                      uint8_t style = 1, uint16_t model = 0,
                                      float rotationX = 0.0f, float rotationY = 0.0f,
                                      float rotationZ = 0.0f, float zoom = 1.0f,
                                      int16_t colour1 = 0, int16_t colour2 = 0) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 134};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(wireId, 16);
  const uint8_t flags = static_cast<uint8_t>((useBox ? 1u : 0u) | (1u << 1u) | (1u << 4u));
  writer.writeBits(flags, 8);  // left alignment, proportional
  writeFloat(writer, 0.42f);
  writeFloat(writer, 1.15f);
  writer.writeBits(0xFF80FFFFu, 32);  // ABGR
  writeFloat(writer, 180.0f);
  writeFloat(writer, 30.0f);
  writer.writeBits(0x80202020u, 32);  // ABGR
  writer.writeBits(1, 8);   // Shadow
  writer.writeBits(1, 8);   // Outline
  writer.writeBits(0x00000000u, 32);
  writer.writeBits(style, 8);   // Font style
  writer.writeBits(selectable ? 1u : 0u, 8);
  writeFloat(writer, 120.0f);
  writeFloat(writer, 80.0f);
  writer.writeBits(model, 16);
  writeFloat(writer, rotationX);
  writeFloat(writer, rotationY);
  writeFloat(writer, rotationZ);
  writeFloat(writer, zoom);
  writer.writeBits(static_cast<uint16_t>(colour1), 16);
  writer.writeBits(static_cast<uint16_t>(colour2), 16);
  writer.writeBits(static_cast<uint32_t>(text.size()), 16);
  if (!text.empty()) writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  return writer.buffer;
}

void writeCompressedHello(samp::raknet::BitWriter& writer) {
  // Exact default RakNet StringCompressor fixture used by the dialog coverage.
  // It decodes to "Hello" in 42 meaningful bits and leaves byte padding only.
  const std::vector<uint8_t> compressedHello = {0x88, 0x1F, 0x02, 0x52, 0x85, 0x40};
  samp::raknet::BitReader fixture(compressedHello.data(), compressedHello.size());
  for (int bit = 0; bit < 42; ++bit) writer.writeBit(fixture.readBit());
}

std::vector<uint8_t> makeShowTextLabel(uint16_t wireId, uint32_t rgba,
                                       float x, float y, float z, float drawDistance,
                                       bool los = false, uint16_t playerAttach = 0xFFFFu,
                                       uint16_t vehicleAttach = 0xFFFFu) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 36};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(wireId, 16);
  writer.writeBits(rgba, 32);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, drawDistance);
  writer.writeBits(los ? 1u : 0u, 8);
  writer.writeBits(playerAttach, 16);
  writer.writeBits(vehicleAttach, 16);
  writeCompressedHello(writer);
  return writer.buffer;
}

std::vector<uint8_t> makeHideTextLabel(uint16_t wireId) {
  return {20, 58, static_cast<uint8_t>(wireId & 0xFFu), static_cast<uint8_t>(wireId >> 8u)};
}

std::vector<uint8_t> makeSetPlayerMapIcon(uint8_t id, float x, float y, float z,
                                          uint8_t type, uint32_t rgba, uint8_t style) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 56};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 8);
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writer.writeBits(type, 8);
  writer.writeBits(rgba, 32);
  writer.writeBits(style, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeRemovePlayerMapIcon(uint8_t id) {
  return {20, 144, id};
}

std::vector<uint8_t> makeHideTextDraw(uint16_t wireId) {
  return {20, 135, static_cast<uint8_t>(wireId & 0xFFu), static_cast<uint8_t>(wireId >> 8u)};
}

std::vector<uint8_t> makeTextDrawSetString(uint16_t wireId, const std::string& text) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 105};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(wireId, 16);
  writer.writeBits(static_cast<uint32_t>(text.size()), 16);
  if (!text.empty()) writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  return writer.buffer;
}

std::vector<uint8_t> makeTextDrawSelection(bool enabled, uint32_t rgba = 0xFFFFFFFFu) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 83};
  writer.writeBytes(header, sizeof(header));
  writer.writeBit(enabled);
  writer.writeBits(rgba, 32);
  return writer.buffer;
}

std::vector<uint8_t> makePlayerBeginObjectSelect() {
  return {20, 27};
}

std::vector<uint8_t> makePlayerBeginObjectEdit(bool playerObject, uint16_t objectId) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 117};
  writer.writeBytes(header, sizeof(header));
  writer.writeBit(playerObject);
  writer.writeBits(objectId, 16);
  return writer.buffer;
}

std::vector<uint8_t> makePlayerBeginAttachedObjectEdit(uint32_t slot) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 116};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(slot, 32);
  return writer.buffer;
}

std::vector<uint8_t> makePlayerCancelObjectEdit() {
  return {20, 28};
}

std::vector<uint8_t> makeGameText(uint32_t style, uint32_t durationMs, const std::string& text) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 73};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(style, 32);
  writer.writeBits(durationMs, 32);
  writer.writeBits(static_cast<uint32_t>(text.size()), 32);
  if (!text.empty()) writer.writeBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  return writer.buffer;
}

std::vector<uint8_t> makeShowGangZone(int16_t id, float minX, float minY,
                                      float maxX, float maxY, uint32_t colourAbgr) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 108};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(static_cast<uint16_t>(id), 16);
  writeFloat(writer, minX);
  writeFloat(writer, minY);
  writeFloat(writer, maxX);
  writeFloat(writer, maxY);
  writer.writeBits(colourAbgr, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeGangZoneIdRpc(uint8_t rpcId, int16_t id, uint32_t colourAbgr = 0) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(static_cast<uint16_t>(id), 16);
  if (rpcId == 121) writer.writeBits(colourAbgr, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeCameraLookAt(float x, float y, float z, uint8_t cutType = 0) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 158};
  writer.writeBytes(header, sizeof(header));
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writer.writeBits(cutType, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeInterpolateCamera(bool position, float fromX, float fromY, float fromZ,
                                           float toX, float toY, float toZ, uint32_t durationMs,
                                           uint8_t cutType = 0) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 82};
  writer.writeBytes(header, sizeof(header));
  writer.writeBit(position);
  writeFloat(writer, fromX);
  writeFloat(writer, fromY);
  writeFloat(writer, fromZ);
  writeFloat(writer, toX);
  writeFloat(writer, toY);
  writeFloat(writer, toZ);
  writer.writeBits(durationMs, 32);
  writer.writeBits(cutType, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeFloatRpc(uint8_t rpcId, float value) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writeFloat(writer, value);
  return writer.buffer;
}

std::vector<uint8_t> makeWorldBounds(float maxX, float minX, float maxY, float minY) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 17};
  writer.writeBytes(header, sizeof(header));
  // Current open.mp SetWorldBounds writer emits Vector4 in API order:
  // maxX, minX, maxY, minY.
  writeFloat(writer, maxX);
  writeFloat(writer, minX);
  writeFloat(writer, maxY);
  writeFloat(writer, minY);
  return writer.buffer;
}

std::vector<uint8_t> makePlayerTime(uint8_t hour, uint8_t minute) {
  return {20, 29, hour, minute};
}

std::vector<uint8_t> makeAudioStream(const std::string& url, float x, float y, float z,
                                     float distance, uint8_t usePosition) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 41};
  writer.writeBytes(header, sizeof(header));
  if (url.size() > 255u) return {};
  writer.writeBits(static_cast<uint32_t>(url.size()), 8);
  if (!url.empty()) {
    writer.writeBytes(reinterpret_cast<const uint8_t*>(url.data()), url.size());
  }
  writeFloat(writer, x);
  writeFloat(writer, y);
  writeFloat(writer, z);
  writeFloat(writer, distance);
  writer.writeBits(usePosition, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeByteRpc(uint8_t rpcId, uint8_t value) {
  return {20, rpcId, value};
}

std::vector<uint8_t> makeInt32Rpc(uint8_t rpcId, int32_t value) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "int32 wire helper");
  std::memcpy(&bits, &value, sizeof(bits));
  writer.writeBits(bits, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeToggleSpectating(uint32_t enabled) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 124};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(enabled, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeSpectateTarget(uint8_t rpcId, uint16_t id, uint8_t mode) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, rpcId};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 16);
  writer.writeBits(mode, 8);
  return writer.buffer;
}

void writeFixedMenuText(samp::raknet::BitWriter& writer, const std::string& text) {
  if (text.size() >= 32u) fail("fixed menu fixture text overflow");
  std::array<uint8_t, 32> bytes{};
  std::memcpy(bytes.data(), text.data(), text.size());
  writer.writeBytes(bytes.data(), bytes.size());
}

std::vector<uint8_t> makePlayerInitMenu(uint8_t id, bool twoColumns = true) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 76};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(id, 8);
  writer.writeBits(twoColumns ? 1u : 0u, 32);
  writeFixedMenuText(writer, "Transport");
  writeFloat(writer, 120.0f);
  writeFloat(writer, 80.0f);
  writeFloat(writer, 160.0f);
  if (twoColumns) writeFloat(writer, 120.0f);
  writer.writeBits(1u, 32);  // MenuEnabled
  for (uint32_t row = 0; row < 12; ++row) writer.writeBits(row == 1u ? 0u : 1u, 32);
  writeFixedMenuText(writer, "Route");
  writer.writeBits(3u, 8);
  writeFixedMenuText(writer, "Los Santos");
  writeFixedMenuText(writer, "San Fierro");
  writeFixedMenuText(writer, "Las Venturas");
  if (twoColumns) {
    writeFixedMenuText(writer, "Fare");
    writer.writeBits(3u, 8);
    writeFixedMenuText(writer, "$10");
    writeFixedMenuText(writer, "$20");
    writeFixedMenuText(writer, "$30");
  }
  return writer.buffer;
}

std::vector<uint8_t> makeShowPlayerNameTag(uint16_t playerId, uint8_t show) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 80};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(playerId, 16);
  writer.writeBits(show, 8);
  return writer.buffer;
}

std::vector<uint8_t> makeGivePlayerWeapon(uint32_t weapon, uint32_t ammo) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 22};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(weapon, 32);
  writer.writeBits(ammo, 32);
  return writer.buffer;
}

std::vector<uint8_t> makeSetPlayerAmmo(uint8_t weapon, uint16_t ammo) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 145};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(weapon, 8);
  writer.writeBits(ammo, 16);
  return writer.buffer;
}

std::vector<uint8_t> makeGivePlayerMoney(int32_t amount) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 18};
  writer.writeBytes(header, sizeof(header));
  uint32_t raw = 0;
  static_assert(sizeof(raw) == sizeof(amount), "legacy money amount is int32");
  std::memcpy(&raw, &amount, sizeof(raw));
  writer.writeBits(raw, 32);
  return writer.buffer;
}

struct ScoreFixture {
  uint16_t id = 0;
  int32_t score = 0;
  uint32_t ping = 0;
};

std::vector<uint8_t> makeScoresAndPings(const std::vector<ScoreFixture>& entries) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 155};
  writer.writeBytes(header, sizeof(header));
  for (const ScoreFixture& entry : entries) {
    writer.writeBits(entry.id, 16);
    uint32_t rawScore = 0;
    static_assert(sizeof(rawScore) == sizeof(entry.score), "legacy score is int32");
    std::memcpy(&rawScore, &entry.score, sizeof(rawScore));
    writer.writeBits(rawScore, 32);
    writer.writeBits(entry.ping, 32);
  }
  return writer.buffer;
}

std::vector<uint8_t> makeSetPlayerArmedWeapon(uint32_t weapon) {
  samp::raknet::BitWriter writer;
  const uint8_t header[] = {20, 67};
  writer.writeBytes(header, sizeof(header));
  writer.writeBits(weapon, 32);
  return writer.buffer;
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

void testLegacyHandshakeBuilders() {
  using samp::handshake::buildClientJoin;
  using samp::handshake::buildEmptyStaticData;
  using samp::handshake::buildInternalPing;
  using samp::handshake::buildNewIncoming;
  using samp::handshake::generateLegacySerial;
  using samp::handshake::isPostAcceptControlId;
  using samp::handshake::isTerminalSessionControlId;
  using samp::handshake::isValidLegacySerial;
  using samp::handshake::isValidNickname;
  using samp::handshake::parseAuthChallenge;

  expect(isValidNickname("Native_01") && !isValidNickname("hi") &&
             !isValidNickname("has space") && !isValidNickname("line\nbreak"),
         "legacy nickname bounds/control validation changed");
  const std::string generatedSerial = generateLegacySerial();
  expect(isValidLegacySerial(generatedSerial) && !isValidLegacySerial("123456") &&
             !isValidLegacySerial("not-hex"),
         "generated legacy serial must remain bounded hexadecimal multiple of 1001");
  expect(isTerminalSessionControlId(28u) && isTerminalSessionControlId(29u) &&
             isTerminalSessionControlId(31u) && isTerminalSessionControlId(32u) &&
             isTerminalSessionControlId(33u) && isTerminalSessionControlId(36u) &&
             isTerminalSessionControlId(37u) && !isTerminalSessionControlId(12u) &&
             !isTerminalSessionControlId(34u) && !isTerminalSessionControlId(41u),
         "terminal legacy RakNet control classification changed");
  expect(isPostAcceptControlId(6u) && isPostAcceptControlId(41u) &&
             !isPostAcceptControlId(20u) && !isPostAcceptControlId(30u),
         "post-accept transport-control classification changed");

  const std::string authText = "6C407EC29DE59E2";
  std::vector<uint8_t> authChallenge = {12u, static_cast<uint8_t>(authText.size() + 1u)};
  authChallenge.insert(authChallenge.end(), authText.begin(), authText.end());
  authChallenge.push_back(0u);
  const auto parsedAuth = parseAuthChallenge(authChallenge);
  expect(parsedAuth && *parsedAuth == authText,
         "legacy NUL-terminated AUTH_KEY challenge parser changed");
  const std::vector<uint8_t> authLengthOverrun = {12u, 0xFFu, 'x', 0u};
  const std::vector<uint8_t> authMissingNul = {12u, 2u, 'x', 'y'};
  const std::vector<uint8_t> authEmbeddedNul = {12u, 4u, 'x', 0u, 'y', 0u};
  std::vector<uint8_t> authTrailing = authChallenge;
  authTrailing.push_back(0u);
  std::vector<uint8_t> authTooLong = {12u, 18u};
  authTooLong.insert(authTooLong.end(), 17u, 'A');
  authTooLong.push_back(0u);
  expect(!parseAuthChallenge(authLengthOverrun) && !parseAuthChallenge(authMissingNul) &&
             !parseAuthChallenge(authEmbeddedNul) && !parseAuthChallenge(authTrailing) &&
             !parseAuthChallenge(authTooLong),
         "AUTH_KEY parser must reject malformed length/NUL/trailing records safely");

  const auto incoming = buildNewIncoming({94u, 23u, 145u, 137u}, 7776u);
  expect(incoming == std::vector<uint8_t>({30u, 94u, 23u, 145u, 137u, 0x60u, 0x1Eu}),
         "NEW_INCOMING must retain exact seven-byte legacy layout");

  // RakPeer sends these source-defined post-accept controls before game RPC
  // traffic: an unreliable internal ping and an empty reliable static record.
  const auto internalPing = buildInternalPing(0x12345678u);
  expect(internalPing == std::vector<uint8_t>({6u, 0x78u, 0x56u, 0x34u, 0x12u}) &&
             buildEmptyStaticData() == std::vector<uint8_t>({41u}),
         "post-accept ping/static payload builders changed");
  const auto framedPing = samp::raknet::buildUnreliable(2u, internalPing);
  const auto decodedPing = samp::raknet::decodeDatagram(framedPing);
  const auto framedStatic = samp::raknet::buildReliable(3u, buildEmptyStaticData());
  const auto decodedStatic = samp::raknet::decodeDatagram(framedStatic);
  expect(decodedPing.ok && decodedPing.packets.size() == 1u &&
             decodedPing.packets[0].messageNumber == 2u &&
             decodedPing.packets[0].reliability == samp::raknet::kReliabilityUnreliable &&
             decodedPing.ackMessageNumbers.empty() && decodedPing.packets[0].payload == internalPing &&
             decodedStatic.ok && decodedStatic.packets.size() == 1u &&
             decodedStatic.packets[0].messageNumber == 3u &&
             decodedStatic.packets[0].reliability == samp::raknet::kReliabilityReliable &&
             decodedStatic.ackMessageNumbers == std::vector<uint16_t>({3u}) &&
             decodedStatic.packets[0].payload == std::vector<uint8_t>({41u}),
         "post-accept RakPeer reliability framing changed");

  const auto join = buildClientJoin(0x12345678u, "Native_01", "3E9");
  const std::vector<uint8_t> expectedJoin = {
      20u, 25u, 0xD9u, 0x0Fu, 0x00u, 0x00u, 1u, 9u,
      'N', 'a', 't', 'i', 'v', 'e', '_', '0', '1',
      0x78u, 0x56u, 0x34u, 0x12u, 3u, '3', 'E', '9', 8u,
      '0', '.', '3', '.', '7', '-', 'R', '2',
  };
  expect(join == expectedJoin, "ClientJoin field layout/version identity changed");
  expect(buildClientJoin(0u, "bad nick", "3E9").empty() &&
             buildClientJoin(0u, "Native_01", "123").empty(),
         "ClientJoin must reject unsafe nickname or invalid serial before framing");

  // open.mp's public OrderingChannel enum assigns SyncRPC to channel 2.
  // ClientJoin consumes index zero there, so RequestClass starts at one on the
  // same stream. A live UIF capture independently returns its RPC frames on 2.
  static_assert(samp::clientrpc::kReliableOrderedChannel == 2u,
                "SyncRPC ordering channel changed");
  // RakPeer's accepted-path schedule consumes messages 2/3/4 for
  // NEW_INCOMING/internal-ping/static-data, so its first game RPC is message 5.
  const auto framedJoin = samp::raknet::buildReliableOrdered(
      5u, samp::clientrpc::kReliableOrderedChannel, 0u, join);
  // Independently emitted by FakeBots' vendored RakNet with RAKNET_LEGACY=1
  // for these same fields. Keep this byte fixture separate from a merely
  // self-decoded assertion: the 0x48 header carries legacy reliability=9.
  expect(framedJoin == std::vector<uint8_t>({
             0x02u, 0x80u, 0x48u, 0x80u, 0x00u, 0x01u, 0x00u, 0x10u,
             0x14u, 0x19u, 0xD9u, 0x0Fu, 0x00u, 0x00u, 0x01u, 0x09u,
             0x4Eu, 0x61u, 0x74u, 0x69u, 0x76u, 0x65u, 0x5Fu, 0x30u,
             0x31u, 0x78u, 0x56u, 0x34u, 0x12u, 0x03u, 0x33u, 0x45u,
             0x39u, 0x08u, 0x30u, 0x2Eu, 0x33u, 0x2Eu, 0x37u, 0x2Du,
             0x52u, 0x32u,
         }), "ClientJoin SyncRPC-channel wire vector changed");
  const auto decoded = samp::raknet::decodeDatagram(framedJoin);
  expect(decoded.ok && decoded.packets.size() == 1u &&
             decoded.packets[0].messageNumber == 5u &&
             decoded.packets[0].reliability == samp::raknet::kReliabilityReliableOrdered &&
             decoded.packets[0].orderingChannel == samp::clientrpc::kReliableOrderedChannel &&
             decoded.packets[0].orderingIndex == 0u && decoded.packets[0].payload == join,
         "ClientJoin reliable ordered channel/index framing changed");

  // RakPeer's sender coalesces the accepted-path control records plus the
  // high-priority PlayerConnect RPC into one no-ACK datagram on an ordinary
  // network tick. This entire 70-byte fixture was also emitted independently
  // by FakeBots' legacy BitStream (same IP/port/time/name/serial inputs).
  const auto bootstrap = samp::raknet::buildCoalescedDatagram({
      {2u, samp::raknet::kReliabilityReliable, 0u, 0u,
       buildNewIncoming({94u, 23u, 145u, 137u}, 7776u)},
      {3u, samp::raknet::kReliabilityUnreliable, 0u, 0u,
       buildInternalPing(0x12345678u)},
      {4u, samp::raknet::kReliabilityReliable, 0u, 0u, buildEmptyStaticData()},
      {5u, samp::raknet::kReliabilityReliableOrdered,
       samp::clientrpc::kReliableOrderedChannel, 0u, join},
  });
  expect(bootstrap == std::vector<uint8_t>({
             0x01u, 0x00u, 0x42u, 0x38u, 0x1Eu, 0x5Eu, 0x17u, 0x91u, 0x89u,
             0x60u, 0x1Eu, 0x03u, 0x00u, 0x64u, 0x50u, 0x06u, 0x78u, 0x56u,
             0x34u, 0x12u, 0x04u, 0x00u, 0x87u, 0x00u, 0x29u, 0x05u, 0x00u,
             0x91u, 0x00u, 0x00u, 0x02u, 0x00u, 0x20u, 0x14u, 0x19u, 0xD9u,
             0x0Fu, 0x00u, 0x00u, 0x01u, 0x09u, 0x4Eu, 0x61u, 0x74u, 0x69u,
             0x76u, 0x65u, 0x5Fu, 0x30u, 0x31u, 0x78u, 0x56u, 0x34u, 0x12u,
             0x03u, 0x33u, 0x45u, 0x39u, 0x08u, 0x30u, 0x2Eu, 0x33u, 0x2Eu,
             0x37u, 0x2Du, 0x52u, 0x32u,
         }), "source-equivalent coalesced post-accept bootstrap changed");
  const auto decodedBootstrap = samp::raknet::decodeDatagram(bootstrap);
  expect(decodedBootstrap.ok && decodedBootstrap.packets.size() == 4u &&
             decodedBootstrap.packets[0].messageNumber == 2u &&
             decodedBootstrap.packets[0].payload == buildNewIncoming({94u, 23u, 145u, 137u}, 7776u) &&
             decodedBootstrap.packets[1].messageNumber == 3u &&
             decodedBootstrap.packets[1].reliability == samp::raknet::kReliabilityUnreliable &&
             decodedBootstrap.packets[1].payload == buildInternalPing(0x12345678u) &&
             decodedBootstrap.packets[2].messageNumber == 4u &&
             decodedBootstrap.packets[2].payload == buildEmptyStaticData() &&
             decodedBootstrap.packets[3].messageNumber == 5u &&
             decodedBootstrap.packets[3].reliability == samp::raknet::kReliabilityReliableOrdered &&
             decodedBootstrap.packets[3].orderingChannel == samp::clientrpc::kReliableOrderedChannel &&
             decodedBootstrap.packets[3].orderingIndex == 0u &&
             decodedBootstrap.packets[3].payload == join,
         "coalesced post-accept bootstrap did not retain individual frames");
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

  // Outbound ACKs now canonicalize unordered/duplicate IDs into a compact
  // non-wrapping range plus singleton. It must decode to exactly the unique
  // IDs and never turn 0xFFFF -> 0 into one giant wrap range.
  const auto compactAck = samp::raknet::buildAck({13, 10, 12, 11, 13, 99});
  samp::raknet::BitReader compactAckReader(compactAck.data(), compactAck.size());
  expect(compactAckReader.readBit() && compactAckReader.readCompressedU16() == 2u &&
         !compactAckReader.readBit() && compactAckReader.readBits(16) == 10u &&
         compactAckReader.readBits(16) == 13u && compactAckReader.readBit() &&
         compactAckReader.readBits(16) == 99u && compactAckReader.ok() &&
         samp::raknet::decodeDatagram(compactAck).receivedAckMessageNumbers ==
             std::vector<uint16_t>({10, 11, 12, 13, 99}),
         "outbound ACK range canonicalization changed");
  const auto wrappingAck = samp::raknet::buildAck({65535u, 0u, 65534u});
  expect(samp::raknet::decodeDatagram(wrappingAck).receivedAckMessageNumbers ==
             std::vector<uint16_t>({0u, 65534u, 65535u}),
         "outbound ACK must not coalesce counter wrap");
  std::vector<uint16_t> longAckRange;
  for (uint16_t message = 1; message <= 513; ++message) longAckRange.push_back(message);
  expect(samp::raknet::decodeDatagram(samp::raknet::buildAck(longAckRange)).receivedAckMessageNumbers ==
             longAckRange,
         "outbound ACK span must split at bounded range limit");

  const auto ordered = samp::raknet::buildReliableOrdered(
      2, samp::clientrpc::kReliableOrderedChannel, 17, {20, 128, 3, 0});
  // Locks the actual five-bit SyncRPC header at channel 2, not merely a
  // decoder round-trip that could otherwise drift together with its encoder.
  expect(ordered == std::vector<uint8_t>({
             0x01, 0x00, 0x48, 0x84, 0x40, 0x11, 0x00, 0x14, 0x80, 0x03, 0x00}),
         "SyncRPC channel-2 wire vector changed");
  const auto orderedDecoded = samp::raknet::decodeDatagram(ordered);
  expect(orderedDecoded.ok && orderedDecoded.packets.size() == 1,
         "reliable ordered packet did not decode");
  const auto& packet = orderedDecoded.packets[0];
  expect(packet.reliability == samp::raknet::kReliabilityReliableOrdered,
         "wrong ordered reliability code");
  expect(packet.orderingChannel == samp::clientrpc::kReliableOrderedChannel &&
             packet.orderingIndex == 17,
         "ordered channel/index changed");
  expect(packet.payload == std::vector<uint8_t>({20, 128, 3, 0}),
         "ordered payload changed");
}

void testUnreliableSequencedTransport() {
  // This vector has a deliberately non-byte-aligned five-bit payload. It
  // locks the SyncPacket channel-1 header and alignment before the payload;
  // a byte-length-only builder or a channel drift would silently differ.
  expect(samp::sync::kSyncOrderingChannel == 1u &&
             samp::clientrpc::kReliableOrderedChannel == 2u,
         "SyncPacket and SyncRPC channel assignments changed");
  const auto sequenced = samp::raknet::buildUnreliableSequenced(
      0x1234, samp::sync::kSyncOrderingChannel, 0x00A5, {0xA8}, 5);
  const std::vector<uint8_t> expected = {0x1A, 0x09, 0x38, 0x69, 0x40, 0x1A, 0x80, 0xA8};
  expect(sequenced == expected, "SyncPacket channel-1 unreliable sequenced wire vector changed");

  samp::raknet::BitReader header(sequenced.data(), sequenced.size());
  expect(!header.readBit(), "sequenced packet unexpectedly has ACKs");
  expect(header.readBits(16) == 0x1234, "sequenced message number changed");
  expect(header.readBits(4) == samp::raknet::kReliabilityUnreliableSequenced,
         "sequenced reliability changed");
  expect(header.readBits(5) == samp::sync::kSyncOrderingChannel &&
             header.readBits(16) == 0x00A5,
         "sequenced channel/index changed");
  expect(!header.readBit() && header.readCompressedU16() == 5,
         "sequenced payload bit length changed");
  expect(header.readAlignedBytes(1) == std::vector<uint8_t>({0xA8}) && header.ok(),
         "sequenced payload alignment changed");

  const auto decoded = samp::raknet::decodeDatagram(sequenced);
  expect(decoded.ok && decoded.ackMessageNumbers.empty() && decoded.packets.size() == 1,
         "unreliable sequenced packet decode/ACK behavior changed");
  expect(decoded.packets[0].reliability == samp::raknet::kReliabilityUnreliableSequenced &&
         decoded.packets[0].orderingChannel == samp::sync::kSyncOrderingChannel &&
         decoded.packets[0].orderingIndex == 0x00A5 &&
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

void testReliableMessageTransportGate() {
  samp::raknet::ReliableMessageGate gate;
  // Message-number delivery is independent from application ordering: a
  // reliable packet can fill a transport hole before an earlier number arrives.
  expect(gate.accept(0u) && gate.accept(2u) && gate.accept(1u) && gate.seenCount() == 3u,
         "reliable message gate must accept first-seen out-of-order holes");
  expect(!gate.accept(2u) && !gate.accept(0u),
         "duplicate reliable transport messages must not reach reducers twice");

  gate.clear();
  expect(gate.accept(0xFFFEu) && gate.accept(0xFFFFu) && gate.accept(0u) &&
             gate.accept(1u) && !gate.accept(0xFFFFu),
         "reliable message gate must retain duplicate rejection across u16 wrap");

  gate.clear();
  expect(gate.accept(0u) && gate.accept(samp::raknet::ReliableMessageGate::kHistoryWindow) &&
             gate.seenCount() == 1u,
         "reliable message gate must prune stale receive history after a forward window advance");
  expect(!gate.accept(0u) &&
             !gate.accept(static_cast<uint16_t>(samp::raknet::ReliableMessageGate::kHistoryWindow + 0x8000u)),
         "stale or ambiguous reliable message numbers must be rejected safely");

  // This receipt history starts during authentication and moves into the
  // retained socket, so a handshake duplicate must stay recognized afterward.
  gate.clear();
  expect(gate.accept(77u), "reliable receipt hand-off fixture failed");
  samp::raknet::ReliableMessageGate handedOff = std::move(gate);
  expect(!handedOff.accept(77u) && handedOff.accept(76u),
         "moving reliable receipt gate must preserve handshake history");

  gate.clear();
  expect(gate.seenCount() == 0u && gate.accept(77u),
         "reliable message gate clear must reset transport receive history");
}

void testReliableOrderedTransportGate() {
  const auto orderedPacket = [](uint8_t channel, uint16_t index, uint8_t marker) {
    samp::raknet::IncomingPacket packet;
    packet.reliability = samp::raknet::kReliabilityReliableOrdered;
    packet.orderingChannel = channel;
    packet.orderingIndex = index;
    packet.payload = {marker};
    packet.payloadBitLength = 8;
    return packet;
  };

  samp::raknet::ReliableOrderedGate gate;
  expect(gate.push(orderedPacket(0, 1, 0xB1)).empty() && gate.pendingCount() == 1 &&
             gate.pendingBytes() == 1 && !gate.overflowed(),
         "later reliable-ordered frame must wait for its missing predecessor");
  const auto released = gate.push(orderedPacket(0, 0, 0xA0));
  expect(released.size() == 2 && released[0].payload == std::vector<uint8_t>({0xA0}) &&
             released[1].payload == std::vector<uint8_t>({0xB1}) && gate.pendingCount() == 0,
         "reliable-ordered gate must release a contiguous channel run in order");
  expect(gate.push(orderedPacket(0, 1, 0xB1)).empty() &&
             gate.push(orderedPacket(0, 0xFFFFu, 0xFF)).empty(),
         "duplicate/stale reliable-ordered frames must not re-enter reducers");

  // The synchronous ClientJoin phase moves this gate into LiveSession. A
  // buffered predecessor must survive that ownership hand-off intact.
  gate.clear();
  expect(gate.push(orderedPacket(4, 1, 0x41)).empty(),
         "handoff fixture needs one buffered ordered packet");
  samp::raknet::ReliableOrderedGate handedOff = std::move(gate);
  const auto afterHandoff = handedOff.push(orderedPacket(4, 0, 0x40));
  expect(afterHandoff.size() == 2 &&
             afterHandoff[0].payload == std::vector<uint8_t>({0x40}) &&
             afterHandoff[1].payload == std::vector<uint8_t>({0x41}),
         "moving reliable-ordered gate must preserve buffered handshake state");

  gate.clear();
  expect(gate.push(orderedPacket(1, 1, 0x11)).empty(),
         "second ordered channel should buffer independently");
  const auto channelZero = gate.push(orderedPacket(0, 0, 0x00));
  expect(channelZero.size() == 1 && channelZero[0].payload == std::vector<uint8_t>({0x00}),
         "one ordered channel must not wait on another channel's missing frame");
  const auto channelOne = gate.push(orderedPacket(1, 0, 0x10));
  expect(channelOne.size() == 2 && channelOne[0].payload == std::vector<uint8_t>({0x10}) &&
             channelOne[1].payload == std::vector<uint8_t>({0x11}),
         "per-channel reliable ordering release changed");

  // The legacy u16 ordering index wraps. Advance to 65535, receive zero early,
  // then prove delivery releases the final pre-wrap index before its buffered
  // post-wrap successor.
  gate.clear();
  for (uint32_t index = 0; index < 0xFFFFu; ++index) {
    const auto one = gate.push(orderedPacket(2, static_cast<uint16_t>(index), 0x22));
    expect(one.size() == 1, "in-order reliable-ordered frame must pass immediately");
  }
  expect(gate.push(orderedPacket(2, 0, 0x23)).empty(),
         "post-wrap reliable-ordered frame must wait for index 65535");
  const auto wrapped = gate.push(orderedPacket(2, 0xFFFFu, 0x24));
  expect(wrapped.size() == 2 && wrapped[0].payload == std::vector<uint8_t>({0x24}) &&
             wrapped[1].payload == std::vector<uint8_t>({0x23}),
         "reliable-ordered buffer/release across u16 wrap changed");

  gate.clear();
  for (size_t index = 1; index <= samp::raknet::ReliableOrderedGate::kMaxBufferedPacketsPerChannel;
       ++index) {
    expect(gate.push(orderedPacket(3, static_cast<uint16_t>(index), 0x33)).empty(),
           "bounded reliable-ordered prebuffer should accept its configured window");
  }
  expect(gate.pendingCount() == samp::raknet::ReliableOrderedGate::kMaxBufferedPacketsPerChannel &&
             !gate.overflowed(),
         "reliable-ordered pending window accounting changed");
  expect(gate.push(orderedPacket(
                    3, static_cast<uint16_t>(samp::raknet::ReliableOrderedGate::kMaxBufferedPacketsPerChannel + 1),
                    0x34)).empty() &&
             gate.overflowed(),
         "reliable-ordered overflow must be observable, not silently accepted");
  gate.clear();
  expect(gate.pendingCount() == 0 && gate.pendingBytes() == 0 && !gate.overflowed(),
         "reliable-ordered clear must reset bounded pending state");
}

void testRakNetMalformedDatagramSmoke() {
  // Deterministic malformed-input smoke coverage for the full bounded ingress
  // ordering: decode → reliable receipt → split → reliable-order/sequenced.
  // This is not a protocol oracle; it locks the promise that arbitrary UDP
  // bytes cannot escape the codec/gate resource boundaries or crash the host.
  uint32_t prng = 0xA51C9E37u;
  const auto next = [&prng]() {
    prng ^= prng << 13u;
    prng ^= prng >> 17u;
    prng ^= prng << 5u;
    return prng;
  };
  samp::raknet::ReliableMessageGate reliableReceipt;
  samp::raknet::SplitReassembler reassembler;
  samp::raknet::ReliableOrderedGate reliableOrdered;
  samp::raknet::SequencedPacketGate sequenced;
  for (size_t round = 0; round < 4096u; ++round) {
    const size_t length = static_cast<size_t>(next() % 2049u);
    std::vector<uint8_t> bytes(length);
    for (uint8_t& value : bytes) value = static_cast<uint8_t>(next());
    const auto datagram = samp::raknet::decodeDatagram(bytes);
    for (const auto& raw : datagram.packets) {
      const bool reliable = raw.reliability == samp::raknet::kReliabilityReliable ||
          raw.reliability == samp::raknet::kReliabilityReliableOrdered ||
          raw.reliability == samp::raknet::kReliabilityReliableSequenced;
      if (reliable && !reliableReceipt.accept(raw.messageNumber)) continue;
      auto complete = reassembler.push(raw);
      if (reassembler.overflowed()) {
        reassembler.clear();
        reliableReceipt.clear();
        reliableOrdered.clear();
        sequenced.clear();
        continue;
      }
      for (auto packet : complete) {
        if (packet.reliability == samp::raknet::kReliabilityReliableOrdered) {
          (void)reliableOrdered.push(std::move(packet));
          if (reliableOrdered.overflowed()) reliableOrdered.clear();
        } else if ((packet.reliability == samp::raknet::kReliabilityUnreliableSequenced ||
                    packet.reliability == samp::raknet::kReliabilityReliableSequenced)) {
          (void)sequenced.accept(packet.orderingChannel, packet.orderingIndex);
        }
      }
    }
  }
  expect(reassembler.pendingBytes() <= samp::raknet::SplitReassembler::kMaxPendingBytes &&
             reliableOrdered.pendingBytes() <= samp::raknet::ReliableOrderedGate::kMaxBufferedBytesTotal &&
             reliableReceipt.seenCount() <= samp::raknet::ReliableMessageGate::kHistoryWindow,
         "malformed datagram smoke exceeded a bounded ingress resource limit");
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
  // SyncRPC (2) and SyncPacket (1) are separate streams, and RakNet also
  // stores their ordered/sequenced indices in independent arrays.
  expect(ordering.takeRpc() == 0 && ordering.takeRpc() == 1 && ordering.takeSync() == 0 &&
         ordering.takeRpc() == 2 && ordering.takeSync() == 1,
         "RPC and USeq sync ordering counters must remain independent");

  expect(samp::syncpolicy::boundedOnFootRateMs(0) == 30 &&
         samp::syncpolicy::boundedOnFootRateMs(1) == 20 &&
         samp::syncpolicy::boundedOnFootRateMs(5000) == 1000,
         "on-foot rate bounds changed");
  expect(samp::syncpolicy::onFootSendDue(100, 0, 30) &&
         !samp::syncpolicy::onFootSendDue(129, 100, 30) &&
         samp::syncpolicy::onFootSendDue(130, 100, 30) &&
         !samp::syncpolicy::onFootSendDue(99, 100, 30),
         "on-foot rate gate changed");
  expect(samp::syncpolicy::boundedInCarRateMs(0) == 30 &&
         samp::syncpolicy::boundedInCarRateMs(1) == 20 &&
         samp::syncpolicy::boundedInCarRateMs(5000) == 1000 &&
         samp::syncpolicy::inCarSendDue(100, 0, 30) &&
         !samp::syncpolicy::inCarSendDue(129, 100, 30) &&
         samp::syncpolicy::inCarSendDue(130, 100, 30),
         "in-car rate bounds/gate changed");
  expect(samp::syncpolicy::boundedWeaponRateMs(0) == 30 &&
         samp::syncpolicy::boundedWeaponRateMs(1) == 20 &&
         samp::syncpolicy::boundedWeaponRateMs(5000) == 1000 &&
         samp::syncpolicy::weaponSendDue(100, 0, 30) &&
         !samp::syncpolicy::weaponSendDue(129, 100, 30) &&
         samp::syncpolicy::weaponSendDue(130, 100, 30),
         "weapon rate bounds/gate changed");
  expect(samp::syncpolicy::onFootSessionReady(true, true, true, true, true) &&
         !samp::syncpolicy::onFootSessionReady(false, true, true, true, true) &&
         !samp::syncpolicy::onFootSessionReady(true, false, true, true, true) &&
         !samp::syncpolicy::onFootSessionReady(true, true, false, true, true) &&
         !samp::syncpolicy::onFootSessionReady(true, true, true, false, true) &&
         !samp::syncpolicy::onFootSessionReady(true, true, true, true, false),
         "on-foot spawn/ACK gate changed");
}

void testPlayerVehicleSyncBuilder() {
  samp::sync::InCarState idle;
  idle.vehicleId = 1;
  const auto idlePayload = samp::sync::buildPlayerVehicleSync(idle);
  expect(idlePayload.valid() && idlePayload.bitLength == 512 && idlePayload.bytes.size() == 64,
         "idle PlayerVehicleSync fixed layout changed");
  std::vector<uint8_t> expectedIdle(64, 0);
  expectedIdle[0] = 200;
  expectedIdle[1] = 1;
  expectedIdle[11] = 0x80;
  expectedIdle[12] = 0x3F;
  expectedIdle[51] = 0x7A;
  expectedIdle[52] = 0x44;
  expectedIdle[53] = 100;
  expect(idlePayload.bytes == expectedIdle, "idle PlayerVehicleSync wire vector changed");

  samp::sync::InCarState state;
  state.vehicleId = 0xBEEF;
  state.leftRight = 0xFFFF;
  state.upDown = 7;
  state.keys = 0x1234;
  state.quaternionW = -0.5f;
  state.quaternionX = 0.5f;
  state.quaternionY = -0.5f;
  state.quaternionZ = 0.5f;
  state.positionX = 1.5f;
  state.positionY = -2.25f;
  state.positionZ = 3.75f;
  state.velocityX = 3.0f;
  state.velocityY = 4.0f;
  state.vehicleHealth = 1000.0f;
  state.playerHealth = 99;
  state.playerArmour = 50;
  state.additionalKeyWeapon = 0x98;
  state.siren = 1;
  state.trailerId = 123;
  state.hydraThrustAngle = 0x12345678u;
  const auto payload = samp::sync::buildPlayerVehicleSync(state);
  const std::vector<uint8_t> expected = {
      0xC8, 0xEF, 0xBE, 0xFF, 0xFF, 0x07, 0x00, 0x34, 0x12,
      0x00, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x10, 0xC0,
      0x00, 0x00, 0x70, 0x40, 0x00, 0x00, 0x40, 0x40,
      0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x7A, 0x44, 0x63, 0x32, 0x98, 0x01,
      0x00, 0x7B, 0x00, 0x78, 0x56, 0x34, 0x12};
  expect(payload.valid() && payload.bytes == expected,
         "PlayerVehicleSync client reader-order vector changed");

  state.vehicleId = 0;
  expect(!samp::sync::isValidInCarState(state) && !samp::sync::buildPlayerVehicleSync(state).valid(),
         "zero vehicle ID must reject PlayerVehicleSync");
  state.vehicleId = 1;
  state.velocityX = 101.0f;
  expect(!samp::sync::isValidInCarState(state),
         "out-of-range PlayerVehicleSync velocity must be rejected");
  state.velocityX = 0.0f;
  state.siren = 2;
  expect(!samp::sync::isValidInCarState(state),
         "nonboolean PlayerVehicleSync siren must be rejected");
}

void testPlayerAimSyncBuilder() {
  samp::sync::AimState state;
  state.cameraMode = 53;
  state.cameraFrontX = 0.0f;
  state.cameraFrontY = 1.0f;
  state.cameraFrontZ = 0.0f;
  state.cameraPositionX = 1.5f;
  state.cameraPositionY = -2.25f;
  state.cameraPositionZ = 3.75f;
  state.aimZ = -0.5f;
  state.cameraZoom = 37;
  state.weaponState = 2;
  state.aspectRatio = 0x7F;
  const auto payload = samp::sync::buildPlayerAimSync(state);
  const std::vector<uint8_t> expected = {
      0xCB, 0x35,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x10, 0xC0, 0x00, 0x00, 0x70, 0x40,
      0x00, 0x00, 0x00, 0xBF, 0xA5, 0x7F};
  expect(payload.valid() && payload.bitLength == 256 && payload.bytes == expected,
         "PlayerAimSync client writer vector changed");

  state.cameraMode = 0;
  expect(!samp::sync::isValidAimState(state) && !samp::sync::buildPlayerAimSync(state).valid(),
         "invalid PlayerAimSync camera mode must reject");
  state.cameraMode = 53;
  state.cameraFrontY = 0.0f;
  expect(!samp::sync::isValidAimState(state),
         "zero PlayerAimSync front vector must reject");
  state.cameraFrontY = 1.0f;
  state.cameraZoom = 64;
  expect(!samp::sync::isValidAimState(state),
         "six-bit PlayerAimSync zoom overflow must reject");
  state.cameraZoom = 0;
  state.weaponState = 4;
  expect(!samp::sync::isValidAimState(state),
         "two-bit PlayerAimSync weapon state overflow must reject");
  state.weaponState = 0;
  state.cameraPositionX = 20000.0f;
  expect(!samp::sync::isValidAimState(state),
         "out-of-range PlayerAimSync camera position must reject");
}

void testPlayerNoTargetBulletSyncBuilder() {
  // Wire order audited against open.mp PlayerBulletSync::read and its NPC
  // client-originated generator at commit 3d20f3dc; this remains a local
  // serializer regression, not a live-server compatibility result.
  samp::sync::BulletState state;
  state.originX = 1.5f;
  state.originY = -2.25f;
  state.originZ = 3.75f;
  state.weaponId = 24;
  const auto payload = samp::sync::buildPlayerNoTargetBulletSync(state);
  const std::vector<uint8_t> expected = {
      0xCE, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xC0, 0x3F,
      0x00, 0x00, 0x10, 0xC0,
      0x00, 0x00, 0x70, 0x40,
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x18};
  expect(payload.valid() && payload.bitLength == 328 && payload.bytes == expected,
         "PlayerBulletSync no-target client writer vector changed");

  state.hitType = 1;
  expect(!samp::sync::isValidNoTargetBulletState(state) &&
             !samp::sync::buildPlayerNoTargetBulletSync(state).valid(),
         "targeted PlayerBulletSync must reject at Android boundary");
  state.hitType = 0;
  state.hitId = 1;
  expect(!samp::sync::isValidNoTargetBulletState(state),
         "no-target PlayerBulletSync must keep neutral hit ID");
  state.hitId = 0;
  state.hitPositionX = 1.0f;
  state.hitPositionY = 2.0f;
  state.hitPositionZ = 3.0f;
  auto expectedStaticWorld = expected;
  expectedStaticWorld[16] = 0x00;
  expectedStaticWorld[17] = 0x00;
  expectedStaticWorld[18] = 0x80;
  expectedStaticWorld[19] = 0x3F;
  expectedStaticWorld[20] = 0x00;
  expectedStaticWorld[21] = 0x00;
  expectedStaticWorld[22] = 0x00;
  expectedStaticWorld[23] = 0x40;
  expectedStaticWorld[24] = 0x00;
  expectedStaticWorld[25] = 0x00;
  expectedStaticWorld[26] = 0x40;
  expectedStaticWorld[27] = 0x40;
  const auto staticWorldPayload = samp::sync::buildPlayerNoTargetBulletSync(state);
  expect(samp::sync::isValidNoTargetBulletState(state) && staticWorldPayload.valid() &&
             staticWorldPayload.bytes == expectedStaticWorld,
         "no-target PlayerBulletSync must encode a bounded absolute static-world point");
  state.hitPositionX = std::numeric_limits<float>::quiet_NaN();
  expect(!samp::sync::isValidNoTargetBulletState(state),
         "no-target PlayerBulletSync must reject a non-finite static-world point");
  state.hitPositionX = 0.0f;
  state.hitPositionY = 0.0f;
  state.hitPositionZ = 0.0f;
  state.offsetX = 1.0f;
  expect(!samp::sync::isValidNoTargetBulletState(state),
         "no-target PlayerBulletSync must retain neutral offsets");
  state.offsetX = 0.0f;
  state.weaponId = 35;
  expect(!samp::sync::isValidNoTargetBulletState(state),
         "nonsupported PlayerBulletSync weapon must reject");
  state.weaponId = 24;
  state.originX = 20000.0f;
  expect(!samp::sync::isValidNoTargetBulletState(state),
         "out-of-range PlayerBulletSync origin must reject");
}

void testPlayerPassengerSyncBuilder() {
  samp::sync::PassengerState idle;
  idle.vehicleId = 1;
  idle.seatId = 1;
  const auto idlePayload = samp::sync::buildPlayerPassengerSync(idle);
  expect(idlePayload.valid() && idlePayload.bitLength == 200 && idlePayload.bytes.size() == 25,
         "idle PlayerPassengerSync fixed layout changed");
  std::vector<uint8_t> expectedIdle(25, 0);
  expectedIdle[0] = 211;
  expectedIdle[1] = 1;
  expectedIdle[3] = 1;
  expectedIdle[5] = 100;
  expect(idlePayload.bytes == expectedIdle, "idle PlayerPassengerSync wire vector changed");

  samp::sync::PassengerState state;
  state.vehicleId = 0xBEEF;
  state.seatId = 3;
  state.driveBy = true;
  state.cuffed = true;
  state.weaponAdditionalKey = 0x98;
  state.health = 99;
  state.armour = 50;
  state.leftRight = 0xFFFF;
  state.upDown = 7;
  state.keys = 0x1234;
  state.positionX = 1.5f;
  state.positionY = -2.25f;
  state.positionZ = 3.75f;
  const auto payload = samp::sync::buildPlayerPassengerSync(state);
  const std::vector<uint8_t> expected = {
      0xD3, 0xEF, 0xBE, 0xC3, 0x98, 0x63, 0x32, 0xFF, 0xFF, 0x07, 0x00, 0x34, 0x12,
      0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x10, 0xC0, 0x00, 0x00, 0x70, 0x40};
  expect(payload.valid() && payload.bytes == expected,
         "PlayerPassengerSync client reader-order vector changed");

  state.seatId = 0;
  expect(!samp::sync::isValidPassengerState(state) &&
         !samp::sync::buildPlayerPassengerSync(state).valid(),
         "driver seat must reject PlayerPassengerSync");
  state.seatId = 64;
  expect(!samp::sync::isValidPassengerState(state),
         "six-bit overflow seat must reject PlayerPassengerSync");
  state.seatId = 1;
  state.health = 101;
  expect(!samp::sync::isValidPassengerState(state),
         "out-of-range PlayerPassengerSync health must reject");
  state.health = 100;
  state.positionX = 20000.0f;
  expect(!samp::sync::isValidPassengerState(state),
         "out-of-range PlayerPassengerSync position must reject");
}

void testRemoteAimBroadcastDecoder() {
  const auto payload = makeRemoteAimBroadcast(7);
  const std::vector<uint8_t> expected = {
      0xCB, 0x07, 0x00, 0x35,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x10, 0xC0, 0x00, 0x00, 0x70, 0x40,
      0x00, 0x00, 0x00, 0xBF, 0xA5, 0x7F};
  expect(payload == expected && payload.size() == 34,
         "remote PlayerAimSync broadcast wire vector changed");

  samp::syncdecode::RemoteAimSync remote;
  expect(samp::syncdecode::decodeRemoteAimBroadcast(payload, remote) &&
         remote.playerId == 7 && remote.cameraMode == 53 && remote.cameraZoom == 37 &&
         remote.weaponState == 2 && remote.aspectRatio == 0x7F &&
         std::fabs(remote.cameraFrontX) < 0.001f &&
         std::fabs(remote.cameraFrontY - 1.0f) < 0.001f &&
         std::fabs(remote.cameraFrontZ) < 0.001f &&
         std::fabs(remote.cameraPositionX - 1.5f) < 0.001f &&
         std::fabs(remote.cameraPositionY + 2.25f) < 0.001f &&
         std::fabs(remote.cameraPositionZ - 3.75f) < 0.001f &&
         std::fabs(remote.aimZ + 0.5f) < 0.001f,
         "remote PlayerAimSync broadcast decoder changed");
  expect(samp::syncdecode::summarize(remote).find("#7") != std::string::npos,
         "remote PlayerAimSync summary missing player ID");

  auto truncated = payload;
  truncated.pop_back();
  expect(!samp::syncdecode::decodeRemoteAimBroadcast(truncated, remote),
         "truncated remote PlayerAimSync must be rejected");
  expect(!samp::syncdecode::decodeRemoteAimBroadcast(payload, remote, payload.size() * 8 - 1),
         "declared-short remote PlayerAimSync must be rejected");
  auto zeroFront = payload;
  zeroFront[8] = 0;
  zeroFront[9] = 0;
  zeroFront[10] = 0;
  zeroFront[11] = 0;
  expect(!samp::syncdecode::decodeRemoteAimBroadcast(zeroFront, remote),
         "zero remote PlayerAimSync front vector must reject");
  auto invalidMode = payload;
  invalidMode[3] = 5;
  expect(!samp::syncdecode::decodeRemoteAimBroadcast(invalidMode, remote),
         "invalid remote PlayerAimSync camera mode must reject");
  auto invalidAimZ = payload;
  invalidAimZ[28] = 0;
  invalidAimZ[29] = 0;
  invalidAimZ[30] = 0xC0;
  invalidAimZ[31] = 0x7F;  // IEEE quiet NaN, little-endian
  expect(!samp::syncdecode::decodeRemoteAimBroadcast(invalidAimZ, remote),
         "non-finite remote PlayerAimSync AimZ must reject");
}

void testRemoteBulletBroadcastDecoder() {
  const auto payload = makeRemoteBulletBroadcast(7);
  const std::vector<uint8_t> expected = {
      0xCE, 0x07, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0x10, 0xC0, 0x00, 0x00, 0x70, 0x40,
      0x00, 0x00, 0x38, 0x41, 0x00, 0x00, 0x10, 0xC0, 0x00, 0x00, 0x70, 0x40,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x18};
  expect(payload == expected && payload.size() == 43,
         "remote PlayerBulletSync broadcast wire vector changed");

  samp::syncdecode::RemoteBulletSync remote;
  expect(samp::syncdecode::decodeRemoteBulletBroadcast(payload, remote) &&
         remote.playerId == 7 && remote.hitType == 0 && remote.hitId == 0 && remote.weaponId == 24 &&
         std::fabs(remote.originX - 1.5f) < 0.001f &&
         std::fabs(remote.originY + 2.25f) < 0.001f &&
         std::fabs(remote.originZ - 3.75f) < 0.001f &&
         std::fabs(remote.hitPositionX - 11.5f) < 0.001f &&
         std::fabs(remote.hitPositionY + 2.25f) < 0.001f &&
         std::fabs(remote.hitPositionZ - 3.75f) < 0.001f,
         "remote PlayerBulletSync broadcast decoder changed");
  expect(samp::syncdecode::summarize(remote).find("weapon 24") != std::string::npos,
         "remote PlayerBulletSync summary missing weapon");
  auto relayedPlayerHit = payload;
  relayedPlayerHit[3] = 1;  // PlayerBulletHitType_Player
  relayedPlayerHit[4] = 9;
  relayedPlayerHit[5] = 0;
  expect(samp::syncdecode::decodeRemoteBulletBroadcast(relayedPlayerHit, remote) &&
         remote.hitType == 1 && remote.hitId == 9,
         "valid server-relayed remote PlayerBulletSync player hit must remain decodable");

  auto truncated = payload;
  truncated.pop_back();
  expect(!samp::syncdecode::decodeRemoteBulletBroadcast(truncated, remote),
         "truncated remote PlayerBulletSync must be rejected");
  expect(!samp::syncdecode::decodeRemoteBulletBroadcast(payload, remote, payload.size() * 8 - 1),
         "declared-short remote PlayerBulletSync must be rejected");
  auto invalidHitType = payload;
  invalidHitType[3] = 5;
  expect(!samp::syncdecode::decodeRemoteBulletBroadcast(invalidHitType, remote),
         "invalid remote PlayerBulletSync hit type must reject");
  auto invalidWeapon = payload;
  invalidWeapon.back() = 35;
  expect(!samp::syncdecode::decodeRemoteBulletBroadcast(invalidWeapon, remote),
         "non-bullet remote PlayerBulletSync weapon must reject");
  auto invalidHitPosition = payload;
  invalidHitPosition[18] = 0;
  invalidHitPosition[19] = 0;
  invalidHitPosition[20] = 0xC0;
  invalidHitPosition[21] = 0x7F;  // IEEE quiet NaN, little-endian
  expect(!samp::syncdecode::decodeRemoteBulletBroadcast(invalidHitPosition, remote),
         "non-finite remote PlayerBulletSync position must reject");
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

void testRemoteVehicleBroadcastDecoder() {
  samp::syncdecode::RemoteVehicleSync remote;
  const auto driver = makeRemoteVehicleDriverBroadcast(7, 90);
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(driver, remote) &&
         remote.driverSync && remote.playerId == 7 && remote.vehicleId == 90 &&
         std::fabs(remote.x - 125.0f) < 0.001f && std::fabs(remote.y + 75.0f) < 0.001f &&
         std::fabs(remote.z - 6.0f) < 0.001f && std::fabs(remote.health - 875.0f) < 0.001f &&
         std::fabs(remote.velocityX - 3.0f) < 0.02f && std::fabs(remote.velocityY - 4.0f) < 0.02f &&
         remote.attitudeKnown && std::fabs(remote.rotationX) < 0.2f &&
         std::fabs(remote.rotationY) < 0.2f && std::fabs(remote.rotationZ - 90.0f) < 0.2f &&
         std::fabs(remote.angle - 90.0f) < 0.2f,
         "remote PlayerVehicleSync decode/attitude changed");
  constexpr float kRootHalf = 0.70710678f;
  const auto pitched = makeRemoteVehicleDriverBroadcast(7, 90, kRootHalf, 0.0f, kRootHalf, 0.0f);
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(pitched, remote) && remote.attitudeKnown &&
         std::fabs(remote.rotationX) < 0.2f && std::fabs(remote.rotationY + 90.0f) < 0.2f &&
         std::fabs(remote.rotationZ) < 0.2f,
         "Packet-200 quaternion pitch must survive source-derived DFF attitude reduction");
  expect(samp::syncdecode::summarize(remote).find("#90") != std::string::npos,
         "remote vehicle sync summary missing vehicle ID");

  const auto unoccupied = makeRemoteUnoccupiedVehicleBroadcast(8, 90);
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(unoccupied, remote) &&
         !remote.driverSync && remote.playerId == 8 && remote.vehicleId == 90 &&
         std::fabs(remote.x - 225.0f) < 0.001f && std::fabs(remote.y + 175.0f) < 0.001f &&
         std::fabs(remote.z - 8.0f) < 0.001f && std::fabs(remote.health - 640.0f) < 0.001f,
         "remote PlayerUnoccupiedSync decode changed");
  auto truncated = driver;
  truncated.pop_back();
  expect(!samp::syncdecode::decodeRemoteVehicleBroadcast(truncated, remote),
         "truncated remote PlayerVehicleSync must be rejected");
  expect(!samp::syncdecode::decodeRemoteVehicleBroadcast({209, 0, 0, 0, 0}, remote),
         "malformed remote vehicle sync must be rejected");
}

void testRemoteTrailerBroadcastDecoder() {
  samp::syncdecode::RemoteTrailerSync remote;
  const auto packet = makeRemoteTrailerBroadcast(7u, 90u);
  const std::vector<uint8_t> expected = {
      0xD2, 0x07, 0x00, 0x5A, 0x00, 0x00, 0x80, 0xA2, 0x43,
      0x00, 0x80, 0x89, 0xC3, 0x00, 0x00, 0xF0, 0x40,
      0xF3, 0x04, 0x35, 0x3F, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xF3, 0x04, 0x35, 0xBF,
      0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x80, 0x40,
      0x00, 0x00, 0x00, 0x00, 0xCD, 0xCC, 0xCC, 0x3D,
      0xCD, 0xCC, 0x4C, 0xBE, 0x9A, 0x99, 0x99, 0x3E};
  expect(packet == expected && packet.size() == 57u,
         "remote PlayerTrailerSync broadcast writer vector changed");
  expect(samp::syncdecode::decodeRemoteTrailerBroadcast(packet, remote) &&
         remote.playerId == 7u && remote.vehicleId == 90u && remote.attitudeKnown &&
         std::fabs(remote.x - 325.0f) < 0.001f && std::fabs(remote.y + 275.0f) < 0.001f &&
         std::fabs(remote.z - 7.5f) < 0.001f && std::fabs(remote.velocityX - 3.0f) < 0.001f &&
         std::fabs(remote.velocityY - 4.0f) < 0.001f &&
         std::fabs(remote.turnVelocityX - 0.1f) < 0.001f &&
         std::fabs(remote.turnVelocityY + 0.2f) < 0.001f &&
         std::fabs(remote.turnVelocityZ - 0.3f) < 0.001f &&
         std::fabs(remote.rotationZ - 90.0f) < 0.2f,
         "remote PlayerTrailerSync decode/raw quaternion reduction changed");
  expect(samp::syncdecode::summarize(remote).find("trailer #90") != std::string::npos,
         "remote trailer summary missing vehicle ID");

  auto truncated = packet;
  truncated.pop_back();
  expect(!samp::syncdecode::decodeRemoteTrailerBroadcast(truncated, remote),
         "truncated remote PlayerTrailerSync must be rejected");
  expect(!samp::syncdecode::decodeRemoteTrailerBroadcast(packet, remote, packet.size() * 8 - 1),
         "declared-short remote PlayerTrailerSync must be rejected");
  auto invalidQuaternion = packet;
  invalidQuaternion[17] = 0x00u;
  invalidQuaternion[18] = 0x00u;
  invalidQuaternion[19] = 0xC0u;
  invalidQuaternion[20] = 0x7Fu;  // IEEE quiet NaN, little-endian
  expect(!samp::syncdecode::decodeRemoteTrailerBroadcast(invalidQuaternion, remote),
         "non-finite remote trailer quaternion must reject");
  auto invalidTurnVelocity = packet;
  invalidTurnVelocity[45] = 0x00u;
  invalidTurnVelocity[46] = 0x00u;
  invalidTurnVelocity[47] = 0x00u;
  invalidTurnVelocity[48] = 0x40u;  // 2.0, beyond source turn envelope
  expect(!samp::syncdecode::decodeRemoteTrailerBroadcast(invalidTurnVelocity, remote),
         "out-of-envelope remote trailer turn velocity must reject");
  expect(samp::syncdecode::decodeRemoteTrailerBroadcast(packet, remote),
         "valid remote trailer fixture must decode again after malformed input");

  samp::state::WorldState world;
  samp::state::RpcEvent event;
  expect(world.applyRpc(makeStreamInVehicle(90u, 435u, 10.0f, 20.0f, 5.0f, 0.0f,
                                            1u, 2u, 850.0f), event) &&
         world.applyRpc(makePlayerStreamIn(7u, 0u, 7u, 0.0f, 0.0f, 3.0f, 0.0f,
                                            0xFFFFFFFFu, 0u), event),
         "remote trailer lifecycle fixtures rejected");
  samp::syncdecode::RemoteVehicleSync driver;
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteVehicleDriverBroadcast(7u, 90u), driver) &&
         world.applyRemoteVehicle(driver, event) && world.findVehicle(90u)->driverKnown &&
         world.findVehicle(90u)->driverId == 7u &&
         std::fabs(world.findVehicle(90u)->health - 875.0f) < 0.001f,
         "remote trailer fixture driver baseline rejected");
  expect(world.applyRemoteTrailer(remote, event) && event.kind == samp::state::RpcEventKind::RemoteTrailerSync &&
         event.playerId == 90u && world.findVehicle(90u)->dynamicSyncKnown &&
         world.findVehicle(90u)->driverKnown && world.findVehicle(90u)->driverId == 7u &&
         std::fabs(world.findVehicle(90u)->x - 325.0f) < 0.001f &&
         std::fabs(world.findVehicle(90u)->rotationZ - 90.0f) < 0.2f &&
         std::fabs(world.findVehicle(90u)->health - 875.0f) < 0.001f,
         "Packet-210 must update only an existing trailer transform, preserving driver/health facts");
  samp::state::WorldState unknownWorld;
  expect(!unknownWorld.applyRemoteTrailer(remote, event),
         "Packet-210 must not allocate an unstreamed trailer vehicle");
}

void testRemotePassengerBroadcastDecoder() {
  samp::syncdecode::RemotePassengerSync remote;
  const auto packet = makeRemotePassengerBroadcast(0, 90, 3);
  expect(packet.size() == 27 &&
         samp::syncdecode::decodeRemotePassengerBroadcast(packet, remote) &&
         remote.playerId == 0 && remote.vehicleId == 90 && remote.seatId == 3 && remote.driveBy &&
         !remote.cuffed && remote.weaponAdditionalKey == 0x82 && std::fabs(remote.health - 87.0f) < 0.001f &&
         std::fabs(remote.armour - 31.0f) < 0.001f && remote.leftRight == -7 && remote.upDown == 9 &&
         remote.keys == 0x4321 && std::fabs(remote.x - 121.0f) < 0.001f &&
         std::fabs(remote.y + 74.0f) < 0.001f && std::fabs(remote.z - 6.5f) < 0.001f,
         "remote PlayerPassengerSync decode changed");
  expect(samp::syncdecode::summarize(remote).find("seat 3") != std::string::npos,
         "remote passenger sync summary missing seat");
  auto truncated = packet;
  truncated.pop_back();
  expect(!samp::syncdecode::decodeRemotePassengerBroadcast(truncated, remote),
         "truncated remote PlayerPassengerSync must be rejected");
  expect(!samp::syncdecode::decodeRemotePassengerBroadcast(packet, remote, packet.size() * 8 - 1),
         "declared-short remote PlayerPassengerSync must be rejected");
  auto invalidHealth = packet;
  invalidHealth[7] = 101;  // byte-aligned health after ID/player/vehicle/seat
  expect(!samp::syncdecode::decodeRemotePassengerBroadcast(invalidHealth, remote),
         "out-of-range remote passenger health must be rejected");
}

void testNativeInputAndLocalPreview() {
  samp::input::TouchIntent raw;
  raw.moveX = 0.5f;
  raw.moveY = 1.0f;
  raw.lookDeltaDegrees = 90.0f;
  raw.lookPitchDeltaDegrees = 30.0f;
  raw.heldActions = samp::input::kHeldUse | samp::input::kHeldFire |
                    samp::input::kHeldSprint | samp::input::kHeldJump |
                    samp::input::kHeldAim;

  samp::input::TouchIntent sanitized;
  expect(samp::input::sanitizeTouchIntent(raw, sanitized) &&
         std::fabs(sanitized.moveX - 0.5f) < 0.001f &&
         std::fabs(sanitized.lookPitchDeltaDegrees - 30.0f) < 0.001f &&
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
         std::fabs(wire.headingDegrees - 90.0f) < 0.001f &&
         std::fabs(wire.aimPitchDegrees - 30.0f) < 0.001f,
         "semantic touch-to-SA-MP input mapping changed");

  raw.moveX = 4.0f;
  raw.moveY = -4.0f;
  raw.lookDeltaDegrees = -180.0f;
  raw.lookPitchDeltaDegrees = -180.0f;
  raw.heldActions = 0;
  expect(mapper.update(raw), "bounded touch input should be accepted");
  const auto bounded = mapper.wireInput();
  expect(bounded.leftRight == 128 && bounded.upDown == 128 && bounded.keys == 0 &&
         std::fabs(bounded.headingDegrees - 0.0f) < 0.001f &&
         std::fabs(bounded.aimPitchDegrees + 60.0f) < 0.001f,
         "touch axis/look bounds changed");
  raw.lookPitchDeltaDegrees = -90.0f;
  expect(mapper.update(raw) && std::fabs(mapper.wireInput().aimPitchDegrees + 75.0f) < 0.001f,
         "aim pitch must stop at the native -75 degree bound");

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
  local.armedWeaponKnown = true;
  local.armedWeapon = 24;
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
         preview.state.weaponAdditionalKey == 24 && preview.state.specialAction == 3 &&
         preview.state.velocityY == 4.0f &&
         samp::sync::buildPlayerFootSync(preview.state).valid(),
         "local player OnFoot preview bridge changed");

  local.controllableKnown = true;
  local.controllable = false;
  expect(samp::bridge::buildOnFootPreview(local, wire).status ==
             samp::bridge::OnFootPreviewStatus::InvalidState,
         "server freeze must block local OnFoot preview");
  local.controllable = true;
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

  // The partially assembled state is moved from the synchronous handshake
  // into LiveSession, so an out-of-order fragment must survive that hand-off.
  samp::raknet::SplitReassembler handoffReassembler;
  samp::raknet::IncomingPacket handoffSecond;
  handoffSecond.split = true;
  handoffSecond.splitId = 79;
  handoffSecond.splitIndex = 1;
  handoffSecond.splitCount = 2;
  handoffSecond.payload = {0x12};
  expect(handoffReassembler.push(handoffSecond).empty(),
         "split hand-off fixture completed too early");
  samp::raknet::SplitReassembler movedReassembler = std::move(handoffReassembler);
  samp::raknet::IncomingPacket handoffFirst;
  handoffFirst.split = true;
  handoffFirst.splitId = 79;
  handoffFirst.splitIndex = 0;
  handoffFirst.splitCount = 2;
  handoffFirst.payload = {0x11};
  const auto handoffResult = movedReassembler.push(handoffFirst);
  expect(handoffResult.size() == 1 &&
             handoffResult[0].payload == std::vector<uint8_t>({0x11, 0x12}),
         "moving split reassembler must preserve handshake fragments");

  // RakNet copies reliability/order metadata to every fragment. If a wrapped
  // split ID is recycled with a different transport identity, its body must
  // replace—not splice into—the stale retained sequence and its accounting.
  samp::raknet::SplitReassembler identityReassembler;
  samp::raknet::IncomingPacket staleIdentity;
  staleIdentity.split = true;
  staleIdentity.splitId = 80;
  staleIdentity.splitIndex = 1;
  staleIdentity.splitCount = 2;
  staleIdentity.reliability = samp::raknet::kReliabilityReliableOrdered;
  staleIdentity.orderingChannel = 2;
  staleIdentity.orderingIndex = 9;
  staleIdentity.payload = {0xA1};
  expect(identityReassembler.push(staleIdentity).empty() && identityReassembler.pendingBytes() == 1u,
         "split transport-identity fixture failed to retain stale fragment");
  samp::raknet::IncomingPacket replacementIdentity = staleIdentity;
  replacementIdentity.splitIndex = 0;
  replacementIdentity.orderingChannel = 3;
  replacementIdentity.orderingIndex = 4;
  replacementIdentity.payload = {0xB0};
  expect(identityReassembler.push(replacementIdentity).empty() &&
             identityReassembler.pendingBytes() == 1u,
         "recycled split transport identity must replace old accounting");
  samp::raknet::IncomingPacket replacementTail = replacementIdentity;
  replacementTail.splitIndex = 1;
  replacementTail.payload = {0xB1};
  const auto identityResult = identityReassembler.push(replacementTail);
  expect(identityResult.size() == 1 &&
             identityResult[0].payload == std::vector<uint8_t>({0xB0, 0xB1}) &&
             identityResult[0].orderingChannel == 3u && identityResult[0].orderingIndex == 4u &&
             identityReassembler.pendingBytes() == 0u,
         "split fragments with mismatched transport identity were spliced");

  samp::raknet::IncomingPacket ambiguous;
  ambiguous.split = true;
  ambiguous.splitId = 78;
  ambiguous.splitIndex = 0;
  ambiguous.splitCount = 2;
  ambiguous.payload = {0xA8};
  ambiguous.payloadBitLength = 5;
  expect(reassembler.push(ambiguous).empty() && reassembler.pendingCount() == 0,
         "non-byte-aligned non-final split must be rejected");

  samp::raknet::IncomingPacket oversizedBacking;
  oversizedBacking.payload = {0x01, 0x02};
  oversizedBacking.payloadBitLength = 8;
  expect(reassembler.push(oversizedBacking).empty(),
         "split reassembler must reject bytes beyond declared payload bits");

  samp::raknet::SplitReassembler sequenceBounded;
  for (size_t index = 0; index < samp::raknet::SplitReassembler::kMaxPendingSequences; ++index) {
    samp::raknet::IncomingPacket fragment;
    fragment.split = true;
    fragment.splitId = static_cast<uint16_t>(1000u + index);
    fragment.splitIndex = 0;
    fragment.splitCount = 2;
    fragment.payload = {0x55};
    expect(sequenceBounded.push(fragment).empty(),
           "bounded split sequence fixture should remain incomplete");
  }
  expect(sequenceBounded.pendingCount() == samp::raknet::SplitReassembler::kMaxPendingSequences &&
             !sequenceBounded.overflowed(),
         "split pending-sequence budget accounting changed");
  samp::raknet::IncomingPacket sequenceOverflow;
  sequenceOverflow.split = true;
  sequenceOverflow.splitId = 2000;
  sequenceOverflow.splitIndex = 0;
  sequenceOverflow.splitCount = 2;
  sequenceOverflow.payload = {0x56};
  expect(sequenceBounded.push(sequenceOverflow).empty() && sequenceBounded.overflowed(),
         "split sequence overflow must be observable for session teardown");
  sequenceBounded.clear();
  expect(sequenceBounded.pendingCount() == 0 && sequenceBounded.pendingBytes() == 0 &&
             !sequenceBounded.overflowed(),
         "split clear must release pending bytes and overflow state");

  // Fragment payloads are each legal, but a long out-of-order set must still
  // obey the aggregate retained-byte budget before any untrusted allocation
  // reaches an Android session.
  samp::raknet::SplitReassembler byteBounded;
  const std::vector<uint8_t> nearMaxFragment(8191, 0x77);
  constexpr uint16_t kNearMaxFragmentBits = 65528;
  const size_t allowedFragments =
      samp::raknet::SplitReassembler::kMaxPendingBytes / nearMaxFragment.size();
  for (size_t index = 0; index < allowedFragments; ++index) {
    samp::raknet::IncomingPacket fragment;
    fragment.split = true;
    fragment.splitId = static_cast<uint16_t>(3000u + index);
    fragment.splitIndex = 0;
    fragment.splitCount = 2;
    fragment.payload = nearMaxFragment;
    fragment.payloadBitLength = kNearMaxFragmentBits;
    expect(byteBounded.push(std::move(fragment)).empty(),
           "aggregate split byte budget fixture should remain incomplete");
  }
  expect(byteBounded.pendingBytes() == allowedFragments * nearMaxFragment.size() &&
             !byteBounded.overflowed(),
         "split aggregate byte accounting changed");
  samp::raknet::IncomingPacket byteOverflow;
  byteOverflow.split = true;
  byteOverflow.splitId = 4000;
  byteOverflow.splitIndex = 0;
  byteOverflow.splitCount = 2;
  byteOverflow.payload = nearMaxFragment;
  byteOverflow.payloadBitLength = kNearMaxFragmentBits;
  expect(byteBounded.push(std::move(byteOverflow)).empty() && byteBounded.overflowed(),
         "split aggregate byte overflow must be observable for session teardown");

  samp::raknet::SplitReassembler representationBounded;
  samp::raknet::IncomingPacket highBits;
  highBits.split = true;
  highBits.splitId = 5000;
  highBits.splitIndex = 0;
  highBits.splitCount = 2;
  highBits.payload = nearMaxFragment;
  highBits.payloadBitLength = kNearMaxFragmentBits;
  expect(representationBounded.push(std::move(highBits)).empty(),
         "high-bit split prefix should wait for final fragment");
  samp::raknet::IncomingPacket oneBitTooFar;
  oneBitTooFar.split = true;
  oneBitTooFar.splitId = 5000;
  oneBitTooFar.splitIndex = 1;
  oneBitTooFar.splitCount = 2;
  oneBitTooFar.payload = {0x78};
  oneBitTooFar.payloadBitLength = 8;
  expect(representationBounded.push(oneBitTooFar).empty() && representationBounded.overflowed(),
         "split total beyond u16 payload-bit representation must not accumulate");
}

void testDefaultStringCompressorDecoder() {
  struct Fixture {
    std::vector<uint8_t> bytes;
    const char* expected;
  };
  // Independently generated with the public legacy RakNet default
  // StringCompressor implementation. They exercise the compressed-bit-count
  // prefix and MSB-first Huffman data used by ShowDialog bodies.
  const std::vector<Fixture> fixtures = {
      {{0xC0}, ""},
      {{0x88, 0x1F, 0x02, 0x52, 0x85, 0x40}, "Hello"},
      {{0x94, 0x0C, 0xD4, 0x4C, 0x84, 0x6C, 0xEC, 0x6D, 0xEE, 0x98, 0xCB, 0x80},
       "Login\nPassword"},
      {{0x9E, 0x15, 0xC1, 0x2A, 0x23, 0x41, 0xEE, 0x3D, 0x58, 0xDA, 0x28, 0x67,
        0xCC, 0x23, 0x95, 0xD5, 0x40},
       "Welcome to SampDroid!"},
  };
  for (const Fixture& fixture : fixtures) {
    samp::raknet::BitReader reader(fixture.bytes.data(), fixture.bytes.size());
    std::string decoded;
    expect(samp::raknet::stringcompressor::decodeDefaultString(reader, 256, decoded) &&
           decoded == fixture.expected,
           "default StringCompressor fixture decode changed");
  }
  {
    const std::vector<uint8_t> hello = {0x88, 0x1F, 0x02, 0x52, 0x85, 0x40};
    samp::raknet::BitReader reader(hello.data(), hello.size());
    std::string decoded;
    expect(!samp::raknet::stringcompressor::decodeDefaultString(reader, 4, decoded) && decoded.empty(),
           "StringCompressor output cap must fail closed");
  }
  {
    const std::vector<uint8_t> truncated = {0x88};
    samp::raknet::BitReader reader(truncated.data(), truncated.size());
    std::string decoded;
    expect(!samp::raknet::stringcompressor::decodeDefaultString(reader, 256, decoded) && decoded.empty(),
           "truncated StringCompressor data must fail closed");
  }
}

void testGtaTxdTextureData() {
  const auto fixture = makePortableTxdFixture();
  std::vector<samp::gta::txd::Texture> textures;
  std::string error;
  expect(samp::gta::txd::parsePortableTextures(fixture, textures, error) && textures.size() == 1 &&
         textures[0].name == "testtex" && textures[0].width == 2 && textures[0].height == 2 &&
         textures[0].rgba.size() == 16 && textures[0].rgba[0] == 255 && textures[0].rgba[1] == 0 &&
         textures[0].rgba[2] == 0 && textures[0].rgba[3] == 255 &&
         textures[0].rgba[4] == 0 && textures[0].rgba[5] == 255,
         "portable TXD raw texture parse changed");
  const auto spacedFixture = makePortableTxdFixture("New Road");
  expect(samp::gta::txd::parsePortableTextures(spacedFixture, textures, error) && textures.size() == 1 &&
         textures[0].name == "new road",
         "stock-style spaced TXD dictionary key must remain resolvable");
  const auto emptyDictionaryFixture = makeEmptyPortableTxdFixture();
  expect(samp::gta::txd::parsePortableTextures(emptyDictionaryFixture, textures, error) && textures.empty(),
         "valid empty stock-style TXD dictionary must be a safe no-op");
  const auto dxtFixture = makePortableDxt1TxdFixture();
  expect(samp::gta::txd::parsePortableTextures(dxtFixture, textures, error) && textures.size() == 1 &&
         textures[0].name == "dxtred" && textures[0].width == 4 && textures[0].height == 4 &&
         textures[0].rgba[0] == 255 && textures[0].rgba[1] == 0 && textures[0].rgba[2] == 0 &&
         textures[0].rgba[3] == 255 && textures[0].rgba[60] == 255,
         "portable TXD DXT1 decode changed");
  auto sectorPadded = fixture;
  sectorPadded.resize(sectorPadded.size() + 37u, 0u);
  expect(samp::gta::txd::parsePortableTextures(sectorPadded, textures, error) && textures.size() == 1,
         "zero-padded IMG-sector TXD must remain readable");
  sectorPadded.back() = 0x7Fu;
  expect(!samp::gta::txd::parsePortableTextures(sectorPadded, textures, error) && textures.empty(),
         "nonzero trailing TXD bytes must not be treated as IMG padding");
  auto corrupt = fixture;
  writeLe32At(corrupt, 140, 1);  // top mip declares fewer bytes than 2x2 BGRA requires
  expect(!samp::gta::txd::parsePortableTextures(corrupt, textures, error) && textures.empty(),
         "TXD truncated raster must fail closed transactionally");
}

void testGtaDffMeshData() {
  const auto fixture = makePortableDffFixture();
  std::vector<samp::gta::dff::Mesh> meshes;
  std::string error;
  expect(samp::gta::dff::parsePortableMeshes(fixture, meshes, error) && meshes.size() == 1 &&
         meshes[0].vertices.size() == 3 && meshes[0].indices == std::vector<uint32_t>({0, 1, 2}) &&
         meshes[0].triangleMaterials == std::vector<uint16_t>({0}) && meshes[0].materials.size() == 1 &&
         meshes[0].materials[0].textureName == "testtex" && meshes[0].materials[0].color.r == 128 &&
         meshes[0].texCoords.size() == 3 && std::fabs(meshes[0].texCoords[1].u - 1.0f) < 0.001f &&
         std::fabs(meshes[0].vertices[0].x + 1.0f) < 0.001f &&
         std::fabs(meshes[0].vertices[2].y - 1.0f) < 0.001f,
         "portable DFF geometry/material parse changed");
  const auto frameAtomicFixture = makeFrameAtomicDffFixture();
  expect(samp::gta::dff::parsePortableMeshes(frameAtomicFixture, meshes, error) && meshes.size() == 1 &&
         meshes[0].vertices.size() == 3 && std::fabs(meshes[0].vertices[0].x - 9.0f) < 0.001f &&
         std::fabs(meshes[0].vertices[0].y - 19.0f) < 0.001f &&
         std::fabs(meshes[0].vertices[0].z - 30.0f) < 0.001f,
         "DFF FrameList/Atomic transform must place portable geometry");
  const auto cyclicFrameAtomicFixture = makeFrameAtomicDffFixture(true);
  expect(!samp::gta::dff::parsePortableMeshes(cyclicFrameAtomicFixture, meshes, error) && meshes.empty(),
         "cyclic DFF FrameList hierarchy must fail closed transactionally");
  const auto triStripFixture = makePortableDffFixture(0x00000007u);
  expect(samp::gta::dff::parsePortableMeshes(triStripFixture, meshes, error) && meshes.size() == 1 &&
         meshes[0].indices == std::vector<uint32_t>({0, 1, 2}),
         "portable DFF with stock BinMesh triangle-strip flag must retain canonical triangles");
  const auto spacedTextureFixture = makePortableDffFixture(0x00000006u, "New Road");
  expect(samp::gta::dff::parsePortableMeshes(spacedTextureFixture, meshes, error) && meshes.size() == 1 &&
         meshes[0].materials.size() == 1 && meshes[0].materials[0].textureName == "new road",
         "stock-style spaced DFF texture key must remain resolvable");
  auto nonfiniteUvFixture = fixture;
  writeFloatAt(nonfiniteUvFixture, 68, std::numeric_limits<float>::quiet_NaN());
  expect(samp::gta::dff::parsePortableMeshes(nonfiniteUvFixture, meshes, error) && meshes.size() == 1 &&
         meshes[0].texCoords.size() == 3 && meshes[0].texCoords[0].u == 0.0f &&
         meshes[0].texCoords[0].v == 0.0f,
         "non-finite optional stock-style DFF UV must canonicalize safely");
  auto sectorPadded = fixture;
  sectorPadded.resize(sectorPadded.size() + 91u, 0u);
  expect(samp::gta::dff::parsePortableMeshes(sectorPadded, meshes, error) && meshes.size() == 1,
         "zero-padded IMG-sector DFF must remain readable");
  sectorPadded.back() = 0x7Fu;
  expect(!samp::gta::dff::parsePortableMeshes(sectorPadded, meshes, error) && meshes.empty(),
         "nonzero trailing DFF bytes must not be treated as IMG padding");
  auto invalidTextureName = fixture;
  const std::vector<uint8_t> textureMarker = {'T', 'e', 's', 't', 'T', 'e', 'x', 0};
  const auto textureAt = std::search(invalidTextureName.begin(), invalidTextureName.end(),
                                     textureMarker.begin(), textureMarker.end());
  expect(textureAt != invalidTextureName.end(), "DFF fixture texture marker missing");
  *textureAt = '/';
  expect(!samp::gta::dff::parsePortableMeshes(invalidTextureName, meshes, error) && meshes.empty(),
         "DFF texture path-like material name must fail closed");
  auto corruptIndex = fixture;
  writeLe16At(corruptIndex, 98, 9);  // triangle vertex3 is outside the 3 vertices
  expect(!samp::gta::dff::parsePortableMeshes(corruptIndex, meshes, error) && meshes.empty(),
         "DFF invalid triangle index must fail closed transactionally");
  auto nativeGeometry = fixture;
  writeLe32At(nativeGeometry, 52, 0x01000002u);  // native platform geometry flag
  expect(!samp::gta::dff::parsePortableMeshes(nativeGeometry, meshes, error) && meshes.empty(),
         "DFF native geometry must not be guessed");
}

void testGtaImgAndCollisionData() {
  {
    const std::vector<uint8_t> fixture = makeImgV2Fixture();
    samp::gta::img::MemoryByteSource source(fixture);
    samp::gta::img::ArchiveIndex index;
    std::string error;
    expect(samp::gta::img::parseV2(source, index, error) && index.entries().size() == 2,
           "IMG v2 fixture index rejected");
    const auto* col = index.find("test.col");
    const auto* dff = index.find("PLAYER.DFF");
    expect(col != nullptr && dff != nullptr && col->offsetBytes == 2048 && dff->offsetBytes == 4096,
           "IMG v2 names or sector offsets changed");
    std::vector<uint8_t> entry;
    expect(samp::gta::img::readEntry(source, *col, 2048, entry, error) && entry.size() == 2048 &&
           entry[0] == 0xC0 && entry[1] == 0x13,
           "IMG v2 entry extraction changed");
    expect(!samp::gta::img::readEntry(source, *col, 64, entry, error) && entry.empty(),
           "IMG extraction cap must fail closed");
  }
  {
    auto corrupt = makeImgV2Fixture();
    writeLe32At(corrupt, 8, 99);  // sector outside the source
    samp::gta::img::MemoryByteSource source(corrupt);
    samp::gta::img::ArchiveIndex index;
    std::string error;
    expect(!samp::gta::img::parseV2(source, index, error) && index.empty(),
           "out-of-range IMG entry must be rejected transactionally");
  }
  {
    const auto fixture = makeCol3Fixture();
    std::vector<samp::gta::col::Model> models;
    std::string error;
    expect(samp::gta::col::parseArchive(fixture, models, error) && models.size() == 1 &&
           models[0].name == "testfloor" && models[0].boxes.size() == 1 &&
           models[0].vertices.size() == 3 && models[0].triangles.size() == 1,
           "COL3 fixture parse changed");
    samp::gta::col::FloorHit hit;
    expect(samp::gta::col::findHighestFloor(models[0], 0.0f, 0.0f, -1.0f, 3.0f, hit) &&
           std::fabs(hit.z - 2.0f) < 0.001f && hit.material == 7,
           "COL box floor query changed");
    expect(samp::gta::col::findHighestFloor(models[0], 0.5f, -0.5f, -1.0f, 1.0f, hit) &&
           std::fabs(hit.z) < 0.001f && hit.material == 15,
           "COL triangle floor query changed");
  }
  {
    const auto fixture = makeImgWithCol3Fixture();
    samp::gta::img::MemoryByteSource source(fixture);
    samp::gta::img::ArchiveIndex index;
    std::string error;
    expect(samp::gta::img::parseV2(source, index, error),
           "IMG/COL bridge fixture index rejected");
    samp::gta::world::CollisionCatalog catalog;
    expect(samp::gta::world::loadCollisionImgMember(source, index, "TEST.COL", catalog, error) &&
           catalog.modelCount() == 1 && catalog.find("testfloor") != nullptr &&
           catalog.find("TESTFLOOR")->boxes.size() == 1,
           "bounded IMG member to collision catalog bridge changed");
    samp::gta::map::Instance placedFloor;
    placedFloor.modelId = 1000;
    placedFloor.modelName = "testfloor";
    placedFloor.position = {10.0f, -20.0f, 5.0f};
    placedFloor.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    samp::gta::map::Instance rotatedFloor = placedFloor;
    rotatedFloor.position = {30.0f, -20.0f, 5.0f};
    rotatedFloor.rotation = {0.0f, 0.0f, 0.70710677f, 0.70710677f};
    samp::gta::map::Instance interiorFloor = placedFloor;
    interiorFloor.interior = 7;
    samp::gta::map::Instance missingFloor = placedFloor;
    missingFloor.modelName = "not_loaded";
    std::vector<samp::gta::map::Instance> placements = {
        placedFloor, rotatedFloor, interiorFloor, missingFloor};
    samp::gta::world::CollisionWorld world;
    samp::gta::world::CollisionWorldBuildReport buildReport;
    expect(samp::gta::world::buildCollisionWorld(catalog, placements, world, buildReport, error) &&
           world.instanceCount() == 3 && buildReport.consideredPlacements == 4 &&
           buildReport.attachedPlacements == 3 && buildReport.missingCollisionModels == 1,
           "IPL placement to spatial collision world build changed");
    samp::gta::col::FloorHit worldHit;
    expect(world.findHighestFloor(10.0f, -20.0f, 4.0f, 8.0f, 0, worldHit) &&
           std::fabs(worldHit.z - 7.0f) < 0.001f && worldHit.material == 7 &&
           world.findHighestFloor(30.0f, -20.0f, 4.0f, 8.0f, 0, worldHit) &&
           std::fabs(worldHit.z - 7.0f) < 0.001f &&
           world.findHighestFloor(10.0f, -20.0f, 4.0f, 8.0f, 7, worldHit) &&
           std::fabs(worldHit.z - 7.0f) < 0.001f &&
           !world.findHighestFloor(10.0f, -20.0f, 4.0f, 8.0f, 1, worldHit),
           "transformed/interior world collision floor query changed");
    const std::vector<samp::gta::removal::Directive> removePlacedFloor = {
        {1000u, 10.0f, -20.0f, 5.0f, 2.0f}};
    expect(!world.findHighestFloor(10.0f, -20.0f, 4.0f, 8.0f, 0, worldHit,
                                   &removePlacedFloor) &&
               !world.findHighestFloor(10.0f, -20.0f, 4.0f, 8.0f, 7, worldHit,
                                       &removePlacedFloor),
           "RemoveBuilding filter must remove matching static COL floors in every interior");
    samp::gta::world::CollisionCatalog wallCatalog;
    expect(samp::gta::world::loadCollisionArchiveBytes(makeWallCol3Fixture(), wallCatalog, error),
           "wall collision fixture could not enter catalog");
    samp::gta::map::Instance wallPlacement;
    wallPlacement.modelId = 9000u;
    wallPlacement.modelName = "wall";
    wallPlacement.position = {0.0f, 0.0f, 0.0f};
    wallPlacement.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    samp::gta::world::CollisionWorld wallWorld;
    samp::gta::world::CollisionWorldBuildReport wallReport;
    expect(samp::gta::world::buildCollisionWorld(wallCatalog, {wallPlacement}, wallWorld,
                                                  wallReport, error),
           "wall collision world could not build");
    const auto wallMove = wallWorld.resolveHorizontalMove(
        -0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.36f, 1.55f, 0);
    expect(wallMove.valid && wallMove.blocked && std::fabs(wallMove.position.x + 0.5f) < 0.001f,
           "horizontal wall cylinder sweep must reject passage");
    const auto freeMove = wallWorld.resolveHorizontalMove(
        -0.5f, 3.0f, 0.5f, 3.0f, 0.0f, 0.36f, 1.55f, 0);
    expect(freeMove.valid && !freeMove.blocked && std::fabs(freeMove.position.x - 0.5f) < 0.001f,
           "horizontal cylinder sweep must preserve unobstructed movement");
    const std::vector<samp::gta::removal::Directive> removeWall = {
        {samp::gta::removal::kAnyModel, 0.0f, 0.0f, 0.0f, 4.0f}};
    const auto removedWallMove = wallWorld.resolveHorizontalMove(
        -0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.36f, 1.55f, 0, &removeWall);
    expect(removedWallMove.valid && !removedWallMove.blocked &&
               std::fabs(removedWallMove.position.x - 0.5f) < 0.001f,
           "wildcard RemoveBuilding must clear matching static COL side collision");
    samp::gta::world::RaycastHit wallRay;
    expect(wallWorld.raycastStaticWorld({-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 4.0f, 0, wallRay) &&
               wallRay.valid && wallRay.hit && std::fabs(wallRay.position.x - 0.15f) < 0.001f &&
               wallRay.normal.x < -0.99f,
           "bounded static ray must resolve the nearest COL wall face");
    samp::gta::world::RaycastHit removedWallRay;
    expect(wallWorld.raycastStaticWorld({-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 4.0f, 0,
                                         removedWallRay, &removeWall) &&
               removedWallRay.valid && !removedWallRay.hit,
           "RemoveBuilding must clear static COL ray contacts as well as movement");
    samp::gta::world::RaycastHit otherInteriorRay;
    expect(wallWorld.raycastStaticWorld({-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 4.0f, 7,
                                         otherInteriorRay) &&
               otherInteriorRay.valid && !otherInteriorRay.hit,
           "static ray must not cross an exact IPL interior boundary");
    samp::gta::world::RaycastHit overlongRay;
    expect(!wallWorld.raycastStaticWorld({-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 257.0f, 0,
                                          overlongRay) &&
               !overlongRay.valid,
           "static ray must reject an unbounded distance request");
    samp::movement::LocalMotionState motion;
    expect(samp::movement::resetFromAuthoritative({10.0f, -20.0f, 7.0f}, 0.0f, 0, motion),
           "collision motion could not initialize authoritative local position");
    samp::input::TouchIntent motionInput;
    auto motionStep = samp::movement::advance(world, motionInput, 0.0f, 0.02f, motion);
    expect(motionStep.status == samp::movement::StepStatus::Applied && motion.grounded &&
           std::fabs(motion.position.z - 7.0f) < 0.001f,
           "collision motion must establish validated ground");
    motionInput.moveY = 0.2f;
    motionStep = samp::movement::advance(world, motionInput, 0.0f, 0.04f, motion);
    expect(motionStep.status == samp::movement::StepStatus::Applied && motionStep.moved && motion.grounded &&
           std::fabs(motion.position.z - 7.0f) < 0.001f,
           "grounded collision movement changed");
    motionInput.moveY = 0.0f;
    motionInput.heldActions = samp::input::kHeldJump;
    motionStep = samp::movement::advance(world, motionInput, 0.0f, 0.05f, motion);
    expect(motionStep.status == samp::movement::StepStatus::Applied && motionStep.jumped && !motion.grounded &&
           motion.position.z > 7.0f,
           "collision-validated jump launch changed");
    motionInput.heldActions = 0;
    for (int i = 0; i < 30 && !motion.grounded; ++i) {
      motionStep = samp::movement::advance(world, motionInput, 0.0f, 0.05f, motion);
      expect(motionStep.status == samp::movement::StepStatus::Applied,
             "jump descent must stay collision-validated");
    }
    expect(motion.grounded && std::fabs(motion.position.z - 7.0f) < 0.001f,
           "collision motion must land on world floor");
    // Server gravity is measured in GTA units (default 0.008). A supported
    // lower authoritative value must change only the bounded vertical pilot;
    // unsupported values must never become a guessed trajectory.
    samp::movement::LocalMotionState defaultGravityMotion;
    samp::movement::LocalMotionState lowGravityMotion;
    expect(samp::movement::resetFromAuthoritative({10.0f, -20.0f, 7.0f}, 0.0f, 0,
                                                   defaultGravityMotion) &&
           samp::movement::resetFromAuthoritative({10.0f, -20.0f, 7.0f}, 0.0f, 0,
                                                   lowGravityMotion),
           "gravity motion fixtures could not initialize");
    samp::input::TouchIntent jumpInput;
    jumpInput.heldActions = samp::input::kHeldJump;
    const auto defaultGravityStep = samp::movement::advance(
        world, jumpInput, 0.0f, 0.05f, defaultGravityMotion, 0.008f);
    const auto lowGravityStep = samp::movement::advance(
        world, jumpInput, 0.0f, 0.05f, lowGravityMotion, 0.004f);
    expect(defaultGravityStep.status == samp::movement::StepStatus::Applied &&
           lowGravityStep.status == samp::movement::StepStatus::Applied &&
           lowGravityMotion.position.z > defaultGravityMotion.position.z &&
           samp::movement::advance(world, jumpInput, 0.0f, 0.05f, lowGravityMotion, 0.05f).status ==
               samp::movement::StepStatus::InvalidState,
           "server gravity must scale supported jumps and reject unsupported values");

    // The socket-free gameplay bridge is the only path allowed to turn touch
    // input into a movement candidate. It must start from server facts, query
    // actual collision, and produce an ordinary validated Packet-207 state.
    samp::state::LocalPlayer local;
    local.positionKnown = true;
    local.facingKnown = true;
    local.healthKnown = true;
    local.armourKnown = true;
    local.x = 10.0f;
    local.y = -20.0f;
    local.z = 7.0f;
    local.angle = 0.0f;
    local.health = 100.0f;
    local.armour = 25.0f;
    samp::input::OnFootInputMapper gameplayControls;
    gameplayControls.synchronizeHeading(0.0f);
    samp::input::TouchIntent gameplayInput;
    gameplayInput.moveY = 0.5f;
    expect(gameplayControls.update(gameplayInput), "gameplay bridge touch input rejected");
    samp::gameplay::LocalGameplayState gameplay;
    const auto gameplayStep = samp::gameplay::advanceLocalOnFoot(
        local, gameplayControls, world, 0.04f, gameplay);
    expect(gameplayStep.status == samp::gameplay::LocalGameplayStatus::Applied &&
           gameplayStep.onFootReady && gameplay.predictionActive && gameplayStep.grounded &&
           gameplayStep.onFoot.positionY > local.y &&
           std::fabs(gameplayStep.onFoot.positionZ - 7.0f) < 0.001f &&
           gameplayStep.onFoot.armour == 25 &&
           samp::sync::isValidOnFootState(gameplayStep.onFoot),
           "collision gameplay bridge must output a valid grounded sync candidate");
    // RPC-17 server bounds are a recipient policy layered after collision:
    // exterior prediction keeps its last grounded position rather than
    // inventing the server's push-back vector or crossing the maxY edge.
    samp::state::WorldEnvironment boundedExterior;
    boundedExterior.worldBoundsKnown = true;
    boundedExterior.worldMaxX = 100.0f;
    boundedExterior.worldMinX = -100.0f;
    boundedExterior.worldMaxY = -19.99f;
    boundedExterior.worldMinY = -100.0f;
    samp::state::LocalPlayer boundedLocal = local;
    boundedLocal.interiorKnown = true;
    boundedLocal.interior = 0;
    samp::gameplay::LocalGameplayState boundedGameplay;
    const auto boundedStep = samp::gameplay::advanceLocalOnFoot(
        boundedLocal, gameplayControls, world, 0.04f, boundedGameplay, 0.008f, &boundedExterior);
    expect(boundedStep.status == samp::gameplay::LocalGameplayStatus::Applied &&
           boundedStep.onFootReady && !boundedStep.moved && boundedStep.grounded &&
           std::fabs(boundedStep.onFoot.positionX - local.x) < 0.001f &&
           std::fabs(boundedStep.onFoot.positionY - local.y) < 0.001f &&
           samp::sync::isValidOnFootState(boundedStep.onFoot),
           "exterior world bounds must stop only the crossing on-foot prediction");
    samp::gameplay::LocalGameplayState unsupportedGravityState;
    const auto unsupportedGravityGameplay = samp::gameplay::advanceLocalOnFoot(
        local, gameplayControls, world, 0.04f, unsupportedGravityState, 0.05f);
    expect(unsupportedGravityGameplay.status == samp::gameplay::LocalGameplayStatus::GravityUnsupported &&
           !unsupportedGravityGameplay.onFootReady && !unsupportedGravityState.predictionActive,
           "gameplay bridge must fail closed for unsupported server gravity");
    samp::state::LocalPlayer spectatingLocal = local;
    spectatingLocal.spectatingKnown = true;
    spectatingLocal.spectating = true;
    samp::gameplay::LocalGameplayState spectatingGameplay;
    const auto spectatingStep = samp::gameplay::advanceLocalOnFoot(
        spectatingLocal, gameplayControls, world, 0.04f, spectatingGameplay);
    expect(spectatingStep.status == samp::gameplay::LocalGameplayStatus::Spectating &&
           !spectatingStep.onFootReady && !spectatingGameplay.predictionActive,
           "spectator local state must block collision gameplay output");
    samp::state::Vehicle spectatingVehicle;
    const auto spectatingDriver = samp::gameplay::advanceLocalDriverVehicle(
        spectatingLocal, spectatingVehicle, gameplayControls, world, 0.04f, spectatingGameplay);
    const auto spectatingAim = samp::gameplay::buildLocalAimSync(
        spectatingLocal, gameplayControls, spectatingGameplay);
    const auto spectatingPassenger = samp::gameplay::buildLocalPassengerSync(
        spectatingLocal, spectatingVehicle, gameplayControls, spectatingGameplay);
    expect(spectatingDriver.status == samp::gameplay::LocalGameplayStatus::Spectating &&
           spectatingAim.status == samp::gameplay::LocalGameplayStatus::Spectating &&
           spectatingPassenger.status == samp::gameplay::LocalGameplayStatus::Spectating,
           "spectator local state must block driver, aim, and passenger builders");
    // A known zero from server SetPlayerHealth is a local-output stop only.
    // It must not leak a final movement/aim/passenger candidate or invent a
    // death/respawn packet on Android.
    samp::state::LocalPlayer deadLocal = local;
    deadLocal.healthKnown = true;
    deadLocal.health = 0.0f;
    samp::gameplay::LocalGameplayState deadGameplay;
    const auto deadOnFoot = samp::gameplay::advanceLocalOnFoot(
        deadLocal, gameplayControls, world, 0.04f, deadGameplay);
    const auto deadDriver = samp::gameplay::advanceLocalDriverVehicle(
        deadLocal, spectatingVehicle, gameplayControls, world, 0.04f, deadGameplay);
    const auto deadAim = samp::gameplay::buildLocalAimSync(
        deadLocal, gameplayControls, deadGameplay);
    const auto deadPassenger = samp::gameplay::buildLocalPassengerSync(
        deadLocal, spectatingVehicle, gameplayControls, deadGameplay);
    expect(deadOnFoot.status == samp::gameplay::LocalGameplayStatus::Dead &&
           deadDriver.status == samp::gameplay::LocalGameplayStatus::Dead &&
           deadAim.status == samp::gameplay::LocalGameplayStatus::Dead &&
           deadPassenger.status == samp::gameplay::LocalGameplayStatus::Dead &&
           !deadOnFoot.onFootReady && !deadDriver.inCarReady && !deadAim.aimReady &&
           !deadPassenger.passengerReady && !deadGameplay.predictionActive &&
           !deadGameplay.vehiclePredictionActive,
           "authoritative zero health must block every local gameplay sync candidate");
    // The same collision index has an interior-7 placement. A known server
    // interior must flow through reset/floor/sweep unchanged rather than being
    // rejected solely because it is nonzero.
    samp::state::LocalPlayer interiorLocal = local;
    interiorLocal.interiorKnown = true;
    interiorLocal.interior = 7;
    samp::gameplay::LocalGameplayState interiorGameplay;
    const auto interiorGameplayStep = samp::gameplay::advanceLocalOnFoot(
        interiorLocal, gameplayControls, world, 0.04f, interiorGameplay);
    expect(interiorGameplayStep.status == samp::gameplay::LocalGameplayStatus::Applied &&
           interiorGameplayStep.onFootReady && interiorGameplay.motion.initialized &&
           interiorGameplay.motion.interior == 7 && interiorGameplayStep.grounded,
           "collision gameplay bridge must honor authoritative nonzero interior");
    interiorLocal.interior = 6;
    samp::gameplay::LocalGameplayState missingInteriorGameplay;
    const auto missingInteriorStep = samp::gameplay::advanceLocalOnFoot(
        interiorLocal, gameplayControls, world, 0.04f, missingInteriorGameplay);
    expect(missingInteriorStep.status == samp::gameplay::LocalGameplayStatus::MissingGround &&
           !missingInteriorStep.onFootReady && !missingInteriorGameplay.predictionActive,
           "missing authoritative interior collision must remain fail-closed");
    // AimSync is a separate source-verified Packet-203 path. It may use only
    // the collision-grounded local transform, semantic AIM, and an exact
    // server-owned armed weapon/ammo record. Its Packet-206 form starts as a
    // neutral no-target sentinel and may gain only a close static-COL absolute
    // point; it cannot choose an entity target or make a damage claim.
    local.armedWeaponKnown = true;
    local.armedWeapon = 24;
    local.weaponAmmoKnown[24] = true;
    local.weaponAmmo[24] = 7;
    gameplayInput.heldActions = samp::input::kHeldAim;
    gameplayInput.lookPitchDeltaDegrees = 30.0f;
    expect(gameplayControls.update(gameplayInput), "aim semantic input rejected");
    const auto aimSync = samp::gameplay::buildLocalAimSync(local, gameplayControls, gameplay);
    expect(aimSync.aimReady && aimSync.aim.cameraMode == 53 &&
           aimSync.aim.weaponState == 2 && std::fabs(aimSync.aim.cameraFrontZ - 0.5f) < 0.001f &&
           std::fabs(aimSync.aim.aimZ + 0.5235988f) < 0.001f &&
           std::fabs(aimSync.aim.cameraPositionZ - (gameplay.motion.position.z + 0.70f)) < 0.001f &&
           samp::sync::isValidAimState(aimSync.aim),
           "aim bridge must build only a grounded server-weapon Packet-203 candidate");
    gameplayInput.heldActions = samp::input::kHeldAim | samp::input::kHeldFire;
    gameplayInput.lookPitchDeltaDegrees = 0.0f;
    expect(gameplayControls.update(gameplayInput), "aim/fire semantic input rejected");
    samp::sync::BulletState noTargetBullet;
    expect(samp::gameplay::buildLocalNoTargetBulletSync(
               local, gameplayControls, gameplay, aimSync.aim, noTargetBullet) &&
           noTargetBullet.hitType == 0 && noTargetBullet.hitId == 0 &&
           noTargetBullet.weaponId == 24 &&
           std::fabs(noTargetBullet.originZ - aimSync.aim.cameraPositionZ) < 0.001f &&
           samp::sync::isValidNoTargetBulletState(noTargetBullet),
           "AIM+FIRE bridge must emit a neutral server-weapon BulletSync candidate");
    samp::gameplay::LocalGameplayState staticImpactState;
    staticImpactState.predictionActive = true;
    staticImpactState.motion.initialized = true;
    staticImpactState.motion.interior = 0;
    samp::sync::AimState staticImpactAim;
    staticImpactAim.cameraMode = 53;
    staticImpactAim.cameraFrontX = 1.0f;
    staticImpactAim.cameraFrontY = 0.0f;
    staticImpactAim.cameraPositionX = -1.0f;
    staticImpactAim.cameraPositionY = 0.0f;
    staticImpactAim.cameraPositionZ = 1.0f;
    staticImpactAim.weaponState = 2;
    samp::sync::BulletState staticImpactBullet;
    staticImpactBullet.originX = -1.0f;
    staticImpactBullet.originY = 0.0f;
    staticImpactBullet.originZ = 1.0f;
    staticImpactBullet.weaponId = 24;
    expect(samp::gameplay::applyLocalNoTargetStaticWorldImpact(
               wallWorld, staticImpactState, staticImpactAim, staticImpactBullet) &&
               staticImpactBullet.hitType == 0 && staticImpactBullet.hitId == 0 &&
               staticImpactBullet.offsetX == 0.0f && staticImpactBullet.offsetY == 0.0f &&
               staticImpactBullet.offsetZ == 0.0f &&
               std::fabs(staticImpactBullet.hitPositionX - 0.15f) < 0.001f &&
               samp::sync::isValidNoTargetBulletState(staticImpactBullet),
           "static COL bridge must add only a bounded world point to no-target BulletSync");
    gameplayInput.heldActions = samp::input::kHeldAim;
    expect(gameplayControls.update(gameplayInput), "fire release semantic input rejected");
    expect(!samp::gameplay::buildLocalNoTargetBulletSync(
               local, gameplayControls, gameplay, aimSync.aim, noTargetBullet),
           "BulletSync candidate must require simultaneous AIM and FIRE");
    local.armedWeapon = 0;
    local.weaponAmmoKnown[0] = true;
    local.weaponAmmo[0] = 0;
    const auto meleeAim = samp::gameplay::buildLocalAimSync(local, gameplayControls, gameplay);
    expect(meleeAim.aimReady && meleeAim.aim.cameraMode == 4 && meleeAim.aim.weaponState == 0,
           "aim bridge must preserve the compatible melee camera/zero-ammo state");
    local.armedWeapon = 24;
    local.weaponAmmoKnown[24] = false;
    const auto unarmedAim = samp::gameplay::buildLocalAimSync(local, gameplayControls, gameplay);
    expect(!unarmedAim.aimReady &&
           unarmedAim.status == samp::gameplay::LocalGameplayStatus::WeaponUnavailable,
           "aim bridge must reject unknown server weapon ammo");
    gameplayInput.heldActions = 0;
    gameplayInput.lookPitchDeltaDegrees = 0.0f;
    expect(gameplayControls.update(gameplayInput), "aim release semantic input rejected");

    // Driver prediction uses a separate kinematic/collision state and exact
    // Packet-200 builder; it must not reuse pedestrian position assumptions.
    samp::vehiclemotion::LocalVehicleMotionState vehicleMotion;
    expect(samp::vehiclemotion::resetFromAuthoritative(88, {10.0f, -20.0f, 7.0f}, 0.0f, 0,
                                                        vehicleMotion),
           "vehicle collision motion could not initialize authoritative transform");
    samp::input::TouchIntent vehicleInput;
    vehicleInput.moveY = 1.0f;
    const auto vehicleMotionStep = samp::vehiclemotion::advance(world, vehicleInput, 0.05f,
                                                                  vehicleMotion);
    expect((vehicleMotionStep.status == samp::vehiclemotion::StepStatus::Applied ||
            vehicleMotionStep.status == samp::vehiclemotion::StepStatus::Blocked) &&
           vehicleMotion.grounded,
           "collision vehicle motion must retain grounded throttle/brake state");

    // A validated user-data profile may specialize only the conservative
    // kinematic response envelope; it must not bypass the same grounded COL
    // step or exceed the shared tuning validator.
    samp::vehiclemotion::LocalVehicleMotionState genericHandlingMotion;
    samp::vehiclemotion::LocalVehicleMotionState tunedHandlingMotion;
    expect(samp::vehiclemotion::resetFromAuthoritative(
               88, {10.0f, -20.0f, 7.0f}, 0.0f, 0, genericHandlingMotion) &&
           samp::vehiclemotion::resetFromAuthoritative(
               88, {10.0f, -20.0f, 7.0f}, 0.0f, 0, tunedHandlingMotion),
           "handling motion fixtures could not initialize");
    samp::vehiclemotion::VehicleMotionTuning modelTuning;
    modelTuning.forwardAcceleration = 25.0f;
    modelTuning.maxForwardSpeed = 44.0f;
    modelTuning.maxReverseSpeed = 13.0f;
    modelTuning.maxSteeringDegreesPerSecond = 90.0f;
    expect(samp::vehiclemotion::validMotionTuning(modelTuning),
           "validated handling tuning fixture rejected");
    const auto genericHandlingStep = samp::vehiclemotion::advance(
        world, vehicleInput, 0.05f, genericHandlingMotion);
    const auto tunedHandlingStep = samp::vehiclemotion::advance(
        world, vehicleInput, 0.05f, tunedHandlingMotion, nullptr, 0.008f, &modelTuning);
    auto invalidModelTuning = modelTuning;
    invalidModelTuning.forwardAcceleration = 0.0f;
    expect((genericHandlingStep.status == samp::vehiclemotion::StepStatus::Applied ||
            genericHandlingStep.status == samp::vehiclemotion::StepStatus::Blocked) &&
           (tunedHandlingStep.status == samp::vehiclemotion::StepStatus::Applied ||
            tunedHandlingStep.status == samp::vehiclemotion::StepStatus::Blocked) &&
           tunedHandlingMotion.speed > genericHandlingMotion.speed &&
           !samp::vehiclemotion::validMotionTuning(invalidModelTuning),
           "handling tuning must influence only valid collision-grounded motion");

    // RPC-91 linear values are GTA 50 Hz CVector units, not arbitrary Android
    // metres/second. The pilot must retain a world-space sideways vector and a
    // bounded vertical launch rather than turning either into a guessed vehicle
    // ID or a yaw change.
    samp::vehiclemotion::LocalVehicleMotionState rpc91LinearMotion;
    expect(samp::vehiclemotion::resetFromAuthoritative(
               88, {10.0f, -20.0f, 7.0f}, 0.0f, 0, rpc91LinearMotion) &&
               samp::vehiclemotion::applyServerLinearVelocity(
                   0.0f, 0.01f, 0.0f, 41u, rpc91LinearMotion),
           "bounded RPC-91 linear velocity could not seed driver motion");
    samp::input::TouchIntent neutralVehicleInput;
    const auto rpc91LinearStep = samp::vehiclemotion::advance(
        world, neutralVehicleInput, 0.05f, rpc91LinearMotion);
    expect((rpc91LinearStep.status == samp::vehiclemotion::StepStatus::Applied ||
            rpc91LinearStep.status == samp::vehiclemotion::StepStatus::Blocked) &&
           rpc91LinearMotion.appliedServerLinearVelocityRevision == 41u &&
           rpc91LinearMotion.grounded &&
           (rpc91LinearMotion.position.y > -19.99f || rpc91LinearStep.blocked),
           "RPC-91 linear vector must remain collision-bounded without yaw fabrication");
    samp::vehiclemotion::LocalVehicleMotionState rpc91JumpMotion;
    expect(samp::vehiclemotion::resetFromAuthoritative(
               88, {10.0f, -20.0f, 7.0f}, 0.0f, 0, rpc91JumpMotion) &&
               samp::vehiclemotion::applyServerLinearVelocity(
                   0.0f, 0.0f, 0.2f, 42u, rpc91JumpMotion),
           "RPC-91 jump velocity could not seed driver motion");
    const auto rpc91JumpStep = samp::vehiclemotion::advance(
        world, neutralVehicleInput, 0.05f, rpc91JumpMotion);
    expect(rpc91JumpStep.status == samp::vehiclemotion::StepStatus::Applied &&
           !rpc91JumpMotion.grounded && rpc91JumpMotion.position.z > 7.0f &&
           rpc91JumpMotion.verticalVelocity > 0.0f &&
           !samp::vehiclemotion::applyServerLinearVelocity(
               3.1f, 0.0f, 0.0f, 43u, rpc91JumpMotion),
           "RPC-91 vehicle pilot must support bounded launches and reject unsafe local trajectories");

    samp::state::LocalPlayer driver = local;
    driver.vehicleKnown = true;
    driver.inVehicle = true;
    driver.vehicleId = 88;
    driver.vehicleSeat = 0;
    samp::state::Vehicle driverVehicle;
    driverVehicle.id = 88;
    driverVehicle.streamedIn = true;
    driverVehicle.x = 10.0f;
    driverVehicle.y = -20.0f;
    driverVehicle.z = 7.0f;
    driverVehicle.angle = 0.0f;
    driverVehicle.health = 1000.0f;
    samp::gameplay::LocalGameplayState vehicleGameplay;
    const auto driverGameplay = samp::gameplay::advanceLocalDriverVehicle(
        driver, driverVehicle, gameplayControls, world, 0.05f, vehicleGameplay);
    expect(driverGameplay.inCarReady && vehicleGameplay.vehiclePredictionActive &&
           driverGameplay.inCar.vehicleId == 88 &&
           driverGameplay.inCar.positionY >= -20.0f &&
           samp::sync::isValidInCarState(driverGameplay.inCar),
           "collision gameplay bridge must output a valid driver Packet-200 candidate");
    // Only retained server RPC-24 facts may affect the local driver path:
    // engine-off suppresses Android throttle only under PlayerInit's manual
    // engine/light mode, and siren mirrors exactly. There is no local
    // engine/siren ownership or invented vehicle-system state.
    samp::state::Vehicle serverParamDriverVehicle = driverVehicle;
    serverParamDriverVehicle.parameters.known = true;
    serverParamDriverVehicle.parameters.values[
        static_cast<size_t>(samp::state::VehicleParameter::Engine)] = 0;
    serverParamDriverVehicle.parameters.values[
        static_cast<size_t>(samp::state::VehicleParameter::Siren)] = 1;
    // An ordinary automatic-engine server still displays/forwards the received
    // state, but its PlayerInit bit says the client must not turn it into an
    // invented drivetrain restriction.
    samp::gameplay::LocalGameplayState automaticEngineDriverGameplay;
    const auto automaticEngineDriverStep = samp::gameplay::advanceLocalDriverVehicle(
        driver, serverParamDriverVehicle, gameplayControls, world, 0.05f,
        automaticEngineDriverGameplay);
    expect(automaticEngineDriverStep.inCarReady && automaticEngineDriverStep.inCar.siren == 1u &&
           static_cast<int16_t>(automaticEngineDriverStep.inCar.upDown) != 0,
           "engine-off must not suppress throttle when PlayerInit keeps GTA automatic engine mode");
    samp::gameplay::LocalGameplayState serverParamDriverGameplay;
    const auto serverParamDriverStep = samp::gameplay::advanceLocalDriverVehicle(
        driver, serverParamDriverVehicle, gameplayControls, world, 0.05f,
        serverParamDriverGameplay, nullptr, nullptr, nullptr, true);
    expect(serverParamDriverStep.inCarReady && serverParamDriverStep.inCar.siren == 1u &&
           static_cast<int16_t>(serverParamDriverStep.inCar.upDown) == 0 &&
           std::fabs(serverParamDriverStep.inCar.positionY - serverParamDriverVehicle.y) < 0.001f &&
           samp::sync::isValidInCarState(serverParamDriverStep.inCar),
           "server engine-off/siren must control only bounded local driver sync facts");
    serverParamDriverVehicle.parameters.values[
        static_cast<size_t>(samp::state::VehicleParameter::Engine)] = 1;
    serverParamDriverVehicle.parameters.values[
        static_cast<size_t>(samp::state::VehicleParameter::Siren)] = 0;
    samp::gameplay::LocalGameplayState serverEngineOnDriverGameplay;
    const auto serverEngineOnDriverStep = samp::gameplay::advanceLocalDriverVehicle(
        driver, serverParamDriverVehicle, gameplayControls, world, 0.05f, serverEngineOnDriverGameplay);
    expect(serverEngineOnDriverStep.inCarReady && serverEngineOnDriverStep.inCar.siren == 0u &&
           static_cast<int16_t>(serverEngineOnDriverStep.inCar.upDown) != 0,
           "server engine-on must preserve held Android throttle without inventing siren state");
    // A later full RPC-24 snapshot may explicitly reset both fields to -1.
    // Even on a manual-engine server that must release the Android throttle
    // gate: unset is neither an implicit off state nor a stale prior engine=0.
    serverParamDriverVehicle.parameters.values[
        static_cast<size_t>(samp::state::VehicleParameter::Engine)] = -1;
    serverParamDriverVehicle.parameters.values[
        static_cast<size_t>(samp::state::VehicleParameter::Siren)] = -1;
    samp::gameplay::LocalGameplayState serverEngineUnsetDriverGameplay;
    const auto serverEngineUnsetDriverStep = samp::gameplay::advanceLocalDriverVehicle(
        driver, serverParamDriverVehicle, gameplayControls, world, 0.05f,
        serverEngineUnsetDriverGameplay, nullptr, nullptr, nullptr, true);
    expect(serverEngineUnsetDriverStep.inCarReady && serverEngineUnsetDriverStep.inCar.siren == 0u &&
           static_cast<int16_t>(serverEngineUnsetDriverStep.inCar.upDown) != 0,
           "explicit full-snapshot engine unset must release manual throttle suppression");
    samp::state::Vehicle rpc91DriverVehicle = driverVehicle;
    rpc91DriverVehicle.linearVelocityKnown = true;
    rpc91DriverVehicle.linearVelocityRevision = 17u;
    rpc91DriverVehicle.linearVelocityZ = 0.2f;
    samp::gameplay::LocalGameplayState rpc91DriverGameplay;
    const auto rpc91DriverStep = samp::gameplay::advanceLocalDriverVehicle(
        driver, rpc91DriverVehicle, gameplayControls, world, 0.05f, rpc91DriverGameplay);
    expect(rpc91DriverStep.inCarReady && rpc91DriverGameplay.vehiclePredictionActive &&
           !rpc91DriverStep.grounded && rpc91DriverStep.inCar.positionZ > driverVehicle.z &&
           rpc91DriverStep.inCar.velocityZ > 0.0f && rpc91DriverStep.inCar.velocityZ < 0.2f &&
           rpc91DriverGameplay.consumedVehicleLinearVelocityRevision == 17u &&
           samp::sync::isValidInCarState(rpc91DriverStep.inCar),
           "driver bridge must consume one bounded linear RPC-91 command into Packet-200 motion");
    // A SetVehiclePosition-style reset for the same vehicle must not replay an
    // already-consumed ID-less launch command merely because its retained state
    // still exists; only a newer reducer revision may seed it again.
    samp::gameplay::invalidateLocalPrediction(rpc91DriverGameplay);
    const auto postResetRpc91DriverStep = samp::gameplay::advanceLocalDriverVehicle(
        driver, rpc91DriverVehicle, gameplayControls, world, 0.05f, rpc91DriverGameplay);
    expect(postResetRpc91DriverStep.inCarReady && postResetRpc91DriverStep.grounded &&
           std::fabs(postResetRpc91DriverStep.inCar.positionZ - driverVehicle.z) < 0.001f &&
           rpc91DriverGameplay.consumedVehicleLinearVelocityRevision == 17u,
           "same-vehicle authoritative reset must not replay an old RPC-91 linear command");
    samp::state::WorldEnvironment boundedDriverExterior;
    boundedDriverExterior.worldBoundsKnown = true;
    boundedDriverExterior.worldMaxX = 100.0f;
    boundedDriverExterior.worldMinX = -100.0f;
    boundedDriverExterior.worldMaxY = -19.99f;
    boundedDriverExterior.worldMinY = -100.0f;
    driver.interiorKnown = true;
    driver.interior = 0;
    samp::gameplay::LocalGameplayState boundedVehicleGameplay;
    const auto boundedDriverGameplay = samp::gameplay::advanceLocalDriverVehicle(
        driver, driverVehicle, gameplayControls, world, 0.05f, boundedVehicleGameplay,
        &boundedDriverExterior);
    expect(boundedDriverGameplay.inCarReady &&
           boundedDriverGameplay.status == samp::gameplay::LocalGameplayStatus::VehicleBlocked &&
           boundedDriverGameplay.grounded &&
           std::fabs(boundedDriverGameplay.inCar.positionX - driverVehicle.x) < 0.001f &&
           std::fabs(boundedDriverGameplay.inCar.positionY - driverVehicle.y) < 0.001f &&
           std::fabs(boundedDriverGameplay.inCar.velocityX) < 0.001f &&
           std::fabs(boundedDriverGameplay.inCar.velocityY) < 0.001f &&
           samp::sync::isValidInCarState(boundedDriverGameplay.inCar),
           "exterior world bounds must stop the driver pilot without a push-back guess");
    samp::state::LocalPlayer interiorDriver = driver;
    interiorDriver.interiorKnown = true;
    interiorDriver.interior = 7;
    samp::state::Vehicle interiorDriverVehicle = driverVehicle;
    interiorDriverVehicle.interior = 7;
    samp::gameplay::LocalGameplayState interiorVehicleGameplay;
    const auto interiorDriverGameplay = samp::gameplay::advanceLocalDriverVehicle(
        interiorDriver, interiorDriverVehicle, gameplayControls, world, 0.05f,
        interiorVehicleGameplay);
    expect(interiorDriverGameplay.inCarReady &&
           interiorVehicleGameplay.vehicleMotion.initialized &&
           interiorVehicleGameplay.vehicleMotion.interior == 7,
           "driver collision bridge must honor matching authoritative interior");
    interiorDriverVehicle.interior = 0;
    samp::gameplay::LocalGameplayState mismatchedVehicleGameplay;
    const auto mismatchedDriverGameplay = samp::gameplay::advanceLocalDriverVehicle(
        interiorDriver, interiorDriverVehicle, gameplayControls, world, 0.05f,
        mismatchedVehicleGameplay);
    expect(mismatchedDriverGameplay.status == samp::gameplay::LocalGameplayStatus::CollisionUnavailable &&
           !mismatchedDriverGameplay.inCarReady,
           "mismatched local/vehicle interior must remain fail-closed");
    driver.vehicleSeat = 1;
    const auto passengerGameplay = samp::gameplay::advanceLocalDriverVehicle(
        driver, driverVehicle, gameplayControls, world, 0.05f, vehicleGameplay);
    expect(passengerGameplay.status == samp::gameplay::LocalGameplayStatus::PassengerSeat &&
           !passengerGameplay.inCarReady,
           "passenger must never receive a driver Packet-200 candidate");
    // A passenger has its own exact Packet-211 path: no collision/seat-offset
    // prediction, only server seat + streamed vehicle anchor + semantic input.
    driver.vehicleSeat = 3;
    driver.specialActionKnown = true;
    driver.specialAction = 24;  // source-verified SpecialAction_Cuffed
    driver.armedWeaponKnown = true;
    driver.armedWeapon = 28;
    driver.weaponAmmoKnown[28] = true;
    driver.weaponAmmo[28] = 30;
    gameplayInput.heldActions = samp::input::kHeldAim | samp::input::kHeldFire;
    expect(gameplayControls.update(gameplayInput), "cuffed passenger input rejected");
    const auto passengerSync = samp::gameplay::buildLocalPassengerSync(
        driver, driverVehicle, gameplayControls, vehicleGameplay);
    expect(passengerSync.passengerReady &&
           passengerSync.passenger.vehicleId == 88 && passengerSync.passenger.seatId == 3 &&
           passengerSync.passenger.cuffed && !passengerSync.passenger.driveBy &&
           passengerSync.passenger.weaponAdditionalKey == 28 &&
           std::fabs(passengerSync.passenger.positionX - driverVehicle.x) < 0.001f &&
           std::fabs(passengerSync.passenger.positionY - driverVehicle.y) < 0.001f &&
           std::fabs(passengerSync.passenger.positionZ - driverVehicle.z) < 0.001f &&
           !vehicleGameplay.vehiclePredictionActive &&
           samp::sync::isValidPassengerState(passengerSync.passenger),
           "passenger bridge must output anchored Packet-211 without prediction");
    // Passenger drive-by is intentionally narrower than generic armed-weapon
    // sync: only an authoritative stocked SMG plus held AIM may set the one
    // legacy bit. It never emits BulletSync or nominates a target.
    driver.specialAction = 0;
    driver.armedWeapon = 28;
    driver.weaponAmmoKnown[28] = true;
    driver.weaponAmmo[28] = 30;
    gameplayInput.heldActions = samp::input::kHeldFire;
    expect(gameplayControls.update(gameplayInput), "passenger FIRE-only input rejected");
    const auto fireOnlyPassenger = samp::gameplay::buildLocalPassengerSync(
        driver, driverVehicle, gameplayControls, vehicleGameplay);
    expect(fireOnlyPassenger.passengerReady && !fireOnlyPassenger.passenger.driveBy,
           "passenger FIRE alone must not set drive-by mode");
    gameplayInput.heldActions = samp::input::kHeldAim | samp::input::kHeldFire;
    expect(gameplayControls.update(gameplayInput), "passenger drive-by input rejected");
    const auto passengerDriveBy = samp::gameplay::buildLocalPassengerSync(
        driver, driverVehicle, gameplayControls, vehicleGameplay);
    expect(passengerDriveBy.passengerReady && passengerDriveBy.passenger.driveBy &&
           !passengerDriveBy.passenger.cuffed && passengerDriveBy.passenger.weaponAdditionalKey == 28 &&
           samp::sync::isValidPassengerState(passengerDriveBy.passenger),
           "server-armed SMG passenger AIM must set only the compatible drive-by bit");
    driver.weaponAmmo[28] = 0;
    const auto emptyPassengerDriveBy = samp::gameplay::buildLocalPassengerSync(
        driver, driverVehicle, gameplayControls, vehicleGameplay);
    expect(emptyPassengerDriveBy.passengerReady && !emptyPassengerDriveBy.passenger.driveBy,
           "empty passenger SMG ammo must clear drive-by mode without blocking seat sync");
    driver.vehicleSeat = 0;
    const auto invalidPassenger = samp::gameplay::buildLocalPassengerSync(
        driver, driverVehicle, gameplayControls, vehicleGameplay);
    expect(!invalidPassenger.passengerReady &&
           invalidPassenger.status == samp::gameplay::LocalGameplayStatus::PassengerSeat,
           "driver seat must not receive a passenger Packet-211 candidate");
    driver.vehicleSeat = 2;
    driver.controllableKnown = true;
    driver.controllable = false;
    const auto frozenPassenger = samp::gameplay::buildLocalPassengerSync(
        driver, driverVehicle, gameplayControls, vehicleGameplay);
    expect(!frozenPassenger.passengerReady &&
           frozenPassenger.status == samp::gameplay::LocalGameplayStatus::FrozenByServer,
           "server freeze must reject a passenger Packet-211 candidate");

    local.controllableKnown = true;
    local.controllable = false;
    const auto frozenGameplay = samp::gameplay::advanceLocalOnFoot(
        local, gameplayControls, world, 0.04f, gameplay);
    expect(frozenGameplay.status == samp::gameplay::LocalGameplayStatus::FrozenByServer &&
           !gameplay.predictionActive && gameplay.authoritativeResetPending,
           "gameplay bridge must discard prediction on authoritative freeze");

    motion.position.x = 100.0f;
    const auto unsupportedStep = samp::movement::advance(world, motionInput, 0.0f, 0.02f, motion);
    expect(unsupportedStep.status == samp::movement::StepStatus::MissingGround,
           "motion must refuse free-flight when collision is unavailable");
    expect(!samp::gta::world::loadCollisionImgMember(source, index, "test.col", catalog, error, 64) &&
           catalog.modelCount() == 1 && catalog.find("testfloor") != nullptr,
           "collision member extraction cap must preserve catalog state");
  }
  {
    auto corrupt = makeCol3Fixture();
    writeLe32At(corrupt, 100, 0xFFFFFF00u);  // invalid face offset
    std::vector<samp::gta::col::Model> models;
    std::string error;
    expect(!samp::gta::col::parseArchive(corrupt, models, error) && models.empty(),
           "invalid COL offsets must fail closed");
  }
}

void testGtaFileDescriptorDataSource() {
  char temporaryPath[] = "/tmp/sampdroid-img-fixture-XXXXXX";
  const int descriptor = ::mkstemp(temporaryPath);
  expect(descriptor >= 0, "temporary IMG fixture file could not be created");
  const std::vector<uint8_t> fixture = makeImgV2Fixture();
  size_t written = 0;
  while (written < fixture.size()) {
    const ssize_t result = ::write(descriptor, fixture.data() + written, fixture.size() - written);
    expect(result > 0, "temporary IMG fixture file could not be written");
    written += static_cast<size_t>(result);
  }

  std::string error;
  auto source = samp::gta::img::FileDescriptorByteSource::duplicateFrom(descriptor, error);
  ::close(descriptor);
  ::unlink(temporaryPath);
  expect(source != nullptr && source->size() == fixture.size(),
         "descriptor-backed user data source could not be opened");
  samp::gta::img::ArchiveIndex index;
  expect(samp::gta::img::parseV2(*source, index, error) && index.find("test.col") != nullptr,
         "IMG parse via descriptor-backed data source changed");
  uint8_t twoBytes[2] = {};
  expect(!source->readAt(source->size() - 1, twoBytes, sizeof(twoBytes)),
         "descriptor source must reject out-of-range reads");
  expect(samp::gta::img::FileDescriptorByteSource::duplicateFrom(-1, error) == nullptr,
         "invalid descriptor must fail closed");
}

void testGtaDataRuntimeAssembly() {
  const auto writeTemporaryFile = [](const std::vector<uint8_t>& bytes) {
    char temporaryPath[] = "/tmp/sampdroid-runtime-fixture-XXXXXX";
    const int descriptor = ::mkstemp(temporaryPath);
    if (descriptor < 0) fail("runtime temporary fixture could not be created");
    size_t written = 0;
    while (written < bytes.size()) {
      const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
      if (result <= 0) {
        ::close(descriptor);
        ::unlink(temporaryPath);
        fail("runtime temporary fixture could not be written");
      }
      written += static_cast<size_t>(result);
    }
    ::unlink(temporaryPath);
    return descriptor;
  };
  const auto textBytes = [](const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
  };

  samp::gta::runtime::GameDataRuntime runtime;
  std::string error;
  int descriptor = writeTemporaryFile(makeImgWithCol3Fixture());
  expect(runtime.mountImgFileDescriptor(descriptor, error), "runtime could not mount selected IMG");
  ::close(descriptor);
  auto snapshot = runtime.snapshot();
  expect(snapshot.imgMounted && snapshot.imgEntries == 1 && snapshot.collisionModels == 0 &&
         snapshot.iplPlacements == 0,
         "runtime IMG mount snapshot changed");

  descriptor = writeTemporaryFile(textBytes(
      "IMG models/gta3.img\nIDE data/maps/test.ide\nIPL data/maps/test.ipl\n"
      "COLFILE 0 models/coll/test.col\n"));
  expect(runtime.loadGtaDatFileDescriptor(descriptor, error),
         "runtime could not load selected gta.dat manifest");
  ::close(descriptor);
  snapshot = runtime.snapshot();
  expect(snapshot.manifestImgPaths == 1 && snapshot.manifestIdePaths == 1 &&
         snapshot.manifestIplPaths == 1 && snapshot.manifestColPaths == 1 &&
         runtime.assetManifest().imgPaths == std::vector<std::string>({"models/gta3.img"}),
         "runtime gta.dat manifest snapshot changed");
  runtime.clearMapAssembly();
  expect(runtime.snapshot().imgMounted && runtime.snapshot().manifestImgPaths == 1,
         "runtime map clear must preserve IMG and gta.dat manifest");
  expect(runtime.loadAllImgCollisionMembers(error) && runtime.snapshot().collisionModels == 1,
         "runtime full IMG collision catalog sync changed");
  samp::gta::runtime::GameDataRuntime oversizedCollisionCatalog;
  descriptor = writeTemporaryFile(makeImgWithColEntryCount(
      samp::gta::runtime::kMaxRuntimeImgCollisionMembers + 1u));
  expect(oversizedCollisionCatalog.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount oversized collision-catalog fixture");
  ::close(descriptor);
  expect(!oversizedCollisionCatalog.loadAllImgCollisionMembers(error) &&
         oversizedCollisionCatalog.snapshot().collisionModels == 0,
         "runtime IMG collision catalog must enforce its member cap transactionally");
  // Companion IMG archives are user-selected/read-only too. Their COL members
  // must participate in the same bounded world catalog rather than becoming
  // DFF/TXD-only visual files.
  samp::gta::runtime::GameDataRuntime multiCollisionRuntime;
  descriptor = writeTemporaryFile(makeImgWithNamedCol3Fixture("PRIMARY.COL", "primaryfloor"));
  expect(multiCollisionRuntime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount primary IMG for multi-IMG collision sync");
  ::close(descriptor);
  descriptor = writeTemporaryFile(makeImgWithNamedCol3Fixture("AUXILIARY.COL", "auxfloor"));
  expect(multiCollisionRuntime.mountAdditionalImgFileDescriptors(std::vector<int>{descriptor}, error),
         "runtime could not mount auxiliary IMG for collision sync");
  ::close(descriptor);
  expect(multiCollisionRuntime.loadAllImgCollisionMembers(error) &&
         multiCollisionRuntime.snapshot().collisionModels == 2,
         "full collision sync must include primary and auxiliary IMG members");
  multiCollisionRuntime.clearMapAssembly();
  expect(multiCollisionRuntime.loadImgCollisionMember("auxiliary.col", error) &&
         multiCollisionRuntime.snapshot().collisionModels == 1,
         "explicit collision member load must resolve a mounted auxiliary IMG");
  samp::gta::runtime::GameDataRuntime cappedMultiCollisionRuntime;
  descriptor = writeTemporaryFile(makeImgWithColEntryCount(
      samp::gta::runtime::kMaxRuntimeImgCollisionMembers));
  expect(cappedMultiCollisionRuntime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount cap-sized primary collision IMG");
  ::close(descriptor);
  descriptor = writeTemporaryFile(makeImgWithNamedCol3Fixture("EXTRA.COL", "extrafloor"));
  expect(cappedMultiCollisionRuntime.mountAdditionalImgFileDescriptors(std::vector<int>{descriptor}, error),
         "runtime could not mount extra auxiliary collision IMG");
  ::close(descriptor);
  expect(!cappedMultiCollisionRuntime.loadAllImgCollisionMembers(error) &&
         cappedMultiCollisionRuntime.snapshot().collisionModels == 0,
         "multi-IMG collision sync must share one transactional member cap");

  samp::gta::runtime::GameDataRuntime batchRuntime;
  const int firstIdeDescriptor = writeTemporaryFile(textBytes(
      "objs\n2000, BatchOne, Generic, 100.0, 0\nend\n"));
  const int secondIdeDescriptor = writeTemporaryFile(textBytes(
      "objs\n2001, BatchTwo, Generic, 100.0, 0\nend\n"));
  expect(batchRuntime.loadIdeFileDescriptors(
             std::vector<int>{firstIdeDescriptor, secondIdeDescriptor}, error) &&
         batchRuntime.snapshot().ideObjects == 2,
         "runtime IDE descriptor batch changed");
  ::close(firstIdeDescriptor);
  ::close(secondIdeDescriptor);
  const int cyclicTxdParentDescriptor = writeTemporaryFile(textBytes(
      "txdp\nfirst, second\nsecond, first\nend\n"));
  expect(!batchRuntime.loadIdeFileDescriptor(cyclicTxdParentDescriptor, error) &&
         batchRuntime.snapshot().ideObjects == 2,
         "runtime must reject cyclic txdp graph transactionally");
  ::close(cyclicTxdParentDescriptor);

  // handling.cfg has an independent user-data lifetime: it can load before
  // cars.ide, survives a map-source rebuild, and binds only after the exact
  // source model → handling token join exists.
  samp::gta::runtime::GameDataRuntime handlingRuntime;
  descriptor = writeTemporaryFile(textBytes(
      "LANDSTAL 1700.0 4000.0 2.0 0.0 0.1 -0.3 70 0.75 0.85 0.52 5 160.0 25.0 10.0 R P 10.0 0.53 0 30.0 1.0 0.15 0.0 0.28 -0.14 0.5 0.0 0.2 0.5 0.5 20000 0 0 0 0 0\n"));
  expect(handlingRuntime.loadHandlingFileDescriptor(descriptor, error),
         "runtime could not load bounded handling.cfg descriptor");
  ::close(descriptor);
  samp::vehiclemotion::VehicleMotionTuning resolvedTuning;
  expect(handlingRuntime.snapshot().handlingProfiles == 1u &&
         !handlingRuntime.vehicleMotionTuning(400u, resolvedTuning),
         "handling source cannot tune an unknown/non-IDE vehicle model");
  descriptor = writeTemporaryFile(textBytes(
      "cars\n400, Landstal, landstal, car, LANDSTAL, landstal, null, executive, 10, 0, 0, -1, 0.7, 0.7, 0\nend\n"));
  expect(handlingRuntime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not load cars IDE handling join");
  ::close(descriptor);
  expect(handlingRuntime.vehicleMotionTuning(400u, resolvedTuning) &&
         std::fabs(resolvedTuning.maxForwardSpeed - (160.0f / 3.6f)) < 0.01f &&
         !handlingRuntime.vehicleMotionTuning(401u, resolvedTuning),
         "runtime must bind handling only through loaded cars IDE model IDs");
  handlingRuntime.clearMapAssembly();
  expect(handlingRuntime.snapshot().handlingProfiles == 1u &&
         !handlingRuntime.vehicleMotionTuning(400u, resolvedTuning),
         "map source rebuild must preserve profiles but drop stale model join");
  descriptor = writeTemporaryFile(textBytes("IMG models/gta3.img\n"));
  expect(handlingRuntime.loadGtaDatFileDescriptor(descriptor, error) &&
         handlingRuntime.snapshot().handlingProfiles == 0u,
         "new gta.dat manifest must drop handling from a prior source tree");
  ::close(descriptor);

  descriptor = writeTemporaryFile(makeCol3Fixture());
  expect(runtime.loadExternalCollisionFileDescriptor(descriptor, error),
         "runtime could not load selected external COLFILE");
  ::close(descriptor);
  expect(runtime.snapshot().collisionModels == 1,
         "runtime external COLFILE catalog changed");
  expect(runtime.loadImgCollisionMember("test.col", error),
         "runtime could not load named IMG collision member");

  descriptor = writeTemporaryFile(textBytes(
      "objs\n1000, TestFloor, Generic, 250.0, 16\nend\n"));
  expect(runtime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not load selected text IDE");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "inst\n1000, TestFloor, 0, 10, -20, 5, 0, 0, 0, 1, -1\nend\n"));
  expect(runtime.loadIplFileDescriptor(descriptor, error),
         "runtime could not load selected text IPL");
  ::close(descriptor);
  snapshot = runtime.snapshot();
  expect(snapshot.ideObjects == 1 && snapshot.iplPlacements == 1 &&
         snapshot.collisionInstances == 1 && snapshot.missingCollisionPlacements == 0,
         "runtime IDE/IPL/collision assembly snapshot changed");
  samp::gta::col::FloorHit floor;
  expect(runtime.collisionWorld().findHighestFloor(10.0f, -20.0f, 4.0f, 8.0f, 0, floor) &&
         std::fabs(floor.z - 7.0f) < 0.001f,
         "runtime assembled world floor query changed");

  descriptor = writeTemporaryFile(makeBinaryIplFixture(1000u, 0u));
  expect(runtime.loadIplFileDescriptor(descriptor, error),
         "runtime could not resolve binary IPL INST through loaded IDE");
  ::close(descriptor);
  snapshot = runtime.snapshot();
  expect(snapshot.iplPlacements == 2 && snapshot.collisionInstances == 2 &&
         runtime.collisionWorld().findHighestFloor(10.0f, -20.0f, 4.0f, 8.0f, 0, floor),
         "runtime binary IPL assembly changed");

  descriptor = writeTemporaryFile(textBytes(
      "inst\n1000, Dummy, 0, 10, -20, 5, 0, 0, 0, 1, -1\nend\n"));
  expect(runtime.loadIplFileDescriptor(descriptor, error),
         "runtime must resolve ignored text IPL model name through IDE");
  ::close(descriptor);
  snapshot = runtime.snapshot();
  expect(snapshot.iplPlacements == 3 && snapshot.collisionInstances == 3,
         "text IPL IDE-name resolution changed");
  descriptor = writeTemporaryFile(makeBinaryIplFixture(9999u, 0u));
  expect(!runtime.loadIplFileDescriptor(descriptor, error),
         "runtime must reject binary IPL without mapped model ID");
  ::close(descriptor);
  snapshot = runtime.snapshot();
  expect(snapshot.iplPlacements == 3 && snapshot.collisionInstances == 3,
         "failed runtime IPL load must preserve mounted world");

  samp::gta::runtime::GameDataRuntime binaryWithoutIde;
  descriptor = writeTemporaryFile(makeBinaryIplFixture());
  expect(!binaryWithoutIde.loadIplFileDescriptor(descriptor, error) &&
         binaryWithoutIde.snapshot().iplPlacements == 0,
         "binary IPL without IDE mapping must fail closed");
  ::close(descriptor);

  samp::gta::runtime::GameDataRuntime streamingRuntime;
  descriptor = writeTemporaryFile(makeImgWithBinaryStreamingIplFixture());
  expect(streamingRuntime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount IMG carrying streaming IPL");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "IMG models/gta3.img\nIDE data/maps/la/lae2.ide\nIPL data/maps/la/lae2.ipl\n"));
  expect(streamingRuntime.loadGtaDatFileDescriptor(descriptor, error),
         "runtime could not load manifest for streaming IPL");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "objs\n1000, TestFloor, Generic, 250.0, 16\nend\n"));
  expect(streamingRuntime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not load IDE for streaming IPL");
  ::close(descriptor);
  expect(streamingRuntime.loadImgStreamingIpls(error) &&
         streamingRuntime.snapshot().iplPlacements == 1,
         "runtime IMG binary streaming IPL discovery changed");

  descriptor = writeTemporaryFile(makeImgWithDffAndTxdFixture("TESTFLOOR.DFF", "GENERIC.TXD"));
  expect(runtime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount IMG carrying placement DFF/TXD");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "objs\n1000, TestFloor, Generic, 250.0, 16\nend\n"
      "cars\n400, TestFloor, Generic, car\nend\n"
      "peds\n0, TestFloor, Generic, CIVMALE\nend\n"));
  expect(runtime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not reload IDE/car mapping for placement DFF");
  ::close(descriptor);
  descriptor = writeTemporaryFile(makeBinaryIplFixture(1000u, 0u));
  expect(runtime.loadIplFileDescriptor(descriptor, error),
         "runtime could not reload binary IPL for placement DFF");
  ::close(descriptor);
  samp::gta::runtime::NearbyWorldRenderBatch nearbyWorld;
  const bool nearbyWorldCopied = runtime.copyNearbyWorldRenderBatch(
      10.0f, -20.0f, 5.0f, 40.0f, 4, 12, nearbyWorld, error);
  const bool nearbyWorldExpected = nearbyWorldCopied && nearbyWorld.renderedPlacements == 1 &&
      nearbyWorld.renderedModels == 1 && nearbyWorld.vertices.size() == 21 &&
      std::fabs(nearbyWorld.vertices[0] + 1.0f) < 0.001f &&
      std::fabs(nearbyWorld.vertices[1]) < 0.001f &&
      std::fabs(nearbyWorld.vertices[2] - 1.0f) < 0.001f;
  if (!nearbyWorldExpected) {
    const std::string batchError = error;
    const bool directDffLoad = runtime.loadImgDffMember("testfloor.dff", error);
    fail("runtime placement-aware DFF batch changed: copied=" +
         std::to_string(nearbyWorldCopied) + " placements=" +
         std::to_string(nearbyWorld.renderedPlacements) + " models=" +
         std::to_string(nearbyWorld.renderedModels) + " vertices=" +
         std::to_string(nearbyWorld.vertices.size()) + " directDffLoad=" +
         std::to_string(directDffLoad) + " batchError=" + batchError +
         " directError=" + error);
  }
  samp::gta::runtime::NearbyWorldTexturedRenderBatch nearbyTexturedWorld;
  expect(runtime.copyNearbyWorldTexturedRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 12, 4,
                                                    nearbyTexturedWorld, error) &&
         nearbyTexturedWorld.vertices.size() == 27 && nearbyTexturedWorld.textureWidth == 2 &&
         nearbyTexturedWorld.textureHeight == 2 && nearbyTexturedWorld.textureRgba.size() == 16 &&
         std::fabs(nearbyTexturedWorld.vertices[0] + 1.0f) < 0.001f &&
         std::fabs(nearbyTexturedWorld.vertices[1]) < 0.001f &&
         std::fabs(nearbyTexturedWorld.vertices[2] - 1.0f) < 0.001f,
         "runtime placement-aware DFF/TXD batch changed");
  const std::vector<samp::gta::removal::Directive> removeRenderedFloor = {
      {1000u, 10.0f, -20.0f, 5.0f, 3.0f}};
  samp::gta::runtime::NearbyWorldRenderBatch removedNearbyWorld;
  expect(runtime.copyNearbyWorldRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 4, 12,
                                            removedNearbyWorld, error, 0,
                                            &removeRenderedFloor) &&
             removedNearbyWorld.vertices.empty() &&
             removedNearbyWorld.renderedPlacements == 0u,
         "RemoveBuilding must hide matching static IPL geometry in color rendering");
  samp::gta::runtime::NearbyWorldTexturedRenderBatch removedTexturedWorld;
  expect(runtime.copyNearbyWorldTexturedRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 12, 4,
                                                    removedTexturedWorld, error, 0,
                                                    &removeRenderedFloor) &&
             removedTexturedWorld.vertices.empty() && removedTexturedWorld.textureRgba.empty(),
         "RemoveBuilding must hide matching static IPL texture overlays");

  // A server-authoritative nonzero interior must select only matching IPL
  // placements, never blend nearby exterior records into the same batch.
  descriptor = writeTemporaryFile(textBytes(
      "inst\n1000, InteriorFloor, 7, 10, -20, 5, 0, 0, 0, 1, -1\nend\n"));
  expect(runtime.loadIplFileDescriptor(descriptor, error),
         "runtime could not load selected interior IPL placement");
  ::close(descriptor);
  samp::gta::runtime::NearbyWorldRenderBatch interiorWorld;
  expect(runtime.copyNearbyWorldRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 4, 12,
                                            interiorWorld, error, 7) &&
         interiorWorld.consideredPlacements == 1 && interiorWorld.renderedPlacements == 1 &&
         interiorWorld.vertices.size() == 21,
         "runtime must render only the requested interior IPL batch");
  samp::gta::runtime::NearbyWorldTexturedRenderBatch interiorTexturedWorld;
  expect(runtime.copyNearbyWorldTexturedRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 12, 4,
                                                    interiorTexturedWorld, error, 7) &&
         interiorTexturedWorld.vertices.size() == 27 &&
         interiorTexturedWorld.textureRgba.size() == 16,
         "runtime must resolve a texture overlay in the requested interior");
  expect(!runtime.copyNearbyWorldRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 4, 12,
                                             interiorWorld, error, 256) &&
         interiorWorld.vertices.empty(),
         "out-of-range requested interior must fail closed");
  // A stock-style txdp relation must resolve a material from its explicit
  // parent dictionary when the child TXD is structurally valid but empty.
  samp::gta::runtime::GameDataRuntime txdParentRuntime;
  descriptor = writeTemporaryFile(makeImgWithTxdParentFixture());
  expect(txdParentRuntime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount IMG carrying TXD parent fixture");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "objs\n1000, TestFloor, Child, 250.0, 16\nend\n"
      "txdp\nchild, parent\nend\n"));
  expect(txdParentRuntime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not load txdp parent relation");
  ::close(descriptor);
  descriptor = writeTemporaryFile(makeBinaryIplFixture(1000u, 0u));
  expect(txdParentRuntime.loadIplFileDescriptor(descriptor, error),
         "runtime could not load placement for TXD parent relation");
  ::close(descriptor);
  samp::gta::runtime::TexturePreview parentTextDrawSprite;
  expect(txdParentRuntime.copyTexturePreview("CHILD", "TESTTEX", 4,
                                              parentTextDrawSprite, error) &&
         parentTextDrawSprite.width == 2 && parentTextDrawSprite.height == 2 &&
         parentTextDrawSprite.rgba.size() == 16,
         "font-4 sprite lookup must lazily reuse cycle-safe txdp parent resolution");
  expect(txdParentRuntime.loadImgDffMember("testfloor.dff", error),
         "runtime mixed DFF fixture must remain portable");
  samp::gta::runtime::NearbyWorldRenderBatch txdParentColor;
  expect(txdParentRuntime.copyNearbyWorldRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 4, 12,
                                                      txdParentColor, error) &&
         txdParentColor.renderedPlacements == 1,
         "runtime could not prepare model before TXD parent fallback");
  samp::gta::runtime::NearbyWorldTexturedRenderBatch txdParentTextured;
  expect(txdParentRuntime.copyNearbyWorldTexturedRenderBatch(
             10.0f, -20.0f, 5.0f, 40.0f, 12, 4, txdParentTextured, error) &&
         txdParentTextured.vertices.size() == 27 && txdParentTextured.textureWidth == 2 &&
         txdParentTextured.textureHeight == 2 && txdParentTextured.textureRgba.size() == 16 &&
         txdParentRuntime.snapshot().txdDictionaries == 2 &&
         txdParentRuntime.snapshot().txdParentRelations == 1 &&
         txdParentRuntime.snapshot().textures == 1,
         "runtime TXD child-to-parent material fallback changed");

  // Auxiliary IMG lookup keeps the primary map/collision archive intact while
  // allowing a txdp child in a second archive to fall back to a parent TXD in
  // the primary archive. This is the stock cutscene.img -> gta3.img shape.
  samp::gta::runtime::GameDataRuntime multiImgRuntime;
  descriptor = writeTemporaryFile(makeImgWithNamedTxdFixture("PARENT.TXD"));
  expect(multiImgRuntime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount primary IMG for auxiliary fallback");
  ::close(descriptor);
  descriptor = writeTemporaryFile(makeImgWithDffAndEmptyTxdFixture());
  expect(multiImgRuntime.mountAdditionalImgFileDescriptors(std::vector<int>{descriptor}, error),
         "runtime could not mount bounded auxiliary IMG");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "objs\n1000, TestFloor, Child, 250.0, 0\nend\n"
      "txdp\nchild, parent\nend\n"));
  expect(multiImgRuntime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not load multi-IMG txdp relation");
  ::close(descriptor);
  descriptor = writeTemporaryFile(makeBinaryIplFixture(1000u, 0u));
  expect(multiImgRuntime.loadIplFileDescriptor(descriptor, error),
         "runtime could not load multi-IMG placement");
  ::close(descriptor);
  samp::gta::runtime::NearbyWorldRenderBatch multiImgColor;
  expect(multiImgRuntime.copyNearbyWorldRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 4, 12,
                                                     multiImgColor, error) &&
         multiImgColor.renderedPlacements == 1,
         "runtime DFF lookup in auxiliary IMG changed");
  samp::gta::runtime::NearbyWorldTexturedRenderBatch multiImgTextured;
  expect(multiImgRuntime.copyNearbyWorldTexturedRenderBatch(
             10.0f, -20.0f, 5.0f, 40.0f, 12, 4, multiImgTextured, error) &&
         multiImgTextured.vertices.size() == 27 && multiImgTextured.textureRgba.size() == 16 &&
         multiImgRuntime.snapshot().imgEntries == 1 &&
         multiImgRuntime.snapshot().additionalImgArchives == 1 &&
         multiImgRuntime.snapshot().additionalImgEntries == 2 &&
         multiImgRuntime.snapshot().additionalImgBytes > 0 &&
         multiImgRuntime.snapshot().txdDictionaries == 2,
         "runtime cross-IMG txdp texture fallback changed");
  expect(!multiImgRuntime.mountAdditionalImgFileDescriptors(std::vector<int>{-1}, error) &&
         multiImgRuntime.snapshot().additionalImgArchives == 1,
         "invalid auxiliary IMG mount must preserve runtime state");
  multiImgRuntime.clearMapAssembly();
  expect(multiImgRuntime.snapshot().imgMounted &&
         multiImgRuntime.snapshot().additionalImgArchives == 1 &&
         multiImgRuntime.snapshot().txdParentRelations == 0,
         "map clear must retain mounted auxiliary IMG but discard txdp metadata");

  // Multiple nearby placements may carry distinct texture dictionaries. The
  // batch API must preserve complete triangle groups, share its vertex/pixel
  // caps, and leave the legacy one-overlay wrapper deterministic.
  samp::gta::runtime::GameDataRuntime multiTextureRuntime;
  descriptor = writeTemporaryFile(makeImgWithTwoDffAndTxdFixture());
  expect(multiTextureRuntime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount two-material IMG fixture");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "objs\n1000, TestOne, One, 250.0, 0\n1001, TestTwo, Two, 250.0, 0\nend\n"));
  expect(multiTextureRuntime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not load two-material IDE fixture");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "inst\n1000, ignored, 0, 10, -20, 5, 0, 0, 0, 1, -1\n"
      "1001, ignored, 0, 14, -20, 5, 0, 0, 0, 1, -1\n"
      // Repeated TestOne placement verifies texture-slot coalescing without
      // dropping its transformed triangles.
      "1000, ignored, 0, 18, -20, 5, 0, 0, 0, 1, -1\nend\n"));
  expect(multiTextureRuntime.loadIplFileDescriptor(descriptor, error),
         "runtime could not load two-material IPL fixture");
  ::close(descriptor);
  samp::gta::runtime::NearbyWorldRenderBatch multiTextureColor;
  expect(multiTextureRuntime.copyNearbyWorldRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 4, 12,
                                                         multiTextureColor, error) &&
         multiTextureColor.renderedPlacements == 3,
         "runtime could not prepare nearby textured models");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches multiTextureBatches;
  expect(multiTextureRuntime.copyNearbyWorldTexturedRenderBatches(
             10.0f, -20.0f, 5.0f, 40.0f, 4, 12, 4, multiTextureBatches, error) &&
         multiTextureBatches.batches.size() == 2 &&
         multiTextureBatches.batches[0].vertices.size() == 54 &&
         multiTextureBatches.batches[1].vertices.size() == 27 &&
         multiTextureBatches.batches[0].textureRgba.size() == 16 &&
         multiTextureBatches.batches[1].textureRgba.size() == 16,
         "runtime bounded multi-material nearby texture batch changed");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches cappedTextureBatches;
  expect(multiTextureRuntime.copyNearbyWorldTexturedRenderBatches(
             10.0f, -20.0f, 5.0f, 40.0f, 1, 12, 4, cappedTextureBatches, error) &&
         cappedTextureBatches.batches.size() == 1 &&
         cappedTextureBatches.batches[0].vertices.size() == 54,
         "runtime multi-material texture batch count cap changed");
  samp::gta::runtime::NearbyWorldTexturedRenderBatch legacyTextureBatch;
  expect(multiTextureRuntime.copyNearbyWorldTexturedRenderBatch(
             10.0f, -20.0f, 5.0f, 40.0f, 12, 4, legacyTextureBatch, error) &&
         legacyTextureBatch.vertices.size() == 54,
         "legacy one-material nearby texture wrapper changed");

  std::vector<samp::gta::runtime::ServerObjectRenderInput> texturedDynamicInputs = {
      {1000u, 10.0f, -20.0f, 5.0f, 0.0f, 0.0f, 0.0f, 40.0f},
      {1001u, 14.0f, -20.0f, 5.0f, 0.0f, 0.0f, 15.0f, 40.0f},
      // A second instance of TestOne must append to the first material slot,
      // not spend a duplicate GLES texture batch.
      {1000u, 18.0f, -20.0f, 5.0f, 0.0f, 0.0f, -20.0f, 40.0f},
  };
  samp::gta::runtime::NearbyWorldTexturedRenderBatches texturedDynamicBatches;
  expect(multiTextureRuntime.copyServerObjectTexturedRenderBatches(
             10.0f, -20.0f, 5.0f, 40.0f, texturedDynamicInputs, 4, 12, 4,
             texturedDynamicBatches, error) &&
         texturedDynamicBatches.batches.size() == 2 &&
         texturedDynamicBatches.batches[0].vertices.size() == 54 &&
         texturedDynamicBatches.batches[1].vertices.size() == 27,
         "runtime bounded dynamic-object texture batches changed");

  samp::gta::runtime::NearbyWorldTexturedRenderBatches cappedDynamicTextureBatches;
  expect(multiTextureRuntime.copyServerObjectTexturedRenderBatches(
             10.0f, -20.0f, 5.0f, 40.0f, texturedDynamicInputs, 1, 12, 4,
             cappedDynamicTextureBatches, error) &&
         cappedDynamicTextureBatches.batches.size() == 1 &&
         cappedDynamicTextureBatches.batches[0].vertices.size() == 54,
         "runtime dynamic-object texture batch count cap changed");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches vertexCappedDynamicTextureBatches;
  expect(multiTextureRuntime.copyServerObjectTexturedRenderBatches(
             10.0f, -20.0f, 5.0f, 40.0f, texturedDynamicInputs, 4, 3, 4,
             vertexCappedDynamicTextureBatches, error) &&
         vertexCappedDynamicTextureBatches.batches.size() == 1 &&
         vertexCappedDynamicTextureBatches.batches[0].vertices.size() == 27,
         "runtime dynamic-object texture vertex aggregate cap changed");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches rejectedDynamicTextureBatches;
  expect(multiTextureRuntime.copyServerObjectTexturedRenderBatches(
             10.0f, -20.0f, 5.0f, 40.0f, texturedDynamicInputs, 4, 12, 3,
             rejectedDynamicTextureBatches, error) && rejectedDynamicTextureBatches.batches.empty(),
         "runtime dynamic-object oversized texture must skip safely");

  // A single portable DFF can contain several independently textured meshes.
  // Both the IPL and dynamic-transform APIs must emit all resolvable materials,
  // rather than stopping after the first texture name in that model.
  samp::gta::runtime::GameDataRuntime multiMaterialRuntime;
  descriptor = writeTemporaryFile(makeImgWithMultiMaterialDffAndTxdFixture());
  expect(multiMaterialRuntime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount multi-material IMG fixture");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes("objs\n1002, Multi, Multi, 250.0, 0\nend\n"));
  expect(multiMaterialRuntime.loadIdeFileDescriptor(descriptor, error),
         "runtime could not load multi-material IDE fixture");
  ::close(descriptor);
  descriptor = writeTemporaryFile(textBytes(
      "inst\n1002, ignored, 0, 20, -20, 5, 0, 0, 0, 1, -1\nend\n"));
  expect(multiMaterialRuntime.loadIplFileDescriptor(descriptor, error),
         "runtime could not load multi-material IPL fixture");
  ::close(descriptor);
  samp::gta::runtime::NearbyWorldRenderBatch multiMaterialColor;
  expect(multiMaterialRuntime.copyNearbyWorldRenderBatch(
             20.0f, -20.0f, 5.0f, 40.0f, 4, 12, multiMaterialColor, error) &&
         multiMaterialColor.vertices.size() == 42,
         "runtime could not prepare multi-material color model");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches multiMaterialStatic;
  expect(multiMaterialRuntime.copyNearbyWorldTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f, 4, 12, 4, multiMaterialStatic, error) &&
         multiMaterialStatic.batches.size() == 2 &&
         multiMaterialStatic.batches[0].vertices.size() == 27 &&
         multiMaterialStatic.batches[1].vertices.size() == 27 &&
         multiMaterialStatic.batches[0].textureRgba.size() == 16 &&
         multiMaterialStatic.batches[1].textureRgba.size() == 16,
         "runtime static DFF multi-material overlays changed");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches cappedMultiMaterialStatic;
  expect(multiMaterialRuntime.copyNearbyWorldTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f, 1, 12, 4, cappedMultiMaterialStatic, error) &&
         cappedMultiMaterialStatic.batches.size() == 1 &&
         cappedMultiMaterialStatic.batches[0].vertices.size() == 27,
         "runtime static DFF multi-material slot cap changed");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches multiMaterialDynamic;
  expect(multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{
                 {1002u, 20.0f, -20.0f, 5.0f, 0.0f, 0.0f, 23.0f, 40.0f}},
             4, 12, 4, multiMaterialDynamic, error) &&
         multiMaterialDynamic.batches.size() == 2 &&
         multiMaterialDynamic.batches[0].vertices.size() == 27 &&
         multiMaterialDynamic.batches[1].vertices.size() == 27,
         "runtime dynamic DFF multi-material overlays changed");

  // A server ObjectMaterial slot maps to the flattened portable DFF material
  // order. Replacing slot zero with the second texture must redraw only that
  // slot, coalesce the now-shared immutable TXD texture, and apply ABGR
  // modulation without contaminating the untouched slot's original color.
  samp::gta::runtime::ServerObjectRenderInput materialOverrideObject;
  materialOverrideObject.modelId = 1002u;
  materialOverrideObject.x = 20.0f;
  materialOverrideObject.y = -20.0f;
  materialOverrideObject.z = 5.0f;
  materialOverrideObject.rotationZ = 23.0f;
  materialOverrideObject.drawDistance = 40.0f;
  materialOverrideObject.materialOverrides[0].active = true;
  materialOverrideObject.materialOverrides[0].colourAbgr = 0x80402010u;
  materialOverrideObject.materialOverrides[0].textureDictionary = "multi";
  materialOverrideObject.materialOverrides[0].textureName = "twotex";
  samp::gta::runtime::NearbyWorldRenderBatch overriddenMaterialColor;
  expect(multiMaterialRuntime.copyServerObjectRenderBatch(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{materialOverrideObject},
             12, overriddenMaterialColor, error) &&
         overriddenMaterialColor.vertices.size() == 42 &&
         std::fabs(overriddenMaterialColor.vertices[3] - 16.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialColor.vertices[4] - 32.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialColor.vertices[5] - 64.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialColor.vertices[6] - 128.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialColor.vertices[24] - 128.0f / 255.0f) < 0.001f,
         "runtime server material color override must also reach base mesh");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches overriddenMaterialDynamic;
  expect(multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{materialOverrideObject},
             4, 12, 4, overriddenMaterialDynamic, error) &&
         overriddenMaterialDynamic.batches.size() == 1 &&
         overriddenMaterialDynamic.batches[0].vertices.size() == 54 &&
         std::fabs(overriddenMaterialDynamic.batches[0].vertices[3] - 16.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialDynamic.batches[0].vertices[4] - 32.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialDynamic.batches[0].vertices[5] - 64.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialDynamic.batches[0].vertices[6] - 128.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialDynamic.batches[0].vertices[30] - 128.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialDynamic.batches[0].vertices[31] - 200.0f / 255.0f) < 0.001f &&
         std::fabs(overriddenMaterialDynamic.batches[0].vertices[32] - 1.0f) < 0.001f,
         "runtime flattened server material texture/color override changed");
  materialOverrideObject.materialOverrides[0].colourAbgr = 0u;
  samp::gta::runtime::NearbyWorldTexturedRenderBatches zeroColourMaterialDynamic;
  expect(multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{materialOverrideObject},
             4, 12, 4, zeroColourMaterialDynamic, error) &&
         zeroColourMaterialDynamic.batches.size() == 1 &&
         std::fabs(zeroColourMaterialDynamic.batches[0].vertices[3] - 128.0f / 255.0f) < 0.001f &&
         std::fabs(zeroColourMaterialDynamic.batches[0].vertices[4] - 200.0f / 255.0f) < 0.001f &&
         std::fabs(zeroColourMaterialDynamic.batches[0].vertices[5] - 1.0f) < 0.001f,
         "zero server material colour must preserve DFF modulation");
  materialOverrideObject.materialOverrides[0].sourceModel = 0xFFFFu;  // API model -1.
  materialOverrideObject.materialOverrides[0].colourAbgr = 0x80402010u;
  materialOverrideObject.materialOverrides[0].textureDictionary = "none";
  materialOverrideObject.materialOverrides[0].textureName = "none";
  samp::gta::runtime::NearbyWorldTexturedRenderBatches colorOnlyMaterialDynamic;
  expect(multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{materialOverrideObject},
             4, 12, 4, colorOnlyMaterialDynamic, error) &&
         colorOnlyMaterialDynamic.batches.size() == 1 &&
         colorOnlyMaterialDynamic.batches[0].vertices.size() == 27,
         "model minus-one material must stay color-only without hiding other slots");
  materialOverrideObject.materialOverrides[0].textureDictionary.clear();
  materialOverrideObject.materialOverrides[0].textureName.clear();
  samp::gta::runtime::NearbyWorldTexturedRenderBatches emptyNameColorOnlyDynamic;
  expect(multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{materialOverrideObject},
             4, 12, 4, emptyNameColorOnlyDynamic, error) &&
         emptyNameColorOnlyDynamic.batches.size() == 1 &&
         emptyNameColorOnlyDynamic.batches[0].vertices.size() == 27,
         "empty-name model minus-one material must remain a valid color-only overlay");
  materialOverrideObject.materialOverrides[0].textureDictionary = "../invalid";
  samp::gta::runtime::NearbyWorldTexturedRenderBatches invalidMaterialDynamic;
  expect(!multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{materialOverrideObject},
             4, 12, 4, invalidMaterialDynamic, error) && invalidMaterialDynamic.batches.empty(),
         "unsafe dynamic server material lookup key must fail closed");

  // Text=2 uses exact source material dimensions/ARGB fields but an asset-free
  // native fallback glyph raster. It owns a texture batch for only its flattened
  // slot, suppresses prelight just like a nonzero colour replacement, and does
  // not make the untouched DFF material disappear.
  samp::gta::runtime::ServerObjectRenderInput textMaterialObject;
  textMaterialObject.modelId = 1002u;
  textMaterialObject.x = 20.0f;
  textMaterialObject.y = -20.0f;
  textMaterialObject.z = 5.0f;
  textMaterialObject.rotationZ = 23.0f;
  textMaterialObject.drawDistance = 40.0f;
  auto& textMaterial = textMaterialObject.materialOverrides[0];
  textMaterial.textActive = true;
  textMaterial.textMaterialSize = 10u;  // exact ObjectMaterialSize_32x32
  textMaterial.textFontSize = 12u;
  textMaterial.textBold = true;
  textMaterial.textColourArgb = 0xFF102030u;
  textMaterial.textBackgroundColourArgb = 0x80405060u;
  textMaterial.textAlignment = 1u;
  textMaterial.text = "A";
  samp::gta::runtime::NearbyWorldRenderBatch textMaterialColor;
  expect(multiMaterialRuntime.copyServerObjectRenderBatch(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{textMaterialObject},
             12, textMaterialColor, error) && textMaterialColor.vertices.size() == 42 &&
         std::fabs(textMaterialColor.vertices[3] - 1.0f) < 0.001f &&
         std::fabs(textMaterialColor.vertices[4] - 1.0f) < 0.001f &&
         std::fabs(textMaterialColor.vertices[5] - 1.0f) < 0.001f &&
         std::fabs(textMaterialColor.vertices[6] - 1.0f) < 0.001f,
         "text material must suppress base prelight with neutral modulation");
  samp::gta::runtime::NearbyWorldTexturedRenderBatches textMaterialDynamic;
  expect(multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{textMaterialObject},
             4, 12, 1024, textMaterialDynamic, error) &&
         textMaterialDynamic.batches.size() == 2 &&
         textMaterialDynamic.batches[0].textureWidth == 32u &&
         textMaterialDynamic.batches[0].textureHeight == 32u &&
         textMaterialDynamic.batches[0].textureRgba.size() == 4096u &&
         textMaterialDynamic.batches[0].vertices.size() == 27u &&
         textMaterialDynamic.batches[0].textureRgba[0] == 0x40u &&
         textMaterialDynamic.batches[0].textureRgba[1] == 0x50u &&
         textMaterialDynamic.batches[0].textureRgba[2] == 0x60u &&
         textMaterialDynamic.batches[0].textureRgba[3] == 0x80u &&
         textMaterialDynamic.batches[1].vertices.size() == 27u,
         "text material must emit bounded ARGB-generated overlay plus sibling slot");
  auto malformedTextMaterialObject = textMaterialObject;
  malformedTextMaterialObject.materialOverrides[0].textMaterialSize = 11u;
  samp::gta::runtime::NearbyWorldTexturedRenderBatches malformedTextMaterialDynamic;
  expect(!multiMaterialRuntime.copyServerObjectTexturedRenderBatches(
             20.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{malformedTextMaterialObject},
             4, 12, 1024, malformedTextMaterialDynamic, error) &&
         malformedTextMaterialDynamic.batches.empty(),
         "invalid text material dimensions must fail closed before rendering");

  samp::gta::runtime::ServerObjectRenderInput serverObject;
  serverObject.modelId = 1000;
  serverObject.x = 10.0f;
  serverObject.y = -20.0f;
  serverObject.z = 5.0f;
  serverObject.drawDistance = 100.0f;
  samp::gta::runtime::NearbyWorldRenderBatch serverObjectBatch;
  expect(runtime.copyServerObjectRenderBatch(10.0f, -20.0f, 5.0f, 40.0f,
                                             std::vector<samp::gta::runtime::ServerObjectRenderInput>{serverObject},
                                             12, serverObjectBatch, error) &&
         serverObjectBatch.vertices.size() == 21 && serverObjectBatch.renderedPlacements == 1 &&
         std::fabs(serverObjectBatch.vertices[0] + 1.0f) < 0.001f &&
         std::fabs(serverObjectBatch.vertices[1]) < 0.001f &&
         std::fabs(serverObjectBatch.vertices[2] - 1.0f) < 0.001f,
         "runtime server-object DFF batch changed");
  auto scaledServerObject = serverObject;
  scaledServerObject.scaleX = 2.0f;
  scaledServerObject.scaleY = 2.0f;
  scaledServerObject.scaleZ = 2.0f;
  samp::gta::runtime::NearbyWorldRenderBatch scaledServerObjectBatch;
  expect(runtime.copyServerObjectRenderBatch(
             10.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{scaledServerObject},
             12, scaledServerObjectBatch, error) &&
         scaledServerObjectBatch.vertices.size() == 21 &&
         std::fabs(scaledServerObjectBatch.vertices[0] + 2.0f) < 0.001f &&
         std::fabs(scaledServerObjectBatch.vertices[1]) < 0.001f &&
         std::fabs(scaledServerObjectBatch.vertices[2] - 2.0f) < 0.001f,
         "dynamic DFF source scale must affect copied geometry before rotation");
  scaledServerObject.scaleX = 0.0f;
  expect(!runtime.copyServerObjectRenderBatch(
             10.0f, -20.0f, 5.0f, 40.0f,
             std::vector<samp::gta::runtime::ServerObjectRenderInput>{scaledServerObject},
             12, scaledServerObjectBatch, error) && scaledServerObjectBatch.vertices.empty(),
         "degenerate dynamic DFF scale must fail closed");
  samp::gta::runtime::ServerObjectRenderInput streamedVehicleModel;
  streamedVehicleModel.modelId = 400;
  streamedVehicleModel.x = 10.0f;
  streamedVehicleModel.y = -20.0f;
  streamedVehicleModel.z = 5.0f;
  streamedVehicleModel.drawDistance = 100.0f;
  samp::gta::runtime::NearbyWorldRenderBatch vehicleModelBatch;
  expect(runtime.copyServerObjectRenderBatch(10.0f, -20.0f, 5.0f, 40.0f,
                                             std::vector<samp::gta::runtime::ServerObjectRenderInput>{streamedVehicleModel},
                                             12, vehicleModelBatch, error) &&
         vehicleModelBatch.vertices.size() == 21 && vehicleModelBatch.renderedPlacements == 1 &&
         std::fabs(vehicleModelBatch.vertices[0] + 1.0f) < 0.001f &&
         std::fabs(vehicleModelBatch.vertices[1]) < 0.001f &&
         std::fabs(vehicleModelBatch.vertices[2] - 1.0f) < 0.001f,
         "runtime cars IDE mapping must resolve streamed vehicle DFF batch");
  samp::gta::runtime::ServerObjectRenderInput streamedPedModel;
  streamedPedModel.modelId = 0;  // CJ is a valid ped model ID.
  streamedPedModel.x = 10.0f;
  streamedPedModel.y = -20.0f;
  streamedPedModel.z = 5.0f;
  streamedPedModel.drawDistance = 100.0f;
  samp::gta::runtime::NearbyWorldRenderBatch pedModelBatch;
  expect(runtime.copyServerObjectRenderBatch(10.0f, -20.0f, 5.0f, 40.0f,
                                             std::vector<samp::gta::runtime::ServerObjectRenderInput>{streamedPedModel},
                                             12, pedModelBatch, error) &&
         pedModelBatch.vertices.size() == 21 && pedModelBatch.renderedPlacements == 1 &&
         std::fabs(pedModelBatch.vertices[0] + 1.0f) < 0.001f &&
         std::fabs(pedModelBatch.vertices[1]) < 0.001f &&
         std::fabs(pedModelBatch.vertices[2] - 1.0f) < 0.001f,
         "runtime peds IDE mapping must resolve model zero DFF batch");
  expect(!runtime.copyNearbyWorldRenderBatch(10.0f, -20.0f, 5.0f, 40.0f, 4, 2,
                                             nearbyWorld, error) && nearbyWorld.vertices.empty(),
         "runtime placement DFF batch vertex cap must fail closed");

  descriptor = writeTemporaryFile(makeImgWithDffFixture());
  expect(runtime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount IMG carrying portable DFF");
  ::close(descriptor);
  expect(runtime.loadAllImgCollisionMembers(error) && runtime.snapshot().collisionModels == 0,
         "IMG roots without .col chunks must remain a safe collision-sync no-op");
  expect(runtime.loadImgDffMember("test.dff", error),
         "runtime could not load named IMG DFF member");
  snapshot = runtime.snapshot();
  expect(snapshot.dffModels == 1 && snapshot.dffMeshes == 1 && snapshot.dffVertices == 3 &&
         snapshot.collisionModels == 0 && snapshot.iplPlacements == 0,
         "runtime DFF mesh catalog snapshot changed");
  std::vector<float> dffPreview;
  expect(runtime.copyLatestDffPreview(3, dffPreview, error) && dffPreview.size() == 21 &&
         std::fabs(dffPreview[0] + 1.0f) < 0.001f && std::fabs(dffPreview[1]) < 0.001f &&
         std::fabs(dffPreview[2] - 1.0f) < 0.001f &&
         std::fabs(dffPreview[3] - (128.0f / 255.0f)) < 0.001f &&
         std::fabs(dffPreview[4] - (200.0f / 255.0f)) < 0.001f &&
         std::fabs(dffPreview[5] - 1.0f) < 0.001f,
         "runtime DFF GLES preview bridge changed");
  expect(!runtime.copyLatestDffPreview(2, dffPreview, error) && dffPreview.empty(),
         "runtime DFF preview vertex cap must fail closed");

  descriptor = writeTemporaryFile(makeImgWithTxdFixture());
  expect(runtime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount IMG carrying portable TXD");
  ::close(descriptor);
  expect(runtime.loadImgTxdMember("test.txd", error),
         "runtime could not load named IMG TXD member");
  snapshot = runtime.snapshot();
  expect(snapshot.txdDictionaries == 1 && snapshot.textures == 1 && snapshot.texturePixels == 4 &&
         snapshot.dffModels == 0,
         "runtime TXD texture catalog snapshot changed");
  samp::gta::runtime::TexturePreview textDrawSprite;
  expect(runtime.copyTexturePreview("TEST", "TESTTEX", 4, textDrawSprite, error) &&
         textDrawSprite.width == 2 && textDrawSprite.height == 2 &&
         textDrawSprite.rgba.size() == 16 && textDrawSprite.rgba[0] == 255 &&
         textDrawSprite.rgba[1] == 0,
         "font-4 textdraw TXD lookup must normalize bounded dictionary and texture keys");
  expect(!runtime.copyTexturePreview("test", "testtex", 3, textDrawSprite, error) &&
         textDrawSprite.rgba.empty() &&
         !runtime.copyTexturePreview("../test", "testtex", 4, textDrawSprite, error) &&
         textDrawSprite.rgba.empty() &&
         !runtime.copyTexturePreview("test.txd", "testtex", 4, textDrawSprite, error) &&
         textDrawSprite.rgba.empty(),
         "textdraw TXD preview must fail closed on pixel cap and path-like/dotted key");

  descriptor = writeTemporaryFile(makeImgWithDffAndTxdFixture());
  expect(runtime.mountImgFileDescriptor(descriptor, error),
         "runtime could not mount IMG carrying matching portable DFF/TXD");
  ::close(descriptor);
  expect(runtime.loadImgDffMember("test.dff", error) && runtime.loadImgTxdMember("test.txd", error),
         "runtime could not load matching DFF/TXD preview members");
  samp::gta::runtime::TexturedDffPreview texturedPreview;
  expect(runtime.copyLatestTexturedDffPreview(3, 4, texturedPreview, error) &&
         texturedPreview.vertices.size() == 27 && texturedPreview.textureWidth == 2 &&
         texturedPreview.textureHeight == 2 && texturedPreview.textureRgba.size() == 16 &&
         std::fabs(texturedPreview.vertices[0] + 1.0f) < 0.001f &&
         std::fabs(texturedPreview.vertices[1]) < 0.001f &&
         std::fabs(texturedPreview.vertices[2] - 1.0f) < 0.001f &&
         std::fabs(texturedPreview.vertices[7]) < 0.001f &&
         std::fabs(texturedPreview.vertices[8] - 1.0f) < 0.001f &&
         texturedPreview.textureRgba[0] == 255 && texturedPreview.textureRgba[1] == 0,
         "runtime DFF/TXD textured preview bridge changed");
  expect(!runtime.copyLatestTexturedDffPreview(3, 3, texturedPreview, error) &&
         texturedPreview.vertices.empty() && texturedPreview.textureRgba.empty(),
         "runtime textured preview pixel cap must fail closed");
}

void testGtaMapManifestData() {
  {
    const std::string gtaDat =
        "# user-owned GTA SA manifest\n"
        "IDE DATA\\MAPS\\LA\\LAE.IDE\n"
        "IPL data/maps/la/lae.ipl\n"
        "COLFILE 0 MODELS\\COLL\\GENERIC.COL\n"
        "IMG MODELS\\GTA3.IMG\n"
        "SPLASH loadsc0\n";
    samp::gta::map::AssetManifest manifest;
    std::string error;
    expect(samp::gta::map::parseGtaDat(gtaDat, manifest, error) &&
           manifest.idePaths == std::vector<std::string>({"data/maps/la/lae.ide"}) &&
           manifest.iplPaths == std::vector<std::string>({"data/maps/la/lae.ipl"}) &&
           manifest.colPaths == std::vector<std::string>({"models/coll/generic.col"}) &&
           manifest.imgPaths == std::vector<std::string>({"models/gta3.img"}),
           "gta.dat normalized manifest parse changed");
    expect(!samp::gta::map::parseGtaDat("IDE ../outside.ide\n", manifest, error) &&
           manifest.idePaths.empty() && manifest.iplPaths.empty() && manifest.colPaths.empty() &&
           manifest.imgPaths.empty(),
           "gta.dat traversal path must fail closed transactionally");
  }
  {
    const std::string ide =
        "objs\n"
        "1000, TestFloor, Generic, 250.0, 16\n"
        "end\n"
        "cars\n"
        "400, ignored, ignored, 1\n"
        "end\n";
    std::vector<samp::gta::map::IdeObject> objects;
    std::string error;
    expect(samp::gta::map::parseIdeObjects(ide, objects, error) && objects.size() == 2 &&
           objects[0].modelId == 1000 && objects[0].modelName == "testfloor" &&
           objects[0].textureDictionary == "generic" &&
           std::fabs(objects[0].drawDistance - 250.0f) < 0.001f && objects[0].flags == 16 &&
           objects[1].modelId == 400 && objects[1].modelName == "ignored" &&
           objects[1].textureDictionary == "ignored" && objects[1].drawDistance == 0.0f,
           "IDE objs/cars parse changed");
    expect(!samp::gta::map::parseIdeObjects(
               "objs\n1000, one, txd, 1, 0\n1000, two, txd, 1, 0\nend\n", objects, error) &&
           objects.empty(),
           "duplicate IDE model IDs must fail closed transactionally");
    const std::string stockLayouts =
        "objs\n"
        "1001, MultiMesh, Generic, 2, 50.0, 100.0, 17\n"
        "end\n"
        "tobj\n"
        "1002, Timed, Generic, 1, 120.0, 2, 6, 18\n"
        "end\n"
        "anim\n"
        "1003, Animated, Generic, DoorAnim, 75.0, 4\n"
        "end\n"
        "cars\n"
        "400, Landstal, Generic, car, LANDSTAL, landstal, null, executive, 10, 0, 0, 0, 0.65, 0.65, 0\n"
        "585, emperor emperor, car, EMPEROR, null, normal, 10, 0, 0, -1, 0.74, 0.74, 0\n"
        "446, squalo, squalo, boat, SQUALO, SQUALO, null, ignore, 10, 0, 0,\n"
        "end\n"
        "peds\n"
        "0, null, generic, PLAYER1, STAT_PLAYER, player, 0, 0, null, 9, 9, PED_TYPE_PLAYER, VOICE_PLY_CR\n"
        "end\n";
    expect(samp::gta::map::parseIdeObjects(stockLayouts, objects, error) && objects.size() == 7 &&
           objects[0].modelName == "multimesh" && std::fabs(objects[0].drawDistance - 50.0f) < 0.001f &&
           objects[0].flags == 17u && objects[1].modelName == "timed" &&
           std::fabs(objects[1].drawDistance - 120.0f) < 0.001f && objects[2].modelName == "animated" &&
           objects[3].modelId == 400u && objects[3].modelName == "landstal" &&
           objects[3].textureDictionary == "generic" && objects[3].handlingName == "landstal" &&
           objects[3].drawDistance == 0.0f && objects[4].modelId == 585u &&
           objects[4].modelName == "emperor" && objects[4].textureDictionary == "emperor" &&
           objects[4].handlingName == "emperor" && objects[5].modelId == 446u &&
           objects[5].modelName == "squalo" && objects[5].textureDictionary == "squalo" &&
           objects[5].handlingName == "squalo" &&
           objects[6].modelId == 0u && objects[6].modelName == "player" &&
           objects[6].textureDictionary == "generic" && objects[6].drawDistance == 0.0f,
           "stock SA objs/tobj/anim/cars/peds IDE layouts changed");
    expect(!samp::gta::map::parseIdeObjects(
               "cars\n586, first different, car, ignored\nend\n", objects, error) && objects.empty(),
           "only duplicated stock cars DFF/TXD repair tokens may be accepted");
    const std::string stockPunctuation =
        "objs\n"
        "1004, CJ_FIREBIN_(L0), gta_tree+palms, 80.0, 3\n"
        "end\n";
    expect(samp::gta::map::parseIdeObjects(stockPunctuation, objects, error) && objects.size() == 1 &&
           objects[0].modelName == "cj_firebin_(l0)" &&
           objects[0].textureDictionary == "gta_tree+palms",
           "stock GTA punctuation in DFF/TXD names must remain resolvable");
    expect(!samp::gta::map::parseIdeObjects(
               "objs\n1005, ../not-a-model, safe, 1, 0\nend\n", objects, error) &&
           objects.empty(),
           "IDE model identifiers must never become paths");
    expect(samp::gta::map::parseIdeObjects(
               "txdp\nchild_dictionary, parent_dictionary\nend\n", objects, error) && objects.empty(),
           "stock txdp-only IDE files must be a bounded safe no-op");
    samp::gta::map::IdeParseResult parsedTxdParents;
    expect(samp::gta::map::parseIdeFile(
               "txdp\nchild_dictionary, parent_dictionary\nend\n", parsedTxdParents, error) &&
           parsedTxdParents.objects.empty() && parsedTxdParents.txdParents.size() == 1 &&
           parsedTxdParents.txdParents[0].child == "child_dictionary" &&
           parsedTxdParents.txdParents[0].parent == "parent_dictionary",
           "txdp parser must retain normalized child-parent relation");
    expect(!samp::gta::map::parseIdeFile(
               "txdp\nchild, parent_a\nchild, parent_b\nend\n", parsedTxdParents, error) &&
           parsedTxdParents.objects.empty() && parsedTxdParents.txdParents.empty(),
           "conflicting txdp children must fail closed transactionally");
    expect(!samp::gta::map::parseIdeObjects(
               "txdp\nchild_dictionary, ../not-a-dictionary\nend\n", objects, error) && objects.empty(),
           "txdp identifiers must never become paths");
  }
  {
    const std::string handlingCfg =
        "; stock-compatible source row; extension tail remains opaque\n"
        "LANDSTAL 1700.0 4000.0 2.0 0.0 0.1 -0.3 70 0.75 0.85 0.52 5 160.0 25.0 10.0 R P 10.0 0.53 0 30.0 1.0 0.15 0.0 0.28 -0.14 0.5 0.0 0.2 0.5 0.5 20000 0 0 0 0 0\n";
    std::unordered_map<std::string, samp::gta::handling::Profile> profiles;
    std::string error;
    expect(samp::gta::handling::parseHandlingCfg(handlingCfg, profiles, error) &&
           profiles.size() == 1u && profiles.count("landstal") == 1u &&
           std::fabs(profiles.at("landstal").maxVelocityKph - 160.0f) < 0.001f,
           "handling.cfg parser must retain bounded normalized source profile");
    samp::vehiclemotion::VehicleMotionTuning tuning;
    expect(samp::gta::handling::makeMotionTuning(profiles.at("landstal"), tuning) &&
           samp::vehiclemotion::validMotionTuning(tuning) &&
           std::fabs(tuning.maxForwardSpeed - (160.0f / 3.6f)) < 0.01f &&
           std::fabs(tuning.forwardAcceleration - 25.0f) < 0.001f &&
           std::fabs(tuning.maxSteeringDegreesPerSecond - 90.0f) < 0.001f,
           "handling source conversion must stay within local-driver envelope");
    const size_t retainedProfileCount = profiles.size();
    const float retainedMaxVelocity = profiles.at("landstal").maxVelocityKph;
    expect(!samp::gta::handling::parseHandlingCfg(
               "LANDSTAL 1 2 3\n", profiles, error) && profiles.size() == retainedProfileCount &&
               profiles.count("landstal") == 1u &&
               std::fabs(profiles.at("landstal").maxVelocityKph - retainedMaxVelocity) < 0.001f,
           "malformed handling.cfg must fail closed without replacing prior profiles");

  }
  {
    const std::string ipl =
        "inst\n"
        "1000, TestFloor, 0, 10.5, -20.25, 30, 0, 0, 0, 1, -1\n"
        "end\n"
        "zone\n"
        "ignored, section, values\n"
        "end\n";
    std::vector<samp::gta::map::Instance> instances;
    std::string error;
    expect(samp::gta::map::parseIplInstances(ipl, instances, error) && instances.size() == 1 &&
           instances[0].modelId == 1000 && instances[0].modelName == "testfloor" &&
           instances[0].interior == 0 && instances[0].lodIndex == -1 &&
           std::fabs(instances[0].position.x - 10.5f) < 0.001f &&
           std::fabs(instances[0].position.y + 20.25f) < 0.001f &&
           std::fabs(instances[0].rotation.w - 1.0f) < 0.001f,
           "IPL inst parse changed");
    expect(!samp::gta::map::parseIplInstances(
               "inst\n1000, bad, 0, 0, 0, 3, 0, 0, 0, 0, -1\nend\n", instances, error) &&
           instances.empty(),
           "zero-length IPL quaternion must fail closed transactionally");
    expect(samp::gta::map::parseIplInstances(
               "zone\nLosSantos, 1, 0, 0, 0, 1, 1, 1, 0, UNUSED\nend\n", instances, error) &&
           instances.empty(),
           "stock zone-only IPL files must be a bounded safe no-op");
    expect(samp::gta::map::parseIplInstances("inst\nend\nenex\nend\n", instances, error) &&
           instances.empty(),
           "stock IPLs with an empty inst section must remain valid no-ops");
    expect(!samp::gta::map::parseIplInstances("unknown\nvalue, 1\nend\n", instances, error) &&
           instances.empty(),
           "unknown-only IPL files must remain rejected");
  }
  {
    std::vector<samp::gta::map::Instance> instances;
    std::string error;
    const std::vector<uint8_t> binary = makeBinaryIplFixture(1000u, 0x00000305u);
    expect(samp::gta::map::parseBinaryIplInstances(binary, instances, error) && instances.size() == 1 &&
           instances[0].modelId == 1000u && instances[0].modelName.empty() &&
           instances[0].interior == 5 && instances[0].lodIndex == -1 &&
           std::fabs(instances[0].position.x - 10.0f) < 0.001f &&
           std::fabs(instances[0].position.y + 20.0f) < 0.001f &&
           std::fabs(instances[0].rotation.w - 1.0f) < 0.001f,
           "binary IPL INST parse changed");
    auto carsOnly = binary;
    writeLe32At(carsOnly, 4, 0u);
    expect(samp::gta::map::parseBinaryIplInstances(carsOnly, instances, error) && instances.empty(),
           "binary IPL without INST must be a valid empty placement block");
    auto corrupt = binary;
    writeLe32At(corrupt, 28, 75u);
    expect(!samp::gta::map::parseBinaryIplInstances(corrupt, instances, error) && instances.empty(),
           "binary IPL malformed INST offset must fail closed transactionally");
  }
}

void testJsonEscaping() {
  const std::string validUtf8 = "caf\xC3\xA9";
  expect(samp::json_escape(validUtf8) == validUtf8,
         "valid UTF-8 must remain readable through the JSON bridge");
  std::string malformed = "bad";
  malformed.push_back(static_cast<char>(0xC0));
  malformed.push_back(static_cast<char>(0xAF));
  malformed += "text";
  expect(samp::json_escape(malformed) == "bad??text",
         "malformed UTF-8 must not cross the JNI/JSON bridge");
  expect(samp::json_escape("quote\"\\slash\n") == "quote\\\"\\\\slash\\n",
         "JSON escaping changed for ASCII control syntax");
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
  expect(std::fabs(init.gravity - 0.008f) < 0.00001f &&
         std::fabs(init.nameTagDrawDistance - 70.0f) < 0.00001f &&
         init.disableNameTagLos && init.showNameTags && !init.manualVehicleEngineAndLights,
         "PlayerInit presentation policy wrong");
  expect(samp::rpc::decodePlayerInit(makePlayerInit(true), init) &&
         init.manualVehicleEngineAndLights,
         "PlayerInit ManualVehicleEngineAndLights bit was not retained");
  expect(init.onFootSyncRateMs == 30 && init.inCarSyncRateMs == 30 &&
         init.weaponSyncRateMs == 30 && init.syncMultiplier == 1 && init.lagCompensation == 1,
         "PlayerInit sync rates were not retained");
  expect(samp::rpc::summarize(payload).find("onfoot=30ms") != std::string::npos,
         "PlayerInit on-foot rate missing from summary");
  expect(samp::rpc::summarize(payload).find("PlayerInit") != std::string::npos,
         "PlayerInit summary has no RPC name");
  expect(samp::rpc::summarize({20u, 40u}).find("PlayerClose") != std::string::npos &&
             samp::rpc::summarize({20u, 130u, 2u}).find("ConnectionRejected") != std::string::npos,
         "terminal RPC summary has no RPC name");
}

void testClientRpcBuilders() {
  expect(samp::clientrpc::buildRequestClass(3) == std::vector<uint8_t>({20, 128, 3, 0}),
         "request class payload changed");
  expect(samp::clientrpc::buildRequestSpawn() == std::vector<uint8_t>({20, 129}),
         "request spawn payload changed");
  expect(samp::clientrpc::buildPlayerSpawn() == std::vector<uint8_t>({20, 52}),
         "player spawn confirmation payload changed");
  expect(samp::clientrpc::buildPickedUpPickup(0x1234567) ==
             std::vector<uint8_t>({20, 131, 0x67, 0x45, 0x23, 0x01}) &&
         samp::clientrpc::buildPickedUpPickup(-1).empty(),
         "pickup notification payload/bounds changed");
  expect(samp::clientrpc::buildEnterVehicle(0x1234, false) ==
             std::vector<uint8_t>({20, 26, 0x34, 0x12, 0}) &&
         samp::clientrpc::buildEnterVehicle(0x1234, true) ==
             std::vector<uint8_t>({20, 26, 0x34, 0x12, 1}) &&
         samp::clientrpc::buildExitVehicle(0x1234) ==
             std::vector<uint8_t>({20, 154, 0x34, 0x12}),
         "vehicle interaction payload changed");
  expect(samp::clientrpc::buildSelectTextDraw(0x0801) ==
             std::vector<uint8_t>({20, 83, 0x01, 0x08}),
         "textdraw select payload changed");
  expect(samp::clientrpc::buildSelectObject(0x1234, 19379u, 1.0f, -2.0f, 3.5f) ==
             std::vector<uint8_t>({20, 27, 0, 0, 0, 0, 0x34, 0x12, 0xB3, 0x4B, 0, 0,
                                   0, 0, 0x80, 0x3F, 0, 0, 0, 0xC0, 0, 0, 0x60, 0x40}) &&
         samp::clientrpc::buildSelectObject(0, 19379u, 1.0f, -2.0f, 3.5f).empty() &&
         samp::clientrpc::buildSelectObject(0xFFFFu, 19379u, 1.0f, -2.0f, 3.5f).empty() &&
         samp::clientrpc::buildSelectObject(1, 0, 1.0f, -2.0f, 3.5f).empty() &&
         samp::clientrpc::buildSelectObject(1, 19379u, std::numeric_limits<float>::infinity(),
                                            0.0f, 0.0f).empty(),
         "object select payload/bounds changed");
  const auto objectEdit = samp::clientrpc::buildObjectEditResponse(
      true, 77u, 2u, 1.0f, -2.0f, 3.5f, 90.0f, 0.0f, -45.0f);
  samp::raknet::BitReader objectEditReader(objectEdit.data(), objectEdit.size());
  const auto readBuilderFloat = [&objectEditReader]() {
    const uint32_t raw = objectEditReader.readBits(32);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
  };
  expect(objectEdit.size() == 33u && objectEditReader.readBits(8) == 20u &&
         objectEditReader.readBits(8) == 117u && objectEditReader.readBit() &&
         objectEditReader.readBits(16) == 77u && objectEditReader.readBits(32) == 2u &&
         std::fabs(readBuilderFloat() - 1.0f) < 0.001f &&
         std::fabs(readBuilderFloat() + 2.0f) < 0.001f &&
         std::fabs(readBuilderFloat() - 3.5f) < 0.001f &&
         std::fabs(readBuilderFloat() - 90.0f) < 0.001f &&
         std::fabs(readBuilderFloat()) < 0.001f &&
         std::fabs(readBuilderFloat() + 45.0f) < 0.001f && objectEditReader.ok() &&
         samp::clientrpc::buildObjectEditResponse(false, 0u, 2u, 0.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f).empty() &&
         samp::clientrpc::buildObjectEditResponse(false, 0xFFFFu, 2u, 0.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f).empty() &&
         samp::clientrpc::buildObjectEditResponse(false, 1u, 3u, 0.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f).empty() &&
         samp::clientrpc::buildObjectEditResponse(false, 1u, 2u,
                                                   std::numeric_limits<float>::infinity(), 0.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f).empty(),
         "object edit payload/bounds changed");
  const auto attachedEdit = samp::clientrpc::buildAttachedObjectEditResponse(
      1u, 3u, 18637u, 2u, 1.0f, -2.0f, 3.5f, 90.0f, 0.0f, -45.0f,
      1.25f, -0.75f, 2.0f, 0x11443322u, 0x55667788u);
  samp::raknet::BitReader attachedEditReader(attachedEdit.data(), attachedEdit.size());
  const auto readAttachedBuilderFloat = [&attachedEditReader]() {
    const uint32_t raw = attachedEditReader.readBits(32);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
  };
  expect(attachedEdit.size() == 62u && attachedEditReader.readBits(8) == 20u &&
         attachedEditReader.readBits(8) == 116u && attachedEditReader.readBits(32) == 1u &&
         attachedEditReader.readBits(32) == 3u && attachedEditReader.readBits(32) == 18637u &&
         attachedEditReader.readBits(32) == 2u &&
         std::fabs(readAttachedBuilderFloat() - 1.0f) < 0.001f &&
         std::fabs(readAttachedBuilderFloat() + 2.0f) < 0.001f &&
         std::fabs(readAttachedBuilderFloat() - 3.5f) < 0.001f &&
         std::fabs(readAttachedBuilderFloat() - 90.0f) < 0.001f &&
         std::fabs(readAttachedBuilderFloat()) < 0.001f &&
         std::fabs(readAttachedBuilderFloat() + 45.0f) < 0.001f &&
         std::fabs(readAttachedBuilderFloat() - 1.25f) < 0.001f &&
         std::fabs(readAttachedBuilderFloat() + 0.75f) < 0.001f &&
         std::fabs(readAttachedBuilderFloat() - 2.0f) < 0.001f &&
         attachedEditReader.readBits(32) == 0x11443322u &&
         attachedEditReader.readBits(32) == 0x55667788u && attachedEditReader.ok() &&
         samp::clientrpc::buildAttachedObjectEditResponse(2u, 3u, 18637u, 2u,
                                                            0.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f,
                                                            1.0f, 1.0f, 1.0f, 0u, 0u).empty() &&
         samp::clientrpc::buildAttachedObjectEditResponse(1u, 10u, 18637u, 2u,
                                                            0.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f,
                                                            1.0f, 1.0f, 1.0f, 0u, 0u).empty() &&
         samp::clientrpc::buildAttachedObjectEditResponse(1u, 3u, 0u, 2u,
                                                            0.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f,
                                                            1.0f, 1.0f, 1.0f, 0u, 0u).empty() &&
         samp::clientrpc::buildAttachedObjectEditResponse(1u, 3u, 18637u, 19u,
                                                            0.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f,
                                                            1.0f, 1.0f, 1.0f, 0u, 0u).empty() &&
         samp::clientrpc::buildAttachedObjectEditResponse(1u, 3u, 18637u, 2u,
                                                            0.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f,
                                                            0.0f, 1.0f, 1.0f, 0u, 0u).empty(),
         "attached object edit payload/bounds changed");
  expect(samp::clientrpc::buildRequestScoresAndPings() == std::vector<uint8_t>({20, 155}),
         "scoreboard request payload changed");
  expect(samp::clientrpc::buildChatMessage("halo") ==
             std::vector<uint8_t>({20, 101, 4, 'h', 'a', 'l', 'o'}),
         "chat request payload changed");
  expect(samp::clientrpc::buildCommandMessage("/login 123") ==
             std::vector<uint8_t>({20, 50, 10, 0, 0, 0, '/', 'l', 'o', 'g', 'i', 'n', ' ', '1', '2', '3'}),
         "command request payload changed");
  expect(samp::clientrpc::buildDialogResponse(7, true, 2, "abc") ==
             std::vector<uint8_t>({20, 62, 7, 0, 1, 2, 0, 3, 'a', 'b', 'c'}),
         "dialog response payload changed");
  expect(samp::clientrpc::buildDialogResponse(-1, false, -1, "") ==
             std::vector<uint8_t>({20, 62, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0}),
         "signed dialog response payload changed");
  expect(samp::clientrpc::buildDialogResponse(7, true, 2, "a\tb") ==
             std::vector<uint8_t>({20, 62, 7, 0, 1, 2, 0, 3, 'a', '\t', 'b'}),
         "tab-separated dialog row payload changed");
  expect(samp::clientrpc::buildMenuSelect(3) == std::vector<uint8_t>({20, 132, 3}) &&
         samp::clientrpc::buildMenuExit() == std::vector<uint8_t>({20, 140}),
         "legacy menu response payload changed");
  expect(samp::clientrpc::buildChatMessage("").empty() &&
         samp::clientrpc::buildChatMessage("line\nbreak").empty() &&
         samp::clientrpc::buildCommandMessage("/").empty() &&
         samp::clientrpc::buildCommandMessage("hello").empty() &&
         samp::clientrpc::buildDialogResponse(7, true, -1, "line\nbreak").empty(),
         "chat/command/dialog builder bounds changed");
}

void testObjectSelectionReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;
  expect(world.applyRpc(makePlayerInit(), event), "object selection PlayerInit rejected");
  expect(world.applyRpc(makeCreateObject(77, 19379u, 11.0f, -7.0f, 4.5f,
                                         0.0f, 0.0f, 90.0f, 80.0f), event) &&
         event.kind == samp::state::RpcEventKind::ObjectCreated,
         "object selection fixture CreateObject rejected");
  samp::state::ServerObjectView selected;
  expect(!world.objectSelectionActive() && !world.selectedObjectView(77, selected),
         "object selection must stay gated before server RPC 27");
  expect(!world.applyRpc({20, 27, 0}, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.objectSelectionActive(),
         "PlayerBeginObjectSelect trailing data must fail closed");
  expect(world.applyRpc(makePlayerBeginObjectSelect(), event) &&
         event.kind == samp::state::RpcEventKind::ObjectSelectionChanged && world.objectSelectionActive() &&
         world.objectSelectionRevision() != 0u &&
         world.selectedObjectView(77, selected) && selected.id == 77 && selected.model == 19379u &&
         std::fabs(selected.x - 11.0f) < 0.001f && std::fabs(selected.y + 7.0f) < 0.001f &&
         std::fabs(selected.z - 4.5f) < 0.001f && !world.selectedObjectView(0, selected) &&
         !world.selectedObjectView(78, selected),
         "PlayerBeginObjectSelect state/native object revalidation changed");
  const uint64_t firstRevision = world.objectSelectionRevision();
  expect(world.applyRpc(makePlayerBeginObjectSelect(), event) &&
         world.objectSelectionActive() && world.objectSelectionRevision() != firstRevision,
         "repeated PlayerBeginObjectSelect must advance stale-tap revision");
  const uint64_t secondRevision = world.objectSelectionRevision();
  expect(world.applyRpc(makePlayerCancelObjectEdit(), event) &&
         event.kind == samp::state::RpcEventKind::ObjectSelectionChanged && !world.objectSelectionActive() &&
         world.objectSelectionRevision() != secondRevision && !world.selectedObjectView(77, selected),
         "PlayerCancelObjectEdit must close Android selection gate and invalidate list taps");
}

void testObjectEditReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;
  expect(world.applyRpc(makePlayerInit(), event), "object edit PlayerInit rejected");
  expect(world.applyRpc(makeCreateObject(77, 19379u, 11.0f, -7.0f, 4.5f,
                                         0.0f, 0.0f, 90.0f, 80.0f), event),
         "object edit CreateObject rejected");
  expect(!world.applyRpc({20, 117, 0}, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.objectEdit().active,
         "short PlayerBeginObjectEdit must fail closed");
  expect(!world.applyRpc(makePlayerBeginObjectEdit(true, 0), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.objectEdit().active,
         "zero PlayerBeginObjectEdit ID must fail closed");
  expect(world.applyRpc(makePlayerBeginObjectEdit(true, 77), event) &&
         event.kind == samp::state::RpcEventKind::ObjectEditChanged && world.objectEdit().active &&
         world.objectEdit().playerObject && world.objectEdit().objectId == 77 &&
         world.objectEdit().revision != 0u,
         "PlayerBeginObjectEdit state changed");
  samp::state::ServerObjectView edited;
  expect(world.editedObjectView(77, true, edited) && edited.id == 77 &&
         std::fabs(edited.rotationZ - 90.0f) < 0.001f &&
         !world.editedObjectView(77, false, edited),
         "object edit tuple/current-object revalidation changed");
  const uint64_t firstRevision = world.objectEdit().revision;
  expect(world.applyRpc(makePlayerBeginObjectEdit(false, 77), event) &&
         world.objectEdit().active && !world.objectEdit().playerObject &&
         world.objectEdit().revision != firstRevision,
         "new PlayerBeginObjectEdit must replace kind and revision");
  expect(world.applyRpc(makePlayerCancelObjectEdit(), event) &&
         event.kind == samp::state::RpcEventKind::ObjectEditChanged && !world.objectEdit().active &&
         !world.editedObjectView(77, false, edited),
         "PlayerCancelObjectEdit must close Android edit gate");
  expect(world.applyRpc(makePlayerBeginObjectEdit(false, 77), event) &&
         world.objectEdit().active && world.applyRpc(makeDestroyObject(77), event) &&
         event.kind == samp::state::RpcEventKind::ObjectDestroyed && !world.objectEdit().active,
         "DestroyObject must invalidate an active edit target");
}

void testAttachedObjectEditReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;
  expect(world.applyRpc(makePlayerInit(), event), "attachment edit PlayerInit rejected");
  expect(world.applyRpc(makeSetPlayerAttachedObject(42, 3, true, 18637u, 2u,
                                                    1.0f, -2.0f, 3.5f,
                                                    90.0f, 0.0f, -45.0f,
                                                    1.25f, -0.75f, 2.0f,
                                                    0x11223344u, 0x55667788u), event),
         "local SetPlayerAttachedObject for edit rejected");
  expect(!world.applyRpc({20, 116}, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.attachedObjectEdit().active,
         "short PlayerBeginAttachedObjectEdit must fail closed");
  expect(!world.applyRpc(makePlayerBeginAttachedObjectEdit(10), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.attachedObjectEdit().active,
         "out-of-range PlayerBeginAttachedObjectEdit slot must fail closed");
  expect(world.applyRpc(makePlayerBeginAttachedObjectEdit(3), event) &&
         event.kind == samp::state::RpcEventKind::AttachedObjectEditChanged &&
         world.attachedObjectEdit().active && world.attachedObjectEdit().slot == 3 &&
         world.attachedObjectEdit().revision != 0u,
         "PlayerBeginAttachedObjectEdit state changed");
  samp::state::PlayerAttachedObject attachment;
  expect(world.editedAttachedObject(3, attachment) && attachment.active &&
         attachment.model == 18637u && attachment.bone == 2u &&
         std::fabs(attachment.offsetY + 2.0f) < 0.001f &&
         std::fabs(attachment.scaleY + 0.75f) < 0.001f &&
         attachment.colour1Abgr == 0x11223344u &&
         !world.editedAttachedObject(4, attachment),
         "active attachment edit must re-resolve only server slot");
  const uint64_t firstRevision = world.attachedObjectEdit().revision;
  expect(world.applyRpc(makePlayerBeginAttachedObjectEdit(3), event) &&
         world.attachedObjectEdit().revision != firstRevision,
         "repeated PlayerBeginAttachedObjectEdit must advance revision");
  expect(world.applyRpc(makePlayerCancelObjectEdit(), event) &&
         event.kind == samp::state::RpcEventKind::AttachedObjectEditChanged &&
         !world.attachedObjectEdit().active && !world.editedAttachedObject(3, attachment),
         "PlayerCancelObjectEdit must close attachment edit gate");
  expect(world.applyRpc(makePlayerBeginAttachedObjectEdit(3), event) &&
         world.attachedObjectEdit().active &&
         world.applyRpc(makeSetPlayerAttachedObject(42, 3, false), event) &&
         !world.attachedObjectEdit().active,
         "RemovePlayerAttachedObject must invalidate an active local attachment edit");
}

void testSessionSafetyWithoutSocket() {
  const auto status = samp::raknet_session_status();
  expect(!status.active && !status.error.empty(), "idle session status must be safe");
  const auto world = samp::raknet_session_world_snapshot();
  expect(!world.active && !world.error.empty() && world.actors.empty() && world.chat.empty(),
         "idle world snapshot must be safe and bounded");
  const auto classRequest = samp::raknet_session_request_class(0);
  expect(!classRequest.active && !classRequest.error.empty(),
         "class request without session must be rejected");
  const auto spawnRequest = samp::raknet_session_request_spawn();
  expect(!spawnRequest.active && !spawnRequest.error.empty(),
         "spawn request without session must be rejected");
  const auto controls = samp::raknet_session_update_controls(samp::input::TouchIntent{});
  expect(!controls.active && !controls.error.empty(),
         "touch input without session must be rejected");
  const auto chat = samp::raknet_session_send_chat("halo");
  expect(!chat.active && !chat.error.empty(),
         "chat without session must be rejected");
  const auto dialogResponse = samp::raknet_session_respond_dialog(1, 1, true, -1, "");
  expect(!dialogResponse.active && !dialogResponse.error.empty(),
         "dialog response without session must be rejected");
  const auto menuSelection = samp::raknet_session_select_menu(1, 1, 0);
  const auto menuExit = samp::raknet_session_exit_menu(1, 1);
  expect(!menuSelection.active && !menuSelection.error.empty() &&
         !menuExit.active && !menuExit.error.empty(),
         "menu responses without session must be rejected");
  const auto nearbyUse = samp::raknet_session_use_nearby();
  const auto nearbyPassengerUse = samp::raknet_session_use_nearby(true);
  expect(!nearbyUse.active && !nearbyUse.error.empty() &&
         !nearbyPassengerUse.active && !nearbyPassengerUse.error.empty(),
         "nearby driver/passenger USE without session must be rejected");
  const auto scoreRequest = samp::raknet_session_request_scores();
  expect(!scoreRequest.active && !scoreRequest.error.empty(),
         "scoreboard request without session must be rejected");
  const auto textDrawSelect = samp::raknet_session_select_textdraw(1);
  expect(!textDrawSelect.active && !textDrawSelect.error.empty(),
         "textdraw selection without session must be rejected");
  const auto objectSelect = samp::raknet_session_select_object(1, 1);
  expect(!objectSelect.active && !objectSelect.error.empty(),
         "object selection without session must be rejected");
  const auto objectEdit = samp::raknet_session_edit_object(2u, 1u, 0.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f);
  expect(!objectEdit.active && !objectEdit.error.empty(),
         "object edit without session must be rejected");
  const auto attachedObjectEdit = samp::raknet_session_edit_attached_object(
      1u, 1u, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
  expect(!attachedObjectEdit.active && !attachedObjectEdit.error.empty(),
         "attached object edit without session must be rejected");
  const auto onFoot = samp::raknet_session_send_onfoot(samp::sync::OnFootState{});
  expect(!onFoot.active && !onFoot.error.empty(),
         "on-foot send without session must be rejected");
  samp::sync::InCarState inCar;
  inCar.vehicleId = 1;
  const auto vehicleSync = samp::raknet_session_send_incar(inCar);
  expect(!vehicleSync.active && !vehicleSync.error.empty(),
         "in-car send without session must be rejected");
  const auto closed = samp::raknet_session_disconnect();
  expect(!closed.active && !closed.stateSummary.empty(), "idle session close must be safe");
}

// This loopback peer exercises the real retained-session hand-off rather than
// only a payload builder: it grants the legacy transport/auth sequence, sends a
// minimal valid PlayerInit, and verifies the native client immediately sends the
// first class request on SyncRPC channel 2/index 1. No Internet or gameplay
// action is involved.
std::vector<uint8_t> unwrapClientToServerForLoopback(const uint8_t* wire, size_t size,
                                                      uint16_t serverPort) {
  if (size < 2u) return {};
  static const uint8_t kCrypto[256] = {
#include "samp_crypto_table.inc"
  };
  const uint8_t portMask = static_cast<uint8_t>((serverPort ^ 0xCCu) & 0xFFu);
  std::vector<uint8_t> plain(size - 1u);
  uint8_t checksum = 0;
  for (size_t i = 0; i < plain.size(); ++i) {
    uint8_t value = wire[i + 1u];
    if (((i + 1u) & 1u) == 0u) value ^= portMask;
    plain[i] = kCrypto[value];
    checksum ^= static_cast<uint8_t>(plain[i] & 0xAAu);
  }
  return checksum == wire[0] ? plain : std::vector<uint8_t>{};
}

// A legacy datagram can contain several internal frames after its one leading
// hasAcks bit. Batch the test stream so the loopback uses ordinary bounded UDP
// datagrams rather than relying on a host socket's burst-buffer size.
void appendLoopbackReliableOrderedFrame(samp::raknet::BitWriter& writer,
                                        uint16_t messageNumber,
                                        uint8_t orderingChannel,
                                        uint16_t orderingIndex,
                                        const std::vector<uint8_t>& payload) {
  writer.writeBits(messageNumber, 16);
  writer.writeBits(samp::raknet::kReliabilityReliableOrdered, 4);
  writer.writeBits(orderingChannel & 0x1Fu, 5);
  writer.writeBits(orderingIndex, 16);
  writer.writeBit(false);  // isSplit
  writer.writeCompressedU16(static_cast<uint16_t>(payload.size() * 8u));
  writer.writeAlignedBytes(payload.data(), payload.size());
}

// A minimal raw-open peer used to lock the Android-facing error path and the
// connection-wide deadline. It intentionally does not need to decode the
// client's obfuscated OPEN request: once the loopback source endpoint is
// observed, legacy server replies are ordinary plain RakNet bytes.
struct LoopbackOpenReplyPeer {
  std::atomic<bool> ready{false};
  uint16_t port = 0;
  std::vector<uint8_t> reply;
  std::chrono::milliseconds keepOpenFor{0};
  std::string error;
  std::thread worker;

  void start() { worker = std::thread([this] { run(); }); }
  void join() {
    if (worker.joinable()) worker.join();
  }

  void run() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      error = "open-reply socket";
      ready = true;
      return;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
      error = "open-reply bind";
      ::close(fd);
      ready = true;
      return;
    }
    socklen_t localLength = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLength) != 0) {
      error = "open-reply getsockname";
      ::close(fd);
      ready = true;
      return;
    }
    port = ntohs(local.sin_port);
    ready = true;

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeval wait{1, 0};
    if (::select(fd + 1, &readable, nullptr, nullptr, &wait) <= 0) {
      error = "open-reply receive timeout";
      ::close(fd);
      return;
    }
    uint8_t wire[256]{};
    sockaddr_in peer{};
    socklen_t peerLength = sizeof(peer);
    const ssize_t received = ::recvfrom(fd, wire, sizeof(wire), 0,
                                        reinterpret_cast<sockaddr*>(&peer), &peerLength);
    if (received <= 0 || reply.empty()) {
      error = "open-reply receive";
      ::close(fd);
      return;
    }
    if (::sendto(fd, reply.data(), reply.size(), 0,
                 reinterpret_cast<const sockaddr*>(&peer), peerLength) !=
        static_cast<ssize_t>(reply.size())) {
      error = "open-reply send";
      ::close(fd);
      return;
    }
    if (keepOpenFor.count() > 0) std::this_thread::sleep_for(keepOpenFor);
    ::close(fd);
  }
};

void testBoundedConnectErrors() {
  {
    LoopbackOpenReplyPeer peer;
    peer.reply = {31u};  // ID_NO_FREE_INCOMING_CONNECTIONS before cookie/auth.
    peer.start();
    for (int attempt = 0; attempt < 200 && !peer.ready; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(peer.ready && peer.port != 0u && peer.error.empty(),
           "open-refusal peer did not initialize");
    const samp::SessionSnapshot refused =
        samp::raknet_session_connect("127.0.0.1", peer.port, 100, "LoopbackRefused");
    peer.join();
    const samp::SessionSnapshot closed = samp::raknet_session_disconnect();
    expect(!refused.active && refused.error == "server penuh sebelum open connection" &&
               peer.error.empty() && !closed.active,
           "pre-cookie server-full reply must remain actionable in Android session status");
  }

  {
    LoopbackOpenReplyPeer peer;
    peer.reply = {99u, 0xAAu};  // Unknown raw-open reply must stay diagnosable.
    peer.start();
    for (int attempt = 0; attempt < 200 && !peer.ready; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(peer.ready && peer.port != 0u && peer.error.empty(),
           "open-unknown peer did not initialize");
    const samp::SessionSnapshot unknown =
        samp::raknet_session_connect("127.0.0.1", peer.port, 100, "LoopbackUnknown");
    peer.join();
    const samp::SessionSnapshot closed = samp::raknet_session_disconnect();
    expect(!unknown.active && unknown.error.find("id 99, 2 byte") != std::string::npos &&
               peer.error.empty() && !closed.active,
           "unknown pre-cookie reply must preserve RakNet ID and byte count for diagnosis");
  }

  {
    LoopbackOpenReplyPeer peer;
    peer.reply = {26u, 0x34u, 0x12u};  // Valid cookie, then deliberately silent.
    peer.keepOpenFor = std::chrono::milliseconds(1400);
    peer.start();
    for (int attempt = 0; attempt < 200 && !peer.ready; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(peer.ready && peer.port != 0u && peer.error.empty(),
           "cookie-stall peer did not initialize");
    const auto began = std::chrono::steady_clock::now();
    const samp::SessionSnapshot stalled =
        samp::raknet_session_connect("127.0.0.1", peer.port, 100, "LoopbackDeadline");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began).count();
    peer.join();
    const samp::SessionSnapshot closed = samp::raknet_session_disconnect();
    expect(!stalled.active && stalled.error.find("batas waktu handshake native habis") !=
               std::string::npos &&
               elapsed >= 600 && elapsed < 3000 && peer.error.empty() && !closed.active,
           "connect-wide deadline must stop a silent cookie path without a long UI wait");
  }
}

struct LoopbackClassBootstrapPeer {
  std::atomic<bool> ready{false};
  // Test-only loss switch: the peer ignores the first source-coalesced burst
  // so the client must recover each tracked reliable frame independently.
  bool dropInitialSourceBootstrap = false;
  std::atomic<bool> droppedSourceBootstrap{false};
  std::atomic<unsigned> bootstrapRetryMask{0u};
  std::atomic<bool> sawSourceBootstrapDatagram{false};
  std::atomic<bool> sawClientJoin{false};
  std::atomic<bool> sawPostAcceptPong{false};
  std::atomic<bool> sawInitialClass{false};
  std::atomic<unsigned> initialClassRequestMessages{0u};
  // Optional in-order, bodyless server RPCs placed before PlayerInit. They
  // exercise the synchronous-to-retained hand-off when a legacy server emits
  // a substantial ordered bootstrap stream before RPC 139.
  size_t prePlayerInitOrderedFrames = 0u;
  bool duplicatePlayerInit = false;
  size_t sentPrePlayerInitOrderedFrames = 0u;
  uint16_t port = 0;
  std::string error;
  std::thread worker;

  void start() { worker = std::thread([this] { run(); }); }
  void join() {
    if (worker.joinable()) worker.join();
  }

  void run() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      error = "loopback socket";
      ready = true;
      return;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
      error = "loopback bind";
      ::close(fd);
      ready = true;
      return;
    }
    socklen_t localLength = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLength) != 0) {
      error = "loopback getsockname";
      ::close(fd);
      ready = true;
      return;
    }
    port = ntohs(local.sin_port);
    ready = true;

    sockaddr_in peer{};
    socklen_t peerLength = sizeof(peer);
    bool sentAuth = false;
    bool sentAccepted = false;
    bool sentPostAcceptPing = false;
    bool sentPlayerInit = false;
    uint16_t initialClassMessageNumber = 0u;
    bool initialClassMessageKnown = false;
    std::chrono::steady_clock::time_point firstInitialClassAt{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    auto sendServer = [&](const std::vector<uint8_t>& plain) {
      return ::sendto(fd, plain.data(), plain.size(), 0,
                      reinterpret_cast<const sockaddr*>(&peer), peerLength) ==
             static_cast<ssize_t>(plain.size());
    };

    // Keep receiving briefly after the first class request: a retransmitted
    // packet with the same message number is normal, while a second fresh
    // RequestClass would expose an accidental duplicate PlayerInit bootstrap.
    while (std::chrono::steady_clock::now() < deadline &&
           (!sawInitialClass ||
            std::chrono::steady_clock::now() - firstInitialClassAt < std::chrono::milliseconds(120))) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(fd, &readable);
      timeval wait{0, 20 * 1000};
      if (::select(fd + 1, &readable, nullptr, nullptr, &wait) <= 0) continue;

      uint8_t wire[2048]{};
      const ssize_t received = ::recvfrom(fd, wire, sizeof(wire), 0,
                                          reinterpret_cast<sockaddr*>(&peer), &peerLength);
      if (received <= 0) continue;
      const std::vector<uint8_t> plain = unwrapClientToServerForLoopback(
          wire, static_cast<size_t>(received), port);
      if (plain.empty()) {
        error = "loopback client transform";
        break;
      }
      if (plain[0] == 24u) {  // OPEN_CONNECTION_REQUEST
        (void)sendServer({25u, 0u});
        continue;
      }
      // ACK-only frames have the first legacy bit set and no application body.
      if ((plain[0] & 0x80u) != 0u) continue;

      const samp::raknet::Datagram datagram = samp::raknet::decodeDatagram(plain);
      if (!datagram.ok) {
        error = "loopback datagram decode";
        break;
      }
      // The source peer's accepted path puts its three controls and ClientJoin
      // in one normal no-ACK datagram. Keep this end-to-end assertion here so
      // a future direct-send refactor cannot silently undo the byte fixture.
      bool sourceBootstrapDatagram = false;
      if (datagram.packets.size() == 4u) {
        const auto& first = datagram.packets[0];
        const auto& second = datagram.packets[1];
        const auto& third = datagram.packets[2];
        const auto& fourth = datagram.packets[3];
        sourceBootstrapDatagram =
            first.messageNumber == 2u && first.reliability == samp::raknet::kReliabilityReliable &&
            first.payload.size() == 7u && first.payload[0] == 30u &&
            second.messageNumber == 3u && second.reliability == samp::raknet::kReliabilityUnreliable &&
            second.payload.size() == 5u && second.payload[0] == 6u &&
            third.messageNumber == 4u && third.reliability == samp::raknet::kReliabilityReliable &&
            third.payload == std::vector<uint8_t>({41u}) &&
            fourth.messageNumber == 5u && fourth.reliability == samp::raknet::kReliabilityReliableOrdered &&
            fourth.orderingChannel == samp::clientrpc::kReliableOrderedChannel &&
            fourth.orderingIndex == 0u && fourth.payload.size() >= 2u &&
            fourth.payload[0] == 20u && fourth.payload[1] == 25u;
      }
      if (sourceBootstrapDatagram) {
        sawSourceBootstrapDatagram = true;
        // Suppress transport ACKs and semantic handling only once. The retry
        // path must then retain msg 2/4/5 individually; unreliable ping msg 3
        // is intentionally not resent.
        if (dropInitialSourceBootstrap && !droppedSourceBootstrap.exchange(true)) continue;
      }
      if (droppedSourceBootstrap && datagram.packets.size() == 1u) {
        const auto& retried = datagram.packets.front();
        if (retried.messageNumber == 2u &&
            retried.reliability == samp::raknet::kReliabilityReliable &&
            retried.payload.size() == 7u && retried.payload[0] == 30u) {
          bootstrapRetryMask.fetch_or(1u);
        } else if (retried.messageNumber == 4u &&
                   retried.reliability == samp::raknet::kReliabilityReliable &&
                   retried.payload == std::vector<uint8_t>({41u})) {
          bootstrapRetryMask.fetch_or(2u);
        } else if (retried.messageNumber == 5u &&
                   retried.reliability == samp::raknet::kReliabilityReliableOrdered &&
                   retried.orderingChannel == samp::clientrpc::kReliableOrderedChannel &&
                   retried.orderingIndex == 0u && retried.payload.size() >= 2u &&
                   retried.payload[0] == 20u && retried.payload[1] == 25u) {
          bootstrapRetryMask.fetch_or(4u);
        }
      }
      for (const samp::raknet::IncomingPacket& packet : datagram.packets) {
        const bool reliable = packet.reliability == samp::raknet::kReliabilityReliable ||
                              packet.reliability == samp::raknet::kReliabilityReliableOrdered ||
                              packet.reliability == samp::raknet::kReliabilityReliableSequenced;
        if (reliable) (void)sendServer(samp::raknet::buildAck({packet.messageNumber}));
        if (packet.payload.empty()) continue;

        const uint8_t id = packet.payload[0];
        if (id == 11u && !sentAuth) {  // CONNECTION_REQUEST
          const std::string challenge = "6C407EC29DE59E2";
          std::vector<uint8_t> auth = {12u, static_cast<uint8_t>(challenge.size() + 1u)};
          auth.insert(auth.end(), challenge.begin(), challenge.end());
          auth.push_back(0u);
          (void)sendServer(samp::raknet::buildReliable(0u, auth));
          sentAuth = true;
        } else if (id == 12u && sentAuth && !sentAccepted) {  // AUTH_KEY response
          const std::vector<uint8_t> accepted = {
              34u, 127u, 0u, 0u, 1u, 0u, 0u, 0u, 0u,
              0x78u, 0x56u, 0x34u, 0x12u,
          };
          (void)sendServer(samp::raknet::buildReliable(1u, accepted));
          sentAccepted = true;
        } else if (id == 30u && sentAccepted) {
          // RakPeer receives NEW_INCOMING before the peer's game thread sends
          // PlayerConnect. Deliberately withhold the server ping here: a source
          // client emits local NEW_INCOMING/ping/static as message 2/3/4 and
          // ClientJoin as message 5 without waiting for inbound metadata.
        } else if (id == 20u && packet.payload.size() >= 2u && packet.payload[1] == 25u) {
          if (packet.reliability != samp::raknet::kReliabilityReliableOrdered ||
              packet.orderingChannel != samp::clientrpc::kReliableOrderedChannel ||
              packet.orderingIndex != 0u) {
            error = "ClientJoin must use SyncRPC channel 2/index 0";
            break;
          }
          if (packet.messageNumber != 5u) {
            error = "ClientJoin must preserve RakPeer post-accept message number 5";
            break;
          }
          sawClientJoin = true;
          if (!sentPostAcceptPing) {
            // The normal server control still arrives and must be answered, but
            // only after ClientJoin has kept the source's counter progression.
            (void)sendServer(samp::raknet::buildUnreliable(2u, {6u, 1u, 2u, 3u, 4u}));
            sentPostAcceptPing = true;
          }
          if (!sentPlayerInit) {
            uint16_t serverMessageNumber = 3u;
            uint16_t serverOrderingIndex = 0u;
            // RPC 28 is a source-backed bodyless, idempotent server record.
            // It is deliberately chosen here so the large ordered stream is
            // valid but cannot create an unbounded world fixture before Init.
            constexpr size_t kFramesPerDatagram = 8u;
            while (sentPrePlayerInitOrderedFrames < prePlayerInitOrderedFrames) {
              const size_t remaining = prePlayerInitOrderedFrames - sentPrePlayerInitOrderedFrames;
              const size_t batchSize = std::min(kFramesPerDatagram, remaining);
              samp::raknet::BitWriter batch;
              batch.writeBit(false);  // hasAcks
              for (size_t frame = 0; frame < batchSize; ++frame) {
                appendLoopbackReliableOrderedFrame(
                    batch, serverMessageNumber++, samp::clientrpc::kReliableOrderedChannel,
                    serverOrderingIndex++, makeEmptyRpc(28u));
              }
              if (!sendServer(batch.buffer)) {
                error = "loopback pre-PlayerInit ordered send";
                break;
              }
              sentPrePlayerInitOrderedFrames += batchSize;
            }
            if (!error.empty()) break;
            samp::raknet::BitWriter playerInitBatch;
            playerInitBatch.writeBit(false);  // hasAcks
            appendLoopbackReliableOrderedFrame(
                playerInitBatch, serverMessageNumber++, samp::clientrpc::kReliableOrderedChannel,
                serverOrderingIndex++, makePlayerInit());
            if (duplicatePlayerInit) {
              appendLoopbackReliableOrderedFrame(
                  playerInitBatch, serverMessageNumber++, samp::clientrpc::kReliableOrderedChannel,
                  serverOrderingIndex++, makePlayerInit());
            }
            if (!sendServer(playerInitBatch.buffer)) {
              error = "loopback PlayerInit send";
              break;
            }
            sentPlayerInit = true;
          }
        } else if (id == 9u && sentPostAcceptPing) {
          if (packet.reliability != samp::raknet::kReliabilityUnreliable ||
              packet.messageNumber != 6u || packet.payload.size() != 9u ||
              packet.payload[1] != 1u || packet.payload[2] != 2u ||
              packet.payload[3] != 3u || packet.payload[4] != 4u) {
            error = "post-ClientJoin internal ping must receive source-numbered pong";
            break;
          }
          sawPostAcceptPong = true;
        } else if (packet.payload == std::vector<uint8_t>({20u, 128u, 0u, 0u})) {
          if (packet.reliability != samp::raknet::kReliabilityReliableOrdered ||
              packet.orderingChannel != samp::clientrpc::kReliableOrderedChannel ||
              packet.orderingIndex != 1u) {
            error = "initial RequestClass must follow ClientJoin on SyncRPC";
            break;
          }
          if (!initialClassMessageKnown) {
            initialClassMessageNumber = packet.messageNumber;
            initialClassMessageKnown = true;
            ++initialClassRequestMessages;
            sawInitialClass = true;
            firstInitialClassAt = std::chrono::steady_clock::now();
          } else if (packet.messageNumber != initialClassMessageNumber) {
            error = "duplicate PlayerInit must not queue a second RequestClass";
            break;
          }
        }
      }
    }
    if (!sawInitialClass && error.empty()) error = "loopback initial RequestClass timeout";
    ::close(fd);
  }
};

void testRetainedSessionInitialClassBootstrap() {
  LoopbackClassBootstrapPeer peer;
  peer.start();
  for (int attempt = 0; attempt < 200 && !peer.ready; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(peer.ready && peer.port != 0u && peer.error.empty(),
         "loopback class-bootstrap peer did not initialize");

  const samp::SessionSnapshot session =
      samp::raknet_session_connect("127.0.0.1", peer.port, 40, "LoopbackNative");
  // The renderer receives only this reduced world copy. Keep the pre-spawn
  // lifecycle boundary explicit so Android cannot show input as playable
  // before native has observed PlayerSpawn's reliable ACK.
  const samp::WorldSnapshot world = samp::raknet_session_world_snapshot();
  peer.join();
  const samp::SessionSnapshot closed = samp::raknet_session_disconnect();

  expect(session.active && session.accepted && session.playerInit && session.joined &&
             session.classRequestPending && session.requestedClassId == 0u,
         "PlayerInit must retain an active session and queue its first class");
  expect(world.active && world.playerInit && world.classRequestPending &&
             !world.spawnResponseKnown && !world.spawnAllowed && !world.spawnRequestPending &&
             !world.spawnConfirmationSent && !world.spawnConfirmationAcked,
         "world snapshot must mirror the native pre-spawn input gate");
  expect(peer.sawSourceBootstrapDatagram && peer.sawClientJoin && peer.sawPostAcceptPong &&
             peer.sawInitialClass && peer.error.empty(),
         "retained session must coalesce source bootstrap, then pong and initial RequestClass");
  expect(!closed.active, "loopback retained session must close cleanly");
}

void testRetainedSessionCoalescedBootstrapRetriesIndividually() {
  LoopbackClassBootstrapPeer peer;
  peer.dropInitialSourceBootstrap = true;
  peer.start();
  for (int attempt = 0; attempt < 200 && !peer.ready; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(peer.ready && peer.port != 0u && peer.error.empty(),
         "retry loopback peer did not initialize");

  samp::SessionSnapshot session =
      samp::raknet_session_connect("127.0.0.1", peer.port, 250, "LoopbackRetry");
  // A retry becomes due at 750 ms. Let retained polling consume a late
  // PlayerInit on unusually slow host schedulers without extending the server
  // fixture beyond its fixed four-second deadline.
  for (int attempt = 0; attempt < 8 && !session.playerInit && session.error.empty(); ++attempt) {
    session = samp::raknet_session_poll(80);
  }
  peer.join();
  const samp::SessionSnapshot closed = samp::raknet_session_disconnect();

  expect(session.active && session.accepted && session.playerInit && session.joined &&
             session.classRequestPending && session.error.empty(),
         "individually retried bootstrap must still retain an initialized session");
  expect(peer.sawSourceBootstrapDatagram && peer.droppedSourceBootstrap &&
             peer.bootstrapRetryMask.load() == 0x7u && peer.sawClientJoin &&
             peer.sawPostAcceptPong && peer.sawInitialClass && peer.error.empty(),
         "loss of the coalesced burst must retry reliable msg 2/4/5 separately with original identity");
  expect(!closed.active, "retried bootstrap session must close cleanly");
}

// A busy server can send far more than the synchronous handshake's bounded
// receive passes before its PlayerInit. Prove those ordered records survive the
// retained-session hand-off in order and that a duplicate PlayerInit cannot
// enqueue another automatic class request.
void testRetainedSessionLargeOrderedBootstrap() {
  constexpr size_t kBootstrapRpcCount = 256u;
  LoopbackClassBootstrapPeer peer;
  peer.prePlayerInitOrderedFrames = kBootstrapRpcCount;
  peer.duplicatePlayerInit = true;
  peer.start();
  for (int attempt = 0; attempt < 200 && !peer.ready; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(peer.ready && peer.port != 0u && peer.error.empty(),
         "large ordered-bootstrap peer did not initialize");

  samp::SessionSnapshot session =
      samp::raknet_session_connect("127.0.0.1", peer.port, 40, "LoopbackBurst");
  expect(session.active && session.accepted && !session.playerInit && !session.classRequestPending,
         "large ordered bootstrap must cross into the retained session before PlayerInit");
  for (int attempt = 0; attempt < 64 && !session.playerInit; ++attempt) {
    session = samp::raknet_session_poll(20);
  }
  // Drain the adjacent duplicate PlayerInit deterministically after the first
  // valid one has caused the single source-defined bootstrap request.
  for (int attempt = 0; attempt < 3; ++attempt) {
    session = samp::raknet_session_poll(10);
  }
  peer.join();
  const samp::SessionSnapshot closed = samp::raknet_session_disconnect();

  expect(session.active && session.accepted && session.playerInit && session.joined &&
             session.classRequestPending && session.requestedClassId == 0u && session.error.empty(),
         "large ordered bootstrap must reach PlayerInit and retain initial RequestClass");
  expect(session.receivedRpcs >= static_cast<int>(kBootstrapRpcCount + 2u),
         "large ordered bootstrap lost valid ordered RPCs before PlayerInit");
  expect(peer.sawSourceBootstrapDatagram && peer.sawClientJoin && peer.sawPostAcceptPong &&
             peer.sawInitialClass && peer.sentPrePlayerInitOrderedFrames == kBootstrapRpcCount &&
             peer.initialClassRequestMessages == 1u && peer.error.empty(),
         "large ordered bootstrap must preserve SyncRPC order and request class exactly once");
  expect(!closed.active, "large ordered bootstrap retained session must close cleanly");
}

void testRenderableWorldBoundary() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;
  expect(world.applyRpc(makePlayerInit(), event), "render boundary PlayerInit rejected");
  for (uint16_t id = 1; id <= 80; ++id) {
    if (id == 42) continue;  // reserved local PlayerInit ID in this vector
    expect(world.applyRpc(makePlayerJoin(id, 0xA0B0C0FFu, false, "P"), event),
           "render boundary PlayerJoin rejected");
    expect(world.applyRpc(makePlayerStreamIn(id, 0, 7, static_cast<float>(id), 0.0f, 3.0f,
                                              0.0f, 0xA0B0C0FFu, 0), event),
           "render boundary PlayerStreamIn rejected");
  }
  const auto capped = world.renderablePlayers(1000);
  expect(capped.size() == 64 && capped.front().id == 1 && capped.back().id == 65,
         "world renderer actor cap/order changed");
  expect(world.renderablePlayers(5).size() == 5 && world.renderablePlayers(0).empty(),
         "world renderer requested cap changed");
  for (uint16_t id = 1; id <= 80; ++id) {
    expect(world.applyRpc(makeStreamInVehicle(id, 400, static_cast<float>(id), 2.0f, 3.0f, 0.0f), event),
           "render boundary StreamInVehicle rejected");
  }
  const auto cappedVehicles = world.renderableVehicles(1000);
  expect(cappedVehicles.size() == 64 && cappedVehicles.front().id == 1 && cappedVehicles.back().id == 64,
         "world renderer vehicle cap/order changed");
  expect(world.renderableVehicles(5).size() == 5 && world.renderableVehicles(0).empty(),
         "world renderer vehicle requested cap changed");
  for (int16_t id = 0; id < 80; ++id) {
    expect(world.applyRpc(makeShowGangZone(id, static_cast<float>(id), -10.0f,
                                           static_cast<float>(id) + 0.5f, 10.0f,
                                           0x80FF00FFu), event),
           "render boundary ShowGangZone rejected");
  }
  const auto cappedZones = world.renderableGangZones(1000);
  expect(cappedZones.size() == 64 && cappedZones.front().id == 0 && cappedZones.back().id == 63,
         "world renderer gang-zone cap/order changed");
  expect(world.renderableGangZones(5).size() == 5 && world.renderableGangZones(0).empty(),
         "world renderer gang-zone requested cap changed");
}

void testWorldStateReducer() {
  // ConnectionRejected is allowed to arrive before PlayerInit. Preserve its
  // terminal meaning rather than requiring an initialized gameplay reducer.
  {
    samp::state::WorldState preInitWorld;
    samp::state::RpcEvent preInitEvent;
    expect(preInitWorld.applyRpc({20u, 130u, 1u}, preInitEvent) &&
               preInitEvent.kind == samp::state::RpcEventKind::SessionCloseRequested &&
               preInitEvent.playerId == 0u &&
               preInitEvent.summary.find("versi") != std::string::npos,
           "pre-PlayerInit ConnectionRejected must remain terminal");
  }
  samp::state::WorldState world;
  samp::state::RpcEvent event;

  expect(world.applyRpc(makePlayerInit(), event), "WorldState rejected PlayerInit");
  expect(world.initialized() && world.playerInit().playerId == 42,
         "WorldState did not retain PlayerInit");
  expect(world.applyRpc({20u, 40u}, event) &&
             event.kind == samp::state::RpcEventKind::SessionCloseRequested &&
             event.playerId == 42u,
         "exact PlayerClose must surface a transport-terminal lifecycle event");
  expect(!world.applyRpc({20u, 40u, 0u}, event) &&
             event.kind == samp::state::RpcEventKind::Malformed,
         "trailing PlayerClose bytes must fail closed");
  expect(world.applyRpc({20u, 130u, 2u}, event) &&
             event.kind == samp::state::RpcEventKind::SessionCloseRequested &&
             event.summary.find("nickname") != std::string::npos,
         "valid ConnectionRejected reason must surface a terminal lifecycle event");
  expect(!world.applyRpc({20u, 130u, 0u}, event) &&
             event.kind == samp::state::RpcEventKind::Malformed &&
             !world.applyRpc({20u, 130u, 1u, 0u}, event) &&
             event.kind == samp::state::RpcEventKind::Malformed,
         "unknown/trailing ConnectionRejected records must fail closed");
  expect(world.environment().timeKnown && world.environment().weatherKnown &&
         world.environment().gravityKnown && world.environment().worldTime == 12 &&
         world.environment().weather == 7 && std::fabs(world.environment().gravity - 0.008f) < 0.00001f,
         "PlayerInit environment was not retained");
  const auto removeFloorRpc = makeRemoveBuilding(1000u, 10.0f, -20.0f, 5.0f, 3.0f);
  expect(world.applyRpc(removeFloorRpc, event) &&
             event.kind == samp::state::RpcEventKind::BuildingRemovalChanged &&
             world.buildingRemovals().size() == 1u &&
             world.buildingRemovals().front().modelId == 1000u &&
             std::fabs(world.buildingRemovals().front().radius - 3.0f) < 0.0001f &&
             std::string(samp::rpc::name(43u)) == "RemoveBuilding",
         "exact RemoveBuilding must retain a static-world directive");
  expect(world.applyRpc(removeFloorRpc, event) && world.buildingRemovals().size() == 1u,
         "duplicate RemoveBuilding must not grow reducer state");
  auto trailingRemoveBuilding = removeFloorRpc;
  trailingRemoveBuilding.push_back(0u);
  expect(!world.applyRpc(trailingRemoveBuilding, event) &&
             event.kind == samp::state::RpcEventKind::Malformed &&
             world.buildingRemovals().size() == 1u,
         "trailing RemoveBuilding bytes must fail closed");
  expect(!world.applyRpc(makeRemoveBuilding(1000u, 10.0f, -20.0f, 5.0f,
                                             std::numeric_limits<float>::quiet_NaN()), event) &&
             event.kind == samp::state::RpcEventKind::Malformed &&
             world.buildingRemovals().size() == 1u,
         "non-finite RemoveBuilding radius must fail closed");
  expect(world.applyRpc(makeRemoveBuilding(0xFFFFFFFFu, 11.0f, -20.0f, 5.0f, 2.0f), event) &&
             event.kind == samp::state::RpcEventKind::BuildingRemovalChanged &&
             world.buildingRemovals().size() == 2u &&
             world.buildingRemovals().back().modelId == samp::gta::removal::kAnyModel,
         "wildcard RemoveBuilding model -1 must remain explicit");
  samp::state::WorldState cappedRemoveWorld;
  samp::state::RpcEvent cappedRemoveEvent;
  for (size_t index = 0; index < samp::gta::removal::kMaxDirectives; ++index) {
    expect(cappedRemoveWorld.applyRpc(makeRemoveBuilding(2000u, static_cast<float>(index),
                                                          0.0f, 0.0f, 0.0f),
                                      cappedRemoveEvent),
           "bounded RemoveBuilding fixture rejected before its cap");
  }
  expect(cappedRemoveWorld.buildingRemovals().size() == samp::gta::removal::kMaxDirectives &&
             cappedRemoveWorld.applyRpc(makeRemoveBuilding(2000u, 2001.0f, 0.0f, 0.0f, 0.0f),
                                         cappedRemoveEvent) &&
             cappedRemoveEvent.kind == samp::state::RpcEventKind::Ignored &&
             cappedRemoveWorld.buildingRemovals().size() == samp::gta::removal::kMaxDirectives,
         "RemoveBuilding directive cap must ignore extra valid records without growing state");
  expect(world.applyRpc(makeGivePlayerMoney(1250), event) &&
         event.kind == samp::state::RpcEventKind::MoneyChanged && world.localPlayer().moneyKnown &&
         world.localPlayer().money == 1250 &&
         world.applyRpc(makeGivePlayerMoney(-300), event) && world.localPlayer().money == 950,
         "GivePlayerMoney delta state wrong");
  expect(world.applyRpc(makeEmptyRpc(20), event) &&
         event.kind == samp::state::RpcEventKind::MoneyChanged && world.localPlayer().moneyKnown &&
         world.localPlayer().money == 0,
         "ResetPlayerMoney state wrong");
  auto malformedMoney = makeGivePlayerMoney(1);
  malformedMoney.pop_back();
  expect(!world.applyRpc(malformedMoney, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.localPlayer().money == 0,
         "truncated GivePlayerMoney must fail closed");
  expect(world.applyRpc(makeGivePlayerWeapon(24, 187), event) &&
         event.kind == samp::state::RpcEventKind::WeaponStateChanged &&
         world.localPlayer().weaponAmmoKnown[24] && world.localPlayer().weaponAmmo[24] == 187 &&
         !world.localPlayer().armedWeaponKnown,
         "GivePlayerWeapon must retain ammo without guessing selection");
  expect(world.applyRpc(makeSetPlayerAmmo(24, 1), event) &&
         event.kind == samp::state::RpcEventKind::WeaponStateChanged &&
         world.localPlayer().weaponAmmoKnown[24] && world.localPlayer().weaponAmmo[24] == 1,
         "SetPlayerAmmo must refresh the compact authoritative weapon count");
  auto malformedSetAmmo = makeSetPlayerAmmo(24, 9);
  malformedSetAmmo.pop_back();
  expect(!world.applyRpc(malformedSetAmmo, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.localPlayer().weaponAmmo[24] == 1,
         "truncated SetPlayerAmmo must fail closed");
  auto invalidSetAmmo = makeSetPlayerAmmo(47, 9);
  expect(!world.applyRpc(invalidSetAmmo, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.localPlayer().weaponAmmo[24] == 1,
         "out-of-range SetPlayerAmmo must fail closed");
  expect(world.applyRpc(makeSetPlayerArmedWeapon(24), event) &&
         event.kind == samp::state::RpcEventKind::WeaponStateChanged &&
         world.localPlayer().armedWeaponKnown && world.localPlayer().armedWeapon == 24,
         "SetPlayerArmedWeapon state wrong");
  auto malformedArmedWeapon = makeSetPlayerArmedWeapon(47);
  expect(!world.applyRpc(malformedArmedWeapon, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.localPlayer().armedWeapon == 24,
         "out-of-range SetPlayerArmedWeapon must fail closed");
  expect(world.applyRpc(makeEmptyRpc(21), event) &&
         event.kind == samp::state::RpcEventKind::WeaponStateChanged &&
         world.localPlayer().armedWeaponKnown && world.localPlayer().armedWeapon == 0 &&
         !world.localPlayer().weaponAmmoKnown[24],
         "ResetPlayerWeapons state wrong");
  auto malformedGiveWeapon = makeGivePlayerWeapon(24, 10);
  malformedGiveWeapon.pop_back();
  expect(!world.applyRpc(malformedGiveWeapon, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.localPlayer().weaponAmmoKnown[24],
         "truncated GivePlayerWeapon must fail closed");
  expect(world.applyRpc(makeByteRpc(94, 23), event) &&
         event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged &&
         world.environment().worldTime == 23,
         "SetWorldTime state wrong");
  expect(world.applyRpc(makePlayerTime(6, 45), event) &&
         event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged &&
         world.environment().playerTimeKnown && world.environment().playerHour == 6 &&
         world.environment().playerMinute == 45,
         "SetPlayerTime must retain an exact local hour/minute pair");
  expect(world.applyRpc(makeByteRpc(30, 0), event) &&
         event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged &&
         world.environment().playerClockKnown && !world.environment().playerClockEnabled,
         "TogglePlayerClock off state wrong");
  expect(world.applyRpc(makeByteRpc(30, 1), event) && world.environment().playerClockEnabled,
         "TogglePlayerClock on state wrong");
  expect(!world.applyRpc(makePlayerTime(24, 0), event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.environment().playerHour == 6 && world.environment().playerMinute == 45,
         "invalid SetPlayerTime must preserve prior local clock");
  auto truncatedPlayerTime = makePlayerTime(7, 15);
  truncatedPlayerTime.pop_back();
  expect(!world.applyRpc(truncatedPlayerTime, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.environment().playerHour == 6 && world.environment().playerMinute == 45,
         "truncated SetPlayerTime must preserve prior local clock");
  expect(!world.applyRpc(makeByteRpc(30, 2), event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.environment().playerClockEnabled,
         "invalid TogglePlayerClock must preserve prior local clock visibility");
  const auto audioUrl = std::string("https://radio.example.test/live.mp3");
  expect(world.applyRpc(makeAudioStream(audioUrl, 10.0f, -20.0f, 8.0f, 150.0f, 1u), event) &&
         event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged &&
         world.audioStream().active && world.audioStream().positional &&
         world.audioStream().revision != 0u && world.audioStream().url == audioUrl &&
         std::fabs(world.audioStream().x - 10.0f) < 0.0001f &&
         std::fabs(world.audioStream().distance - 150.0f) < 0.0001f,
         "RPC-41 must retain a bounded positional audio directive only");
  const uint64_t firstAudioRevision = world.audioStream().revision;
  expect(!world.applyRpc(makeAudioStream("https://bad-flag.example.test/live", 0.0f, 0.0f, 0.0f,
                                          0.0f, 2u), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.audioStream().revision == firstAudioRevision && world.audioStream().url == audioUrl,
         "malformed RPC-41 must preserve a prior audio directive");
  const std::string oversizedAudioUrl = "https://" + std::string(120u, 'a');
  expect(oversizedAudioUrl.size() == 128u &&
         !world.applyRpc(makeAudioStream(oversizedAudioUrl, 0.0f, 0.0f, 0.0f, 0.0f, 0u), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.audioStream().revision == firstAudioRevision,
         "RPC-41 URL above HybridString<128> data capacity must fail closed");
  auto trailingAudio = makeAudioStream(audioUrl, 0.0f, 0.0f, 0.0f, 0.0f, 0u);
  trailingAudio.push_back(0u);
  expect(!world.applyRpc(trailingAudio, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.audioStream().revision == firstAudioRevision,
         "RPC-41 trailing bytes must fail closed");
  expect(world.applyRpc({20, 42}, event) && event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged &&
         !world.audioStream().active && world.audioStream().url.empty() &&
         world.audioStream().revision != firstAudioRevision,
         "RPC-42 must clear the retained audio directive and advance revision");
  expect(!world.applyRpc({20, 42, 0}, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         !world.audioStream().active,
         "RPC-42 trailing payload must not revive or mutate audio state");
  samp::state::WorldState freshClockWorld;
  expect(freshClockWorld.applyRpc(makePlayerInit(), event) &&
         freshClockWorld.applyRpc(makePlayerTime(22, 12), event) &&
         freshClockWorld.applyRpc(makeByteRpc(30, 0), event) &&
         freshClockWorld.applyRpc(makeAudioStream(audioUrl, 0.0f, 0.0f, 0.0f, 0.0f, 0u), event) &&
         freshClockWorld.applyRpc(makePlayerInit(), event) &&
         !freshClockWorld.environment().playerTimeKnown &&
         !freshClockWorld.environment().playerClockKnown &&
         freshClockWorld.environment().playerClockEnabled && !freshClockWorld.audioStream().active,
         "fresh PlayerInit must clear recipient clock/audio directives");
  expect(!world.applyRpc(makeByteRpc(94, 24), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.environment().worldTime == 23,
         "invalid SetWorldTime must be rejected without changing state");
  expect(world.applyRpc(makeByteRpc(152, 42), event) &&
         event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged &&
         world.environment().weather == 42,
         "SetWeather state wrong");
  expect(world.applyRpc(makeWorldBounds(120.0f, -80.0f, 240.0f, -160.0f), event) &&
         event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged && event.worldBoundsUpdated &&
         world.environment().worldBoundsKnown &&
         std::fabs(world.environment().worldMaxX - 120.0f) < 0.0001f &&
         std::fabs(world.environment().worldMinX + 80.0f) < 0.0001f &&
         std::fabs(world.environment().worldMaxY - 240.0f) < 0.0001f &&
         std::fabs(world.environment().worldMinY + 160.0f) < 0.0001f,
         "SetWorldBounds must retain exact max/min Vector4 order");
  expect(!world.applyRpc(makeWorldBounds(-80.0f, 120.0f, 240.0f, -160.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         std::fabs(world.environment().worldMaxX - 120.0f) < 0.0001f,
         "inverted SetWorldBounds must preserve prior recipient bounds");
  auto truncatedWorldBounds = makeWorldBounds(200.0f, -200.0f, 200.0f, -200.0f);
  truncatedWorldBounds.pop_back();
  expect(!world.applyRpc(truncatedWorldBounds, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         std::fabs(world.environment().worldMinY + 160.0f) < 0.0001f,
         "truncated SetWorldBounds must preserve prior recipient bounds");
  expect(world.applyRpc(makeByteRpc(133, 4), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged &&
         world.localPlayer().wantedLevelKnown && world.localPlayer().wantedLevel == 4,
         "SetPlayerWantedLevel state wrong");
  expect(!world.applyRpc(makeByteRpc(133, 7), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.localPlayer().wantedLevel == 4,
         "invalid SetPlayerWantedLevel must preserve prior level");
  expect(world.applyRpc(makeFloatRpc(146, 0.004f), event) &&
         event.kind == samp::state::RpcEventKind::WorldEnvironmentChanged && event.worldGravityUpdated &&
         world.environment().gravityKnown && std::fabs(world.environment().gravity - 0.004f) < 0.00001f,
         "SetPlayerGravity state wrong");
  expect(!world.applyRpc(makeFloatRpc(146, 0.5f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         std::fabs(world.environment().gravity - 0.004f) < 0.00001f,
         "invalid SetPlayerGravity must preserve prior gravity");
  auto malformedGravity = makeFloatRpc(146, 0.008f);
  malformedGravity.pop_back();
  expect(!world.applyRpc(malformedGravity, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         std::fabs(world.environment().gravity - 0.004f) < 0.00001f,
         "truncated SetPlayerGravity must preserve prior gravity");
  expect(world.applyRpc(makeByteRpc(156, 7), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged && event.localInteriorUpdated &&
         world.localPlayer().interiorKnown && world.localPlayer().interior == 7,
         "SetPlayerInterior state wrong");
  auto malformedInterior = makeByteRpc(156, 8);
  malformedInterior.pop_back();
  expect(!world.applyRpc(malformedInterior, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.localPlayer().interior == 7,
         "truncated SetPlayerInterior must preserve the prior collision partition");
  expect(world.applyRpc(makeInt32Rpc(48, -42), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged &&
         world.localPlayer().virtualWorldKnown && world.localPlayer().virtualWorld == -42 &&
         world.localPlayer().interior == 7,
         "SetPlayerVirtualWorld must preserve signed state without changing interior");
  auto malformedVirtualWorld = makeInt32Rpc(48, 91);
  malformedVirtualWorld.pop_back();
  expect(!world.applyRpc(malformedVirtualWorld, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.localPlayer().virtualWorld == -42,
         "truncated SetPlayerVirtualWorld must preserve previous state");
  auto extendedVirtualWorld = makeInt32Rpc(48, 91);
  extendedVirtualWorld.push_back(0);
  expect(!world.applyRpc(extendedVirtualWorld, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.localPlayer().virtualWorld == -42,
         "SetPlayerVirtualWorld must require its exact int32 body");
  expect(world.applyRpc(makeToggleSpectating(1), event) && event.localSpectateUpdated &&
         world.localPlayer().spectatingKnown && world.localPlayer().spectating &&
         world.applyRpc(makeSpectateTarget(126, 77, 2), event) && event.localSpectateUpdated &&
         world.localPlayer().spectateTargetKnown && !world.localPlayer().spectateTargetVehicle &&
         world.localPlayer().spectateTargetId == 77 && world.localPlayer().spectateMode == 2 &&
         world.applyRpc(makeSpectateTarget(127, 91, 3), event) &&
         world.localPlayer().spectateTargetKnown && world.localPlayer().spectateTargetVehicle &&
         world.localPlayer().spectateTargetId == 91 && world.localPlayer().spectateMode == 3,
         "spectator toggle and copied player/vehicle target lifecycle");
  auto malformedSpectateTarget = makeSpectateTarget(126, 4, 1);
  malformedSpectateTarget.pop_back();
  expect(!world.applyRpc(malformedSpectateTarget, event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.localPlayer().spectateTargetId == 91 && world.localPlayer().spectateTargetVehicle,
         "truncated spectator target must preserve the previous target");
  expect(!world.applyRpc(makeToggleSpectating(2), event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.localPlayer().spectating,
         "invalid spectator toggle must preserve active state");
  expect(world.applyRpc(makeToggleSpectating(0), event) && event.localSpectateUpdated &&
         !world.localPlayer().spectating && !world.localPlayer().spectateTargetKnown &&
         world.localPlayer().spectateTargetId == 0,
         "disabling spectator must clear the copied target");
  const auto initMenu = makePlayerInitMenu(7);
  expect(world.applyRpc(initMenu, event) && event.kind == samp::state::RpcEventKind::MenuChanged &&
         !world.menu().active && world.menu().id == 0,
         "PlayerInitMenu must retain hidden definition without showing it");
  expect(world.applyRpc({20, 77, 7}, event) && world.menu().active && world.menu().id == 7 &&
         world.menu().twoColumns && world.menu().enabled && world.menu().column1Count == 3 &&
         world.menu().column2Count == 3 && world.menu().column1Items[0] == "Los Santos" &&
         world.menu().column2Items[2] == "$30" && !world.menu().rowEnabled[1] &&
         world.menu().revision != 0,
         "PlayerShowMenu must expose source-verified fixed-column menu state");
  const uint64_t visibleMenuRevision = world.menu().revision;
  expect(!world.acknowledgeMenuSelection(7, visibleMenuRevision, 1) && world.menu().active &&
         world.acknowledgeMenuSelection(7, visibleMenuRevision, 2) && !world.menu().active,
         "menu selection must reject disabled rows and close only matching revision");
  expect(world.applyRpc({20, 77, 7}, event) && world.menu().active &&
         world.acknowledgeMenuExit(7, world.menu().revision) && !world.menu().active,
         "menu exit must use matching active server revision");
  auto malformedMenu = initMenu;
  malformedMenu.pop_back();
  expect(!world.applyRpc(malformedMenu, event) && !world.menu().active,
         "truncated PlayerInitMenu must preserve prior menu state");
  expect(!world.applyRpc({20, 77, 99}, event),
         "PlayerShowMenu without a valid retained definition must fail closed");
  expect(world.applyRpc(makePlayerInitMenu(8, false), event) &&
         world.applyRpc({20, 77, 8}, event) && world.menu().active &&
         !world.menu().twoColumns && world.menu().column1Count == 3 &&
         world.menu().column2Count == 0 && world.acknowledgeMenuExit(8, world.menu().revision),
         "single-column PlayerInitMenu must not consume a second fixed column");

  // Server NPC actors have a separate lifecycle from players. ShowActor and
  // only its exact follow-up mutators may create/update the bounded visual
  // record; neither model nor transform creates a remote player identity.
  expect(world.applyRpc(makeShowActor(9, 23, 10.0f, -15.0f, 4.0f, 450.0f, 87.5f, 1), event) &&
         event.kind == samp::state::RpcEventKind::ServerActorCreated && event.playerId == 9 &&
         world.serverActorCount() == 1 && world.playerCount() == 0 &&
         world.renderableServerActors().size() == 1 &&
         world.renderableServerActors()[0].model == 23 &&
         std::fabs(world.renderableServerActors()[0].angle - 90.0f) < 0.001f &&
         world.renderableServerActors()[0].invulnerable,
         "ShowActor must create a separate bounded NPC actor record");
  expect(world.applyRpc(makeSetActorPosition(9, 20.0f, -10.0f, 5.0f), event) &&
         world.applyRpc(makeSetActorAngle(9, -90.0f), event) &&
         world.applyRpc(makeSetActorHealth(9, 33.0f), event) &&
         event.kind == samp::state::RpcEventKind::ServerActorChanged &&
         std::fabs(world.renderableServerActors()[0].x - 20.0f) < 0.001f &&
         std::fabs(world.renderableServerActors()[0].angle - 270.0f) < 0.001f &&
         std::fabs(world.renderableServerActors()[0].health - 33.0f) < 0.001f,
         "actor transform/health mutators changed");
  expect(world.applyRpc(makeApplyAnimation(173, 9, "PED", "IDLE_CHAT", 4.1f,
                                            true, true, false, false, 0), event) &&
         event.kind == samp::state::RpcEventKind::ServerActorChanged &&
         world.renderableServerActors()[0].forcedAnimation.active &&
         world.renderableServerActors()[0].forcedAnimation.library == "PED" &&
         world.renderableServerActors()[0].forcedAnimation.name == "IDLE_CHAT" &&
         world.renderableServerActors()[0].forcedAnimation.loop &&
         world.renderableServerActors()[0].forcedAnimation.lockX &&
         world.renderableServerActors()[0].forcedAnimation.revision != 0,
         "packed ApplyActorAnimation must retain a bounded renderer directive");
  const uint64_t actorAnimationRevision = world.renderableServerActors()[0].forcedAnimation.revision;
  auto actorAnimationTrailing = makeApplyAnimation(173, 9, "PED", "WALK", 4.1f,
                                                    false, false, false, false, 1000);
  actorAnimationTrailing.push_back(0x80u);
  expect(!world.applyRpc(actorAnimationTrailing, event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.renderableServerActors()[0].forcedAnimation.revision == actorAnimationRevision &&
         world.renderableServerActors()[0].forcedAnimation.name == "IDLE_CHAT",
         "ApplyActorAnimation must reject non-pad trailing body bits without clobbering state");
  expect(world.applyRpc(makeClearAnimation(174, 9), event) &&
         !world.renderableServerActors()[0].forcedAnimation.active &&
         world.renderableServerActors()[0].forcedAnimation.revision != actorAnimationRevision,
         "ClearActorAnimation must only clear the retained actor directive");
  const auto actorBeforeMalformed = world.renderableServerActors()[0];
  auto malformedActor = makeShowActor(9, 24, 0.0f, 0.0f, 3.0f, 0.0f, 50.0f, 0);
  malformedActor.pop_back();
  expect(!world.applyRpc(malformedActor, event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.renderableServerActors()[0].model == actorBeforeMalformed.model,
         "truncated ShowActor must preserve prior NPC actor state");
  expect(!world.applyRpc(makeSetActorPosition(10, 0.0f, 0.0f, 3.0f), event) &&
         world.serverActorCount() == 1,
         "orphan actor mutator must not fabricate an NPC actor");
  expect(world.applyRpc(makeHideActor(9), event) &&
         event.kind == samp::state::RpcEventKind::ServerActorDestroyed &&
         world.serverActorCount() == 0 && world.renderableServerActors().empty() &&
         world.applyRpc(makeHideActor(9), event) && world.serverActorCount() == 0,
         "HideActor must remain idempotent and remove the NPC actor");
  samp::state::WorldState cappedActorWorld;
  bool cappedActorsApplied = true;
  for (uint16_t id = 1000; id > 0; --id) {
    cappedActorsApplied = cappedActorsApplied && cappedActorWorld.applyRpc(
        makeShowActor(id, 7, static_cast<float>(id), 0.0f, 3.0f, 0.0f, 100.0f, 0), event);
  }
  const auto cappedActorViews = cappedActorWorld.renderableServerActors(999);
  expect(cappedActorsApplied && cappedActorWorld.serverActorCount() == 1000 &&
         cappedActorViews.size() == 64 && cappedActorViews.front().id == 1 &&
         cappedActorViews.back().id == 64 &&
         !cappedActorWorld.applyRpc(makeShowActor(1001, 7, 0.0f, 0.0f, 3.0f, 0.0f, 100.0f, 0), event) &&
         cappedActorWorld.serverActorCount() == 1000 &&
         cappedActorWorld.applyRpc(makeShowActor(500, 8, 500.0f, 1.0f, 3.0f, 0.0f, 99.0f, 0), event) &&
         cappedActorWorld.renderableServerActors(999)[63].id == 64,
         "server actor retention/export caps and map ordering");

  expect(world.applyRpc(makeShowGangZone(17, -120.0f, 40.0f, 240.0f, 390.0f,
                                         0x804020FFu), event) &&
         event.kind == samp::state::RpcEventKind::GangZoneChanged && world.gangZoneCount() == 1,
         "ShowGangZone state wrong");
  const auto gangZones = world.renderableGangZones(8);
  expect(gangZones.size() == 1 && gangZones[0].id == 17 &&
         std::fabs(gangZones[0].minX + 120.0f) < 0.001f &&
         std::fabs(gangZones[0].maxY - 390.0f) < 0.001f &&
         gangZones[0].colourAbgr == 0x804020FFu && !gangZones[0].flashing,
         "gang-zone renderer view changed");
  expect(world.applyRpc(makeGangZoneIdRpc(121, 17, 0xFFE020A0u), event) &&
         event.kind == samp::state::RpcEventKind::GangZoneChanged &&
         world.renderableGangZones(8)[0].flashing &&
         world.renderableGangZones(8)[0].flashColourAbgr == 0xFFE020A0u,
         "FlashGangZone state wrong");
  expect(world.applyRpc(makeGangZoneIdRpc(85, 17), event) &&
         !world.renderableGangZones(8)[0].flashing,
         "StopFlashGangZone state wrong");
  expect(!world.applyRpc(makeGangZoneIdRpc(121, 18, 0xFFFFFFFFu), event) &&
         world.gangZoneCount() == 1,
         "orphan FlashGangZone must not create a zone");
  expect(!world.applyRpc(makeShowGangZone(18, 12.0f, 5.0f, 12.0f, 9.0f,
                                          0xFFFFFFFFu), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.gangZoneCount() == 1,
         "inverted/empty ShowGangZone must fail closed");
  expect(world.applyRpc(makeGangZoneIdRpc(120, 17), event) &&
         event.kind == samp::state::RpcEventKind::GangZoneChanged && world.gangZoneCount() == 0 &&
         world.renderableGangZones(8).empty(),
         "HideGangZone state wrong");

  expect(world.applyRpc(makeVec3Rpc(157, 10.0f, 20.0f, 30.0f), event) &&
         event.kind == samp::state::RpcEventKind::CameraChanged && world.camera().positionKnown &&
         std::fabs(world.camera().positionY - 20.0f) < 0.001f,
         "SetPlayerCameraPosition state wrong");
  expect(world.applyRpc(makeCameraLookAt(12.0f, 26.0f, 31.0f, 1), event) &&
         event.kind == samp::state::RpcEventKind::CameraChanged && world.camera().lookAtKnown &&
         std::fabs(world.camera().lookAtX - 12.0f) < 0.001f,
         "SetPlayerCameraLookAt state wrong");
  const auto interpolatePosition = makeInterpolateCamera(true, 20.0f, 30.0f, 40.0f,
                                                         70.0f, 80.0f, 90.0f, 10'000u, 1);
  expect(interpolatePosition.size() == 32u && world.applyRpc(interpolatePosition, event) &&
         event.kind == samp::state::RpcEventKind::CameraChanged && world.camera().positionKnown &&
         world.camera().interpolatingPosition,
         "bit-packed InterpolateCamera position state wrong");
  expect(world.applyRpc(makeInterpolateCamera(false, 2.0f, 3.0f, 4.0f,
                                               6.0f, 7.0f, 8.0f, 0u), event) &&
         world.camera().lookAtKnown && !world.camera().interpolatingLookAt &&
         std::fabs(world.camera().lookAtX - 6.0f) < 0.001f &&
         std::fabs(world.camera().lookAtZ - 8.0f) < 0.001f,
         "zero-time InterpolateCamera look-at cut wrong");
  auto malformedCamera = makeCameraLookAt(1.0f, 2.0f, 3.0f);
  malformedCamera.pop_back();
  expect(!world.applyRpc(malformedCamera, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.camera().positionKnown && world.camera().lookAtKnown,
         "truncated camera RPC must preserve prior camera state");
  expect(world.applyRpc(makeEmptyRpc(162), event) &&
         event.kind == samp::state::RpcEventKind::CameraChanged && !world.camera().positionKnown &&
         !world.camera().lookAtKnown,
         "SetCameraBehindPlayer must clear server camera override");

  samp::state::WorldState vehicleSelectionWorld;
  samp::state::RpcEvent vehicleSelectionEvent;
  expect(vehicleSelectionWorld.applyRpc(makeStreamInVehicle(33, 400, 10.0f, 11.0f, 4.0f, 0.0f,
                                                            1, 2, 1000.0f, 7), vehicleSelectionEvent) &&
         vehicleSelectionWorld.nearestVehicle(10.4f, 11.0f, 4.0f, 2.0f, 7, true) &&
         vehicleSelectionWorld.nearestVehicle(10.4f, 11.0f, 4.0f, 2.0f, 7, true)->id == 33 &&
         !vehicleSelectionWorld.nearestVehicle(10.4f, 11.0f, 4.0f, 2.0f, 0, true),
         "nearest vehicle must honor stream/interior state");
  expect(!vehicleSelectionWorld.localVehiclePresentationAnchor() &&
         vehicleSelectionWorld.applyRpc(makePutPlayerInVehicle(33, 2), vehicleSelectionEvent) &&
         vehicleSelectionWorld.localVehiclePresentationAnchor() &&
         vehicleSelectionWorld.localVehiclePresentationAnchor()->id == 33 &&
         std::fabs(vehicleSelectionWorld.localVehiclePresentationAnchor()->x - 10.0f) < 0.001f,
         "seated render/camera anchor must use only streamed vehicle transform");
  expect(vehicleSelectionWorld.applyRpc(makeRemovePlayerFromVehicle(), vehicleSelectionEvent) &&
         !vehicleSelectionWorld.localVehiclePresentationAnchor(),
         "vehicle anchor must clear after authoritative exit");
  expect(world.applyRpc(makePutPlayerInVehicle(33, 0), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged &&
         world.localPlayer().vehicleKnown && world.localPlayer().inVehicle &&
         world.localPlayer().vehicleId == 33 && world.localPlayer().vehicleSeat == 0,
         "PutPlayerInVehicle state wrong");
  expect(world.applyRpc(makeRemovePlayerFromVehicle(), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged &&
         world.localPlayer().vehicleKnown && !world.localPlayer().inVehicle,
         "RemovePlayerFromVehicle state wrong");
  expect(!world.applyRpc(makePutPlayerInVehicle(33, 9), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.localPlayer().inVehicle,
         "invalid PutPlayerInVehicle must fail closed");

  // Current open.mp ObjectMaterialData writers send CreateObject records as
  // type/slot followed by Default (u16 model, DynStr8 TXD/name, ABGR) or Text.
  // Retain both exact bounded variants through the renderer snapshot; a Text
  // record replaces any prior Default state in that one flattened slot.
  {
    samp::state::WorldState materialWorld;
    samp::state::RpcEvent materialEvent;
    const std::vector<DefaultObjectMaterialWire> createdMaterials = {
        {3u, 1337u, "MiXeD", "RePlAcE", 0x7F102030u},
        {7u, 900u, "Other", "Second", 0x01020304u},
    };
    expect(materialWorld.applyRpc(
               makeCreateObject(66, 19379, 10.0f, 20.0f, 5.0f,
                                0.0f, 0.0f, 0.0f, 100.0f, true,
                                0xFFFFu, 0xFFFFu, 0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 1u, createdMaterials, 12),
               materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::ObjectCreated,
           "CreateObject must retain source-writer default/text material sequence");
    auto materialViews = materialWorld.renderableObjects(1);
    expect(materialViews.size() == 1 && materialViews[0].materials[3].active &&
           materialViews[0].materials[3].sourceModel == 1337u &&
           materialViews[0].materials[3].colourAbgr == 0x7F102030u &&
           materialViews[0].materials[3].textureDictionary == "mixed" &&
           materialViews[0].materials[3].textureName == "replace" &&
           materialViews[0].materials[7].active &&
           !materialViews[0].materials[12].active && materialViews[0].materials[12].textActive &&
           materialViews[0].materials[12].textMaterialSize == 10u &&
           materialViews[0].materials[12].textFontSize == 12u &&
           !materialViews[0].materials[12].textBold &&
           materialViews[0].materials[12].textColourArgb == 0xFFFFFFFFu &&
           materialViews[0].materials[12].textBackgroundColourArgb == 0u &&
           materialViews[0].materials[12].textAlignment == 0u &&
           materialViews[0].materials[12].textFont == "Arial" &&
           materialViews[0].materials[12].text == "Hello",
           "CreateObject material slot/color/default-text reduction changed");

    const DefaultObjectMaterialWire replacement =
        {3u, 123u, "Second", "TwoTex", 0xAABBCCDDu};
    expect(materialWorld.applyRpc(makeSetObjectMaterialDefault(66, replacement), materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::ObjectChanged,
           "SetObjectMaterial default record must update an existing object");
    materialViews = materialWorld.renderableObjects(1);
    expect(materialViews.size() == 1 && materialViews[0].materials[3].active &&
           materialViews[0].materials[3].sourceModel == 123u &&
           materialViews[0].materials[3].colourAbgr == 0xAABBCCDDu &&
           materialViews[0].materials[3].textureDictionary == "second" &&
           materialViews[0].materials[3].textureName == "twotex",
           "SetObjectMaterial slot/color replacement changed");
    expect(materialWorld.applyRpc(makeSetObjectMaterialText(66, 3u), materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::ObjectChanged &&
           !materialWorld.renderableObjects(1)[0].materials[3].active &&
           materialWorld.renderableObjects(1)[0].materials[3].textActive &&
           materialWorld.renderableObjects(1)[0].materials[3].text == "Hello",
           "SetObjectMaterialText must replace a stale Default slot safely");
    auto invalidTextMaterial = makeSetObjectMaterialText(66, 3u);
    invalidTextMaterial[6] = 0u;  // SDK ObjectMaterialSize begins at 10.
    expect(!materialWorld.applyRpc(invalidTextMaterial, materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::Malformed &&
           !materialWorld.renderableObjects(1)[0].materials[3].active &&
           materialWorld.renderableObjects(1)[0].materials[3].textActive,
           "invalid ObjectMaterialText enum must fail closed");
    auto noneMaterial = makeSetObjectMaterialDefault(66, replacement);
    noneMaterial[4] = 0u;  // SDK None is never serialized as a used record.
    expect(!materialWorld.applyRpc(noneMaterial, materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::Malformed,
           "ObjectMaterial None record must fail closed");

    const size_t retainedObjects = materialWorld.serverObjectCount();
    const std::vector<DefaultObjectMaterialWire> duplicateSlots = {
        {4u, 1u, "one", "one", 0xFFFFFFFFu},
        {4u, 2u, "two", "two", 0xFFFFFFFFu},
    };
    expect(!materialWorld.applyRpc(
               makeCreateObject(67, 19379, 0.0f, 0.0f, 5.0f,
                                0.0f, 0.0f, 0.0f, 100.0f, true,
                                0xFFFFu, 0xFFFFu, 0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 1u, duplicateSlots),
               materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::Malformed &&
           materialWorld.serverObjectCount() == retainedObjects,
           "duplicate CreateObject material slots must fail closed");
    const DefaultObjectMaterialWire unsafeName =
        {4u, 1u, "../unsafe", "texture", 0xFFFFFFFFu};
    expect(!materialWorld.applyRpc(makeSetObjectMaterialDefault(66, unsafeName), materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::Malformed,
           "path-like material lookup key must fail closed");
    auto opaqueMaterial = makeSetObjectMaterialDefault(66, replacement);
    opaqueMaterial.push_back(0u);
    expect(!materialWorld.applyRpc(opaqueMaterial, materialEvent) &&
           materialEvent.kind == samp::state::RpcEventKind::Malformed &&
           !materialWorld.renderableObjects(1)[0].materials[3].active &&
           materialWorld.renderableObjects(1)[0].materials[3].textActive,
           "opaque SetObjectMaterial trailing payload must preserve prior state");
  }

  expect(world.applyRpc(makeCreateObject(12, 19379, 40.0f, -30.0f, 8.0f,
                                         0.0f, 0.0f, 450.0f, 180.0f), event) &&
         event.kind == samp::state::RpcEventKind::ObjectCreated && world.serverObjectCount() == 1,
         "legacy CreateObject state wrong");
  auto objects = world.renderableObjects(8);
  expect(objects.size() == 1 && objects[0].id == 12 && objects[0].model == 19379 &&
         std::fabs(objects[0].rotationZ - 90.0f) < 0.001f &&
         std::fabs(objects[0].drawDistance - 180.0f) < 0.001f,
         "CreateObject renderer view changed");
  expect(world.applyRpc(makeCreateObject(20, 19380, 39.0f, -30.0f, 8.0f,
                                         0.0f, 0.0f, 0.0f, 80.0f, true), event) &&
         event.kind == samp::state::RpcEventKind::ObjectCreated && world.serverObjectCount() == 2 &&
         world.renderableObjects(8).size() == 2,
         "modern static CreateObject state wrong");
  expect(world.applyRpc(makeObjectVec3Rpc(45, 12, 41.0f, -31.0f, 9.0f), event) &&
         event.kind == samp::state::RpcEventKind::ObjectChanged &&
         std::fabs(world.renderableObjects(1)[0].x - 41.0f) < 0.001f,
         "SetObjectPosition state wrong");
  expect(world.applyRpc(makeObjectVec3Rpc(46, 12, -90.0f, 720.0f, 181.0f), event) &&
         event.kind == samp::state::RpcEventKind::ObjectChanged &&
         std::fabs(world.renderableObjects(1)[0].rotationX - 270.0f) < 0.001f &&
         std::fabs(world.renderableObjects(1)[0].rotationY) < 0.001f,
         "SetObjectRotation state wrong");
  expect(world.applyRpc(makeMoveObject(12, 41.0f, -31.0f, 9.0f,
                                       51.0f, -31.0f, 9.0f, 1.0f,
                                       0.0f, 0.0f, 270.0f), event) &&
         event.kind == samp::state::RpcEventKind::ObjectChanged && world.renderableObjects(1)[0].moving,
         "MoveObject interpolation state wrong");
  expect(!world.applyRpc(makeMoveObject(12, 41.0f, -31.0f, 9.0f,
                                        51.0f, -31.0f, 9.0f, 1.0f,
                                        -1000.0f, -1000.0f, -1000.0f, false), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.renderableObjects(1)[0].moving,
         "truncated MoveObject must fail closed");
  expect(world.applyRpc(makeStopObject(12), event) &&
         event.kind == samp::state::RpcEventKind::ObjectChanged && !world.renderableObjects(1)[0].moving,
         "StopObject state wrong");
  expect(world.applyRpc(makeCreateObject(13, 19380, 0.0f, 0.0f, 5.0f,
                                         0.0f, 0.0f, 0.0f, 0.0f, true, 12), event) &&
         world.serverObjectCount() == 3 && world.renderableObjects(8).size() == 2,
         "unresolved vehicle-attached CreateObject must remain hidden");
  expect(!world.applyRpc(makeObjectVec3Rpc(45, 99, 1.0f, 2.0f, 3.0f), event) &&
         world.serverObjectCount() == 3,
         "orphan SetObjectPosition must not create state");
  auto malformedObject = makeCreateObject(14, 19379, 1.0f, 2.0f, 3.0f,
                                          0.0f, 0.0f, 0.0f, 50.0f);
  malformedObject.pop_back();
  expect(!world.applyRpc(malformedObject, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.serverObjectCount() == 3,
         "truncated CreateObject must fail closed");
  expect(world.applyRpc(makeDestroyObject(12), event) &&
         event.kind == samp::state::RpcEventKind::ObjectDestroyed && world.serverObjectCount() == 2,
         "DestroyObject state wrong");

  // Source writer order for modern CreateObject is vehicle-parent then
  // object-parent. Attached renderer copies must follow bounded streamed
  // parents, preserve SyncRotation=false, and hide absent/cyclic parents
  // rather than freezing a child at its unrelated CreateObject transform.
  {
    samp::state::WorldState attachmentWorld;
    samp::state::RpcEvent attachmentEvent;
    expect(attachmentWorld.applyRpc(makePlayerInit(), attachmentEvent) &&
           attachmentWorld.applyRpc(makeVec3Rpc(12, 500.0f, 600.0f, 5.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makeFloatRpc(19, 90.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makeStreamInVehicle(44, 400, 300.0f, 400.0f, 10.0f, 90.0f),
                                    attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(30, 19379, 100.0f, 200.0f, 5.0f,
                                                      0.0f, 0.0f, 90.0f, 120.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(31, 19380, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 120.0f, true,
                                                      0xFFFFu, 30u,
                                                      0.0f, 2.0f, 3.0f,
                                                      0.0f, 0.0f, 10.0f, 1u), attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(32, 19381, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 22.0f, 120.0f, true,
                                                      0xFFFFu, 30u,
                                                      1.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 88.0f, 0u), attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(40, 19382, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 120.0f, true,
                                                      44u, 0xFFFFu,
                                                      0.0f, 2.0f, 3.0f,
                                                      0.0f, 0.0f, 15.0f, 1u), attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(42, 19383, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 120.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makeAttachObjectToPlayer(42, 42,
                                                              0.0f, 2.0f, 3.0f,
                                                              0.0f, 0.0f, 15.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makePlayerStreamIn(7, 0, 7, 700.0f, 800.0f, 5.0f,
                                                        90.0f, 0xAABBCCDDu, 0), attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(43, 19384, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 120.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makeAttachObjectToPlayer(43, 7,
                                                              0.0f, 2.0f, 3.0f,
                                                              0.0f, 0.0f, 15.0f), attachmentEvent),
           "attachment fixture lifecycle could not be retained");
    const auto findObject = [](const std::vector<samp::state::ServerObjectView>& views,
                               uint16_t id) -> const samp::state::ServerObjectView* {
      const auto found = std::find_if(views.begin(), views.end(),
                                      [id](const samp::state::ServerObjectView& view) {
                                        return view.id == id;
                                      });
      return found == views.end() ? nullptr : &*found;
    };
    auto attachedViews = attachmentWorld.renderableObjects(16);
    const auto* objectChild = findObject(attachedViews, 31);
    const auto* noSyncChild = findObject(attachedViews, 32);
    const auto* vehicleChild = findObject(attachedViews, 40);
    const auto* localPlayerChild = findObject(attachedViews, 42);
    const auto* remotePlayerChild = findObject(attachedViews, 43);
    expect(objectChild && noSyncChild && vehicleChild && localPlayerChild && remotePlayerChild &&
           std::fabs(objectChild->x - 102.0f) < 0.001f &&
           std::fabs(objectChild->y - 200.0f) < 0.001f &&
           std::fabs(objectChild->z - 8.0f) < 0.001f &&
           std::fabs(objectChild->rotationZ - 100.0f) < 0.001f &&
           std::fabs(noSyncChild->x - 100.0f) < 0.001f &&
           std::fabs(noSyncChild->y - 199.0f) < 0.001f &&
           std::fabs(noSyncChild->rotationZ - 22.0f) < 0.001f &&
           std::fabs(vehicleChild->x - 302.0f) < 0.001f &&
           std::fabs(vehicleChild->y - 400.0f) < 0.001f &&
           std::fabs(vehicleChild->z - 13.0f) < 0.001f &&
           std::fabs(vehicleChild->rotationZ - 105.0f) < 0.001f &&
           std::fabs(localPlayerChild->x - 502.0f) < 0.001f &&
           std::fabs(localPlayerChild->y - 600.0f) < 0.001f &&
           std::fabs(localPlayerChild->z - 8.0f) < 0.001f &&
           std::fabs(localPlayerChild->rotationZ - 105.0f) < 0.001f &&
           std::fabs(remotePlayerChild->x - 702.0f) < 0.001f &&
           std::fabs(remotePlayerChild->y - 800.0f) < 0.001f &&
           std::fabs(remotePlayerChild->z - 8.0f) < 0.001f &&
           std::fabs(remotePlayerChild->rotationZ - 105.0f) < 0.001f,
           "attached object transform/sync-rotation projection changed");
    samp::state::LocalObjectAttachmentAnchor predictedLocalAnchor;
    predictedLocalAnchor.valid = true;
    predictedLocalAnchor.x = 520.0f;
    predictedLocalAnchor.y = 620.0f;
    predictedLocalAnchor.z = 5.0f;
    predictedLocalAnchor.angle = 270.0f;
    const auto predictedAttachmentViews = attachmentWorld.renderableObjects(16, &predictedLocalAnchor);
    const auto* predictedLocalChild = findObject(predictedAttachmentViews, 42);
    expect(predictedLocalChild && std::fabs(predictedLocalChild->x - 518.0f) < 0.001f &&
           std::fabs(predictedLocalChild->y - 620.0f) < 0.001f &&
           std::fabs(predictedLocalChild->z - 8.0f) < 0.001f &&
           std::fabs(predictedLocalChild->rotationZ - 285.0f) < 0.001f,
           "local player attachment must follow only the copied prediction anchor");
    expect(attachmentWorld.applyRpc(makeSetPlayerAttachedObject(
               7, 3, true, 18637u, 2u, 1.0f, 2.0f, 3.0f,
               4.0f, 5.0f, 15.0f, 1.5f, -2.0f, 0.5f,
               0x11223344u, 0x55667788u), attachmentEvent) &&
           attachmentEvent.kind == samp::state::RpcEventKind::PlayerAttachmentChanged,
           "SetPlayerAttachedObject create must retain a known player slot");
    const auto findPlayerAttachment = [](
        const std::vector<samp::state::PlayerAttachedObjectView>& views,
        uint16_t playerId, uint8_t slot) -> const samp::state::PlayerAttachedObjectView* {
      const auto found = std::find_if(views.begin(), views.end(),
                                      [playerId, slot](const samp::state::PlayerAttachedObjectView& view) {
                                        return view.playerId == playerId && view.slot == slot;
                                      });
      return found == views.end() ? nullptr : &*found;
    };
    auto playerAttachmentViews = attachmentWorld.renderablePlayerAttachedObjects(16);
    const auto* remoteAccessory = findPlayerAttachment(playerAttachmentViews, 7, 3);
    expect(remoteAccessory && remoteAccessory->model == 18637u && remoteAccessory->bone == 2u &&
           std::fabs(remoteAccessory->x - 702.0f) < 0.001f &&
           std::fabs(remoteAccessory->y - 799.0f) < 0.001f &&
           std::fabs(remoteAccessory->z - 9.62f) < 0.001f &&
           std::fabs(remoteAccessory->rotationX - 4.0f) < 0.001f &&
           std::fabs(remoteAccessory->rotationY - 5.0f) < 0.001f &&
           std::fabs(remoteAccessory->rotationZ - 105.0f) < 0.001f &&
           std::fabs(remoteAccessory->scaleX - 1.5f) < 0.001f &&
           std::fabs(remoteAccessory->scaleY + 2.0f) < 0.001f &&
           std::fabs(remoteAccessory->scaleZ - 0.5f) < 0.001f &&
           remoteAccessory->colour1Abgr == 0x11223344u &&
           remoteAccessory->colour2Abgr == 0x55667788u,
           "player attachment must expose sampled parent pose, source scale, and ABGR colors");
    expect(!attachmentWorld.applyRpc(makeSetPlayerAttachedObject(99, 0, true), attachmentEvent) &&
           !attachmentWorld.applyRpc(makeSetPlayerAttachedObject(7, 10, true), attachmentEvent) &&
           !attachmentWorld.applyRpc(makeSetPlayerAttachedObject(7, 4, true, 18637u, 2u,
                                                                  0.0f, 0.0f, 0.0f,
                                                                  0.0f, 0.0f, 0.0f,
                                                                  0.0f, 1.0f, 1.0f), attachmentEvent),
           "unknown, out-of-range, or degenerate player attachments must fail closed");
    expect(attachmentWorld.applyRpc(makeObjectVec3Rpc(45, 30, 120.0f, 200.0f, 5.0f),
                                    attachmentEvent) &&
           attachmentWorld.applyRpc(makeObjectVec3Rpc(46, 30, 0.0f, 0.0f, 180.0f),
                                    attachmentEvent) &&
           attachmentWorld.applyRpc(makeObjectVec3Rpc(159, 44, 310.0f, 410.0f, 10.0f),
                                    attachmentEvent) &&
           attachmentWorld.applyRpc(makeVehicleFloatRpc(160, 44, 180.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makeVec3Rpc(12, 510.0f, 610.0f, 5.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makeFloatRpc(19, 180.0f), attachmentEvent) &&
           attachmentWorld.applyRpc(makePlayerStreamIn(7, 0, 7, 700.0f, 800.0f, 5.0f,
                                                        180.0f, 0xAABBCCDDu, 0), attachmentEvent),
           "attached parent mutations could not be retained");
    attachedViews = attachmentWorld.renderableObjects(16);
    objectChild = findObject(attachedViews, 31);
    noSyncChild = findObject(attachedViews, 32);
    vehicleChild = findObject(attachedViews, 40);
    localPlayerChild = findObject(attachedViews, 42);
    remotePlayerChild = findObject(attachedViews, 43);
    expect(objectChild && noSyncChild && vehicleChild && localPlayerChild && remotePlayerChild &&
           std::fabs(objectChild->x - 120.0f) < 0.001f &&
           std::fabs(objectChild->y - 198.0f) < 0.001f &&
           std::fabs(objectChild->rotationZ - 190.0f) < 0.001f &&
           std::fabs(noSyncChild->x - 119.0f) < 0.001f &&
           std::fabs(noSyncChild->y - 200.0f) < 0.001f &&
           std::fabs(noSyncChild->rotationZ - 22.0f) < 0.001f &&
           std::fabs(vehicleChild->x - 310.0f) < 0.001f &&
           std::fabs(vehicleChild->y - 408.0f) < 0.001f &&
           std::fabs(vehicleChild->rotationZ - 195.0f) < 0.001f &&
           std::fabs(localPlayerChild->x - 510.0f) < 0.001f &&
           std::fabs(localPlayerChild->y - 608.0f) < 0.001f &&
           std::fabs(localPlayerChild->z - 8.0f) < 0.001f &&
           std::fabs(localPlayerChild->rotationZ - 195.0f) < 0.001f &&
           std::fabs(remotePlayerChild->x - 700.0f) < 0.001f &&
           std::fabs(remotePlayerChild->y - 798.0f) < 0.001f &&
           std::fabs(remotePlayerChild->z - 8.0f) < 0.001f &&
           std::fabs(remotePlayerChild->rotationZ - 195.0f) < 0.001f,
           "attached children must follow authoritative parent corrections");
    playerAttachmentViews = attachmentWorld.renderablePlayerAttachedObjects(16);
    remoteAccessory = findPlayerAttachment(playerAttachmentViews, 7, 3);
    expect(remoteAccessory && std::fabs(remoteAccessory->x - 699.0f) < 0.001f &&
           std::fabs(remoteAccessory->y - 798.0f) < 0.001f &&
           std::fabs(remoteAccessory->z - 9.62f) < 0.001f &&
           std::fabs(remoteAccessory->rotationZ - 195.0f) < 0.001f,
           "player attachment must follow later authoritative parent corrections");
    expect(attachmentWorld.applyRpc(makeSetPlayerAttachedObject(7, 3, false), attachmentEvent) &&
           attachmentEvent.kind == samp::state::RpcEventKind::PlayerAttachmentChanged &&
           !findPlayerAttachment(attachmentWorld.renderablePlayerAttachedObjects(16), 7, 3),
           "RemovePlayerAttachedObject must clear only its retained slot");
    const size_t beforeOrphanPlayerAttachment = attachmentWorld.serverObjectCount();
    expect(!attachmentWorld.applyRpc(makeAttachObjectToPlayer(999, 42,
                                                               0.0f, 0.0f, 0.0f,
                                                               0.0f, 0.0f, 0.0f), attachmentEvent) &&
           attachmentWorld.serverObjectCount() == beforeOrphanPlayerAttachment,
           "orphan AttachObjectToPlayer must not allocate a renderer object");
    expect(attachmentWorld.applyRpc(makeCreateObject(41, 19383, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 50.0f, true,
                                                      0xFFFFu, 99u), attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(50, 19384, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 50.0f, true,
                                                      0xFFFFu, 51u), attachmentEvent) &&
           attachmentWorld.applyRpc(makeCreateObject(51, 19385, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 50.0f, true,
                                                      0xFFFFu, 50u), attachmentEvent),
           "unresolved/cyclic attachment records should remain retained");
    attachedViews = attachmentWorld.renderableObjects(16);
    expect(!findObject(attachedViews, 41) && !findObject(attachedViews, 50) &&
           !findObject(attachedViews, 51),
           "unresolved/cyclic attachment must not leak a stale renderer transform");
    bool deepAttachmentAccepted = attachmentWorld.applyRpc(
        makeCreateObject(70, 19387, 600.0f, 700.0f, 5.0f,
                         0.0f, 0.0f, 0.0f, 50.0f), attachmentEvent);
    for (uint16_t id = 71; deepAttachmentAccepted && id <= 79; ++id) {
      deepAttachmentAccepted = attachmentWorld.applyRpc(
          makeCreateObject(id, 19387, 0.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 50.0f, true,
                           0xFFFFu, static_cast<uint16_t>(id - 1u)), attachmentEvent);
    }
    expect(deepAttachmentAccepted, "deep attachment records should remain retained");
    attachedViews = attachmentWorld.renderableObjects(32);
    expect(!findObject(attachedViews, 79),
           "over-deep attachment must not recurse into a renderer frame");
    const size_t attachmentCount = attachmentWorld.serverObjectCount();
    expect(!attachmentWorld.applyRpc(makeCreateObject(60, 19386, 0.0f, 0.0f, 0.0f,
                                                       0.0f, 0.0f, 0.0f, 50.0f, true,
                                                       44u, 30u), attachmentEvent) &&
           attachmentEvent.kind == samp::state::RpcEventKind::Malformed &&
           attachmentWorld.serverObjectCount() == attachmentCount,
           "dual attachment parents must fail closed");
    expect(!attachmentWorld.applyRpc(makeCreateObject(61, 19386, 0.0f, 0.0f, 0.0f,
                                                       0.0f, 0.0f, 0.0f, 50.0f, true,
                                                       44u, 0xFFFFu,
                                                       0.0f, 0.0f, 0.0f,
                                                       0.0f, 0.0f, 0.0f, 2u), attachmentEvent) &&
           attachmentEvent.kind == samp::state::RpcEventKind::Malformed &&
           attachmentWorld.serverObjectCount() == attachmentCount,
           "invalid attachment SyncRotation must fail closed");
  }

  expect(world.applyRpc(makeCreatePickup(75, 1240, 2, 10.0f, 11.0f, 4.0f), event) &&
         event.kind == samp::state::RpcEventKind::PickupCreated && world.pickupCount() == 1,
         "CreatePickup state wrong");
  const auto pickups = world.renderablePickups(8);
  const auto nearestPickup = world.nearestPickup(10.5f, 11.0f, 4.0f, 2.0f);
  expect(pickups.size() == 1 && pickups[0].id == 75 && pickups[0].model == 1240 &&
         pickups[0].spawnType == 2 && nearestPickup && nearestPickup->id == 75,
         "pickup renderer/nearby view changed");
  expect(!world.applyRpc(makeCreatePickup(76, 0, 2, 10.0f, 11.0f, 4.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.pickupCount() == 1,
         "invalid CreatePickup must fail closed");
  expect(world.applyRpc(makeDestroyPickup(75), event) &&
         event.kind == samp::state::RpcEventKind::PickupDestroyed && world.pickupCount() == 0 &&
         world.renderablePickups(8).empty() && world.nearestPickup(10.0f, 11.0f, 4.0f, 2.0f) == nullptr,
         "DestroyPickup state wrong");

  expect(world.applyRpc(makeShowTextDraw(7, "Klik", true), event) &&
         event.kind == samp::state::RpcEventKind::TextDrawChanged && world.textDrawCount() == 1,
         "ShowTextDraw state wrong");
  const auto textDraws = world.renderableTextDraws(8);
  expect(textDraws.size() == 1 && textDraws[0].wireId == 7 && textDraws[0].useBox &&
         textDraws[0].selectable && textDraws[0].text == "Klik" &&
         std::fabs(textDraws[0].x - 120.0f) < 0.001f,
         "textdraw renderer view changed");
  expect(world.applyRpc(makeTextDrawSelection(true, 0x33AA77FFu), event) &&
         event.kind == samp::state::RpcEventKind::TextDrawSelectionChanged &&
         world.textDrawSelectionActive() && world.textDrawSelectionColourRgba() == 0x33AA77FFu &&
         world.isSelectableTextDraw(7),
         "TextDraw selection state wrong");
  expect(world.applyRpc(makeTextDrawSetString(7, "Pilih"), event) &&
         event.kind == samp::state::RpcEventKind::TextDrawChanged &&
         world.renderableTextDraws(8)[0].text == "Pilih",
         "TextDrawSetString state wrong");
  expect(world.applyRpc(makeShowTextDraw(8, "", false, false, 5, 18631,
                                         18.0f, -36.0f, 91.0f, 1.35f,
                                         -7, 12), event) &&
         world.renderableTextDraws(8).size() == 2 &&
         world.renderableTextDraws(8)[1].style == 5 &&
         world.renderableTextDraws(8)[1].model == 18631 &&
         std::fabs(world.renderableTextDraws(8)[1].rotationY + 36.0f) < 0.001f &&
         std::fabs(world.renderableTextDraws(8)[1].zoom - 1.35f) < 0.001f &&
         world.renderableTextDraws(8)[1].colour1 == -7 &&
         world.renderableTextDraws(8)[1].colour2 == 12,
         "font-5 TextDraw preview metadata must cross the bounded renderer view intact");
  auto malformedTextDraw = makeShowTextDraw(9, "bad");
  malformedTextDraw.pop_back();
  expect(!world.applyRpc(malformedTextDraw, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.textDrawCount() == 2,
         "truncated ShowTextDraw must fail closed");
  expect(world.applyRpc(makeHideTextDraw(7), event) &&
         event.kind == samp::state::RpcEventKind::TextDrawChanged && world.textDrawCount() == 1 &&
         !world.isSelectableTextDraw(7) && world.applyRpc(makeHideTextDraw(8), event) &&
         world.textDrawCount() == 0,
         "HideTextDraw state wrong");
  expect(world.applyRpc(makeTextDrawSelection(false), event) &&
         !world.textDrawSelectionActive(), "TextDraw selection disable wrong");

  expect(world.applyRpc(makeShowTextLabel(17, 0x22CC88FFu, 12.0f, -4.0f, 6.0f, 75.0f), event) &&
         event.kind == samp::state::RpcEventKind::TextLabelChanged && world.textLabelCount() == 1,
         "PlayerShowTextLabel state wrong");
  const auto labels = world.renderableTextLabels(8);
  expect(labels.size() == 1 && labels[0].wireId == 17 && !labels[0].playerTextLabel &&
         labels[0].rgba == 0x22CC88FFu && std::fabs(labels[0].x - 12.0f) < 0.001f &&
         std::fabs(labels[0].drawDistance - 75.0f) < 0.001f && !labels[0].los &&
         labels[0].playerAttachId == 0xFFFFu && labels[0].text == "Hello",
         "text-label visual snapshot/decompression changed");
  expect(world.applyRpc(makeShowTextLabel(1024, 0xFFFFFFFFu, 0.0f, 0.0f, 1.0f, 30.0f,
                                           true, 7, 0xFFFFu), event) &&
         world.textLabelCount() == 2 && world.renderableTextLabels(8)[1].playerTextLabel &&
         world.renderableTextLabels(8)[1].los && world.renderableTextLabels(8)[1].playerAttachId == 7,
         "player text-label namespace/attachment changed");
  auto malformedTextLabel = makeShowTextLabel(18, 0xFFFFFFFFu, 1.0f, 2.0f, 3.0f, 10.0f);
  malformedTextLabel.pop_back();
  expect(!world.applyRpc(malformedTextLabel, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.textLabelCount() == 2,
         "truncated PlayerShowTextLabel must fail closed");
  auto trailingTextLabel = makeShowTextLabel(18, 0xFFFFFFFFu, 1.0f, 2.0f, 3.0f, 10.0f);
  trailingTextLabel.push_back(0);
  expect(!world.applyRpc(trailingTextLabel, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.textLabelCount() == 2,
         "PlayerShowTextLabel trailing payload must fail closed");
  expect(!world.applyRpc(makeShowTextLabel(2048, 0xFFFFFFFFu, 1.0f, 2.0f, 3.0f, 10.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.textLabelCount() == 2,
         "out-of-range PlayerShowTextLabel ID must be rejected");
  expect(world.applyRpc(makeHideTextLabel(17), event) &&
         event.kind == samp::state::RpcEventKind::TextLabelChanged && world.textLabelCount() == 1 &&
         world.renderableTextLabels(8)[0].wireId == 1024,
         "PlayerHideTextLabel state wrong");
  expect(world.applyRpc(makeHideTextLabel(17), event) && world.textLabelCount() == 1,
         "PlayerHideTextLabel must remain idempotent");

  expect(world.applyRpc(makeSetPlayerMapIcon(4, 88.0f, -12.0f, 9.0f, 19,
                                              0x66BBEEFFu, 3), event) &&
         event.kind == samp::state::RpcEventKind::MapIconChanged && world.mapIconCount() == 1,
         "SetPlayerMapIcon state wrong");
  const auto mapIcons = world.renderableMapIcons(8);
  expect(mapIcons.size() == 1 && mapIcons[0].id == 4 && mapIcons[0].type == 19 &&
         mapIcons[0].style == 3 && mapIcons[0].rgba == 0x66BBEEFFu &&
         std::fabs(mapIcons[0].x - 88.0f) < 0.001f,
         "map-icon renderer snapshot changed");
  expect(world.applyRpc(makeSetPlayerMapIcon(4, 99.0f, -12.0f, 9.0f, 7,
                                              0xFFFFFFFFu, 0), event) &&
         world.mapIconCount() == 1 && std::fabs(world.renderableMapIcons(8)[0].x - 99.0f) < 0.001f,
         "SetPlayerMapIcon slot replacement wrong");
  auto malformedMapIcon = makeSetPlayerMapIcon(5, 1.0f, 2.0f, 3.0f, 1, 0xFFFFFFFFu, 0);
  malformedMapIcon.pop_back();
  expect(!world.applyRpc(malformedMapIcon, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.mapIconCount() == 1,
         "truncated SetPlayerMapIcon must fail closed");
  expect(world.applyRpc(makeRemovePlayerMapIcon(4), event) &&
         event.kind == samp::state::RpcEventKind::MapIconChanged && world.mapIconCount() == 0,
         "RemovePlayerMapIcon state wrong");
  expect(world.applyRpc(makeRemovePlayerMapIcon(4), event) && world.mapIconCount() == 0,
         "RemovePlayerMapIcon must remain idempotent");

  expect(world.applyRpc(makeGameText(3, 2500, "~g~GO"), event) &&
         event.kind == samp::state::RpcEventKind::GameTextChanged && world.gameText().active &&
         world.gameText().style == 3 && world.gameText().durationMs == 2500 &&
         world.gameText().text == "~g~GO",
         "SendGameText state wrong");
  expect(world.applyRpc(makeGameText(0, 0, ""), event) &&
         event.kind == samp::state::RpcEventKind::GameTextChanged && !world.gameText().active,
         "zero-duration GameText must hide portable state");
  auto malformedGameText = makeGameText(1, 1000, "bad");
  malformedGameText.pop_back();
  expect(!world.applyRpc(malformedGameText, event) &&
         event.kind == samp::state::RpcEventKind::Malformed && !world.gameText().active,
         "truncated GameText must fail closed");

  expect(world.applyRpc(makeShowDialog(17, 1, "Login", "Masuk", "Batal"), event) &&
         event.kind == samp::state::RpcEventKind::DialogChanged && world.dialog().active &&
         world.dialog().id == 17 && world.dialog().style == 1 &&
         world.dialog().title == "Login" && world.dialog().button1 == "Masuk" &&
         world.dialog().button2 == "Batal" && world.dialog().body == "Hello" &&
         world.dialog().revision != 0,
         "ShowDialog state/decompression wrong");
  const uint64_t dialogRevision = world.dialog().revision;
  expect(!world.acknowledgeDialogResponse(17, dialogRevision + 1),
         "stale dialog revision must be rejected");
  expect(world.acknowledgeDialogResponse(17, dialogRevision) && !world.dialog().active &&
         world.dialog().revision != dialogRevision,
         "queued dialog response must close matching dialog");
  expect(!world.acknowledgeDialogResponse(17, dialogRevision),
         "stale dialog response must be rejected");
  expect(world.applyRpc(makeShowDialog(18, 2, "Pilih", "OK", ""), event) && world.dialog().active,
         "second ShowDialog rejected");
  const uint64_t visibleDialogRevision = world.dialog().revision;
  auto malformedDialog = makeShowDialog(19, 0, "Rusak", "OK", "");
  malformedDialog.pop_back();
  expect(!world.applyRpc(malformedDialog, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.dialog().active && world.dialog().id == 18 && world.dialog().revision == visibleDialogRevision,
         "malformed ShowDialog must not replace visible dialog");
  expect(world.applyRpc(makeShowDialog(-1, 0, "", "", ""), event) &&
         event.kind == samp::state::RpcEventKind::DialogChanged && !world.dialog().active,
         "ShowDialog close state wrong");

  expect(world.applyRpc(makeSetSpawnInfo(4, 411, 100.0f, -25.5f, 8.0f, -90.0f), event),
         "WorldState rejected SetSpawnInfo");
  const auto& localSpawn = world.localPlayer();
  expect(event.kind == samp::state::RpcEventKind::SpawnInfoChanged && event.localFacingUpdated &&
         event.localPositionUpdated && !event.localVelocityUpdated &&
         localSpawn.spawnInfoKnown && localSpawn.positionKnown && localSpawn.facingKnown &&
         localSpawn.team == 4 && localSpawn.model == 411 &&
         std::fabs(localSpawn.x - 100.0f) < 0.001f &&
         std::fabs(localSpawn.y + 25.5f) < 0.001f &&
         std::fabs(localSpawn.z - 8.0f) < 0.001f &&
         std::fabs(localSpawn.angle - 270.0f) < 0.001f &&
         localSpawn.spawnWeapons[0] == 24 && localSpawn.spawnAmmo[1] == 50 &&
         localSpawn.weaponAmmoKnown[24] && localSpawn.weaponAmmo[24] == 100 &&
         localSpawn.weaponAmmoKnown[25] && localSpawn.weaponAmmo[25] == 50,
         "SetSpawnInfo local state/ammo facts wrong");
  auto invalidSpawnWeapon = makeSetSpawnInfo(4, 411, 100.0f, -25.5f, 8.0f, -90.0f);
  invalidSpawnWeapon[24] = 47;  // first raw u32 spawn weapon begins after fixed header/floats
  expect(!world.applyRpc(invalidSpawnWeapon, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.localPlayer().weaponAmmo[24] == 100,
         "out-of-range SetSpawnInfo weapon must fail closed");

  expect(world.applyRpc(makeVec3Rpc(12, 1.0f, 2.0f, 3.0f), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged && event.localPositionUpdated &&
         !event.localVelocityUpdated && world.localPlayer().positionKnown && !world.localPlayer().positionFindZ &&
         std::fabs(world.localPlayer().x - 1.0f) < 0.001f,
         "SetPlayerPosition local state wrong");
  expect(world.applyRpc(makeVec3Rpc(13, 4.0f, 5.0f, 6.0f), event) &&
         event.localPositionUpdated && world.localPlayer().positionFindZ &&
         std::fabs(world.localPlayer().z - 6.0f) < 0.001f,
         "SetPlayerPositionFindZ local state wrong");
  expect(world.applyRpc(makeByteRpc(15, 0), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged &&
         world.localPlayer().controllableKnown && !world.localPlayer().controllable,
         "TogglePlayerControllable off state wrong");
  expect(world.applyRpc(makeByteRpc(15, 1), event) && world.localPlayer().controllable,
         "TogglePlayerControllable on state wrong");
  expect(!world.applyRpc(makeByteRpc(15, 2), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.localPlayer().controllable,
         "invalid TogglePlayerControllable must be rejected");
  expect(world.applyRpc(makeFloatRpc(19, 450.0f), event) && event.localFacingUpdated &&
         world.localPlayer().facingKnown && std::fabs(world.localPlayer().angle - 90.0f) < 0.001f,
         "SetPlayerFacingAngle local state wrong");
  expect(world.applyRpc(makeFloatRpc(14, 87.5f), event) && world.localPlayer().healthKnown &&
         std::fabs(world.localPlayer().health - 87.5f) < 0.001f,
         "SetPlayerHealth local state wrong");
  expect(world.applyRpc(makeFloatRpc(14, 0.0f), event) && world.localPlayer().healthKnown &&
         world.localPlayer().health == 0.0f &&
         world.applyRpc(makeFloatRpc(14, 87.5f), event),
         "SetPlayerHealth zero must remain an authoritative retained fact");
  expect(world.applyRpc(makeFloatRpc(66, 25.0f), event) && world.localPlayer().armourKnown &&
         std::fabs(world.localPlayer().armour - 25.0f) < 0.001f,
         "SetPlayerArmour local state wrong");
  expect(world.applyRpc(makeByteRpc(88, 3), event) && world.localPlayer().specialActionKnown &&
         world.localPlayer().specialAction == 3,
         "SetPlayerSpecialAction local state wrong");
  expect(world.applyRpc(makeVec3Rpc(90, 3.0f, 4.0f, 0.0f), event) &&
         event.localVelocityUpdated && !event.localPositionUpdated &&
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

  world.beginClassRequest();
  expect(!world.classSelection().valid && !world.spawnResponseKnown() && !world.spawnAllowed(),
         "new class request must invalidate old class/spawn permission");
  expect(world.applyRpc(makeClassSelection(true, 3, 7), event),
         "class selection after request was rejected");
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
  auto malformedForceClass = makeEmptyRpc(74);
  malformedForceClass.push_back(0u);
  expect(!world.applyRpc(malformedForceClass, event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.classSelection().valid && !world.classSelectionForced() &&
         world.spawnResponseKnown() && world.spawnAllowed(),
         "ForcePlayerClassSelection with body must preserve class/spawn state");
  expect(world.applyRpc(makeEmptyRpc(74), event) &&
         event.kind == samp::state::RpcEventKind::ClassSelection &&
         !world.classSelection().valid && world.classSelectionForced() &&
         !world.spawnResponseKnown() && !world.spawnAllowed(),
         "source-verified ForcePlayerClassSelection must revoke stale spawn permission");

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
  const auto hudChat = world.recentChat(1000);
  const auto shortHudChat = world.recentChat(3);
  expect(hudChat.size() == 8 && shortHudChat.size() == 3 && world.recentChat(0).empty(),
         "renderer HUD chat tail must stay hard-capped");
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
  expect(world.applyRpc(makeShowPlayerNameTag(7, 0), event) &&
         event.kind == samp::state::RpcEventKind::NameTagChanged,
         "ShowPlayerNameTagForPlayer hide state wrong");
  const auto* player = world.findPlayer(7);
  expect(player && !player->nameTagVisible && player->name == "Darius" && player->rgba == 0xAABBCCDDu,
         "join player data wrong");
  expect(world.applyRpc(makeSetPlayerChatBubble(7, 0xCCEE88FFu, 55.0f, 15'000u, "Halo dekat"), event) &&
         event.kind == samp::state::RpcEventKind::ChatBubbleChanged && player->chatBubble.active &&
         player->chatBubble.text == "Halo dekat" && std::fabs(player->chatBubble.drawDistance - 55.0f) < 0.001f,
         "SetPlayerChatBubble remote state wrong");
  expect(world.applyRpc(makeSetPlayerChatBubble(42, 0xFFFFFFFFu, 25.0f, 10'000u, "Saya"), event) &&
         world.hasActiveLocalChatBubble() && world.localPlayer().chatBubble.text == "Saya",
         "SetPlayerChatBubble local state wrong");
  expect(world.applyRpc(makeSetPlayerChatBubble(42, 0u, 0.0f, 0u, ""), event) &&
         !world.hasActiveLocalChatBubble(),
         "zero-duration SetPlayerChatBubble must clear local state");
  expect(!world.applyRpc(makeSetPlayerChatBubble(7, 0u, -1.0f, 1u, "bad"), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "negative SetPlayerChatBubble distance must be rejected");
  expect(!world.applyRpc(makeSetPlayerChatBubble(7, 0u, 5.0f, 1u, std::string(129, 'x')), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "oversized SetPlayerChatBubble must be rejected");
  expect(world.applyRpc(makeScoresAndPings({{42, 125, 31}, {7, -2, 88}}), event) &&
         event.kind == samp::state::RpcEventKind::ScoreboardChanged &&
         world.localPlayer().scorePing.known && world.localPlayer().scorePing.score == 125 &&
         world.localPlayer().scorePing.ping == 31 && player->scorePing.known &&
         player->scorePing.score == -2 && player->scorePing.ping == 88 &&
         world.scoreboardPlayers().size() == 1 && world.scoreboardPlayers()[0].id == 7,
         "SendPlayerScoresAndPings state wrong");
  expect(!world.applyRpc(makeScoresAndPings({{7, 1, 1}, {7, 2, 2}}), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && player->scorePing.score == -2,
         "duplicate score roster IDs must fail closed");
  expect(world.applyRpc(makePlayerChatMessage(7, "Halo semua"), event) &&
         event.kind == samp::state::RpcEventKind::ChatMessage && event.playerId == 7 &&
         world.latestChat() && world.latestChat()->text == "Darius: Halo semua" &&
         world.latestChat()->rgba == 0xAABBCCDDu,
         "broadcast PlayerChatMessage state wrong");
  expect(world.applyRpc(makePlayerChatMessage(42, "Saya"), event) && world.latestChat() &&
         world.latestChat()->text == "YOU: Saya",
         "local PlayerChatMessage label changed");
  expect(!world.applyRpc(makePlayerChatMessage(7, std::string(129, 'x')), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "oversized PlayerChatMessage must be rejected");

  expect(world.applyRpc(makePlayerStreamIn(7, 9, 411, 90.0f, -80.0f, 12.0f, 450.0f,
                                            0x11223344u, 5), event),
         "WorldState rejected PlayerStreamIn");
  expect(event.kind == samp::state::RpcEventKind::PlayerStreamedIn && world.streamedPlayerCount() == 1 &&
         world.remoteOnFootCount() == 0 && world.findPlayer(7)->streamedIn &&
         world.findPlayer(7)->model == 411 && world.findPlayer(7)->team == 9 &&
         std::fabs(world.findPlayer(7)->x - 90.0f) < 0.001f &&
         std::fabs(world.findPlayer(7)->angle - 90.0f) < 0.001f,
         "PlayerStreamIn actor state wrong");
  expect(world.applyRpc(makeRemotePlayerDeath(42), event) &&
         event.kind == samp::state::RpcEventKind::Ignored && !world.findPlayer(7)->remoteWastedKnown,
         "observer RPC-166 must never become local death authority");
  expect(world.applyRpc(makeRemotePlayerDeath(7), event) &&
         event.kind == samp::state::RpcEventKind::RemotePlayerSync && world.findPlayer(7)->remoteWastedKnown &&
         world.renderablePlayers().size() == 1 && world.renderablePlayers()[0].wasted,
         "RPC-166 must produce only a short remote wasted presentation cue");
  expect(!world.applyRpc({20, 166}, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.findPlayer(7)->remoteWastedKnown,
         "truncated RPC-166 must preserve the remote wasted cue");
  expect(world.applyRpc(makePlayerStreamIn(7, 9, 411, 90.0f, -80.0f, 12.0f, 450.0f,
                                            0x11223344u, 5), event) &&
         !world.findPlayer(7)->remoteWastedKnown,
         "fresh remote stream lifecycle must clear stale wasted presentation");
  expect(world.applyRpc(makeApplyAnimation(86, 7, "PED", "WALK_PLAYER", 4.1f,
                                            true, true, false, false, 2'500u), event) &&
         event.kind == samp::state::RpcEventKind::RemotePlayerSync &&
         world.findPlayer(7)->forcedAnimation.active &&
         world.renderablePlayers()[0].forcedAnimation.name == "WALK_PLAYER" &&
         world.renderablePlayers()[0].forcedAnimation.durationMs == 2'500u,
         "ApplyPlayerAnimation must retain only a streamed remote presentation directive");
  const uint64_t remoteAnimationRevision = world.findPlayer(7)->forcedAnimation.revision;
  auto malformedRemoteAnimation = makeApplyAnimation(86, 7, "PED", "../BAD", 4.1f,
                                                      false, false, false, false, 0);
  expect(!world.applyRpc(malformedRemoteAnimation, event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findPlayer(7)->forcedAnimation.revision == remoteAnimationRevision &&
         world.findPlayer(7)->forcedAnimation.name == "WALK_PLAYER",
         "unsafe ApplyPlayerAnimation token must fail closed and preserve prior directive");
  expect(!world.applyRpc(makeApplyAnimation(86, 7, std::string(16, 'A'), "IDLE", 4.1f,
                                             false, false, false, false, 0), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findPlayer(7)->forcedAnimation.revision == remoteAnimationRevision &&
         !world.applyRpc(makeApplyAnimation(86, 7, "PED", std::string(24, 'A'), 4.1f,
                                             false, false, false, false, 0), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "ApplyPlayerAnimation must honor audited HybridString<16>/<24> token capacities");
  expect(world.applyRpc(makeClearAnimation(87, 7), event) &&
         event.kind == samp::state::RpcEventKind::RemotePlayerSync &&
         !world.findPlayer(7)->forcedAnimation.active &&
         world.findPlayer(7)->forcedAnimation.revision != remoteAnimationRevision,
         "ClearPlayerTasks must clear only the remote forced-animation directive");
  expect(world.applyRpc(makeApplyAnimation(86, 42, "PED", "IDLE_CHAT", 4.1f,
                                            true, false, true, true, 0), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged &&
         world.localPlayer().forcedAnimation.active && world.localPlayer().forcedAnimation.lockY &&
         world.localPlayer().forcedAnimation.freeze &&
         world.applyRpc(makeClearAnimation(87, 42), event) &&
         !world.localPlayer().forcedAnimation.active,
         "local ApplyPlayerAnimation/ClearPlayerTasks must not alter physics while retaining lifecycle state");
  expect(world.applyRpc(makeApplyAnimation(86, 7, "PED", "IDLE_CHAT", 4.1f,
                                            false, false, false, false, 0), event) &&
         world.applyRpc(makePlayerStreamIn(7, 9, 411, 90.0f, -80.0f, 12.0f, 450.0f,
                                            0x11223344u, 5), event) &&
         !world.findPlayer(7)->forcedAnimation.active,
         "fresh remote stream lifecycle must clear stale forced-animation directives");
  expect(world.applyRpc(makeSetPlayerSkin(7, 23), event) &&
         event.kind == samp::state::RpcEventKind::PlayerSkinChanged && event.playerId == 7 &&
         world.findPlayer(7)->model == 23 && world.renderablePlayers().size() == 1 &&
         world.renderablePlayers()[0].model == 23,
         "non-DL SetPlayerSkin must replace the streamed remote actor model");
  expect(world.applyRpc(makeSetPlayerSkin(42, 29), event) &&
         event.kind == samp::state::RpcEventKind::LocalPlayerChanged && event.playerId == 42 &&
         world.localPlayer().model == 29 && world.localPlayer().spawnInfoKnown,
         "non-DL SetPlayerSkin must update the spawned local actor model");
  const auto remoteSkinBeforeMalformed = world.findPlayer(7)->model;
  auto malformedSkin = makeSetPlayerSkin(7, 24);
  malformedSkin.pop_back();
  expect(!world.applyRpc(malformedSkin, event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findPlayer(7)->model == remoteSkinBeforeMalformed,
         "truncated SetPlayerSkin must fail closed without changing actor model");
  expect(!world.applyRpc(makeSetPlayerSkin(0x1'0000u, 24), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findPlayer(7)->model == remoteSkinBeforeMalformed,
         "out-of-range SetPlayerSkin player ID must fail closed");
  expect(!world.applyRpc(makeSetPlayerSkin(7, 65'536u), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findPlayer(7)->model == remoteSkinBeforeMalformed,
         "out-of-range SetPlayerSkin model must fail closed");
  samp::syncdecode::RemoteAimSync remoteAim;
  expect(samp::syncdecode::decodeRemoteAimBroadcast(makeRemoteAimBroadcast(7), remoteAim) &&
         world.applyRemoteAim(remoteAim, event) && event.kind == samp::state::RpcEventKind::RemoteAimSync &&
         world.findPlayer(7)->remoteAimKnown,
         "WorldState rejected decoded remote PlayerAimSync");
  const auto aimRenderActors = world.renderablePlayers();
  expect(aimRenderActors.size() == 1 && aimRenderActors[0].aimKnown &&
         aimRenderActors[0].aimCameraMode == 53 && aimRenderActors[0].aimWeaponState == 2 &&
         std::fabs(aimRenderActors[0].aimCameraFrontY - 1.0f) < 0.001f &&
         std::fabs(aimRenderActors[0].aimCameraPositionZ - 3.75f) < 0.001f,
         "remote PlayerAimSync render snapshot changed");
  const auto validRemoteAim = remoteAim;
  remoteAim.cameraFrontY = 0.0f;
  expect(!world.applyRemoteAim(remoteAim, event) && world.findPlayer(7)->remoteAimKnown,
         "invalid remote PlayerAimSync must fail closed without clearing prior aim");
  remoteAim = validRemoteAim;
  samp::syncdecode::RemoteBulletSync remoteBullet;
  expect(samp::syncdecode::decodeRemoteBulletBroadcast(makeRemoteBulletBroadcast(7), remoteBullet) &&
         world.applyRemoteBullet(remoteBullet, event) &&
         event.kind == samp::state::RpcEventKind::RemoteBulletSync &&
         world.findPlayer(7)->remoteBulletKnown,
         "WorldState rejected decoded remote PlayerBulletSync");
  const auto bulletRenderActors = world.renderablePlayers();
  expect(bulletRenderActors.size() == 1 && bulletRenderActors[0].bulletKnown &&
         bulletRenderActors[0].bulletWeaponId == 24 &&
         std::fabs(bulletRenderActors[0].bulletOriginX - 1.5f) < 0.001f &&
         std::fabs(bulletRenderActors[0].bulletDirectionX - 10.0f) < 0.001f,
         "remote PlayerBulletSync renderer cue changed");
  const auto validRemoteBullet = remoteBullet;
  remoteBullet.weaponId = 35;
  expect(!world.applyRemoteBullet(remoteBullet, event) && world.findPlayer(7)->remoteBulletKnown,
         "invalid remote PlayerBulletSync must fail closed without clearing prior cue");
  remoteBullet = validRemoteBullet;
  auto noImpactBulletPayload = makeRemoteBulletBroadcast(7);
  // Packet-206 HitType=None may use all-zero HitPos as the compatible
  // no-impact sentinel. It must not create a renderer ray toward world origin.
  std::fill(noImpactBulletPayload.begin() + 18, noImpactBulletPayload.begin() + 30, 0u);
  expect(samp::syncdecode::decodeRemoteBulletBroadcast(noImpactBulletPayload, remoteBullet) &&
         world.applyRemoteBullet(remoteBullet, event) &&
         event.kind == samp::state::RpcEventKind::RemoteBulletSync &&
         !world.findPlayer(7)->remoteBulletKnown &&
         !world.renderablePlayers()[0].bulletKnown,
         "no-impact remote PlayerBulletSync must clear only the visual direction cue");
  remoteBullet = validRemoteBullet;
  samp::syncdecode::RemoteOnFootSync remote;
  expect(samp::syncdecode::decodeRemoteOnFootBroadcast(makeRemoteOnFootBroadcast(7), remote) &&
         world.applyRemoteOnFoot(remote, event),
         "WorldState rejected decoded remote PlayerFootSync");
  expect(event.kind == samp::state::RpcEventKind::RemotePlayerSync && world.remoteOnFootCount() == 1 &&
         world.findPlayer(7)->remoteOnFootKnown && std::fabs(world.findPlayer(7)->x - 100.5f) < 0.001f &&
         std::fabs(world.findPlayer(7)->velocityY - 4.0f) < 0.01f &&
         std::fabs(world.findPlayer(7)->angle - 45.0f) < 0.1f,
         "remote PlayerFootSync actor state/yaw wrong");
  const auto renderActors = world.renderablePlayers();
  expect(renderActors.size() == 1 && renderActors[0].id == 7 &&
         renderActors[0].name == "Darius" && renderActors[0].onFootKnown &&
         !renderActors[0].nameTagVisible && renderActors[0].chatBubbleActive &&
         renderActors[0].chatBubbleText == "Halo dekat" &&
         std::fabs(renderActors[0].x - 100.5f) < 0.001f,
         "bounded remote render snapshot changed");
  expect(world.applyRpc(makeShowPlayerNameTag(7, 1), event) &&
         world.renderablePlayers()[0].nameTagVisible,
         "ShowPlayerNameTagForPlayer show state wrong");
  const auto validRemote = remote;
  remote.positionX = 20000.0f;
  expect(!world.applyRemoteOnFoot(remote, event) &&
         std::fabs(world.findPlayer(7)->x - 100.5f) < 0.001f,
         "remote state reducer must reject invalid direct state");
  remote = validRemote;
  expect(world.applyRpc(makeStreamInVehicle(90, 411, 80.0f, -70.0f, 9.0f, 450.0f,
                                             12, 34, 875.0f, 2, 5), event) &&
         event.kind == samp::state::RpcEventKind::VehicleStreamedIn && world.vehicleCount() == 1 &&
         world.streamedVehicleCount() == 1,
         "StreamInVehicle state rejected");
  const auto renderVehicles = world.renderableVehicles();
  expect(renderVehicles.size() == 1 && renderVehicles[0].id == 90 &&
         renderVehicles[0].model == 411 && renderVehicles[0].colour1 == 12 &&
         renderVehicles[0].colour2 == 34 && renderVehicles[0].paintjob == 5 &&
         std::fabs(renderVehicles[0].x - 80.0f) < 0.001f &&
         !renderVehicles[0].attitudeKnown && std::fabs(renderVehicles[0].rotationX) < 0.001f &&
         std::fabs(renderVehicles[0].rotationY) < 0.001f &&
         std::fabs(renderVehicles[0].rotationZ - 90.0f) < 0.001f &&
         std::fabs(renderVehicles[0].angle - 90.0f) < 0.001f &&
         std::fabs(renderVehicles[0].health - 875.0f) < 0.001f,
         "StreamInVehicle render snapshot wrong");
  samp::syncdecode::RemoteVehicleSync vehicleRemote;
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteVehicleDriverBroadcast(7, 90), vehicleRemote) &&
         world.applyRemoteVehicle(vehicleRemote, event) &&
         event.kind == samp::state::RpcEventKind::RemoteVehicleSync,
         "WorldState rejected decoded PlayerVehicleSync");
  const auto driverVehicle = world.renderableVehicles();
  expect(driverVehicle.size() == 1 && driverVehicle[0].dynamicSyncKnown && driverVehicle[0].driverKnown &&
         driverVehicle[0].driverId == 7 && std::fabs(driverVehicle[0].x - 125.0f) < 0.001f &&
         driverVehicle[0].attitudeKnown && std::fabs(driverVehicle[0].rotationX) < 0.2f &&
         std::fabs(driverVehicle[0].rotationY) < 0.2f &&
         std::fabs(driverVehicle[0].rotationZ - 90.0f) < 0.2f &&
         std::fabs(driverVehicle[0].angle - 90.0f) < 0.2f &&
         std::fabs(driverVehicle[0].health - 875.0f) < 0.001f &&
         world.renderablePlayers().size() == 1 && world.renderablePlayers()[0].inVehicle &&
         world.renderablePlayers()[0].vehicleId == 90 && !world.renderablePlayers()[0].onFootKnown,
         "PlayerVehicleSync driver occupancy render state wrong");
  // Packet 211 has its own source-verified passenger layout. It anchors the
  // passenger without deleting the Packet-200 driver or making a seat offset.
  samp::syncdecode::RemotePassengerSync passengerRemote;
  expect(world.applyRpc(makePlayerJoin(0, 0xABCDEF01u, false, "Zero"), event) &&
         world.applyRpc(makePlayerStreamIn(0, 0, 0, 0.0f, 0.0f, 3.0f, 0.0f, 0, 0), event) &&
         samp::syncdecode::decodeRemotePassengerBroadcast(makeRemotePassengerBroadcast(0, 90, 3), passengerRemote) &&
         world.applyRemotePassenger(passengerRemote, event) &&
         event.kind == samp::state::RpcEventKind::RemotePassengerSync,
         "remote PassengerSync state rejected");
  const auto passengerActors = world.renderablePlayers();
  expect(world.renderableVehicles()[0].driverKnown && world.renderableVehicles()[0].driverId == 7 &&
         passengerActors.size() == 2 && passengerActors[0].id == 0 && passengerActors[0].inVehicle &&
         !passengerActors[0].vehicleDriver && passengerActors[0].vehicleId == 90 &&
         passengerActors[0].vehicleSeat == 3 && std::fabs(passengerActors[0].x - 121.0f) < 0.001f,
         "Packet-211 passenger must coexist with the remote driver");
  // Packet 209 clears only the driver slot. A still-synced Packet-211
  // passenger stays hidden from ped geometry but retains its packet anchor.
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteUnoccupiedVehicleBroadcast(8, 90), vehicleRemote) &&
         world.applyRemoteVehicle(vehicleRemote, event) && !world.renderableVehicles()[0].driverKnown &&
         world.findPlayer(0)->remoteVehicleKnown && !world.findPlayer(0)->remoteVehicleDriver &&
         world.findPlayer(0)->positionKnown && !world.findPlayer(7)->positionKnown,
         "PlayerUnoccupiedSync incorrectly dropped a remote passenger");
  // ID zero is a valid remote player ID. Occupancy must use driverKnown rather
  // than treating driverId == 0 as an unoccupied-vehicle sentinel.
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteVehicleDriverBroadcast(0, 90), vehicleRemote) &&
         world.applyRemoteVehicle(vehicleRemote, event),
         "zero-ID remote driver sync rejected");
  const auto zeroDriverVehicle = world.renderableVehicles();
  expect(zeroDriverVehicle.size() == 1 && zeroDriverVehicle[0].driverKnown &&
         zeroDriverVehicle[0].driverId == 0 && world.findPlayer(0)->remoteVehicleKnown &&
         world.findPlayer(0)->remoteVehicleDriver && world.findPlayer(0)->remoteVehicleId == 90 &&
         !world.findPlayer(7)->positionKnown,
         "valid zero-ID driver was confused with no driver");
  expect(samp::syncdecode::decodeRemoteOnFootBroadcast(makeRemoteOnFootBroadcast(0), remote) &&
         world.applyRemoteOnFoot(remote, event) && !world.renderableVehicles()[0].driverKnown &&
         world.findPlayer(0)->remoteOnFootKnown && !world.findPlayer(0)->remoteVehicleKnown,
         "Packet-207 must detach a prior vehicle driver");
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteVehicleDriverBroadcast(0, 90), vehicleRemote) &&
         world.applyRemoteVehicle(vehicleRemote, event),
         "zero-ID remote driver reattach rejected");
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteUnoccupiedVehicleBroadcast(8, 90), vehicleRemote) &&
         world.applyRemoteVehicle(vehicleRemote, event),
         "WorldState rejected decoded PlayerUnoccupiedSync");
  const auto unoccupiedVehicle = world.renderableVehicles();
  expect(unoccupiedVehicle.size() == 1 && unoccupiedVehicle[0].dynamicSyncKnown &&
         !unoccupiedVehicle[0].driverKnown && unoccupiedVehicle[0].driverId == 0 &&
         std::fabs(unoccupiedVehicle[0].x - 225.0f) < 0.001f &&
         // Packet-209 has no quaternion: retain the last Packet-200 DFF
         // attitude rather than treating raw direction as guessed Euler angles.
         unoccupiedVehicle[0].attitudeKnown && std::fabs(unoccupiedVehicle[0].rotationZ - 90.0f) < 0.2f &&
         std::fabs(unoccupiedVehicle[0].angle - 90.0f) < 0.2f &&
         std::fabs(unoccupiedVehicle[0].health - 640.0f) < 0.001f &&
         world.renderablePlayers().empty(),
         "PlayerUnoccupiedSync must clear stale driver geometry");
  const auto validVehicleRemote = vehicleRemote;
  vehicleRemote.x = 20000.0f;
  expect(!world.applyRemoteVehicle(vehicleRemote, event) &&
         std::fabs(world.renderableVehicles()[0].x - 225.0f) < 0.001f,
         "remote vehicle reducer must reject invalid direct state");
  vehicleRemote = validVehicleRemote;
  expect(!world.applyRpc(makeStreamInVehicle(91, 0, 0.0f, 0.0f, 3.0f, 0.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "invalid StreamInVehicle model must be rejected");
  expect(world.applyRpc(makeVehicleByteRpc(65, 90, 7), event) &&
         event.kind == samp::state::RpcEventKind::VehicleChanged &&
         world.applyRpc(makeVehicleFloatRpc(147, 90, 612.5f), event) &&
         world.applyRpc(makeObjectVec3Rpc(159, 90, 17.0f, 18.0f, 19.0f), event) &&
         world.applyRpc(makeVehicleFloatRpc(160, 90, 450.0f), event),
         "vehicle mutator RPCs rejected");
  const auto mutatedVehicle = world.renderableVehicles();
  expect(mutatedVehicle.size() == 1 && std::fabs(mutatedVehicle[0].x - 17.0f) < 0.001f &&
         std::fabs(mutatedVehicle[0].health - 612.5f) < 0.001f &&
         // RPC-160 is yaw-only, so it must clear a stale full driver attitude.
         !mutatedVehicle[0].attitudeKnown && std::fabs(mutatedVehicle[0].rotationX) < 0.001f &&
         std::fabs(mutatedVehicle[0].rotationY) < 0.001f &&
         std::fabs(mutatedVehicle[0].rotationZ - 90.0f) < 0.001f &&
         std::fabs(mutatedVehicle[0].angle - 90.0f) < 0.001f,
         "vehicle mutator state wrong");
  expect(!world.applyRpc(makeVehicleFloatRpc(147, 90, -1.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         std::fabs(world.renderableVehicles()[0].health - 612.5f) < 0.001f,
         "invalid vehicle health must fail closed");
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteVehicleDriverBroadcast(0, 90), vehicleRemote) &&
         world.applyRemoteVehicle(vehicleRemote, event) && world.renderableVehicles()[0].driverKnown &&
         world.applyRpc(makePlayerStreamOut(0), event) && !world.renderableVehicles()[0].driverKnown &&
         !world.findPlayer(0)->remoteVehicleKnown,
         "PlayerStreamOut must clear an attached vehicle driver");
  expect(samp::syncdecode::decodeRemoteVehicleBroadcast(makeRemoteVehicleDriverBroadcast(7, 90), vehicleRemote) &&
         world.applyRemoteVehicle(vehicleRemote, event) && world.findPlayer(7)->remoteVehicleKnown,
         "remote driver reattach before vehicle stream-out rejected");
  expect(world.applyRpc(makeStreamOutVehicle(90), event) &&
         event.kind == samp::state::RpcEventKind::VehicleStreamedOut &&
         world.streamedVehicleCount() == 0 && world.renderableVehicles().empty() &&
         !world.findPlayer(7)->remoteVehicleKnown && !world.findPlayer(7)->positionKnown,
         "StreamOutVehicle must clear attached driver geometry");
  expect(!world.applyRemoteVehicle(vehicleRemote, event),
         "orphan remote vehicle sync must be ignored");
  expect(!world.applyRemotePassenger(passengerRemote, event),
         "orphan remote passenger sync must be ignored");

  expect(world.applyRpc(makePlayerStreamOut(7), event) &&
         event.kind == samp::state::RpcEventKind::PlayerStreamedOut && world.streamedPlayerCount() == 0 &&
         world.remoteOnFootCount() == 0 && !world.findPlayer(7)->streamedIn &&
         !world.findPlayer(7)->remoteAimKnown && !world.findPlayer(7)->remoteBulletKnown,
         "PlayerStreamOut actor state wrong");
  // Packet 207 must not invent an entity after the stream lifecycle says it
  // is out; a later StreamIn will explicitly reactivate it.
  expect(!world.applyRemoteOnFoot(remote, event),
         "orphan remote PlayerFootSync must be ignored");
  expect(!world.applyRemoteAim(remoteAim, event),
         "orphan remote PlayerAimSync must be ignored");
  expect(!world.applyRemoteBullet(remoteBullet, event),
         "orphan remote PlayerBulletSync must be ignored");

  expect(world.applyRpc(makePlayerColor(7, 0x10203040u), event),
         "WorldState rejected SetPlayerColor");
  expect(world.findPlayer(7)->rgba == 0x10203040u, "color did not update");

  expect(world.applyRpc(makePlayerName(7, "Darius KG"), event),
         "WorldState rejected SetPlayerName");
  expect(world.findPlayer(7)->name == "Darius KG", "name did not update");

  expect(world.applyRpc(makePlayerQuit(0, 1), event), "WorldState rejected zero-ID PlayerQuit");
  expect(world.applyRpc(makePlayerQuit(7, 1), event), "WorldState rejected PlayerQuit");
  expect(event.kind == samp::state::RpcEventKind::PlayerQuit, "wrong quit event kind");
  expect(world.playerCount() == 0 && world.findPlayer(7) == nullptr,
         "quit did not remove player");
  // RPC 80 can precede a join/stream notification; preserve its bounded
  // per-ID decision rather than flashing an incorrect nametag for one frame.
  expect(world.applyRpc(makeShowPlayerNameTag(8, 0), event) &&
         world.applyRpc(makeScoresAndPings({{8, 22, 77}}), event) &&
         world.applyRpc(makePlayerJoin(8, 0x11223344u, false, "Ari"), event) &&
         world.findPlayer(8) && !world.findPlayer(8)->nameTagVisible &&
         world.findPlayer(8)->scorePing.known && world.findPlayer(8)->scorePing.score == 22,
         "pre-join name-tag/score override did not survive");
  expect(world.applyRpc(makePlayerQuit(8, 1), event) && world.playerCount() == 0,
         "pre-join name-tag cleanup failed");
}


void testSendDeathMessageReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;

  const auto exact = makeSendDeathMessage(42u, 7u, 24u);
  expect(exact == std::vector<uint8_t>({20u, 55u, 42u, 0u, 7u, 0u, 24u}) &&
         samp::rpc::summarize(exact).find("SendDeathMessage") != std::string::npos,
         "RPC-55 fixed kill-feed wire/decoder label changed");
  expect(world.applyRpc(exact, event) && event.kind == samp::state::RpcEventKind::DeathMessageChanged &&
         event.playerId == 7u && !world.localPlayer().healthKnown,
         "RPC-55 must create presentation feed only, not local death state");
  const auto first = world.recentDeathMessages();
  expect(first.size() == 1u && first[0].hasKiller && first[0].killerId == 42u &&
         first[0].victimId == 7u && first[0].reason == 24u && first[0].expiresAtMs != 0u,
         "RPC-55 retained kill-feed values changed");

  // INVALID_PLAYER_ID is the source writer's no-killer sentinel, not an
  // invalid packet and not a fake player/weapon state.
  expect(world.applyRpc(makeSendDeathMessage(0xFFFFu, 42u, 53u), event) &&
         event.kind == samp::state::RpcEventKind::DeathMessageChanged,
         "RPC-55 world/no-killer form rejected");
  const auto feed = world.recentDeathMessages();
  expect(feed.size() == 2u && !feed.back().hasKiller && feed.back().killerId == 0xFFFFu &&
         feed.back().victimId == 42u && feed.back().reason == 53u,
         "RPC-55 no-killer sentinel was not preserved exactly");

  auto trailing = exact;
  trailing.push_back(0u);
  auto shortBody = exact;
  shortBody.pop_back();
  expect(!world.applyRpc(trailing, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         !world.applyRpc(shortBody, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         !world.applyRpc(makeSendDeathMessage(3u, 0xFFFFu, 1u), event) &&
         event.kind == samp::state::RpcEventKind::Malformed && world.recentDeathMessages().size() == 2u,
         "RPC-55 malformed/trailing/invalid-victim input must fail atomically");

  // UI export has a fixed tail independent of a server feed flood; raw reason
  // remains opaque through all valid uint8 values.
  for (uint8_t index = 0; index < 12u; ++index) {
    expect(world.applyRpc(makeSendDeathMessage(index, static_cast<uint16_t>(100u + index),
                                               static_cast<uint8_t>(255u - index)), event),
           "RPC-55 bounded feed fixture rejected");
  }
  const auto bounded = world.recentDeathMessages(99u);
  expect(bounded.size() == 8u && bounded.back().reason == 244u &&
         bounded.back().victimId == 111u,
         "RPC-55 renderer feed must cap the newest opaque rows");
}


void testSetVehicleParamsReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;
  std::array<uint8_t, 16> initial{};
  initial.fill(0xFFu);
  initial[0] = 1u;  // engine
  initial[1] = 0u;  // lights
  initial[7] = 1u;  // siren
  const auto exact = makeSetVehicleParams(77u, initial);
  expect(exact.size() == 20u && exact[0] == 20u && exact[1] == 24u &&
         exact[2] == 77u && exact[3] == 0u && exact[4] == 1u &&
         exact[5] == 0u && exact[11] == 1u &&
         samp::rpc::summarize(exact).find("SetVehicleParams") != std::string::npos,
         "RPC-24 wire vector/decoder label changed");

  // An RPC-24 command cannot allocate a proxy or target an arbitrary raw ID.
  expect(!world.applyRpc(exact, event) && event.kind == samp::state::RpcEventKind::Ignored,
         "RPC-24 must reject an unknown/unstreamed vehicle");
  auto shortBody = exact;
  shortBody.pop_back();
  expect(!world.applyRpc(shortBody, event) && event.kind == samp::state::RpcEventKind::Malformed,
         "RPC-24 body must be exactly vehicle ID plus sixteen bytes");
  auto invalid = exact;
  invalid[4] = 2u;
  expect(!world.applyRpc(invalid, event) && event.kind == samp::state::RpcEventKind::Malformed,
         "RPC-24 values outside signed -1/0/1 must fail closed");
  invalid[4] = 0x80u;
  expect(!world.applyRpc(invalid, event) && event.kind == samp::state::RpcEventKind::Malformed,
         "RPC-24 malformed high bytes must not depend on signed-char conversion");

  expect(world.applyRpc(makeStreamInVehicle(77, 411, 10.0f, 20.0f, 5.0f, 90.0f), event),
         "RPC-24 fixture vehicle stream rejected");
  expect(world.applyRpc(exact, event) && event.kind == samp::state::RpcEventKind::VehicleChanged &&
         event.vehicleParametersUpdated && event.playerId == 77u && world.findVehicle(77) != nullptr &&
         world.findVehicle(77)->parameters.known &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Engine) == 1 &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Lights) == 0 &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Siren) == 1 &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Doors) == -1 &&
         !samp::state::isRecipientVehicleEntryLocked(*world.findVehicle(77)),
         "RPC-24 must decode raw 0xFF as signed explicit unset in its full snapshot");
  const auto presentation = world.renderableVehicles();
  const auto presented = std::find_if(presentation.begin(), presentation.end(),
                                      [](const samp::state::RemoteVehicleView& vehicle) {
                                        return vehicle.id == 77u;
                                      });
  expect(presented != presentation.end() && presented->parameters.known &&
         presented->parameters.value(samp::state::VehicleParameter::Engine) == 1 &&
         presented->parameters.value(samp::state::VehicleParameter::Siren) == 1,
         "RPC-24 facts must cross the bounded vehicle presentation snapshot");

  std::array<uint8_t, 16> partial{};
  partial.fill(0xFFu);
  partial[1] = 1u;
  expect(world.applyRpc(makeSetVehicleParams(77u, partial), event) &&
         event.kind == samp::state::RpcEventKind::VehicleChanged && event.vehicleParametersUpdated &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Engine) == -1 &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Lights) == 1 &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Siren) == -1,
         "RPC-24 must atomically replace prior facts, including explicit signed -1 unsets");
  std::array<uint8_t, 16> allUnset{};
  allUnset.fill(0xFFu);
  expect(world.applyRpc(makeSetVehicleParams(77u, allUnset), event) &&
         event.kind == samp::state::RpcEventKind::VehicleChanged && event.vehicleParametersUpdated &&
         world.findVehicle(77)->parameters.known &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Lights) == -1,
         "all-unset RPC-24 must commit an explicit full unset snapshot");
  expect(world.applyRpc(makeSetVehicleParams(77u, allUnset), event) &&
         event.kind == samp::state::RpcEventKind::Ignored && !event.vehicleParametersUpdated,
         "duplicate all-unset RPC-24 snapshot must not manufacture a vehicle change");

  // Even without an explicit StreamOut, a new StreamIn for the same raw ID is
  // a fresh server entity baseline. Both lifecycle paths must clear recipient
  // parameter facts before a later ID reuse.
  expect(world.applyRpc(makeStreamInVehicle(77, 451, 30.0f, 20.0f, 5.0f, 0.0f), event) &&
         !world.findVehicle(77)->parameters.known &&
         world.findVehicle(77)->parameters.value(samp::state::VehicleParameter::Engine) == -1,
         "RPC-24 state must clear on same-ID StreamIn lifecycle replacement");
  expect(world.applyRpc(makeSetVehicleParams(77u, initial), event) &&
         world.findVehicle(77)->parameters.known &&
         world.applyRpc(makeStreamOutVehicle(77u), event) && world.findVehicle(77) == nullptr &&
         world.applyRpc(makeStreamInVehicle(77, 411, 10.0f, 20.0f, 5.0f, 90.0f), event) &&
         !world.findVehicle(77)->parameters.known,
         "RPC-24 state must clear through StreamOut/raw-ID reuse");
}

void testSetVehicleParamsForPlayerReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;
  const auto exact = makeSetVehicleParamsForPlayer(91u, 1u, 1u);
  expect(exact == std::vector<uint8_t>({20u, 161u, 91u, 0u, 1u, 1u}) &&
         samp::rpc::summarize(exact).find("SetVehicleParamsForPlayer") != std::string::npos,
         "legacy RPC-161 vehicle parameter wire vector/decoder label changed");
  expect(!world.applyRpc(exact, event) && event.kind == samp::state::RpcEventKind::Ignored,
         "RPC-161 must not allocate an unstreamed vehicle proxy");
  auto shortBody = exact;
  shortBody.pop_back();
  expect(!world.applyRpc(shortBody, event) && event.kind == samp::state::RpcEventKind::Malformed,
         "RPC-161 must require exact u16/objective/door-lock body");
  expect(!world.applyRpc(makeSetVehicleParamsForPlayer(91u, 2u, 0u), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "RPC-161 invalid objective bit must fail closed");
  expect(!world.applyRpc(makeSetVehicleParamsForPlayer(91u, 0u, 2u), event) &&
         event.kind == samp::state::RpcEventKind::Malformed,
         "RPC-161 invalid door-lock bit must fail closed");

  expect(world.applyRpc(makeStreamInVehicle(91, 411, 10.0f, 20.0f, 5.0f, 90.0f), event),
         "RPC-161 fixture vehicle stream rejected");
  std::array<uint8_t, 16> params{};
  params.fill(0xFFu);
  params[0] = 1u;
  params[1] = 1u;
  params[7] = 1u;
  expect(world.applyRpc(makeSetVehicleParams(91u, params), event),
         "RPC-161 baseline RPC-24 merge rejected");
  expect(world.applyRpc(exact, event) && event.kind == samp::state::RpcEventKind::VehicleChanged &&
         event.vehicleParametersUpdated && world.findVehicle(91)->parameters.known &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Objective) == 1 &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Doors) == 1 &&
         samp::state::isRecipientVehicleEntryLocked(*world.findVehicle(91)) &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Engine) == 1 &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Lights) == 1 &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Siren) == 1,
         "RPC-161 must update only recipient objective/door-lock fields");
  std::array<uint8_t, 16> globalPartial{};
  globalPartial.fill(0xFFu);
  globalPartial[0] = 0u;
  expect(world.applyRpc(makeSetVehicleParams(91u, globalPartial), event) &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Engine) == 0 &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Objective) == -1 &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Doors) == -1 &&
         !samp::state::isRecipientVehicleEntryLocked(*world.findVehicle(91)),
         "later full RPC-24 snapshot must clear prior recipient objective/door-lock unsets");
  expect(world.applyRpc(exact, event) && event.kind == samp::state::RpcEventKind::VehicleChanged &&
         event.vehicleParametersUpdated,
         "legacy RPC-161 must reapply its two recipient-local overrides after RPC-24 replacement");
  expect(world.applyRpc(exact, event) && event.kind == samp::state::RpcEventKind::Ignored &&
         !event.vehicleParametersUpdated,
         "duplicate RPC-161 must not manufacture a parameter event");
  expect(world.applyRpc(makeSetVehicleParamsForPlayer(91u, 0u, 0u), event) &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Objective) == 0 &&
         world.findVehicle(91)->parameters.value(samp::state::VehicleParameter::Doors) == 0 &&
         !samp::state::isRecipientVehicleEntryLocked(*world.findVehicle(91)),
         "RPC-161 recipient values must overwrite their two authoritative fields");
  expect(world.applyRpc(makeStreamOutVehicle(91u), event) &&
         world.applyRpc(makeStreamInVehicle(91, 411, 10.0f, 20.0f, 5.0f, 90.0f), event) &&
         !world.findVehicle(91)->parameters.known,
         "RPC-161 state must clear on StreamOut/raw-ID reuse");
}

void testSetVehicleVelocityReducer() {
  samp::state::WorldState world;
  samp::state::RpcEvent event;

  const auto exactLinear = makeSetVehicleVelocity(0u, 0.25f, -0.5f, 0.2f);
  expect(exactLinear == std::vector<uint8_t>({20u, 91u, 0u, 0u, 0u, 0x80u, 0x3Eu,
                                               0u, 0u, 0u, 0xBFu, 0xCDu, 0xCCu,
                                               0x4Cu, 0x3Eu}),
         "RPC-91 linear wire fixture changed");
  expect(samp::rpc::summarize(exactLinear).find("SetVehicleVelocity") != std::string::npos,
         "RPC-91 decoder label missing");

  // The ID-less command cannot allocate/guess a vehicle before server stream +
  // authoritative recipient driver seating exist.
  expect(!world.applyRpc(exactLinear, event) && event.kind == samp::state::RpcEventKind::Ignored,
         "RPC-91 without local driver context must fail closed");
  expect(world.applyRpc(makeStreamInVehicle(77, 411, 10.0f, 20.0f, 5.0f, 90.0f), event) &&
         world.applyRpc(makeStreamInVehicle(78, 451, 15.0f, 20.0f, 5.0f, 90.0f), event),
         "RPC-91 fixture vehicle streams rejected");
  expect(!world.applyRpc(exactLinear, event) && !world.findVehicle(77)->linearVelocityKnown &&
         !world.findVehicle(78)->linearVelocityKnown,
         "RPC-91 must not choose an arbitrary streamed vehicle");
  expect(world.applyRpc(makePutPlayerInVehicle(77, 1), event),
         "RPC-91 passenger fixture seating rejected");
  expect(!world.applyRpc(exactLinear, event) && !world.findVehicle(77)->linearVelocityKnown,
         "RPC-91 must not target a passenger vehicle");
  expect(world.applyRpc(makePutPlayerInVehicle(77, 0), event),
         "RPC-91 driver fixture seating rejected");

  expect(world.applyRpc(exactLinear, event) &&
         event.kind == samp::state::RpcEventKind::VehicleChanged && event.playerId == 77 &&
         event.localVehicleVelocityUpdated && !event.localVehicleVelocityAngular &&
         world.findVehicle(77)->linearVelocityKnown &&
         std::fabs(world.findVehicle(77)->linearVelocityX - 0.25f) < 0.0001f &&
         std::fabs(world.findVehicle(77)->linearVelocityY + 0.5f) < 0.0001f &&
         std::fabs(world.findVehicle(77)->linearVelocityZ - 0.2f) < 0.0001f &&
         !world.findVehicle(77)->angularVelocityKnown && !world.findVehicle(78)->linearVelocityKnown,
         "RPC-91 linear command must update only the recipient local driver vehicle");
  const uint64_t linearRevision = world.findVehicle(77)->linearVelocityRevision;

  const auto angular = makeSetVehicleVelocity(1u, 0.0f, 0.0f, 2.0f);
  expect(world.applyRpc(angular, event) && event.localVehicleVelocityUpdated &&
         event.localVehicleVelocityAngular && world.findVehicle(77)->angularVelocityKnown &&
         std::fabs(world.findVehicle(77)->angularVelocityZ - 2.0f) < 0.0001f &&
         world.findVehicle(77)->linearVelocityRevision == linearRevision &&
         std::fabs(world.findVehicle(77)->linearVelocityZ - 0.2f) < 0.0001f,
         "RPC-91 angular selection must not overwrite retained linear velocity");
  const uint64_t angularRevision = world.findVehicle(77)->angularVelocityRevision;

  // All malformed vectors/types fail atomically and retain both prior channels.
  auto trailing = exactLinear;
  trailing.push_back(0u);
  expect(!world.applyRpc(trailing, event) && event.kind == samp::state::RpcEventKind::Malformed &&
         world.findVehicle(77)->linearVelocityRevision == linearRevision,
         "RPC-91 trailing payload must fail closed");
  expect(!world.applyRpc(makeSetVehicleVelocity(2u, 0.0f, 0.0f, 0.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findVehicle(77)->angularVelocityRevision == angularRevision,
         "RPC-91 unknown turn selector must fail closed");
  expect(!world.applyRpc(makeSetVehicleVelocity(0u, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findVehicle(77)->linearVelocityRevision == linearRevision,
         "RPC-91 non-finite vector must fail closed");
  expect(!world.applyRpc(makeSetVehicleVelocity(0u, 101.0f, 0.0f, 0.0f), event) &&
         event.kind == samp::state::RpcEventKind::Malformed &&
         world.findVehicle(77)->linearVelocityRevision == linearRevision,
         "RPC-91 out-of-envelope vector must fail closed");

  // A new StreamIn lifecycle for a raw ID clears retained recipient directives;
  // stream-out must do the same so a later reused ID cannot inherit a command.
  expect(world.applyRpc(makeStreamOutVehicle(77), event) && world.findVehicle(77) == nullptr,
         "RPC-91 stream-out must remove the implicit target from the usable vehicle view");
  expect(!world.applyRpc(exactLinear, event),
         "RPC-91 cannot target a stream-out vehicle");
  expect(world.applyRpc(makeStreamInVehicle(77, 411, 10.0f, 20.0f, 5.0f, 90.0f), event) &&
         !world.findVehicle(77)->linearVelocityKnown && !world.findVehicle(77)->angularVelocityKnown,
         "RPC-91 stream-in ID reuse must start directive-free");

  // Documented train exclusion is enforced from the authoritative streamed
  // model, not a client-nominated vehicle ID or heuristic coordinate test.
  samp::state::WorldState trainWorld;
  expect(trainWorld.applyRpc(makeStreamInVehicle(91, 537, 0.0f, 0.0f, 5.0f, 0.0f), event) &&
         trainWorld.applyRpc(makePutPlayerInVehicle(91, 0), event) &&
         !trainWorld.applyRpc(exactLinear, event) && !trainWorld.findVehicle(91)->linearVelocityKnown,
         "RPC-91 must ignore documented train models");
}

}  // namespace

int main() {
  testLegacyHandshakeBuilders();
  testReliableAndAck();
  testUnreliableSequencedTransport();
  testReliableMessageTransportGate();
  testReliableOrderedTransportGate();
  testRakNetMalformedDatagramSmoke();
  testPlayerFootSyncBuilder();
  testPlayerVehicleSyncBuilder();
  testPlayerAimSyncBuilder();
  testPlayerNoTargetBulletSyncBuilder();
  testPlayerPassengerSyncBuilder();
  testRemoteAimBroadcastDecoder();
  testRemoteBulletBroadcastDecoder();
  testRemoteOnFootBroadcastDecoder();
  testRemoteVehicleBroadcastDecoder();
  testRemoteTrailerBroadcastDecoder();
  testRemotePassengerBroadcastDecoder();
  testNativeInputAndLocalPreview();
  testCompressedU32();
  testSplitReassembly();
  testDefaultStringCompressorDecoder();
  testGtaTxdTextureData();
  testGtaDffMeshData();
  testGtaImgAndCollisionData();
  testGtaFileDescriptorDataSource();
  testGtaDataRuntimeAssembly();
  testGtaMapManifestData();
  testJsonEscaping();
  testPlayerInitDecoder();
  testClientRpcBuilders();
  testObjectSelectionReducer();
  testObjectEditReducer();
  testAttachedObjectEditReducer();
  testSessionSafetyWithoutSocket();
  testBoundedConnectErrors();
  testRetainedSessionInitialClassBootstrap();
  testRetainedSessionCoalescedBootstrapRetriesIndividually();
  testRetainedSessionLargeOrderedBootstrap();
  testRenderableWorldBoundary();
  testSetVehicleParamsReducer();
  testSetVehicleParamsForPlayerReducer();
  testSetVehicleVelocityReducer();
  testSendDeathMessageReducer();
  testWorldStateReducer();
  std::puts("raknet codec/state regression tests: OK");
  return 0;
}
