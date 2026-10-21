// JNI bridge: mengekspos protokol SA-MP ke Kotlin.
#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "gta_data_runtime.h"
#include "gta_file_source.h"
#include "gta_img_archive.h"
#include "raknet_probe.h"
#include "raknet_session.h"
#include "samp_query.h"

namespace {

std::string to_std(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

jstring to_java(JNIEnv* env, const std::string& s) {
  return env->NewStringUTF(s.c_str());
}

std::mutex game_data_mutex;
samp::gta::runtime::GameDataRuntime game_data_runtime;

std::string game_data_json(bool ok, const samp::gta::runtime::Snapshot& data,
                           const std::string& error) {
  std::ostringstream output;
  output << "{\"ok\":" << (ok ? "true" : "false")
         << ",\"imgMounted\":" << (data.imgMounted ? "true" : "false")
         << ",\"imgBytes\":" << data.imgBytes
         << ",\"imgEntries\":" << data.imgEntries
         << ",\"additionalImgArchives\":" << data.additionalImgArchives
         << ",\"additionalImgBytes\":" << data.additionalImgBytes
         << ",\"additionalImgEntries\":" << data.additionalImgEntries
         << ",\"collisionModels\":" << data.collisionModels
         << ",\"dffModels\":" << data.dffModels
         << ",\"dffMeshes\":" << data.dffMeshes
         << ",\"dffVertices\":" << data.dffVertices
         << ",\"txdDictionaries\":" << data.txdDictionaries
         << ",\"txdParentRelations\":" << data.txdParentRelations
         << ",\"textures\":" << data.textures
         << ",\"texturePixels\":" << data.texturePixels
         << ",\"manifestIdePaths\":" << data.manifestIdePaths
         << ",\"manifestIplPaths\":" << data.manifestIplPaths
         << ",\"manifestColPaths\":" << data.manifestColPaths
         << ",\"manifestImgPaths\":" << data.manifestImgPaths
         << ",\"ideObjects\":" << data.ideObjects
         << ",\"handlingProfiles\":" << data.handlingProfiles
         << ",\"iplPlacements\":" << data.iplPlacements
         << ",\"collisionInstances\":" << data.collisionInstances
         << ",\"missingCollisionPlacements\":" << data.missingCollisionPlacements
         << ",\"error\":\"" << samp::json_escape(error) << "\"}";
  return output.str();
}

void append_manifest_paths(std::ostringstream& output, const std::vector<std::string>& paths) {
  output << "[";
  for (size_t index = 0; index < paths.size(); ++index) {
    if (index != 0) output << ",";
    output << "\"" << samp::json_escape(paths[index]) << "\"";
  }
  output << "]";
}

std::string manifest_json(const samp::gta::map::AssetManifest& manifest) {
  std::ostringstream output;
  output << "{\"ok\":true,\"idePaths\":";
  append_manifest_paths(output, manifest.idePaths);
  output << ",\"iplPaths\":";
  append_manifest_paths(output, manifest.iplPaths);
  output << ",\"colPaths\":";
  append_manifest_paths(output, manifest.colPaths);
  output << ",\"imgPaths\":";
  append_manifest_paths(output, manifest.imgPaths);
  output << "}";
  return output.str();
}

bool descriptor_array(JNIEnv* env, jintArray values, std::vector<int>& out, std::string& error) {
  out.clear();
  if (!values) {
    error = "descriptor batch kosong";
    return false;
  }
  const jsize count = env->GetArrayLength(values);
  if (count <= 0 || count > 64) {
    error = "jumlah descriptor batch tidak valid";
    return false;
  }
  std::vector<jint> raw(static_cast<size_t>(count));
  env->GetIntArrayRegion(values, 0, count, raw.data());
  if (env->ExceptionCheck()) {
    error = "gagal membaca descriptor batch";
    return false;
  }
  out.reserve(static_cast<size_t>(count));
  for (const jint value : raw) {
    if (value < 0) {
      error = "descriptor batch tidak valid";
      out.clear();
      return false;
    }
    out.push_back(static_cast<int>(value));
  }
  return true;
}

// Dynamic renderer input is copied across JNI, rather than retaining a Java
// object or a mutable WorldSnapshot. Kotlin serializes bounded Default `D` and
// Text `T` material records per server object. Hex text avoids treating sign
// content as delimiters or a filesystem name.
constexpr size_t kMaxMaterialOverrideSpecBytes = 16u * 192u;

bool parse_decimal_bounded(const std::string& value, uint64_t maximum,
                           uint64_t& out) {
  if (value.empty()) return false;
  uint64_t parsed = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(character - '0');
    if (digit > maximum || parsed > (maximum - digit) / 10u) return false;
    parsed = parsed * 10u + digit;
  }
  out = parsed;
  return true;
}

bool normalize_portable_material_lookup_name(std::string& value) {
  if (value.empty() || value.size() > 31u) return false;
  for (char& character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte > 0x7Eu || byte == '/' || byte == '\\' ||
        byte == ':') {
      return false;
    }
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return true;
}

bool split_material_fields(const std::string& record, std::vector<std::string>& out) {
  out.clear();
  size_t start = 0;
  while (true) {
    const size_t comma = record.find(',', start);
    const size_t end = comma == std::string::npos ? record.size() : comma;
    if (out.size() >= 10u) return false;
    out.push_back(record.substr(start, end - start));
    if (comma == std::string::npos) return true;
    start = comma + 1u;
  }
}

bool decode_hex_text(const std::string& encoded, std::string& out) {
  out.clear();
  if (encoded.size() % 2u != 0u || encoded.size() > 62u) return false;
  const auto nibble = [](char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
  };
  out.reserve(encoded.size() / 2u);
  for (size_t index = 0; index < encoded.size(); index += 2u) {
    const int high = nibble(encoded[index]);
    const int low = nibble(encoded[index + 1u]);
    if (high < 0 || low < 0) {
      out.clear();
      return false;
    }
    const char byte = static_cast<char>((high << 4) | low);
    // Keep the renderer text/key surface identical to Kotlin's bounded source
    // filter: NUL and non-layout control bytes are neither useful glyphs nor
    // safe delimiters. UTF-8 bytes remain intact and use the fallback glyph.
    const unsigned char unsignedByte = static_cast<unsigned char>(byte);
    if (unsignedByte == 0u || (unsignedByte < 0x20u && unsignedByte != '\n' &&
                               unsignedByte != '\r' && unsignedByte != '\t')) {
      out.clear();
      return false;
    }
    out.push_back(byte);
  }
  return true;
}

bool valid_text_material_size(uint64_t value) {
  return value >= 10u && value <= 140u && value % 10u == 0u;
}

bool parse_material_override_spec(
    const std::string& spec,
    samp::gta::runtime::ServerObjectRenderInput& input) {
  if (spec.empty()) return true;
  if (spec.size() > kMaxMaterialOverrideSpecBytes) return false;
  bool seenSlots[16]{};
  size_t recordStart = 0;
  size_t recordCount = 0;
  std::vector<std::string> fields;
  fields.reserve(9u);
  while (recordStart < spec.size()) {
    if (recordCount >= 16u) return false;
    const size_t recordEnd = spec.find(';', recordStart);
    const size_t length = (recordEnd == std::string::npos ? spec.size() : recordEnd) -
                          recordStart;
    if (length == 0u || !split_material_fields(spec.substr(recordStart, length), fields)) {
      return false;
    }

    // `D`/`T` makes the current Kotlin contract explicit. Accept the former
    // five-field Default layout as a narrow backwards-compatible input for an
    // already compiled Java caller, never for Text records.
    const bool legacyDefault = fields.size() == 5u;
    const bool taggedDefault = !legacyDefault && fields.front() == "D";
    const bool taggedText = !legacyDefault && fields.front() == "T";
    const size_t base = legacyDefault ? 0u : 1u;
    uint64_t rawSlot = 0;
    const bool fieldCountValid = legacyDefault ? fields.size() == 5u
        : taggedDefault ? fields.size() == 6u
        : taggedText && fields.size() == 9u;
    if ((!legacyDefault && !taggedDefault && !taggedText) || !fieldCountValid ||
        !parse_decimal_bounded(fields[base], 15u, rawSlot)) {
      return false;
    }
    const size_t slot = static_cast<size_t>(rawSlot);
    if (seenSlots[slot]) return false;
    samp::gta::runtime::ServerObjectMaterialOverrideInput parsed{};

    if (legacyDefault || taggedDefault) {
      if (fields.size() != base + 5u) return false;
      uint64_t rawSourceModel = 0;
      uint64_t rawColour = 0;
      if (!parse_decimal_bounded(fields[base + 1u], std::numeric_limits<uint16_t>::max(),
                                 rawSourceModel) ||
          !parse_decimal_bounded(fields[base + 2u], std::numeric_limits<uint32_t>::max(),
                                 rawColour)) {
        return false;
      }
      std::string dictionary = fields[base + 3u];
      std::string texture = fields[base + 4u];
      // u16 all-ones is API model -1: it changes base colour only and has no
      // TXD lookup. Empty paired names are valid solely for that sentinel;
      // every ordinary/default lookup keeps the portable-name gate.
      const bool emptyColorOnly = rawSourceModel == std::numeric_limits<uint16_t>::max() &&
                                  dictionary.empty() && texture.empty();
      if (!emptyColorOnly && (!normalize_portable_material_lookup_name(dictionary) ||
                              !normalize_portable_material_lookup_name(texture))) {
        return false;
      }
      parsed.active = true;
      parsed.sourceModel = static_cast<uint16_t>(rawSourceModel);
      parsed.colourAbgr = static_cast<uint32_t>(rawColour);
      parsed.textureDictionary = std::move(dictionary);
      parsed.textureName = std::move(texture);
    } else {
      // T,slot,size,fontSize,bold,fontARGB,backgroundARGB,alignment,textHex
      if (fields.size() != base + 8u) return false;
      uint64_t rawSize = 0;
      uint64_t rawFontSize = 0;
      uint64_t rawBold = 0;
      uint64_t rawFontColour = 0;
      uint64_t rawBackgroundColour = 0;
      uint64_t rawAlignment = 0;
      std::string text;
      if (!parse_decimal_bounded(fields[base + 1u], 140u, rawSize) ||
          !valid_text_material_size(rawSize) ||
          !parse_decimal_bounded(fields[base + 2u], 255u, rawFontSize) ||
          !parse_decimal_bounded(fields[base + 3u], 1u, rawBold) ||
          !parse_decimal_bounded(fields[base + 4u], std::numeric_limits<uint32_t>::max(),
                                 rawFontColour) ||
          !parse_decimal_bounded(fields[base + 5u], std::numeric_limits<uint32_t>::max(),
                                 rawBackgroundColour) ||
          !parse_decimal_bounded(fields[base + 6u], 2u, rawAlignment) ||
          !decode_hex_text(fields[base + 7u], text)) {
        return false;
      }
      parsed.textActive = true;
      parsed.textMaterialSize = static_cast<uint8_t>(rawSize);
      parsed.textFontSize = static_cast<uint8_t>(rawFontSize);
      parsed.textBold = rawBold != 0u;
      parsed.textColourArgb = static_cast<uint32_t>(rawFontColour);
      parsed.textBackgroundColourArgb = static_cast<uint32_t>(rawBackgroundColour);
      parsed.textAlignment = static_cast<uint8_t>(rawAlignment);
      parsed.text = std::move(text);
    }
    seenSlots[slot] = true;
    input.materialOverrides[slot] = std::move(parsed);
    ++recordCount;
    if (recordEnd == std::string::npos) break;
    recordStart = recordEnd + 1u;
  }
  return true;
}

bool material_override_specs(
    JNIEnv* env, jobjectArray values,
    std::vector<samp::gta::runtime::ServerObjectRenderInput>& inputs) {
  if (!values || env->GetArrayLength(values) != static_cast<jsize>(inputs.size())) {
    return false;
  }
  for (size_t index = 0; index < inputs.size(); ++index) {
    jobject raw = env->GetObjectArrayElement(values, static_cast<jsize>(index));
    if (env->ExceptionCheck() || !raw) return false;
    jstring value = static_cast<jstring>(raw);
    const jsize encodedBytes = env->GetStringUTFLength(value);
    if (env->ExceptionCheck() || encodedBytes < 0 ||
        static_cast<size_t>(encodedBytes) > kMaxMaterialOverrideSpecBytes) {
      env->DeleteLocalRef(raw);
      return false;
    }
    const char* characters = env->GetStringUTFChars(value, nullptr);
    if (!characters) {
      env->DeleteLocalRef(raw);
      return false;
    }
    const std::string spec(characters, static_cast<size_t>(encodedBytes));
    env->ReleaseStringUTFChars(value, characters);
    env->DeleteLocalRef(raw);
    if (env->ExceptionCheck() || !parse_material_override_spec(spec, inputs[index])) {
      return false;
    }
  }
  return true;
}

void append_animation_json(std::ostringstream& output,
                           const samp::WorldAnimationSnapshot& animation) {
  output << "{\"active\":" << (animation.active ? "true" : "false")
         << ",\"revision\":" << animation.revision
         << ",\"library\":\"" << samp::json_escape(animation.library) << "\""
         << ",\"name\":\"" << samp::json_escape(animation.name) << "\""
         << ",\"delta\":" << animation.delta
         << ",\"loop\":" << (animation.loop ? "true" : "false")
         << ",\"lockX\":" << (animation.lockX ? "true" : "false")
         << ",\"lockY\":" << (animation.lockY ? "true" : "false")
         << ",\"freeze\":" << (animation.freeze ? "true" : "false")
         << ",\"durationMs\":" << animation.durationMs << "}";
}

std::string world_json(const samp::WorldSnapshot& world) {
  std::ostringstream o;
  o << "{\"active\":" << (world.active ? "true" : "false")
    << ",\"playerInit\":" << (world.playerInit ? "true" : "false")
    << ",\"classSelectionKnown\":" << (world.classSelectionKnown ? "true" : "false")
    << ",\"classRequestPending\":" << (world.classRequestPending ? "true" : "false")
    << ",\"spawnResponseKnown\":" << (world.spawnResponseKnown ? "true" : "false")
    << ",\"spawnAllowed\":" << (world.spawnAllowed ? "true" : "false")
    << ",\"spawnRequestPending\":" << (world.spawnRequestPending ? "true" : "false")
    << ",\"spawnConfirmationSent\":" << (world.spawnConfirmationSent ? "true" : "false")
    << ",\"spawnConfirmationAcked\":" << (world.spawnConfirmationAcked ? "true" : "false")
    << ",\"local\":{\"known\":" << (world.localPositionKnown ? "true" : "false")
    << ",\"spawnInfoKnown\":" << (world.localSpawnInfoKnown ? "true" : "false")
    << ",\"facingKnown\":" << (world.localFacingKnown ? "true" : "false")
    << ",\"healthKnown\":" << (world.localHealthKnown ? "true" : "false")
    << ",\"armourKnown\":" << (world.localArmourKnown ? "true" : "false")
    << ",\"moneyKnown\":" << (world.localMoneyKnown ? "true" : "false")
    << ",\"wantedLevelKnown\":" << (world.localWantedLevelKnown ? "true" : "false")
    << ",\"scoreKnown\":" << (world.localScoreKnown ? "true" : "false")
    << ",\"armedWeaponKnown\":" << (world.localArmedWeaponKnown ? "true" : "false")
    << ",\"armedAmmoKnown\":" << (world.localArmedAmmoKnown ? "true" : "false")
    << ",\"controllableKnown\":" << (world.localControllableKnown ? "true" : "false")
    << ",\"controllable\":" << (world.localControllable ? "true" : "false")
    << ",\"interiorKnown\":" << (world.localInteriorKnown ? "true" : "false")
    << ",\"virtualWorldKnown\":" << (world.localVirtualWorldKnown ? "true" : "false")
    << ",\"vehicleKnown\":" << (world.localVehicleKnown ? "true" : "false")
    << ",\"inVehicle\":" << (world.localInVehicle ? "true" : "false")
    << ",\"passengerDriveByActive\":" << (world.localPassengerDriveByActive ? "true" : "false")
    << ",\"interior\":" << static_cast<unsigned>(world.localInterior)
    << ",\"virtualWorld\":" << world.localVirtualWorld
    << ",\"vehicleId\":" << world.localVehicleId
    << ",\"vehicleSeat\":" << static_cast<unsigned>(world.localVehicleSeat)
    << ",\"predictionActive\":" << (world.localPredictionActive ? "true" : "false")
    << ",\"grounded\":" << (world.localGrounded ? "true" : "false")
    << ",\"aimHeld\":" << (world.localAimHeld ? "true" : "false")
    << ",\"fireHeld\":" << (world.localFireHeld ? "true" : "false")
    << ",\"aimSyncReady\":" << (world.localAimSyncReady ? "true" : "false")
    << ",\"bulletSyncServerEnabled\":" << (world.localBulletSyncServerEnabled ? "true" : "false")
    << ",\"staticWorldContactActive\":" << (world.localStaticWorldContactActive ? "true" : "false")
    << ",\"outboundNoTargetBulletPackets\":" << world.outboundNoTargetBulletPackets
    << ",\"aimPitchDegrees\":" << world.localAimPitchDegrees
    << ",\"vehicleAnchorKnown\":" << (world.localVehicleAnchorKnown ? "true" : "false")
    << ",\"id\":" << world.localPlayerId
    << ",\"model\":" << world.localModel
    << ",\"x\":" << world.localX
    << ",\"y\":" << world.localY
    << ",\"z\":" << world.localZ
    << ",\"angle\":" << world.localAngle
    << ",\"health\":" << world.localHealth
    << ",\"armour\":" << world.localArmour
    << ",\"money\":" << world.localMoney
    << ",\"wantedLevel\":" << static_cast<unsigned>(world.localWantedLevel)
    << ",\"score\":" << world.localScore
    << ",\"ping\":" << world.localPing
    << ",\"armedWeapon\":" << static_cast<unsigned>(world.localArmedWeapon)
    << ",\"armedAmmo\":" << world.localArmedAmmo
    << ",\"chatBubbleActive\":" << (world.localChatBubbleActive ? "true" : "false")
    << ",\"chatBubbleRgba\":" << world.localChatBubbleRgba
    << ",\"chatBubbleDrawDistance\":" << world.localChatBubbleDrawDistance
    << ",\"chatBubbleText\":\"" << samp::json_escape(world.localChatBubbleText) << "\""
    << ",\"animation\":";
  append_animation_json(o, world.localForcedAnimation);
  o << "}"
    << ",\"spectate\":{\"known\":" << (world.spectate.known ? "true" : "false")
    << ",\"active\":" << (world.spectate.active ? "true" : "false")
    << ",\"targetKnown\":" << (world.spectate.targetKnown ? "true" : "false")
    << ",\"targetVehicle\":" << (world.spectate.targetVehicle ? "true" : "false")
    << ",\"targetId\":" << world.spectate.targetId
    << ",\"mode\":" << static_cast<unsigned>(world.spectate.mode) << "}"
    << ",\"environment\":{\"timeKnown\":" << (world.worldTimeKnown ? "true" : "false")
    << ",\"playerTimeKnown\":" << (world.playerTimeKnown ? "true" : "false")
    << ",\"playerClockKnown\":" << (world.playerClockKnown ? "true" : "false")
    << ",\"playerClockEnabled\":" << (world.playerClockEnabled ? "true" : "false")
    << ",\"weatherKnown\":" << (world.weatherKnown ? "true" : "false")
    << ",\"gravityKnown\":" << (world.gravityKnown ? "true" : "false")
    << ",\"worldBoundsKnown\":" << (world.worldBoundsKnown ? "true" : "false")
    << ",\"worldTime\":" << static_cast<unsigned>(world.worldTime)
    << ",\"playerHour\":" << static_cast<unsigned>(world.playerHour)
    << ",\"playerMinute\":" << static_cast<unsigned>(world.playerMinute)
    << ",\"weather\":" << static_cast<unsigned>(world.weather)
    << ",\"gravity\":" << world.gravity
    << ",\"worldMaxX\":" << world.worldMaxX
    << ",\"worldMinX\":" << world.worldMinX
    << ",\"worldMaxY\":" << world.worldMaxY
    << ",\"worldMinY\":" << world.worldMinY << "}"
    << ",\"audioStream\":{\"active\":" << (world.audioStreamActive ? "true" : "false")
    << ",\"positional\":" << (world.audioStreamPositional ? "true" : "false")
    << ",\"revision\":" << world.audioStreamRevision
    << ",\"url\":\"" << samp::json_escape(world.audioStreamUrl) << "\""
    << ",\"x\":" << world.audioStreamX
    << ",\"y\":" << world.audioStreamY
    << ",\"z\":" << world.audioStreamZ
    << ",\"distance\":" << world.audioStreamDistance << "}"
    << ",\"nameTags\":{\"show\":" << (world.showNameTags ? "true" : "false")
    << ",\"disableLos\":" << (world.disableNameTagLos ? "true" : "false")
    << ",\"drawDistance\":" << world.nameTagDrawDistance << "}"
    << ",\"camera\":{\"positionKnown\":" << (world.camera.positionKnown ? "true" : "false")
    << ",\"lookAtKnown\":" << (world.camera.lookAtKnown ? "true" : "false")
    << ",\"interpolatingPosition\":" << (world.camera.interpolatingPosition ? "true" : "false")
    << ",\"interpolatingLookAt\":" << (world.camera.interpolatingLookAt ? "true" : "false")
    << ",\"positionX\":" << world.camera.positionX
    << ",\"positionY\":" << world.camera.positionY
    << ",\"positionZ\":" << world.camera.positionZ
    << ",\"lookAtX\":" << world.camera.lookAtX
    << ",\"lookAtY\":" << world.camera.lookAtY
    << ",\"lookAtZ\":" << world.camera.lookAtZ << "}"
    << ",\"dialog\":{\"active\":" << (world.dialog.active ? "true" : "false")
    << ",\"revision\":" << world.dialog.revision
    << ",\"id\":" << world.dialog.id
    << ",\"style\":" << static_cast<unsigned>(world.dialog.style)
    << ",\"title\":\"" << samp::json_escape(world.dialog.title) << "\""
    << ",\"button1\":\"" << samp::json_escape(world.dialog.button1) << "\""
    << ",\"button2\":\"" << samp::json_escape(world.dialog.button2) << "\""
    << ",\"body\":\"" << samp::json_escape(world.dialog.body) << "\"}";
  o << ",\"menu\":{\"active\":" << (world.menu.active ? "true" : "false")
    << ",\"revision\":" << world.menu.revision
    << ",\"id\":" << static_cast<unsigned>(world.menu.id)
    << ",\"twoColumns\":" << (world.menu.twoColumns ? "true" : "false")
    << ",\"enabled\":" << (world.menu.enabled ? "true" : "false")
    << ",\"title\":\"" << samp::json_escape(world.menu.title) << "\""
    << ",\"column1Header\":\"" << samp::json_escape(world.menu.column1Header) << "\""
    << ",\"column2Header\":\"" << samp::json_escape(world.menu.column2Header) << "\""
    << ",\"rowEnabled\":[";
  for (size_t row = 0; row < world.menu.rowEnabled.size(); ++row) {
    if (row) o << ",";
    o << (world.menu.rowEnabled[row] ? "true" : "false");
  }
  o << "],\"column1\":[";
  for (size_t row = 0; row < world.menu.column1Count; ++row) {
    if (row) o << ",";
    o << "\"" << samp::json_escape(world.menu.column1Items[row]) << "\"";
  }
  o << "],\"column2\":[";
  for (size_t row = 0; row < world.menu.column2Count; ++row) {
    if (row) o << ",";
    o << "\"" << samp::json_escape(world.menu.column2Items[row]) << "\"";
  }
  o << "]}"
    << ",\"gameText\":{\"active\":" << (world.gameText.active ? "true" : "false")
    << ",\"revision\":" << world.gameText.revision
    << ",\"style\":" << world.gameText.style
    << ",\"durationMs\":" << world.gameText.durationMs
    << ",\"text\":\"" << samp::json_escape(world.gameText.text) << "\"}"
    << ",\"checkpoint\":{\"active\":" << (world.checkpointActive ? "true" : "false")
    << ",\"x\":" << world.checkpointX
    << ",\"y\":" << world.checkpointY
    << ",\"z\":" << world.checkpointZ
    << ",\"size\":" << world.checkpointSize << "}"
    << ",\"raceCheckpoint\":{\"active\":" << (world.raceCheckpointActive ? "true" : "false")
    << ",\"x\":" << world.raceCheckpointX
    << ",\"y\":" << world.raceCheckpointY
    << ",\"z\":" << world.raceCheckpointZ
    << ",\"size\":" << world.raceCheckpointSize << "}"
    << ",\"streamedPlayers\":" << world.streamedPlayers
    << ",\"remoteOnFootPlayers\":" << world.remoteOnFootPlayers
    << ",\"streamedVehicles\":" << world.streamedVehicles
    << ",\"trackedServerActors\":" << world.trackedServerActors
    << ",\"serverObjects\":" << world.serverObjects
    << ",\"trackedPickups\":" << world.trackedPickups
    << ",\"trackedTextDraws\":" << world.trackedTextDraws
    << ",\"trackedTextLabels\":" << world.trackedTextLabels
    << ",\"trackedMapIcons\":" << world.trackedMapIcons
    << ",\"trackedGangZones\":" << world.trackedGangZones
    << ",\"textDrawSelectionActive\":" << (world.textDrawSelectionActive ? "true" : "false")
    << ",\"objectSelectionActive\":" << (world.objectSelectionActive ? "true" : "false")
    << ",\"objectSelectionRevision\":" << world.objectSelectionRevision
    << ",\"objectEdit\":{\"active\":" << (world.objectEditActive ? "true" : "false")
    << ",\"playerObject\":" << (world.objectEditPlayerObject ? "true" : "false")
    << ",\"id\":" << world.objectEditId
    << ",\"revision\":" << world.objectEditRevision
    << ",\"transformAvailable\":" << (world.objectEditTransformAvailable ? "true" : "false")
    << ",\"x\":" << world.objectEditX
    << ",\"y\":" << world.objectEditY
    << ",\"z\":" << world.objectEditZ
    << ",\"rotationX\":" << world.objectEditRotationX
    << ",\"rotationY\":" << world.objectEditRotationY
    << ",\"rotationZ\":" << world.objectEditRotationZ << "}"
    << ",\"attachedObjectEdit\":{\"active\":" << (world.attachedObjectEdit.active ? "true" : "false")
    << ",\"slot\":" << static_cast<unsigned>(world.attachedObjectEdit.slot)
    << ",\"revision\":" << world.attachedObjectEdit.revision
    << ",\"model\":" << world.attachedObjectEdit.model
    << ",\"bone\":" << static_cast<unsigned>(world.attachedObjectEdit.bone)
    << ",\"offsetX\":" << world.attachedObjectEdit.offsetX
    << ",\"offsetY\":" << world.attachedObjectEdit.offsetY
    << ",\"offsetZ\":" << world.attachedObjectEdit.offsetZ
    << ",\"rotationX\":" << world.attachedObjectEdit.rotationX
    << ",\"rotationY\":" << world.attachedObjectEdit.rotationY
    << ",\"rotationZ\":" << world.attachedObjectEdit.rotationZ
    << ",\"scaleX\":" << world.attachedObjectEdit.scaleX
    << ",\"scaleY\":" << world.attachedObjectEdit.scaleY
    << ",\"scaleZ\":" << world.attachedObjectEdit.scaleZ << "}"
    << ",\"textDrawSelectionColourRgba\":" << world.textDrawSelectionColourRgba
    << ",\"serverName\":\"" << samp::json_escape(world.serverName) << "\""
    << ",\"error\":\"" << samp::json_escape(world.error) << "\""
    << ",\"actors\":[";
  for (size_t i = 0; i < world.actors.size(); ++i) {
    const samp::WorldActorSnapshot& actor = world.actors[i];
    if (i) o << ",";
    o << "{\"id\":" << actor.id
      << ",\"rgba\":" << actor.rgba
      << ",\"model\":" << actor.model
      << ",\"onFootKnown\":" << (actor.onFootKnown ? "true" : "false")
      << ",\"aimKnown\":" << (actor.aimKnown ? "true" : "false")
      << ",\"aimCameraMode\":" << static_cast<unsigned>(actor.aimCameraMode)
      << ",\"aimWeaponState\":" << static_cast<unsigned>(actor.aimWeaponState)
      << ",\"aimCameraFrontX\":" << actor.aimCameraFrontX
      << ",\"aimCameraFrontY\":" << actor.aimCameraFrontY
      << ",\"aimCameraFrontZ\":" << actor.aimCameraFrontZ
      << ",\"aimCameraPositionX\":" << actor.aimCameraPositionX
      << ",\"aimCameraPositionY\":" << actor.aimCameraPositionY
      << ",\"aimCameraPositionZ\":" << actor.aimCameraPositionZ
      << ",\"bulletKnown\":" << (actor.bulletKnown ? "true" : "false")
      << ",\"bulletWeaponId\":" << static_cast<unsigned>(actor.bulletWeaponId)
      << ",\"bulletOriginX\":" << actor.bulletOriginX
      << ",\"bulletOriginY\":" << actor.bulletOriginY
      << ",\"bulletOriginZ\":" << actor.bulletOriginZ
      << ",\"bulletDirectionX\":" << actor.bulletDirectionX
      << ",\"bulletDirectionY\":" << actor.bulletDirectionY
      << ",\"bulletDirectionZ\":" << actor.bulletDirectionZ
      << ",\"wasted\":" << (actor.wasted ? "true" : "false")
      << ",\"animation\":";
    append_animation_json(o, actor.forcedAnimation);
    o << ",\"inVehicle\":" << (actor.inVehicle ? "true" : "false")
      << ",\"vehicleDriver\":" << (actor.vehicleDriver ? "true" : "false")
      << ",\"vehicleId\":" << actor.vehicleId
      << ",\"vehicleSeat\":" << static_cast<unsigned>(actor.vehicleSeat)
      << ",\"nameTagVisible\":" << (actor.nameTagVisible ? "true" : "false")
      << ",\"chatBubbleActive\":" << (actor.chatBubbleActive ? "true" : "false")
      << ",\"chatBubbleRgba\":" << actor.chatBubbleRgba
      << ",\"chatBubbleDrawDistance\":" << actor.chatBubbleDrawDistance
      << ",\"chatBubbleText\":\"" << samp::json_escape(actor.chatBubbleText) << "\""
      << ",\"x\":" << actor.x
      << ",\"y\":" << actor.y
      << ",\"z\":" << actor.z
      << ",\"angle\":" << actor.angle
      << ",\"health\":" << actor.health
      << ",\"armour\":" << actor.armour
      << ",\"name\":\"" << samp::json_escape(actor.name) << "\"}";
  }
  o << "],\"serverActors\":[";
  for (size_t i = 0; i < world.serverActors.size(); ++i) {
    const samp::WorldServerActorSnapshot& actor = world.serverActors[i];
    if (i) o << ",";
    o << "{\"id\":" << actor.id
      << ",\"model\":" << actor.model
      << ",\"x\":" << actor.x
      << ",\"y\":" << actor.y
      << ",\"z\":" << actor.z
      << ",\"angle\":" << actor.angle
      << ",\"health\":" << actor.health
      << ",\"invulnerable\":" << (actor.invulnerable ? "true" : "false")
      << ",\"animation\":";
    append_animation_json(o, actor.forcedAnimation);
    o << "}";
  }
  o << "],\"scoreboard\":[";
  for (size_t i = 0; i < world.scoreboard.size(); ++i) {
    const samp::WorldScoreSnapshot& entry = world.scoreboard[i];
    if (i) o << ",";
    o << "{\"id\":" << entry.id
      << ",\"rgba\":" << entry.rgba
      << ",\"npc\":" << (entry.npc ? "true" : "false")
      << ",\"score\":" << entry.score
      << ",\"ping\":" << entry.ping
      << ",\"name\":\"" << samp::json_escape(entry.name) << "\"}";
  }
  o << "],\"vehicles\":[";
  for (size_t i = 0; i < world.vehicles.size(); ++i) {
    const samp::WorldVehicleSnapshot& vehicle = world.vehicles[i];
    if (i) o << ",";
    o << "{\"id\":" << vehicle.id
      << ",\"dynamicSyncKnown\":" << (vehicle.dynamicSyncKnown ? "true" : "false")
      << ",\"driverKnown\":" << (vehicle.driverKnown ? "true" : "false")
      << ",\"driverId\":" << vehicle.driverId
      << ",\"model\":" << vehicle.model
      << ",\"colour1\":" << static_cast<unsigned>(vehicle.colour1)
      << ",\"colour2\":" << static_cast<unsigned>(vehicle.colour2)
      << ",\"paintjob\":" << static_cast<unsigned>(vehicle.paintjob)
      << ",\"x\":" << vehicle.x
      << ",\"y\":" << vehicle.y
      << ",\"z\":" << vehicle.z
      << ",\"angle\":" << vehicle.angle
      << ",\"attitudeKnown\":" << (vehicle.attitudeKnown ? "true" : "false")
      << ",\"rotationX\":" << vehicle.rotationX
      << ",\"rotationY\":" << vehicle.rotationY
      << ",\"rotationZ\":" << vehicle.rotationZ
      << ",\"parametersKnown\":" << (vehicle.parametersKnown ? "true" : "false")
      << ",\"engine\":" << static_cast<int>(vehicle.parameters[0])
      << ",\"lights\":" << static_cast<int>(vehicle.parameters[1])
      << ",\"alarm\":" << static_cast<int>(vehicle.parameters[2])
      << ",\"doors\":" << static_cast<int>(vehicle.parameters[3])
      << ",\"bonnet\":" << static_cast<int>(vehicle.parameters[4])
      << ",\"boot\":" << static_cast<int>(vehicle.parameters[5])
      << ",\"objective\":" << static_cast<int>(vehicle.parameters[6])
      << ",\"siren\":" << static_cast<int>(vehicle.parameters[7])
      << ",\"doorDriver\":" << static_cast<int>(vehicle.parameters[8])
      << ",\"doorPassenger\":" << static_cast<int>(vehicle.parameters[9])
      << ",\"doorBackLeft\":" << static_cast<int>(vehicle.parameters[10])
      << ",\"doorBackRight\":" << static_cast<int>(vehicle.parameters[11])
      << ",\"windowDriver\":" << static_cast<int>(vehicle.parameters[12])
      << ",\"windowPassenger\":" << static_cast<int>(vehicle.parameters[13])
      << ",\"windowBackLeft\":" << static_cast<int>(vehicle.parameters[14])
      << ",\"windowBackRight\":" << static_cast<int>(vehicle.parameters[15])
      << ",\"health\":" << vehicle.health << "}";
  }
  o << "],\"objects\":[";
  for (size_t i = 0; i < world.objects.size(); ++i) {
    const samp::WorldObjectSnapshot& object = world.objects[i];
    if (i) o << ",";
    o << "{\"id\":" << object.id
      << ",\"model\":" << object.model
      << ",\"moving\":" << (object.moving ? "true" : "false")
      << ",\"x\":" << object.x
      << ",\"y\":" << object.y
      << ",\"z\":" << object.z
      << ",\"rotationX\":" << object.rotationX
      << ",\"rotationY\":" << object.rotationY
      << ",\"rotationZ\":" << object.rotationZ
      << ",\"drawDistance\":" << object.drawDistance
      << ",\"materials\":[";
    bool firstMaterial = true;
    for (size_t materialIndex = 0; materialIndex < object.materials.size(); ++materialIndex) {
      const samp::WorldObjectMaterialSnapshot& material = object.materials[materialIndex];
      if (!material.active && !material.textActive) continue;
      if (!firstMaterial) o << ",";
      firstMaterial = false;
      o << "{\"slot\":" << materialIndex;
      if (material.active) {
        o << ",\"kind\":\"default\""
          << ",\"sourceModel\":" << material.sourceModel
          << ",\"colourAbgr\":" << material.colourAbgr
          << ",\"dictionary\":\"" << samp::json_escape(material.textureDictionary)
          << "\",\"texture\":\"" << samp::json_escape(material.textureName);
      } else {
        o << ",\"kind\":\"text\""
          << ",\"materialSize\":" << static_cast<unsigned>(material.textMaterialSize)
          << ",\"fontSize\":" << static_cast<unsigned>(material.textFontSize)
          << ",\"bold\":" << (material.textBold ? "true" : "false")
          << ",\"fontColourArgb\":" << material.textColourArgb
          << ",\"backgroundColourArgb\":" << material.textBackgroundColourArgb
          << ",\"alignment\":" << static_cast<unsigned>(material.textAlignment)
          << ",\"font\":\"" << samp::json_escape(material.textFont)
          << "\",\"text\":\"" << samp::json_escape(material.text);
      }
      o << "}";
    }
    o << "]}";
  }
  o << "],\"playerAttachedObjects\":[";
  for (size_t i = 0; i < world.playerAttachedObjects.size(); ++i) {
    const samp::WorldPlayerAttachedObjectSnapshot& attachment = world.playerAttachedObjects[i];
    if (i) o << ",";
    o << "{\"playerId\":" << attachment.playerId
      << ",\"slot\":" << static_cast<unsigned>(attachment.slot)
      << ",\"model\":" << attachment.model
      << ",\"bone\":" << static_cast<unsigned>(attachment.bone)
      << ",\"x\":" << attachment.x
      << ",\"y\":" << attachment.y
      << ",\"z\":" << attachment.z
      << ",\"rotationX\":" << attachment.rotationX
      << ",\"rotationY\":" << attachment.rotationY
      << ",\"rotationZ\":" << attachment.rotationZ
      << ",\"scaleX\":" << attachment.scaleX
      << ",\"scaleY\":" << attachment.scaleY
      << ",\"scaleZ\":" << attachment.scaleZ
      << ",\"colour1Abgr\":" << attachment.colour1Abgr
      << ",\"colour2Abgr\":" << attachment.colour2Abgr << "}";
  }
  o << "],\"pickups\":[";
  for (size_t i = 0; i < world.pickups.size(); ++i) {
    const samp::WorldPickupSnapshot& pickup = world.pickups[i];
    if (i) o << ",";
    o << "{\"id\":" << pickup.id
      << ",\"model\":" << pickup.model
      << ",\"spawnType\":" << pickup.spawnType
      << ",\"x\":" << pickup.x
      << ",\"y\":" << pickup.y
      << ",\"z\":" << pickup.z << "}";
  }
  o << "],\"gangZones\":[";
  for (size_t i = 0; i < world.gangZones.size(); ++i) {
    const samp::WorldGangZoneSnapshot& zone = world.gangZones[i];
    if (i) o << ",";
    o << "{\"id\":" << zone.id
      << ",\"minX\":" << zone.minX
      << ",\"minY\":" << zone.minY
      << ",\"maxX\":" << zone.maxX
      << ",\"maxY\":" << zone.maxY
      << ",\"colourAbgr\":" << zone.colourAbgr
      << ",\"flashing\":" << (zone.flashing ? "true" : "false")
      << ",\"flashColourAbgr\":" << zone.flashColourAbgr << "}";
  }
  o << "],\"textLabels\":[";
  for (size_t i = 0; i < world.textLabels.size(); ++i) {
    const samp::WorldTextLabelSnapshot& label = world.textLabels[i];
    if (i) o << ",";
    o << "{\"wireId\":" << label.wireId
      << ",\"playerTextLabel\":" << (label.playerTextLabel ? "true" : "false")
      << ",\"rgba\":" << label.rgba
      << ",\"x\":" << label.x
      << ",\"y\":" << label.y
      << ",\"z\":" << label.z
      << ",\"drawDistance\":" << label.drawDistance
      << ",\"los\":" << (label.los ? "true" : "false")
      << ",\"playerAttachId\":" << label.playerAttachId
      << ",\"vehicleAttachId\":" << label.vehicleAttachId
      << ",\"text\":\"" << samp::json_escape(label.text) << "\"}";
  }
  o << "],\"mapIcons\":[";
  for (size_t i = 0; i < world.mapIcons.size(); ++i) {
    const samp::WorldMapIconSnapshot& icon = world.mapIcons[i];
    if (i) o << ",";
    o << "{\"id\":" << static_cast<unsigned>(icon.id)
      << ",\"x\":" << icon.x
      << ",\"y\":" << icon.y
      << ",\"z\":" << icon.z
      << ",\"type\":" << static_cast<unsigned>(icon.type)
      << ",\"rgba\":" << icon.rgba
      << ",\"style\":" << static_cast<unsigned>(icon.style) << "}";
  }
  o << "],\"textDraws\":[";
  for (size_t i = 0; i < world.textDraws.size(); ++i) {
    const samp::WorldTextDrawSnapshot& draw = world.textDraws[i];
    if (i) o << ",";
    o << "{\"wireId\":" << draw.wireId
      << ",\"playerTextDraw\":" << (draw.playerTextDraw ? "true" : "false")
      << ",\"useBox\":" << (draw.useBox ? "true" : "false")
      << ",\"alignment\":" << static_cast<unsigned>(draw.alignment)
      << ",\"letterWidth\":" << draw.letterWidth
      << ",\"letterHeight\":" << draw.letterHeight
      << ",\"letterColourAbgr\":" << draw.letterColourAbgr
      << ",\"textWidth\":" << draw.textWidth
      << ",\"textHeight\":" << draw.textHeight
      << ",\"boxColourAbgr\":" << draw.boxColourAbgr
      << ",\"shadow\":" << static_cast<unsigned>(draw.shadow)
      << ",\"outline\":" << static_cast<unsigned>(draw.outline)
      << ",\"backgroundColourAbgr\":" << draw.backgroundColourAbgr
      << ",\"style\":" << static_cast<unsigned>(draw.style)
      << ",\"selectable\":" << (draw.selectable ? "true" : "false")
      << ",\"x\":" << draw.x
      << ",\"y\":" << draw.y
      << ",\"model\":" << draw.model
      << ",\"rotationX\":" << draw.rotationX
      << ",\"rotationY\":" << draw.rotationY
      << ",\"rotationZ\":" << draw.rotationZ
      << ",\"zoom\":" << draw.zoom
      << ",\"colour1\":" << draw.colour1
      << ",\"colour2\":" << draw.colour2
      << ",\"text\":\"" << samp::json_escape(draw.text) << "\"}";
  }
  o << "],\"chat\":[";
  for (size_t i = 0; i < world.chat.size(); ++i) {
    const samp::WorldChatSnapshot& chat = world.chat[i];
    if (i) o << ",";
    o << "{\"rgba\":" << chat.rgba
      << ",\"text\":\"" << samp::json_escape(chat.text) << "\"}";
  }
  o << "]"
    << ",\"deathMessages\":[";
  for (size_t i = 0; i < world.deathMessages.size(); ++i) {
    const samp::WorldDeathMessageSnapshot& death = world.deathMessages[i];
    if (i) o << ",";
    o << "{\"hasKiller\":" << (death.hasKiller ? "true" : "false")
      << ",\"killerId\":" << death.killerId
      << ",\"victimId\":" << death.victimId
      << ",\"reason\":" << static_cast<unsigned>(death.reason)
      << ",\"killerName\":\"" << samp::json_escape(death.killerName) << "\""
      << ",\"victimName\":\"" << samp::json_escape(death.victimName) << "\"}";
  }
  o << "]}";
  return o.str();
}

std::string session_json(const samp::SessionSnapshot& s) {
  std::ostringstream o;
  o << "{\"active\":" << (s.active ? "true" : "false")
    << ",\"accepted\":" << (s.accepted ? "true" : "false")
    << ",\"newIncomingTransportAcked\":" << (s.newIncomingTransportAcked ? "true" : "false")
    << ",\"staticDataTransportAcked\":" << (s.staticDataTransportAcked ? "true" : "false")
    << ",\"clientJoinTransportAcked\":" << (s.clientJoinTransportAcked ? "true" : "false")
    << ",\"joined\":" << (s.joined ? "true" : "false")
    << ",\"playerInit\":" << (s.playerInit ? "true" : "false")
    << ",\"classSelectionKnown\":" << (s.classSelectionKnown ? "true" : "false")
    << ",\"classSelectionForced\":" << (s.classSelectionForced ? "true" : "false")
    << ",\"classSelectable\":" << (s.classSelectable ? "true" : "false")
    << ",\"classRequestPending\":" << (s.classRequestPending ? "true" : "false")
    << ",\"spawnResponseKnown\":" << (s.spawnResponseKnown ? "true" : "false")
    << ",\"spawnAllowed\":" << (s.spawnAllowed ? "true" : "false")
    << ",\"spawnRequestPending\":" << (s.spawnRequestPending ? "true" : "false")
    << ",\"spawnConfirmationSent\":" << (s.spawnConfirmationSent ? "true" : "false")
    << ",\"spawnConfirmationAcked\":" << (s.spawnConfirmationAcked ? "true" : "false")
    << ",\"localSpawnInfoKnown\":" << (s.localSpawnInfoKnown ? "true" : "false")
    << ",\"localPositionKnown\":" << (s.localPositionKnown ? "true" : "false")
    << ",\"localControllableKnown\":" << (s.localControllableKnown ? "true" : "false")
    << ",\"localControllable\":" << (s.localControllable ? "true" : "false")
    << ",\"localInteriorKnown\":" << (s.localInteriorKnown ? "true" : "false")
    << ",\"localVirtualWorldKnown\":" << (s.localVirtualWorldKnown ? "true" : "false")
    << ",\"localVehicleKnown\":" << (s.localVehicleKnown ? "true" : "false")
    << ",\"localInVehicle\":" << (s.localInVehicle ? "true" : "false")
    << ",\"localInterior\":" << static_cast<unsigned>(s.localInterior)
    << ",\"localVirtualWorld\":" << s.localVirtualWorld
    << ",\"localVehicleId\":" << s.localVehicleId
    << ",\"localVehicleSeat\":" << static_cast<unsigned>(s.localVehicleSeat)
    << ",\"nativeControlsKnown\":" << (s.nativeControlsKnown ? "true" : "false")
    << ",\"onFootPreviewReady\":" << (s.onFootPreviewReady ? "true" : "false")
    << ",\"inCarPreviewReady\":" << (s.inCarPreviewReady ? "true" : "false")
    << ",\"aimPreviewReady\":" << (s.aimPreviewReady ? "true" : "false")
    << ",\"passengerPreviewReady\":" << (s.passengerPreviewReady ? "true" : "false")
    << ",\"localPredictionActive\":" << (s.localPredictionActive ? "true" : "false")
    << ",\"localGrounded\":" << (s.localGrounded ? "true" : "false")
    << ",\"checkpointActive\":" << (s.checkpointActive ? "true" : "false")
    << ",\"raceCheckpointActive\":" << (s.raceCheckpointActive ? "true" : "false")
    << ",\"playerId\":" << s.playerId
    << ",\"requestedClassId\":" << s.requestedClassId
    << ",\"spawnClassCount\":" << s.spawnClassCount
    << ",\"classModel\":" << s.classModel
    << ",\"localModel\":" << s.localModel
    << ",\"localX\":" << s.localX
    << ",\"localY\":" << s.localY
    << ",\"localZ\":" << s.localZ
    << ",\"localAngle\":" << s.localAngle
    << ",\"controlMoveX\":" << s.controlMoveX
    << ",\"controlMoveY\":" << s.controlMoveY
    << ",\"controlHeading\":" << s.controlHeading
    << ",\"controlAimPitch\":" << s.controlAimPitch
    << ",\"controlLeftRight\":" << s.controlLeftRight
    << ",\"controlUpDown\":" << s.controlUpDown
    << ",\"controlKeys\":" << s.controlKeys
    << ",\"onFootSyncRateMs\":" << s.onFootSyncRateMs
    << ",\"effectiveOnFootSyncRateMs\":" << s.effectiveOnFootSyncRateMs
    << ",\"inCarSyncRateMs\":" << s.inCarSyncRateMs
    << ",\"effectiveInCarSyncRateMs\":" << s.effectiveInCarSyncRateMs
    << ",\"weaponSyncRateMs\":" << s.weaponSyncRateMs
    << ",\"effectiveWeaponSyncRateMs\":" << s.effectiveWeaponSyncRateMs
    << ",\"receivedPackets\":" << s.receivedPackets
    << ",\"receivedRpcs\":" << s.receivedRpcs
    << ",\"inboundIdleMs\":" << s.inboundIdleMs
    << ",\"latestReliableRttMs\":" << s.latestReliableRttMs
    << ",\"smoothedReliableRttMs\":" << s.smoothedReliableRttMs
    << ",\"reliableResends\":" << s.reliableResends
    << ",\"splitFragments\":" << s.splitFragments
    << ",\"reassembledPackets\":" << s.reassembledPackets
    << ",\"trackedPlayers\":" << s.trackedPlayers
    << ",\"streamedPlayers\":" << s.streamedPlayers
    << ",\"remoteOnFootPlayers\":" << s.remoteOnFootPlayers
    << ",\"trackedVehicles\":" << s.trackedVehicles
    << ",\"streamedVehicles\":" << s.streamedVehicles
    << ",\"trackedServerActors\":" << s.trackedServerActors
    << ",\"dlObjectFallbacks\":" << s.dlObjectFallbacks
    << ",\"remoteOnFootPackets\":" << s.remoteOnFootPackets
    << ",\"remoteAimPackets\":" << s.remoteAimPackets
    << ",\"remoteBulletPackets\":" << s.remoteBulletPackets
    << ",\"remoteVehiclePackets\":" << s.remoteVehiclePackets
    << ",\"chatMessages\":" << s.chatMessages
    << ",\"trackedPickups\":" << s.trackedPickups
    << ",\"trackedTextLabels\":" << s.trackedTextLabels
    << ",\"trackedMapIcons\":" << s.trackedMapIcons
    << ",\"trackedGangZones\":" << s.trackedGangZones
    << ",\"outboundPickupNotifications\":" << s.outboundPickupNotifications
    << ",\"outboundVehicleInteractions\":" << s.outboundVehicleInteractions
    << ",\"outboundTextDrawSelections\":" << s.outboundTextDrawSelections
    << ",\"outboundObjectSelections\":" << s.outboundObjectSelections
    << ",\"outboundObjectEdits\":" << s.outboundObjectEdits
    << ",\"outboundAttachedObjectEdits\":" << s.outboundAttachedObjectEdits
    << ",\"outboundScoreRequests\":" << s.outboundScoreRequests
    << ",\"reliableKeepalives\":" << s.reliableKeepalives
    << ",\"connectedPongs\":" << s.connectedPongs
    << ",\"outboundOnFootPackets\":" << s.outboundOnFootPackets
    << ",\"outboundInCarPackets\":" << s.outboundInCarPackets
    << ",\"outboundAimPackets\":" << s.outboundAimPackets
    << ",\"outboundNoTargetBulletPackets\":" << s.outboundNoTargetBulletPackets
    << ",\"outboundPassengerPackets\":" << s.outboundPassengerPackets
    << ",\"outboundChatMessages\":" << s.outboundChatMessages
    << ",\"outboundDialogResponses\":" << s.outboundDialogResponses
    << ",\"localOnFootStateKnown\":" << (s.localOnFootStateKnown ? "true" : "false")
    << ",\"localAimStateKnown\":" << (s.localAimStateKnown ? "true" : "false")
    << ",\"localPassengerStateKnown\":" << (s.localPassengerStateKnown ? "true" : "false")
    << ",\"clientProtocol\":\"" << samp::json_escape(s.clientProtocol) << "\""
    << ",\"transportVariant\":\"" << samp::json_escape(s.transportVariant) << "\""
    << ",\"openMpServerEnvelope\":" << (s.openMpServerEnvelope ? "true" : "false")
    << ",\"serverName\":\"" << samp::json_escape(s.serverName) << "\""
    << ",\"rpcSummary\":\"" << samp::json_escape(s.rpcSummary) << "\""
    << ",\"syncSummary\":\"" << samp::json_escape(s.syncSummary) << "\""
    << ",\"stateSummary\":\"" << samp::json_escape(s.stateSummary) << "\""
    << ",\"error\":\"" << samp::json_escape(s.error) << "\"}";
  return o.str();
}

// Opens only a duplicated, user-authorized descriptor. The response carries
// structural counts rather than member names/content, so UI diagnostics do not
// copy or expose the selected GTA data.
std::string img_inspection_json(int fileDescriptor) {
  std::string error;
  auto source = samp::gta::img::FileDescriptorByteSource::duplicateFrom(fileDescriptor, error);
  if (!source) {
    return "{\"ok\":false,\"format\":\"img-v2\",\"bytes\":0,\"entries\":0,\"colEntries\":0,\"dffEntries\":0,\"txdEntries\":0,\"error\":\"" +
           samp::json_escape(error) + "\"}";
  }

  samp::gta::img::ArchiveIndex index;
  if (!samp::gta::img::parseV2(*source, index, error)) {
    return "{\"ok\":false,\"format\":\"img-v2\",\"bytes\":" +
           std::to_string(source->size()) +
           ",\"entries\":0,\"colEntries\":0,\"dffEntries\":0,\"txdEntries\":0,\"error\":\"" +
           samp::json_escape(error) + "\"}";
  }

  size_t colEntries = 0;
  size_t dffEntries = 0;
  size_t txdEntries = 0;
  for (const samp::gta::img::Entry& entry : index.entries()) {
    const std::string& name = entry.name;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".col") == 0) ++colEntries;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".dff") == 0) ++dffEntries;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".txd") == 0) ++txdEntries;
  }
  std::ostringstream output;
  output << "{\"ok\":true,\"format\":\"img-v2\",\"bytes\":" << source->size()
         << ",\"entries\":" << index.entries().size() << ",\"colEntries\":" << colEntries
         << ",\"dffEntries\":" << dffEntries << ",\"txdEntries\":" << txdEntries
         << ",\"error\":\"\"}";
  return output.str();
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeVersion(JNIEnv* env, jclass) {
  return to_java(env, "0.18.0 / native-gameplay-asset-bridge");
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeInspectGtaImgFile(JNIEnv* env, jclass,
                                                              jint fileDescriptor) {
  return to_java(env, img_inspection_json(static_cast<int>(fileDescriptor)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataStatus(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  return to_java(env, game_data_json(true, game_data_runtime.snapshot(), ""));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataClear(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  game_data_runtime.clear();
  return to_java(env, game_data_json(true, game_data_runtime.snapshot(), ""));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataClearMapSources(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  game_data_runtime.clearMapAssembly();
  return to_java(env, game_data_json(true, game_data_runtime.snapshot(), ""));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadGtaDatFile(JNIEnv* env, jclass,
                                                                    jint fileDescriptor) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadGtaDatFileDescriptor(static_cast<int>(fileDescriptor), error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadHandlingFile(JNIEnv* env, jclass,
                                                                     jint fileDescriptor) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadHandlingFileDescriptor(static_cast<int>(fileDescriptor), error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataManifest(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  return to_java(env, manifest_json(game_data_runtime.assetManifest()));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataMountImgFile(JNIEnv* env, jclass,
                                                                  jint fileDescriptor) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.mountImgFileDescriptor(static_cast<int>(fileDescriptor), error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataMountAdditionalImgFiles(
    JNIEnv* env, jclass, jintArray fileDescriptors) {
  std::vector<int> descriptors;
  std::string error;
  if (!descriptor_array(env, fileDescriptors, descriptors, error)) {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    return to_java(env, game_data_json(false, game_data_runtime.snapshot(), error));
  }
  std::lock_guard<std::mutex> lock(game_data_mutex);
  const bool ok = game_data_runtime.mountAdditionalImgFileDescriptors(descriptors, error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadImgCollisionMember(JNIEnv* env, jclass,
                                                                             jstring memberName) {
  const std::string member = to_std(env, memberName);
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadImgCollisionMember(member, error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadAllImgCollisionMembers(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadAllImgCollisionMembers(error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadImgDffMember(JNIEnv* env, jclass,
                                                                      jstring memberName) {
  const std::string member = to_std(env, memberName);
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadImgDffMember(member, error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jfloatArray JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLatestDffPreview(JNIEnv* env, jclass,
                                                                      jint maxVertices) {
  std::vector<float> vertices;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t limit = maxVertices > 0 ? static_cast<size_t>(maxVertices) : 0;
    if (!game_data_runtime.copyLatestDffPreview(limit, vertices, error)) {
      return env->NewFloatArray(0);
    }
  }
  if (vertices.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return env->NewFloatArray(0);
  }
  jfloatArray result = env->NewFloatArray(static_cast<jsize>(vertices.size()));
  if (result && !vertices.empty()) {
    env->SetFloatArrayRegion(result, 0, static_cast<jsize>(vertices.size()), vertices.data());
  }
  return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataNearbyWorldPreview(
    JNIEnv* env, jclass, jfloat anchorX, jfloat anchorY, jfloat anchorZ, jfloat radius,
    jint maxInstances, jint maxVertices, jint interior) {
  samp::gta::runtime::NearbyWorldRenderBatch batch;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t instanceLimit = maxInstances > 0 ? static_cast<size_t>(maxInstances) : 0;
    const size_t vertexLimit = maxVertices > 0 ? static_cast<size_t>(maxVertices) : 0;
    // Keep the renderer aligned with the retained session's exact RPC-43
    // directives. The order remains data mutex then session mutex, matching
    // nativeSessionAdvanceGameplay and avoiding a lock-order inversion.
    const auto removals = samp::raknet_session_building_removals();
    if (!game_data_runtime.copyNearbyWorldRenderBatch(
            static_cast<float>(anchorX), static_cast<float>(anchorY),
            static_cast<float>(anchorZ), static_cast<float>(radius), instanceLimit,
            vertexLimit, batch, error, static_cast<int32_t>(interior), &removals)) {
      return env->NewFloatArray(0);
    }
  }
  if (batch.vertices.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return env->NewFloatArray(0);
  }
  jfloatArray result = env->NewFloatArray(static_cast<jsize>(batch.vertices.size()));
  if (result && !batch.vertices.empty()) {
    env->SetFloatArrayRegion(result, 0, static_cast<jsize>(batch.vertices.size()),
                             batch.vertices.data());
  }
  return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataServerObjectsPreview(
    JNIEnv* env, jclass, jfloat anchorX, jfloat anchorY, jfloat anchorZ, jfloat radius,
    jintArray modelIds, jfloatArray transforms, jint maxVertices) {
  if (!modelIds || !transforms) return env->NewFloatArray(0);
  const jsize count = env->GetArrayLength(modelIds);
  if (count < 0 || static_cast<size_t>(count) > samp::gta::runtime::kMaxServerObjectRenderInputs ||
      static_cast<size_t>(count) > std::numeric_limits<size_t>::max() / 10u ||
      env->GetArrayLength(transforms) != static_cast<jsize>(static_cast<size_t>(count) * 10u)) {
    return env->NewFloatArray(0);
  }
  std::vector<jint> rawModels(static_cast<size_t>(count));
  std::vector<jfloat> rawTransforms(static_cast<size_t>(count) * 10u);
  if (count > 0) {
    env->GetIntArrayRegion(modelIds, 0, count, rawModels.data());
    if (env->ExceptionCheck()) return env->NewFloatArray(0);
    env->GetFloatArrayRegion(transforms, 0, static_cast<jsize>(rawTransforms.size()),
                             rawTransforms.data());
    if (env->ExceptionCheck()) return env->NewFloatArray(0);
  }
  std::vector<samp::gta::runtime::ServerObjectRenderInput> inputs;
  inputs.reserve(static_cast<size_t>(count));
  for (size_t index = 0; index < static_cast<size_t>(count); ++index) {
    // GTA ped skin 0 (CJ) is a valid dynamic model. Negative Java IDs remain
    // invalid before conversion to the native unsigned model namespace.
    if (rawModels[index] < 0) return env->NewFloatArray(0);
    const size_t offset = index * 10u;
    samp::gta::runtime::ServerObjectRenderInput input;
    input.modelId = static_cast<uint32_t>(rawModels[index]);
    input.x = rawTransforms[offset];
    input.y = rawTransforms[offset + 1u];
    input.z = rawTransforms[offset + 2u];
    input.rotationX = rawTransforms[offset + 3u];
    input.rotationY = rawTransforms[offset + 4u];
    input.rotationZ = rawTransforms[offset + 5u];
    input.drawDistance = rawTransforms[offset + 6u];
    input.scaleX = rawTransforms[offset + 7u];
    input.scaleY = rawTransforms[offset + 8u];
    input.scaleZ = rawTransforms[offset + 9u];
    inputs.push_back(input);
  }
  samp::gta::runtime::NearbyWorldRenderBatch batch;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t vertexLimit = maxVertices > 0 ? static_cast<size_t>(maxVertices) : 0;
    if (!game_data_runtime.copyServerObjectRenderBatch(
            static_cast<float>(anchorX), static_cast<float>(anchorY), static_cast<float>(anchorZ),
            static_cast<float>(radius), inputs, vertexLimit, batch, error)) {
      return env->NewFloatArray(0);
    }
  }
  if (batch.vertices.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return env->NewFloatArray(0);
  }
  jfloatArray result = env->NewFloatArray(static_cast<jsize>(batch.vertices.size()));
  if (result && !batch.vertices.empty()) {
    env->SetFloatArrayRegion(result, 0, static_cast<jsize>(batch.vertices.size()),
                             batch.vertices.data());
  }
  return result;
}

JNIEXPORT jobjectArray JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataServerObjectsTexturedPreviews(
    JNIEnv* env, jclass, jfloat anchorX, jfloat anchorY, jfloat anchorZ, jfloat radius,
    jintArray modelIds, jfloatArray transforms, jobjectArray materialOverrideSpecs,
    jint maxBatches, jint maxVertices, jint maxTexturePixels) {
  // Ten copied floats describe each dynamic model: position, rotation, draw
  // distance, then source scale. The parallel bounded string array is parsed
  // before the runtime lock and carries Default lookup
  // keys and Text=2 source fields only—never Java state or filesystem paths.
  if (!modelIds || !transforms || !materialOverrideSpecs) return nullptr;
  const jsize count = env->GetArrayLength(modelIds);
  if (count < 0 || static_cast<size_t>(count) > samp::gta::runtime::kMaxServerObjectRenderInputs ||
      static_cast<size_t>(count) > std::numeric_limits<size_t>::max() / 10u ||
      env->GetArrayLength(transforms) != static_cast<jsize>(static_cast<size_t>(count) * 10u)) {
    return nullptr;
  }
  std::vector<jint> rawModels(static_cast<size_t>(count));
  std::vector<jfloat> rawTransforms(static_cast<size_t>(count) * 10u);
  if (count > 0) {
    env->GetIntArrayRegion(modelIds, 0, count, rawModels.data());
    if (env->ExceptionCheck()) return nullptr;
    env->GetFloatArrayRegion(transforms, 0, static_cast<jsize>(rawTransforms.size()),
                             rawTransforms.data());
    if (env->ExceptionCheck()) return nullptr;
  }
  std::vector<samp::gta::runtime::ServerObjectRenderInput> inputs;
  inputs.reserve(static_cast<size_t>(count));
  for (size_t index = 0; index < static_cast<size_t>(count); ++index) {
    if (rawModels[index] < 0) return nullptr;
    const size_t offset = index * 10u;
    samp::gta::runtime::ServerObjectRenderInput input;
    input.modelId = static_cast<uint32_t>(rawModels[index]);
    input.x = rawTransforms[offset];
    input.y = rawTransforms[offset + 1u];
    input.z = rawTransforms[offset + 2u];
    input.rotationX = rawTransforms[offset + 3u];
    input.rotationY = rawTransforms[offset + 4u];
    input.rotationZ = rawTransforms[offset + 5u];
    input.drawDistance = rawTransforms[offset + 6u];
    input.scaleX = rawTransforms[offset + 7u];
    input.scaleY = rawTransforms[offset + 8u];
    input.scaleZ = rawTransforms[offset + 9u];
    inputs.push_back(input);
  }
  if (!material_override_specs(env, materialOverrideSpecs, inputs)) return nullptr;

  samp::gta::runtime::NearbyWorldTexturedRenderBatches batches;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t batchLimit = maxBatches > 0 ? static_cast<size_t>(maxBatches) : 0;
    const size_t vertexLimit = maxVertices > 0 ? static_cast<size_t>(maxVertices) : 0;
    const size_t textureLimit = maxTexturePixels > 0 ? static_cast<size_t>(maxTexturePixels) : 0;
    if (!game_data_runtime.copyServerObjectTexturedRenderBatches(
            static_cast<float>(anchorX), static_cast<float>(anchorY), static_cast<float>(anchorZ),
            static_cast<float>(radius), inputs, batchLimit, vertexLimit, textureLimit, batches, error)) {
      // Texture layers are optional. A renderer can keep its prelit color
      // dynamic-model batch when no portable TXD/material was available.
      batches = {};
    }
  }

  jclass previewClass = env->FindClass("com/xykal/sampdroid/NativeTexturedDffPreview");
  if (!previewClass) return nullptr;
  jmethodID constructor = env->GetMethodID(previewClass, "<init>", "([F[BII)V");
  if (!constructor) {
    env->DeleteLocalRef(previewClass);
    return nullptr;
  }
  if (batches.batches.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    env->DeleteLocalRef(previewClass);
    return nullptr;
  }
  jobjectArray result = env->NewObjectArray(static_cast<jsize>(batches.batches.size()), previewClass, nullptr);
  if (!result) {
    env->DeleteLocalRef(previewClass);
    return nullptr;
  }
  for (size_t index = 0; index < batches.batches.size(); ++index) {
    const samp::gta::runtime::NearbyWorldTexturedRenderBatch& batch = batches.batches[index];
    if (batch.vertices.empty() || batch.textureRgba.empty() ||
        batch.vertices.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()) ||
        batch.textureRgba.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    jfloatArray vertices = env->NewFloatArray(static_cast<jsize>(batch.vertices.size()));
    if (!vertices) {
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    env->SetFloatArrayRegion(vertices, 0, static_cast<jsize>(batch.vertices.size()), batch.vertices.data());
    if (env->ExceptionCheck()) {
      env->DeleteLocalRef(vertices);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    jbyteArray pixels = env->NewByteArray(static_cast<jsize>(batch.textureRgba.size()));
    if (!pixels) {
      env->DeleteLocalRef(vertices);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    env->SetByteArrayRegion(pixels, 0, static_cast<jsize>(batch.textureRgba.size()),
                            reinterpret_cast<const jbyte*>(batch.textureRgba.data()));
    if (env->ExceptionCheck()) {
      env->DeleteLocalRef(vertices);
      env->DeleteLocalRef(pixels);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    jobject preview = env->NewObject(previewClass, constructor, vertices, pixels,
                                     static_cast<jint>(batch.textureWidth),
                                     static_cast<jint>(batch.textureHeight));
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    if (!preview || env->ExceptionCheck()) {
      if (preview) env->DeleteLocalRef(preview);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    env->SetObjectArrayElement(result, static_cast<jsize>(index), preview);
    env->DeleteLocalRef(preview);
    if (env->ExceptionCheck()) {
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
  }
  env->DeleteLocalRef(previewClass);
  return result;
}

JNIEXPORT jobject JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataNearbyWorldTexturedPreview(
    JNIEnv* env, jclass, jfloat anchorX, jfloat anchorY, jfloat anchorZ, jfloat radius,
    jint maxVertices, jint maxTexturePixels, jint interior) {
  samp::gta::runtime::NearbyWorldTexturedRenderBatch batch;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t vertexLimit = maxVertices > 0 ? static_cast<size_t>(maxVertices) : 0;
    const size_t textureLimit = maxTexturePixels > 0 ? static_cast<size_t>(maxTexturePixels) : 0;
    const auto removals = samp::raknet_session_building_removals();
    if (!game_data_runtime.copyNearbyWorldTexturedRenderBatch(
            static_cast<float>(anchorX), static_cast<float>(anchorY), static_cast<float>(anchorZ),
            static_cast<float>(radius), vertexLimit, textureLimit, batch, error,
            static_cast<int32_t>(interior), &removals) ||
        batch.vertices.empty() || batch.textureRgba.empty()) {
      return nullptr;
    }
  }
  if (batch.vertices.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()) ||
      batch.textureRgba.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return nullptr;
  }
  jfloatArray vertices = env->NewFloatArray(static_cast<jsize>(batch.vertices.size()));
  if (!vertices) return nullptr;
  env->SetFloatArrayRegion(vertices, 0, static_cast<jsize>(batch.vertices.size()), batch.vertices.data());
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(vertices);
    return nullptr;
  }
  jbyteArray pixels = env->NewByteArray(static_cast<jsize>(batch.textureRgba.size()));
  if (!pixels) {
    env->DeleteLocalRef(vertices);
    return nullptr;
  }
  env->SetByteArrayRegion(pixels, 0, static_cast<jsize>(batch.textureRgba.size()),
                          reinterpret_cast<const jbyte*>(batch.textureRgba.data()));
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jclass previewClass = env->FindClass("com/xykal/sampdroid/NativeTexturedDffPreview");
  if (!previewClass) {
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jmethodID constructor = env->GetMethodID(previewClass, "<init>", "([F[BII)V");
  if (!constructor) {
    env->DeleteLocalRef(previewClass);
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jobject result = env->NewObject(previewClass, constructor, vertices, pixels,
                                  static_cast<jint>(batch.textureWidth),
                                  static_cast<jint>(batch.textureHeight));
  env->DeleteLocalRef(previewClass);
  env->DeleteLocalRef(vertices);
  env->DeleteLocalRef(pixels);
  return result;
}

JNIEXPORT jobjectArray JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataNearbyWorldTexturedPreviews(
    JNIEnv* env, jclass, jfloat anchorX, jfloat anchorY, jfloat anchorZ, jfloat radius,
    jint maxBatches, jint maxVertices, jint maxTexturePixels, jint interior) {
  samp::gta::runtime::NearbyWorldTexturedRenderBatches batches;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t batchLimit = maxBatches > 0 ? static_cast<size_t>(maxBatches) : 0;
    const size_t vertexLimit = maxVertices > 0 ? static_cast<size_t>(maxVertices) : 0;
    const size_t textureLimit = maxTexturePixels > 0 ? static_cast<size_t>(maxTexturePixels) : 0;
    const auto removals = samp::raknet_session_building_removals();
    if (!game_data_runtime.copyNearbyWorldTexturedRenderBatches(
            static_cast<float>(anchorX), static_cast<float>(anchorY), static_cast<float>(anchorZ),
            static_cast<float>(radius), batchLimit, vertexLimit, textureLimit, batches, error,
            static_cast<int32_t>(interior), &removals)) {
      batches = {};
    }
  }
  jclass previewClass = env->FindClass("com/xykal/sampdroid/NativeTexturedDffPreview");
  if (!previewClass) return nullptr;
  jmethodID constructor = env->GetMethodID(previewClass, "<init>", "([F[BII)V");
  if (!constructor) {
    env->DeleteLocalRef(previewClass);
    return nullptr;
  }
  if (batches.batches.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    env->DeleteLocalRef(previewClass);
    return nullptr;
  }
  jobjectArray result = env->NewObjectArray(static_cast<jsize>(batches.batches.size()), previewClass, nullptr);
  if (!result) {
    env->DeleteLocalRef(previewClass);
    return nullptr;
  }
  for (size_t index = 0; index < batches.batches.size(); ++index) {
    const samp::gta::runtime::NearbyWorldTexturedRenderBatch& batch = batches.batches[index];
    if (batch.vertices.empty() || batch.textureRgba.empty() ||
        batch.vertices.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()) ||
        batch.textureRgba.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    jfloatArray vertices = env->NewFloatArray(static_cast<jsize>(batch.vertices.size()));
    if (!vertices) {
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    env->SetFloatArrayRegion(vertices, 0, static_cast<jsize>(batch.vertices.size()), batch.vertices.data());
    if (env->ExceptionCheck()) {
      env->DeleteLocalRef(vertices);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    jbyteArray pixels = env->NewByteArray(static_cast<jsize>(batch.textureRgba.size()));
    if (!pixels) {
      env->DeleteLocalRef(vertices);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    env->SetByteArrayRegion(pixels, 0, static_cast<jsize>(batch.textureRgba.size()),
                            reinterpret_cast<const jbyte*>(batch.textureRgba.data()));
    if (env->ExceptionCheck()) {
      env->DeleteLocalRef(vertices);
      env->DeleteLocalRef(pixels);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    jobject preview = env->NewObject(previewClass, constructor, vertices, pixels,
                                     static_cast<jint>(batch.textureWidth),
                                     static_cast<jint>(batch.textureHeight));
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    if (!preview || env->ExceptionCheck()) {
      if (preview) env->DeleteLocalRef(preview);
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
    env->SetObjectArrayElement(result, static_cast<jsize>(index), preview);
    env->DeleteLocalRef(preview);
    if (env->ExceptionCheck()) {
      env->DeleteLocalRef(result);
      env->DeleteLocalRef(previewClass);
      return nullptr;
    }
  }
  env->DeleteLocalRef(previewClass);
  return result;
}

JNIEXPORT jobject JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLatestTexturedDffPreview(JNIEnv* env, jclass,
                                                                              jint maxVertices,
                                                                              jint maxTexturePixels) {
  samp::gta::runtime::TexturedDffPreview preview;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t vertexLimit = maxVertices > 0 ? static_cast<size_t>(maxVertices) : 0;
    const size_t textureLimit = maxTexturePixels > 0 ? static_cast<size_t>(maxTexturePixels) : 0;
    if (!game_data_runtime.copyLatestTexturedDffPreview(vertexLimit, textureLimit, preview, error)) {
      return nullptr;
    }
  }
  if (preview.vertices.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()) ||
      preview.textureRgba.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return nullptr;
  }
  jfloatArray vertices = env->NewFloatArray(static_cast<jsize>(preview.vertices.size()));
  if (!vertices) return nullptr;
  env->SetFloatArrayRegion(vertices, 0, static_cast<jsize>(preview.vertices.size()), preview.vertices.data());
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(vertices);
    return nullptr;
  }
  jbyteArray pixels = env->NewByteArray(static_cast<jsize>(preview.textureRgba.size()));
  if (!pixels) {
    env->DeleteLocalRef(vertices);
    return nullptr;
  }
  env->SetByteArrayRegion(pixels, 0, static_cast<jsize>(preview.textureRgba.size()),
                          reinterpret_cast<const jbyte*>(preview.textureRgba.data()));
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jclass previewClass = env->FindClass("com/xykal/sampdroid/NativeTexturedDffPreview");
  if (!previewClass) {
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jmethodID constructor = env->GetMethodID(previewClass, "<init>", "([F[BII)V");
  if (!constructor) {
    env->DeleteLocalRef(previewClass);
    env->DeleteLocalRef(vertices);
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jobject result = env->NewObject(previewClass, constructor, vertices, pixels,
                                  static_cast<jint>(preview.textureWidth),
                                  static_cast<jint>(preview.textureHeight));
  env->DeleteLocalRef(previewClass);
  env->DeleteLocalRef(vertices);
  env->DeleteLocalRef(pixels);
  return result;
}

JNIEXPORT jobject JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataTextDrawSpritePreview(
    JNIEnv* env, jclass, jstring dictionaryName, jstring textureName, jint maxPixels) {
  if (!dictionaryName || !textureName || maxPixels < 0) return nullptr;
  const jsize dictionaryBytes = env->GetStringUTFLength(dictionaryName);
  const jsize textureBytes = env->GetStringUTFLength(textureName);
  // Bound JNI UTF-8 copies before the runtime's independent normalized-token
  // check. Neither operand is a path and failure remains an optional HUD asset.
  if (env->ExceptionCheck() || dictionaryBytes <= 0 || textureBytes <= 0 ||
      dictionaryBytes > 31 || textureBytes > 31) {
    return nullptr;
  }
  const std::string dictionary = to_std(env, dictionaryName);
  const std::string texture = to_std(env, textureName);
  if (env->ExceptionCheck() || dictionary.empty() || texture.empty()) return nullptr;
  samp::gta::runtime::TexturePreview preview;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    const size_t pixelLimit = maxPixels > 0 ? static_cast<size_t>(maxPixels) : 0;
    if (!game_data_runtime.copyTexturePreview(dictionary, texture, pixelLimit, preview, error)) {
      return nullptr;
    }
  }
  if (preview.rgba.empty() ||
      preview.rgba.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return nullptr;
  }
  jbyteArray pixels = env->NewByteArray(static_cast<jsize>(preview.rgba.size()));
  if (!pixels) return nullptr;
  env->SetByteArrayRegion(pixels, 0, static_cast<jsize>(preview.rgba.size()),
                          reinterpret_cast<const jbyte*>(preview.rgba.data()));
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jclass previewClass = env->FindClass("com/xykal/sampdroid/NativeTexturePreview");
  if (!previewClass) {
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jmethodID constructor = env->GetMethodID(previewClass, "<init>", "([BII)V");
  if (!constructor) {
    env->DeleteLocalRef(previewClass);
    env->DeleteLocalRef(pixels);
    return nullptr;
  }
  jobject result = env->NewObject(previewClass, constructor, pixels,
                                  static_cast<jint>(preview.width),
                                  static_cast<jint>(preview.height));
  env->DeleteLocalRef(previewClass);
  env->DeleteLocalRef(pixels);
  return result;
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadImgTxdMember(JNIEnv* env, jclass,
                                                                      jstring memberName) {
  const std::string member = to_std(env, memberName);
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadImgTxdMember(member, error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadColFile(JNIEnv* env, jclass,
                                                                 jint fileDescriptor) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadExternalCollisionFileDescriptor(
      static_cast<int>(fileDescriptor), error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadColFiles(JNIEnv* env, jclass,
                                                                  jintArray fileDescriptors) {
  std::vector<int> descriptors;
  std::string error;
  if (!descriptor_array(env, fileDescriptors, descriptors, error)) {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    return to_java(env, game_data_json(false, game_data_runtime.snapshot(), error));
  }
  std::lock_guard<std::mutex> lock(game_data_mutex);
  const bool ok = game_data_runtime.loadExternalCollisionFileDescriptors(descriptors, error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadIdeFile(JNIEnv* env, jclass,
                                                                 jint fileDescriptor) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadIdeFileDescriptor(static_cast<int>(fileDescriptor), error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadIdeFiles(JNIEnv* env, jclass,
                                                                  jintArray fileDescriptors) {
  std::vector<int> descriptors;
  std::string error;
  if (!descriptor_array(env, fileDescriptors, descriptors, error)) {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    return to_java(env, game_data_json(false, game_data_runtime.snapshot(), error));
  }
  std::lock_guard<std::mutex> lock(game_data_mutex);
  const bool ok = game_data_runtime.loadIdeFileDescriptors(descriptors, error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadIplFile(JNIEnv* env, jclass,
                                                                 jint fileDescriptor) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadIplFileDescriptor(static_cast<int>(fileDescriptor), error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadIplFiles(JNIEnv* env, jclass,
                                                                  jintArray fileDescriptors) {
  std::vector<int> descriptors;
  std::string error;
  if (!descriptor_array(env, fileDescriptors, descriptors, error)) {
    std::lock_guard<std::mutex> lock(game_data_mutex);
    return to_java(env, game_data_json(false, game_data_runtime.snapshot(), error));
  }
  std::lock_guard<std::mutex> lock(game_data_mutex);
  const bool ok = game_data_runtime.loadIplFileDescriptors(descriptors, error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeGameDataLoadImgStreamingIpls(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(game_data_mutex);
  std::string error;
  const bool ok = game_data_runtime.loadImgStreamingIpls(error);
  return to_java(env, game_data_json(ok, game_data_runtime.snapshot(), error));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryInfo(JNIEnv* env, jclass, jstring ip,
                                                      jint port, jint timeoutMs) {
  auto host = to_std(env, ip);
  auto info = samp::query_info(host, static_cast<uint16_t>(port), timeoutMs);
  // INFO itself does not carry a protocol version. If the endpoint is alive,
  // augment this one UI-facing response with its fresh `version` rule so the
  // next native ClientJoin/OMP decision never relies on a directory label.
  // A missing/slow rules reply is optional metadata, not an INFO failure.
  std::string advertisedVersion;
  if (info.ok) {
    const int ruleTimeoutMs = timeoutMs < 100 ? 100 : (timeoutMs > 1500 ? 1500 : timeoutMs);
    const auto rules = samp::query_rules(host, static_cast<uint16_t>(port), ruleTimeoutMs);
    for (const samp::Rule& rule : rules) {
      if (rule.name == "version") {
        advertisedVersion = rule.value;
        break;
      }
    }
  }
  std::ostringstream o;
  o << "{";
  o << "\"ok\":" << (info.ok ? "true" : "false") << ",";
  o << "\"error\":\"" << samp::json_escape(info.error) << "\",";
  o << "\"password\":" << (info.password ? "true" : "false") << ",";
  o << "\"players\":" << info.players << ",";
  o << "\"maxPlayers\":" << info.maxPlayers << ",";
  o << "\"hostname\":\"" << samp::json_escape(info.hostname) << "\",";
  o << "\"gamemode\":\"" << samp::json_escape(info.gamemode) << "\",";
  o << "\"language\":\"" << samp::json_escape(info.language) << "\",";
  o << "\"version\":\"" << samp::json_escape(advertisedVersion) << "\"";
  o << "}";
  return to_java(env, o.str());
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryRules(JNIEnv* env, jclass, jstring ip,
                                                       jint port, jint timeoutMs) {
  auto rules = samp::query_rules(to_std(env, ip), static_cast<uint16_t>(port), timeoutMs);
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < rules.size(); ++i) {
    if (i) o << ",";
    o << "{\"name\":\"" << samp::json_escape(rules[i].name) << "\",\"value\":\""
      << samp::json_escape(rules[i].value) << "\"}";
  }
  o << "]";
  return to_java(env, o.str());
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryPlayers(JNIEnv* env, jclass, jstring ip,
                                                         jint port, jboolean detailed,
                                                         jint timeoutMs) {
  auto players = samp::query_players(to_std(env, ip), static_cast<uint16_t>(port),
                                     detailed == JNI_TRUE, timeoutMs);
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < players.size(); ++i) {
    if (i) o << ",";
    o << "{\"id\":" << players[i].id << ",\"name\":\"" << samp::json_escape(players[i].name)
      << "\",\"score\":" << players[i].score << ",\"ping\":" << players[i].ping << "}";
  }
  o << "]";
  return to_java(env, o.str());
}

JNIEXPORT jint JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryPing(JNIEnv* env, jclass, jstring ip,
                                                      jint port, jint timeoutMs) {
  return samp::query_ping(to_std(env, ip), static_cast<uint16_t>(port), timeoutMs);
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeRaknetProbe(JNIEnv* env, jclass, jstring ip,
                                                        jint port, jint timeoutMs,
                                                        jstring advertisedServerVersion) {
  auto r = samp::raknet_probe(to_std(env, ip), static_cast<uint16_t>(port), timeoutMs,
                              to_std(env, advertisedServerVersion));
  std::ostringstream o;
  o << "{\"openConnection\":" << (r.openConnection ? "true" : "false")
    << ",\"cookieOk\":" << (r.cookieOk ? "true" : "false")
    << ",\"authenticated\":" << (r.authenticated ? "true" : "false")
    << ",\"newIncomingTransportAcked\":" << (r.newIncomingTransportAcked ? "true" : "false")
    << ",\"staticDataTransportAcked\":" << (r.staticDataTransportAcked ? "true" : "false")
    << ",\"clientJoinTransportAcked\":" << (r.clientJoinTransportAcked ? "true" : "false")
    << ",\"joined\":" << (r.joined ? "true" : "false")
    << ",\"accepted\":" << (r.accepted ? "true" : "false")
    << ",\"playerInit\":" << (r.playerInit ? "true" : "false")
    << ",\"playerId\":" << r.playerId
    << ",\"receivedPackets\":" << r.receivedPackets
    << ",\"receivedRpcs\":" << r.receivedRpcs
    << ",\"splitFragments\":" << r.splitFragments
    << ",\"reassembledPackets\":" << r.reassembledPackets
    << ",\"trackedPlayers\":" << r.trackedPlayers
    << ",\"clientProtocol\":\"" << samp::json_escape(r.clientProtocol) << "\""
    << ",\"serverName\":\"" << samp::json_escape(r.serverName) << "\""
    << ",\"rpcSummary\":\"" << samp::json_escape(r.rpcSummary) << "\""
    << ",\"stateSummary\":\"" << samp::json_escape(r.stateSummary) << "\""
    << ",\"variant\":\"" << samp::json_escape(r.variant) << "\""
    << ",\"openMpServerEnvelope\":" << (r.openMpServerEnvelope ? "true" : "false")
    << ",\"error\":\"" << samp::json_escape(r.error) << "\""
    << ",\"log\":\"" << samp::json_escape(r.log) << "\"}";
  return to_java(env, o.str());
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionConnect(JNIEnv* env, jclass, jstring ip,
                                                           jint port, jint timeoutMs,
                                                           jstring nickname,
                                                           jstring advertisedServerVersion) {
  return to_java(env, session_json(samp::raknet_session_connect(
                          to_std(env, ip), static_cast<uint16_t>(port), timeoutMs,
                          to_std(env, nickname), to_std(env, advertisedServerVersion))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionPoll(JNIEnv* env, jclass, jint timeoutMs) {
  return to_java(env, session_json(samp::raknet_session_poll(timeoutMs)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionRequestClass(JNIEnv* env, jclass,
                                                                 jint classId) {
  if (classId < 0 || classId > 65535) {
    samp::SessionSnapshot invalid;
    invalid.error = "class ID di luar rentang u16";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(
                          samp::raknet_session_request_class(static_cast<uint16_t>(classId))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionRequestSpawn(JNIEnv* env, jclass) {
  return to_java(env, session_json(samp::raknet_session_request_spawn()));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionSendChat(JNIEnv* env, jclass, jstring message) {
  return to_java(env, session_json(samp::raknet_session_send_chat(to_std(env, message))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionRespondDialog(JNIEnv* env, jclass,
                                                                 jint dialogId, jlong dialogRevision,
                                                                 jboolean response, jint listItem,
                                                                 jstring text) {
  if (dialogId < -32768 || dialogId > 32767 || dialogRevision <= 0 ||
      listItem < -32768 || listItem > 32767) {
    samp::SessionSnapshot invalid;
    invalid.error = "ID, revisi, atau pilihan dialog di luar rentang valid";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(samp::raknet_session_respond_dialog(
                          static_cast<int16_t>(dialogId), static_cast<uint64_t>(dialogRevision),
                          response == JNI_TRUE, static_cast<int16_t>(listItem), to_std(env, text))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionSelectMenu(JNIEnv* env, jclass,
                                                               jint menuId, jlong menuRevision,
                                                               jint row) {
  if (menuId < 0 || menuId > 127 || menuRevision <= 0 || row < 0 || row > 11) {
    samp::SessionSnapshot invalid;
    invalid.error = "ID, revisi, atau baris menu di luar rentang valid";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(samp::raknet_session_select_menu(
                          static_cast<uint8_t>(menuId), static_cast<uint64_t>(menuRevision),
                          static_cast<uint8_t>(row))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionExitMenu(JNIEnv* env, jclass,
                                                             jint menuId, jlong menuRevision) {
  if (menuId < 0 || menuId > 127 || menuRevision <= 0) {
    samp::SessionSnapshot invalid;
    invalid.error = "ID atau revisi menu di luar rentang valid";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(samp::raknet_session_exit_menu(
                          static_cast<uint8_t>(menuId), static_cast<uint64_t>(menuRevision))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionUseNearby(JNIEnv* env, jclass,
                                                              jboolean passengerIntent) {
  return to_java(env, session_json(
      samp::raknet_session_use_nearby(passengerIntent == JNI_TRUE)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionRequestScores(JNIEnv* env, jclass) {
  return to_java(env, session_json(samp::raknet_session_request_scores()));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionSelectTextDraw(JNIEnv* env, jclass,
                                                                   jint wireId) {
  if (wireId < 0 || wireId > 65535) {
    samp::SessionSnapshot invalid;
    invalid.error = "ID textdraw di luar rentang u16";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(
                          samp::raknet_session_select_textdraw(static_cast<uint16_t>(wireId))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionSelectObject(JNIEnv* env, jclass,
                                                                 jint objectId,
                                                                 jlong expectedRevision) {
  // Current open.mp object pools reserve zero and 0xFFFF is the wire sentinel.
  if (objectId <= 0 || objectId >= 65535 || expectedRevision <= 0) {
    samp::SessionSnapshot invalid;
    invalid.error = "ID atau revisi pilihan objek di luar rentang";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(samp::raknet_session_select_object(
      static_cast<uint16_t>(objectId), static_cast<uint64_t>(expectedRevision))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionEditObject(JNIEnv* env, jclass,
                                                               jint response, jlong expectedRevision,
                                                               jfloat x, jfloat y, jfloat z,
                                                               jfloat rotationX, jfloat rotationY,
                                                               jfloat rotationZ) {
  if (response < 0 || response > 2 || expectedRevision <= 0) {
    samp::SessionSnapshot invalid;
    invalid.error = "respons atau revisi edit objek di luar rentang";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(samp::raknet_session_edit_object(
      static_cast<uint32_t>(response), static_cast<uint64_t>(expectedRevision),
      x, y, z, rotationX, rotationY, rotationZ)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionEditAttachedObject(
    JNIEnv* env, jclass, jint response, jlong expectedRevision,
    jfloat offsetX, jfloat offsetY, jfloat offsetZ,
    jfloat rotationX, jfloat rotationY, jfloat rotationZ,
    jfloat scaleX, jfloat scaleY, jfloat scaleZ) {
  if (response < 0 || response > 1 || expectedRevision <= 0) {
    samp::SessionSnapshot invalid;
    invalid.error = "respons atau revisi edit attachment di luar rentang";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(samp::raknet_session_edit_attached_object(
      static_cast<uint32_t>(response), static_cast<uint64_t>(expectedRevision),
      offsetX, offsetY, offsetZ, rotationX, rotationY, rotationZ,
      scaleX, scaleY, scaleZ)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionUpdateControls(JNIEnv* env, jclass,
                                                                  jfloat moveX, jfloat moveY,
                                                                  jfloat lookDeltaDegrees,
                                                                  jfloat lookPitchDeltaDegrees,
                                                                  jint heldActions) {
  if (heldActions < 0 || heldActions > 0xFFFF) {
    samp::SessionSnapshot invalid;
    invalid.error = "mask input native di luar rentang u16";
    return to_java(env, session_json(invalid));
  }
  samp::input::TouchIntent input;
  input.moveX = moveX;
  input.moveY = moveY;
  input.lookDeltaDegrees = lookDeltaDegrees;
  input.lookPitchDeltaDegrees = lookPitchDeltaDegrees;
  input.heldActions = static_cast<uint16_t>(heldActions);
  return to_java(env, session_json(samp::raknet_session_update_controls(input)));
}

JNIEXPORT void JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionAdvanceGameplay(JNIEnv*, jclass,
                                                                    jfloat deltaSeconds) {
  // Keep the immutable collision-world reference valid for the full gameplay
  // step. No session path takes this data mutex first, so this order cannot
  // invert with the retained-session mutex inside the bridge.
  std::lock_guard<std::mutex> lock(game_data_mutex);
  // Resolve a source-owned handling envelope while holding the same immutable
  // data-runtime lock as collision. The session rechecks `tuningModel` under
  // its own mutex before use, so a concurrent seat/stream change falls back to
  // generic bounded vehicle motion rather than borrowing another model's row.
  const uint32_t tuningModel = samp::raknet_session_local_driver_vehicle_model();
  samp::vehiclemotion::VehicleMotionTuning tuning;
  const samp::vehiclemotion::VehicleMotionTuning* tuningPointer =
      tuningModel != 0u && game_data_runtime.vehicleMotionTuning(tuningModel, tuning)
          ? &tuning
          : nullptr;
  (void)samp::raknet_session_advance_gameplay(
      game_data_runtime.collisionWorld(), deltaSeconds, tuningPointer, tuningModel);
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionWorldSnapshot(JNIEnv* env, jclass) {
  return to_java(env, world_json(samp::raknet_session_world_snapshot()));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionDisconnect(JNIEnv* env, jclass) {
  return to_java(env, session_json(samp::raknet_session_disconnect()));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionStatus(JNIEnv* env, jclass) {
  return to_java(env, session_json(samp::raknet_session_status()));
}

}  // extern "C"
