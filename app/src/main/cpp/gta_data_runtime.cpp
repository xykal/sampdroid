// Read-only user-owned GTA data runtime assembly.
#include "gta_data_runtime.h"

#include "gta_vehicle_handling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>

namespace samp::gta::runtime {
namespace {

constexpr size_t kMaxRuntimeBatchDescriptors = 64;

bool sameIdeDefinition(const map::IdeObject &left,
                       const map::IdeObject &right) {
  return left.modelId == right.modelId && left.modelName == right.modelName &&
         left.textureDictionary == right.textureDictionary &&
         left.handlingName == right.handlingName &&
         left.drawDistance == right.drawDistance && left.flags == right.flags;
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

bool meshTotals(const std::vector<dff::Mesh> &meshes, size_t &meshCount,
                size_t &vertexCount, size_t &indexCount) {
  meshCount = meshes.size();
  vertexCount = 0;
  indexCount = 0;
  for (const dff::Mesh &mesh : meshes) {
    if (mesh.vertices.size() >
            std::numeric_limits<size_t>::max() - vertexCount ||
        mesh.indices.size() > std::numeric_limits<size_t>::max() - indexCount) {
      return false;
    }
    vertexCount += mesh.vertices.size();
    indexCount += mesh.indices.size();
  }
  return true;
}

bool textureTotals(const std::vector<txd::Texture> &textures,
                   size_t &textureCount, size_t &pixelCount) {
  textureCount = textures.size();
  pixelCount = 0;
  for (const txd::Texture &texture : textures) {
    const size_t pixels = static_cast<size_t>(texture.width) * texture.height;
    if (pixels == 0 ||
        pixels > std::numeric_limits<size_t>::max() - pixelCount ||
        pixels > std::numeric_limits<size_t>::max() / 4u ||
        texture.rgba.size() != pixels * 4u) {
      return false;
    }
    pixelCount += pixels;
  }
  return true;
}

bool normalizedPlacementRotation(const map::Quaternion &input,
                                 map::Quaternion &out) {
  if (!std::isfinite(input.x) || !std::isfinite(input.y) ||
      !std::isfinite(input.z) || !std::isfinite(input.w)) {
    return false;
  }
  const float squared = input.x * input.x + input.y * input.y +
                        input.z * input.z + input.w * input.w;
  if (!std::isfinite(squared) || squared < 0.25f || squared > 2.25f)
    return false;
  const float inverseLength = 1.0f / std::sqrt(squared);
  out = {input.x * inverseLength, input.y * inverseLength,
         input.z * inverseLength, input.w * inverseLength};
  return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) &&
         std::isfinite(out.w);
}

dff::Vec3 rotateDffVertex(const map::Quaternion &rotation,
                          const dff::Vec3 &vertex) {
  // Unit-quaternion vector rotation. Both GTA IPL and portable DFF geometry use
  // GTA's x/y horizontal, z-up coordinate basis at this boundary.
  const float crossX = rotation.y * vertex.z - rotation.z * vertex.y;
  const float crossY = rotation.z * vertex.x - rotation.x * vertex.z;
  const float crossZ = rotation.x * vertex.y - rotation.y * vertex.x;
  const float twiceX = crossX * 2.0f;
  const float twiceY = crossY * 2.0f;
  const float twiceZ = crossZ * 2.0f;
  return {vertex.x + rotation.w * twiceX +
              (rotation.y * twiceZ - rotation.z * twiceY),
          vertex.y + rotation.w * twiceY +
              (rotation.z * twiceX - rotation.x * twiceZ),
          vertex.z + rotation.w * twiceZ +
              (rotation.x * twiceY - rotation.y * twiceX)};
}

bool validServerObjectAngles(float x, float y, float z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         std::fabs(x) <= 360000.0f && std::fabs(y) <= 360000.0f &&
         std::fabs(z) <= 360000.0f;
}

bool validServerObjectScale(float x, float y, float z) {
  const auto validAxis = [](float value) {
    return std::isfinite(value) && std::fabs(value) >= 0.001f && std::fabs(value) <= 100.0f;
  };
  return validAxis(x) && validAxis(y) && validAxis(z);
}

bool validServerObjectMaterialLookupName(const std::string& value) {
  if (value.empty() || value.size() > 31u) return false;
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte > 0x7Eu || byte == '/' || byte == '\\' || byte == ':') {
      return false;
    }
  }
  return true;
}

// Textdraw sprite operands share the same narrow TXD key surface as material
// lookups. Normalize at the native asset boundary because TXD/IMG decoders
// retain lowercase names; Java never receives a chance to turn a server string
// into a filesystem path or choose a case-sensitive fallback.
bool normalizeTextureLookupName(std::string_view input, std::string& output) {
  output.clear();
  if (input.empty() || input.size() > 31u) return false;
  output.reserve(input.size());
  for (const char character : input) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte > 0x7Eu || byte == '/' || byte == '\\' ||
        byte == ':' || byte == '.') {
      output.clear();
      return false;
    }
    char normalized = static_cast<char>(byte);
    if (normalized >= 'A' && normalized <= 'Z') {
      normalized = static_cast<char>(normalized - 'A' + 'a');
    }
    output.push_back(normalized);
  }
  return true;
}

dff::Rgba rgbaFromAbgr(uint32_t abgr) {
  return {static_cast<uint8_t>(abgr & 0xFFu),
          static_cast<uint8_t>((abgr >> 8u) & 0xFFu),
          static_cast<uint8_t>((abgr >> 16u) & 0xFFu),
          static_cast<uint8_t>((abgr >> 24u) & 0xFFu)};
}

// Text material colour fields are source API ARGB, unlike Default-material
// ABGR. Keep the conversion adjacent to the wire-policy helpers so no GLES
// call has to guess a colour convention.
dff::Rgba rgbaFromArgb(uint32_t argb) {
  return {static_cast<uint8_t>((argb >> 16u) & 0xFFu),
          static_cast<uint8_t>((argb >> 8u) & 0xFFu),
          static_cast<uint8_t>(argb & 0xFFu),
          static_cast<uint8_t>((argb >> 24u) & 0xFFu)};
}

bool textMaterialDimensions(uint8_t materialSize, uint16_t& width, uint16_t& height) {
  switch (materialSize) {
    case 10: width = 32; height = 32; return true;
    case 20: width = 64; height = 32; return true;
    case 30: width = 64; height = 64; return true;
    case 40: width = 128; height = 32; return true;
    case 50: width = 128; height = 64; return true;
    case 60: width = 128; height = 128; return true;
    case 70: width = 256; height = 32; return true;
    case 80: width = 256; height = 64; return true;
    case 90: width = 256; height = 128; return true;
    case 100: width = 256; height = 256; return true;
    case 110: width = 512; height = 64; return true;
    case 120: width = 512; height = 128; return true;
    case 130: width = 512; height = 256; return true;
    case 140: width = 512; height = 512; return true;
    default: width = 0; height = 0; return false;
  }
}

bool validTextMaterialInput(const ServerObjectMaterialOverrideInput& material) {
  uint16_t width = 0;
  uint16_t height = 0;
  if (!material.textActive || !textMaterialDimensions(material.textMaterialSize, width, height) ||
      material.textAlignment > 2u || material.text.size() > 31u) {
    return false;
  }
  for (const unsigned char byte : material.text) {
    if (byte == 0u || (byte < 0x20u && byte != '\n' && byte != '\r' && byte != '\t')) {
      return false;
    }
  }
  return true;
}

struct BitmapGlyph {
  std::array<uint8_t, 7> rows{};
};

// A deliberately small native 5x7 fallback glyph set. ObjectMaterialText's
// font face is a GTA client asset choice; Android has no bundled GTA font, so
// this asset-free rasterizer preserves text, size, alignment, bold, and source
// colours without claiming exact Arial/Pricedown glyph fidelity.
BitmapGlyph bitmapGlyphFor(char character) {
  if (character >= 'a' && character <= 'z') {
    character = static_cast<char>(character - 'a' + 'A');
  }
  switch (character) {
    case 'A': return {{{0x0Eu,0x11u,0x11u,0x1Fu,0x11u,0x11u,0x11u}}};
    case 'B': return {{{0x1Eu,0x11u,0x11u,0x1Eu,0x11u,0x11u,0x1Eu}}};
    case 'C': return {{{0x0Eu,0x11u,0x10u,0x10u,0x10u,0x11u,0x0Eu}}};
    case 'D': return {{{0x1Eu,0x11u,0x11u,0x11u,0x11u,0x11u,0x1Eu}}};
    case 'E': return {{{0x1Fu,0x10u,0x10u,0x1Eu,0x10u,0x10u,0x1Fu}}};
    case 'F': return {{{0x1Fu,0x10u,0x10u,0x1Eu,0x10u,0x10u,0x10u}}};
    case 'G': return {{{0x0Eu,0x11u,0x10u,0x17u,0x11u,0x11u,0x0Fu}}};
    case 'H': return {{{0x11u,0x11u,0x11u,0x1Fu,0x11u,0x11u,0x11u}}};
    case 'I': return {{{0x0Eu,0x04u,0x04u,0x04u,0x04u,0x04u,0x0Eu}}};
    case 'J': return {{{0x07u,0x02u,0x02u,0x02u,0x12u,0x12u,0x0Cu}}};
    case 'K': return {{{0x11u,0x12u,0x14u,0x18u,0x14u,0x12u,0x11u}}};
    case 'L': return {{{0x10u,0x10u,0x10u,0x10u,0x10u,0x10u,0x1Fu}}};
    case 'M': return {{{0x11u,0x1Bu,0x15u,0x15u,0x11u,0x11u,0x11u}}};
    case 'N': return {{{0x11u,0x19u,0x15u,0x13u,0x11u,0x11u,0x11u}}};
    case 'O': return {{{0x0Eu,0x11u,0x11u,0x11u,0x11u,0x11u,0x0Eu}}};
    case 'P': return {{{0x1Eu,0x11u,0x11u,0x1Eu,0x10u,0x10u,0x10u}}};
    case 'Q': return {{{0x0Eu,0x11u,0x11u,0x11u,0x15u,0x12u,0x0Du}}};
    case 'R': return {{{0x1Eu,0x11u,0x11u,0x1Eu,0x14u,0x12u,0x11u}}};
    case 'S': return {{{0x0Fu,0x10u,0x10u,0x0Eu,0x01u,0x01u,0x1Eu}}};
    case 'T': return {{{0x1Fu,0x04u,0x04u,0x04u,0x04u,0x04u,0x04u}}};
    case 'U': return {{{0x11u,0x11u,0x11u,0x11u,0x11u,0x11u,0x0Eu}}};
    case 'V': return {{{0x11u,0x11u,0x11u,0x11u,0x11u,0x0Au,0x04u}}};
    case 'W': return {{{0x11u,0x11u,0x11u,0x15u,0x15u,0x15u,0x0Au}}};
    case 'X': return {{{0x11u,0x11u,0x0Au,0x04u,0x0Au,0x11u,0x11u}}};
    case 'Y': return {{{0x11u,0x11u,0x0Au,0x04u,0x04u,0x04u,0x04u}}};
    case 'Z': return {{{0x1Fu,0x01u,0x02u,0x04u,0x08u,0x10u,0x1Fu}}};
    case '0': return {{{0x0Eu,0x11u,0x13u,0x15u,0x19u,0x11u,0x0Eu}}};
    case '1': return {{{0x04u,0x0Cu,0x04u,0x04u,0x04u,0x04u,0x0Eu}}};
    case '2': return {{{0x0Eu,0x11u,0x01u,0x02u,0x04u,0x08u,0x1Fu}}};
    case '3': return {{{0x1Eu,0x01u,0x01u,0x0Eu,0x01u,0x01u,0x1Eu}}};
    case '4': return {{{0x02u,0x06u,0x0Au,0x12u,0x1Fu,0x02u,0x02u}}};
    case '5': return {{{0x1Fu,0x10u,0x10u,0x1Eu,0x01u,0x01u,0x1Eu}}};
    case '6': return {{{0x0Eu,0x10u,0x10u,0x1Eu,0x11u,0x11u,0x0Eu}}};
    case '7': return {{{0x1Fu,0x01u,0x02u,0x04u,0x08u,0x08u,0x08u}}};
    case '8': return {{{0x0Eu,0x11u,0x11u,0x0Eu,0x11u,0x11u,0x0Eu}}};
    case '9': return {{{0x0Eu,0x11u,0x11u,0x0Fu,0x01u,0x01u,0x0Eu}}};
    case ' ': return {{{0,0,0,0,0,0,0}}};
    case '.': return {{{0,0,0,0,0,0x06u,0x06u}}};
    case ',': return {{{0,0,0,0,0,0x06u,0x04u}}};
    case ':': return {{{0,0x06u,0x06u,0,0x06u,0x06u,0}}};
    case ';': return {{{0,0x06u,0x06u,0,0x06u,0x04u,0}}};
    case '!': return {{{0x04u,0x04u,0x04u,0x04u,0x04u,0,0x04u}}};
    case '?': return {{{0x0Eu,0x11u,0x01u,0x02u,0x04u,0,0x04u}}};
    case '-': return {{{0,0,0,0x1Fu,0,0,0}}};
    case '_': return {{{0,0,0,0,0,0,0x1Fu}}};
    case '+': return {{{0,0x04u,0x04u,0x1Fu,0x04u,0x04u,0}}};
    case '=': return {{{0,0x1Fu,0,0x1Fu,0,0,0}}};
    case '/': return {{{0x01u,0x02u,0x04u,0x08u,0x10u,0,0}}};
    case '\\': return {{{0x10u,0x08u,0x04u,0x02u,0x01u,0,0}}};
    case '(': return {{{0x02u,0x04u,0x08u,0x08u,0x08u,0x04u,0x02u}}};
    case ')': return {{{0x08u,0x04u,0x02u,0x02u,0x02u,0x04u,0x08u}}};
    case '[': return {{{0x0Eu,0x08u,0x08u,0x08u,0x08u,0x08u,0x0Eu}}};
    case ']': return {{{0x0Eu,0x02u,0x02u,0x02u,0x02u,0x02u,0x0Eu}}};
    case '#': return {{{0x0Au,0x1Fu,0x0Au,0x0Au,0x1Fu,0x0Au,0}}};
    case '*': return {{{0,0x15u,0x0Eu,0x1Fu,0x0Eu,0x15u,0}}};
    case '\'': return {{{0x04u,0x04u,0,0,0,0,0}}};
    case '"': return {{{0x0Au,0x0Au,0,0,0,0,0}}};
    default: return {{{0x0Eu,0x11u,0x01u,0x02u,0x04u,0,0x04u}}};
  }
}

void putRgbaPixel(std::vector<uint8_t>& pixels, uint16_t width, uint16_t height,
                  int x, int y, const dff::Rgba& colour) {
  if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return;
  const size_t offset = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4u;
  pixels[offset] = colour.r;
  pixels[offset + 1u] = colour.g;
  pixels[offset + 2u] = colour.b;
  pixels[offset + 3u] = colour.a;
}

bool rasterizeTextMaterial(const ServerObjectMaterialOverrideInput& material,
                           uint16_t& width, uint16_t& height,
                           std::vector<uint8_t>& rgba) {
  rgba.clear();
  if (!validTextMaterialInput(material) || !textMaterialDimensions(material.textMaterialSize,
                                                                     width, height)) {
    return false;
  }
  const size_t pixelCount = static_cast<size_t>(width) * height;
  if (pixelCount == 0u || pixelCount > kMaxDffTexturedPreviewPixels ||
      pixelCount > std::numeric_limits<size_t>::max() / 4u) {
    return false;
  }
  const dff::Rgba background = rgbaFromArgb(material.textBackgroundColourArgb);
  const dff::Rgba foreground = rgbaFromArgb(material.textColourArgb);
  rgba.resize(pixelCount * 4u);
  for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
    const size_t offset = pixel * 4u;
    rgba[offset] = background.r;
    rgba[offset + 1u] = background.g;
    rgba[offset + 2u] = background.b;
    rgba[offset + 3u] = background.a;
  }

  std::vector<std::string> lines(1);
  for (size_t index = 0; index < material.text.size();) {
    const unsigned char byte = static_cast<unsigned char>(material.text[index]);
    if (byte == '\r') {
      ++index;
      continue;
    }
    if (byte == '\n') {
      if (lines.size() < 4u) lines.emplace_back();
      ++index;
      continue;
    }
    // The fallback glyph set is ASCII-only. Consume one well-formed UTF-8
    // codepoint as one visible `?`, rather than exploding every continuation
    // byte into another glyph and changing alignment/line fitting.
    char glyph = static_cast<char>(byte);
    size_t consumed = 1u;
    if (byte >= 0x80u) {
      glyph = '?';
      const size_t expected = byte >= 0xF0u ? 4u : byte >= 0xE0u ? 3u : 2u;
      while (consumed < expected && index + consumed < material.text.size() &&
             (static_cast<unsigned char>(material.text[index + consumed]) & 0xC0u) == 0x80u) {
        ++consumed;
      }
    }
    if (lines.back().size() < 31u) lines.back().push_back(glyph);
    index += consumed;
  }
  size_t widest = 0;
  for (const std::string& line : lines) widest = std::max(widest, line.size());
  if (widest == 0u) return true;
  const int preferredScale = std::clamp((static_cast<int>(material.textFontSize) + 5) / 7,
                                        1, 16);
  const int horizontalScale = std::max(1, (static_cast<int>(width) - 2) /
                                            std::max(1, static_cast<int>(widest) * 6));
  const int verticalScale = std::max(1, (static_cast<int>(height) - 2) /
                                          std::max(1, static_cast<int>(lines.size()) * 8));
  const int scale = std::min(preferredScale, std::min(horizontalScale, verticalScale));
  const int totalHeight = static_cast<int>(lines.size()) * 8 * scale - scale;
  int y = std::max(0, (static_cast<int>(height) - totalHeight) / 2);
  for (const std::string& line : lines) {
    const int lineWidth = static_cast<int>(line.size()) * 6 * scale - scale;
    int x = 1;
    if (material.textAlignment == 1u) x = (static_cast<int>(width) - lineWidth) / 2;
    if (material.textAlignment == 2u) x = static_cast<int>(width) - 1 - lineWidth;
    x = std::max(0, x);
    for (const char character : line) {
      const BitmapGlyph glyph = bitmapGlyphFor(character);
      for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 5; ++column) {
          if ((glyph.rows[static_cast<size_t>(row)] & (1u << (4 - column))) == 0u) continue;
          for (int dy = 0; dy < scale; ++dy) {
            for (int dx = 0; dx < scale; ++dx) {
              putRgbaPixel(rgba, width, height, x + column * scale + dx,
                           y + row * scale + dy, foreground);
              if (material.textBold) {
                putRgbaPixel(rgba, width, height, x + column * scale + dx + 1,
                             y + row * scale + dy, foreground);
              }
            }
          }
        }
      }
      x += 6 * scale;
    }
    y += 8 * scale;
  }
  return true;
}

bool validServerObjectMaterialOverrideInput(const ServerObjectMaterialOverrideInput& material) {
  if (material.active && material.textActive) return false;
  if (material.active) {
    // API model -1 is retained as u16 0xFFFF. It is explicitly color-only:
    // source writers may leave both lookup strings empty because no TXD lookup
    // is legal for this sentinel. Ordinary Default records still require two
    // portable names, so this exception cannot open a filesystem-key path.
    const bool emptyColorOnly = material.sourceModel == 0xFFFFu &&
                                material.textureDictionary.empty() &&
                                material.textureName.empty();
    return emptyColorOnly ||
           (validServerObjectMaterialLookupName(material.textureDictionary) &&
            validServerObjectMaterialLookupName(material.textureName));
  }
  return !material.textActive || validTextMaterialInput(material);
}

std::string textMaterialCacheKey(const ServerObjectMaterialOverrideInput& material) {
  // Unit separators cannot occur in a valid text source (controls are rejected
  // at JNI/source boundaries); this preserves distinct colour/format/text
  // combinations without hashing user bytes or retaining a Java identity.
  return std::to_string(material.textMaterialSize) + "\x1F" +
         std::to_string(material.textFontSize) + "\x1F" +
         std::to_string(material.textBold ? 1 : 0) + "\x1F" +
         std::to_string(material.textColourArgb) + "\x1F" +
         std::to_string(material.textBackgroundColourArgb) + "\x1F" +
         std::to_string(material.textAlignment) + "\x1F" + material.text;
}

const ServerObjectMaterialOverrideInput* materialOverrideAt(
    const ServerObjectRenderInput& input, size_t flattenedMaterialIndex) {
  if (flattenedMaterialIndex >= input.materialOverrides.size()) return nullptr;
  const ServerObjectMaterialOverrideInput& override =
      input.materialOverrides[flattenedMaterialIndex];
  if (!validServerObjectMaterialOverrideInput(override)) return nullptr;
  return (override.active || override.textActive) ? &override : nullptr;
}

dff::Vec3 scaledServerObjectVertex(const ServerObjectRenderInput& input,
                                   const dff::Vec3& vertex) {
  return {vertex.x * input.scaleX, vertex.y * input.scaleY, vertex.z * input.scaleZ};
}

dff::Vec3 rotateServerObjectEuler(float rotationX, float rotationY,
                                  float rotationZ, const dff::Vec3 &vertex) {
  constexpr float kDegreesToRadians = 0.01745329251994329577f;
  const float xRadians = rotationX * kDegreesToRadians;
  const float yRadians = rotationY * kDegreesToRadians;
  const float zRadians = rotationZ * kDegreesToRadians;
  const float cosX = std::cos(xRadians);
  const float sinX = std::sin(xRadians);
  const float cosY = std::cos(yRadians);
  const float sinY = std::sin(yRadians);
  const float cosZ = std::cos(zRadians);
  const float sinZ = std::sin(zRadians);
  // Apply GTA Euler X → Y → Z in model space; the caller performs the one
  // canonical GTA-to-GLES axis conversion after adding world position.
  const float x1 = vertex.x;
  const float y1 = vertex.y * cosX - vertex.z * sinX;
  const float z1 = vertex.y * sinX + vertex.z * cosX;
  const float x2 = x1 * cosY + z1 * sinY;
  const float y2 = y1;
  const float z2 = -x1 * sinY + z1 * cosY;
  return {x2 * cosZ - y2 * sinZ, x2 * sinZ + y2 * cosZ, z2};
}

bool finiteDffVertex(const dff::Vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

const txd::Texture *
findTextureByName(const std::vector<txd::Texture> &dictionary,
                  const std::string &name) {
  for (const txd::Texture &texture : dictionary) {
    if (texture.name == name)
      return &texture;
  }
  return nullptr;
}

bool validateTxdParentGraph(
    const std::unordered_map<std::string, std::string> &parents,
    std::string &error) {
  if (parents.size() > kMaxRuntimeTxdParentRelations) {
    error = "jumlah relasi parent TXD melebihi batas aman";
    return false;
  }
  // Do not recurse through a user-owned relation graph. Each lookup is capped
  // independently, and any cycle (including a self-parent) is rejected before
  // the transaction commits.
  for (const auto &relation : parents) {
    std::unordered_set<std::string> chain;
    chain.reserve(kMaxRuntimeTxdParentLookupDepth + 1u);
    std::string current = relation.first;
    for (size_t depth = 0;; ++depth) {
      if (depth > kMaxRuntimeTxdParentLookupDepth) {
        error = "rantai parent TXD melebihi batas aman";
        return false;
      }
      if (!chain.insert(current).second) {
        error = "siklus parent TXD tidak didukung";
        return false;
      }
      const auto next = parents.find(current);
      if (next == parents.end())
        break;
      current = next->second;
    }
  }
  return true;
}

bool hasBinaryIplMagic(const std::vector<uint8_t> &bytes) {
  return bytes.size() >= 4u && bytes[0] == static_cast<uint8_t>('b') &&
         bytes[1] == static_cast<uint8_t>('n') &&
         bytes[2] == static_cast<uint8_t>('r') &&
         bytes[3] == static_cast<uint8_t>('y');
}

std::string manifestIplBaseName(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  const size_t begin = slash == std::string::npos ? 0u : slash + 1u;
  if (begin >= path.size() || !endsWith(path, ".ipl") ||
      path.size() <= begin + 4u)
    return {};
  return path.substr(begin, path.size() - begin - 4u);
}

bool isStreamingIplMemberFor(
    const std::string &name,
    const std::unordered_set<std::string> &parentBases) {
  if (!endsWith(name, ".ipl") || name.size() <= 4u)
    return false;
  const size_t stream = name.rfind("_stream");
  if (stream == std::string::npos || stream == 0u ||
      stream + 7u >= name.size() - 4u)
    return false;
  if (parentBases.find(name.substr(0, stream)) == parentBases.end())
    return false;
  for (size_t index = stream + 7u; index < name.size() - 4u; ++index) {
    if (name[index] < '0' || name[index] > '9')
      return false;
  }
  return true;
}

} // namespace

void GameDataRuntime::clearMapAssembly() {
  collisionWorld_ = {};
  worldReport_ = {};
  placements_.clear();
  ideObjects_.clear();
  txdParentByChild_.clear();
  collisionCatalog_.reset();
}

void GameDataRuntime::clear() {
  clearMapAssembly();
  dffModels_.clear();
  unavailableDffModels_.clear();
  latestDffModel_.clear();
  dffMeshes_ = 0;
  dffVertices_ = 0;
  dffIndices_ = 0;
  txdDictionaries_.clear();
  unavailableTxdDictionaries_.clear();
  textures_ = 0;
  texturePixels_ = 0;
  assetManifest_ = {};
  handlingProfiles_.clear();
  auxiliaryImages_.clear();
  imageIndex_ = {};
  imageSource_.reset();
}

Snapshot GameDataRuntime::snapshot() const {
  Snapshot out;
  out.imgMounted = imageSource_ != nullptr && !imageIndex_.empty();
  out.imgBytes = imageSource_ ? imageSource_->size() : 0;
  out.imgEntries = imageIndex_.entries().size();
  out.additionalImgArchives = auxiliaryImages_.size();
  for (const AuxiliaryImgArchive &archive : auxiliaryImages_) {
    out.additionalImgEntries += archive.index.entries().size();
    if (archive.source) {
      const uint64_t bytes = archive.source->size();
      out.additionalImgBytes =
          bytes > std::numeric_limits<uint64_t>::max() - out.additionalImgBytes
              ? std::numeric_limits<uint64_t>::max()
              : out.additionalImgBytes + bytes;
    }
  }
  out.collisionModels = collisionCatalog_ ? collisionCatalog_->modelCount() : 0;
  out.dffModels = dffModels_.size();
  out.dffMeshes = dffMeshes_;
  out.dffVertices = dffVertices_;
  out.txdDictionaries = txdDictionaries_.size();
  out.txdParentRelations = txdParentByChild_.size();
  out.textures = textures_;
  out.texturePixels = texturePixels_;
  out.manifestIdePaths = assetManifest_.idePaths.size();
  out.manifestIplPaths = assetManifest_.iplPaths.size();
  out.manifestColPaths = assetManifest_.colPaths.size();
  out.manifestImgPaths = assetManifest_.imgPaths.size();
  out.ideObjects = ideObjects_.size();
  out.handlingProfiles = handlingProfiles_.size();
  out.iplPlacements = placements_.size();
  out.collisionInstances = collisionWorld_.instanceCount();
  out.missingCollisionPlacements = worldReport_.missingCollisionModels;
  return out;
}

bool GameDataRuntime::readDescriptorBytes(int fileDescriptor, size_t maxBytes,
                                          std::vector<uint8_t> &out,
                                          std::string &error) const {
  out.clear();
  error.clear();
  if (maxBytes == 0) {
    error = "batas baca data GTA tidak valid";
    return false;
  }
  auto source =
      img::FileDescriptorByteSource::duplicateFrom(fileDescriptor, error);
  if (!source)
    return false;
  const uint64_t byteCount = source->size();
  if (byteCount == 0 || byteCount > maxBytes ||
      byteCount > std::numeric_limits<size_t>::max()) {
    error = "file data GTA kosong atau melebihi batas aman";
    return false;
  }
  out.resize(static_cast<size_t>(byteCount));
  if (!source->readAt(0, out.data(), out.size())) {
    out.clear();
    error = "gagal membaca file data GTA";
    return false;
  }
  return true;
}

bool GameDataRuntime::readDescriptorText(int fileDescriptor, std::string &out,
                                         std::string &error) const {
  std::vector<uint8_t> bytes;
  if (!readDescriptorBytes(fileDescriptor, kMaxMapTextBytes, bytes, error))
    return false;
  if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) {
    error = "file map text mengandung byte NUL";
    return false;
  }
  size_t offset = 0;
  if (bytes.size() >= 3 && bytes[0] == 0xEFu && bytes[1] == 0xBBu &&
      bytes[2] == 0xBFu) {
    offset = 3; // tolerate a UTF-8 BOM, but not arbitrary binary encodings.
  }
  out.assign(reinterpret_cast<const char *>(bytes.data() + offset),
             bytes.size() - offset);
  return true;
}

bool GameDataRuntime::rebuildWorld(const world::CollisionCatalog &catalog,
                                   const std::vector<map::Instance> &placements,
                                   world::CollisionWorld &out,
                                   world::CollisionWorldBuildReport &report,
                                   std::string &error) const {
  return world::buildCollisionWorld(catalog, placements, out, report, error);
}

bool GameDataRuntime::mountImgFileDescriptor(int fileDescriptor,
                                             std::string &error) {
  error.clear();
  auto candidateSource =
      img::FileDescriptorByteSource::duplicateFrom(fileDescriptor, error);
  if (!candidateSource)
    return false;
  img::ArchiveIndex candidateIndex;
  if (!img::parseV2(*candidateSource, candidateIndex, error))
    return false;

  // Commit only after the descriptor and index both validate. A new IMG is a
  // new source universe, so stale map/collision bindings must not survive.
  collisionWorld_ = {};
  worldReport_ = {};
  placements_.clear();
  ideObjects_.clear();
  txdParentByChild_.clear();
  dffModels_.clear();
  unavailableDffModels_.clear();
  latestDffModel_.clear();
  dffMeshes_ = 0;
  dffVertices_ = 0;
  dffIndices_ = 0;
  txdDictionaries_.clear();
  unavailableTxdDictionaries_.clear();
  textures_ = 0;
  texturePixels_ = 0;
  assetManifest_ = {};
  handlingProfiles_.clear();
  collisionCatalog_.reset();
  auxiliaryImages_.clear();
  imageIndex_ = std::move(candidateIndex);
  imageSource_ = std::move(candidateSource);
  return true;
}

bool GameDataRuntime::mountAdditionalImgFileDescriptors(
    const std::vector<int> &fileDescriptors, std::string &error) {
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA utama belum dipasang";
    return false;
  }
  if (fileDescriptors.empty() ||
      fileDescriptors.size() > kMaxRuntimeAdditionalImgArchives ||
      auxiliaryImages_.size() >
          kMaxRuntimeAdditionalImgArchives - fileDescriptors.size()) {
    error = "jumlah IMG tambahan tidak valid atau melebihi batas aman";
    return false;
  }
  size_t totalEntries = imageIndex_.entries().size();
  if (totalEntries > kMaxRuntimeMountedImgEntries) {
    error = "catalog IMG utama melebihi batas gabungan aman";
    return false;
  }
  for (const AuxiliaryImgArchive &archive : auxiliaryImages_) {
    if (archive.index.entries().size() >
        kMaxRuntimeMountedImgEntries - totalEntries) {
      error = "catalog IMG tambahan saat ini tidak konsisten";
      return false;
    }
    totalEntries += archive.index.entries().size();
  }

  std::vector<AuxiliaryImgArchive> candidates;
  candidates.reserve(fileDescriptors.size());
  for (const int fileDescriptor : fileDescriptors) {
    if (fileDescriptor < 0) {
      error = "descriptor IMG tambahan tidak valid";
      return false;
    }
    AuxiliaryImgArchive candidate;
    candidate.source =
        img::FileDescriptorByteSource::duplicateFrom(fileDescriptor, error);
    if (!candidate.source ||
        !img::parseV2(*candidate.source, candidate.index, error))
      return false;
    if (candidate.index.entries().size() >
        kMaxRuntimeMountedImgEntries - totalEntries) {
      error = "jumlah entry IMG tambahan melebihi batas aman";
      return false;
    }
    totalEntries += candidate.index.entries().size();
    candidates.push_back(std::move(candidate));
  }

  auxiliaryImages_.reserve(auxiliaryImages_.size() + candidates.size());
  for (AuxiliaryImgArchive &candidate : candidates) {
    auxiliaryImages_.push_back(std::move(candidate));
  }
  // A previously absent member may now exist in an auxiliary archive. Retain
  // parsed catalogs, but let bounded lazy rendering retry only those misses.
  unavailableDffModels_.clear();
  unavailableTxdDictionaries_.clear();
  return true;
}

bool GameDataRuntime::findMountedImgMember(
    std::string_view memberName, img::FileDescriptorByteSource *&source,
    const img::Entry *&entry) {
  source = nullptr;
  entry = nullptr;
  if (imageSource_) {
    if (const img::Entry *primary = imageIndex_.find(memberName)) {
      source = imageSource_.get();
      entry = primary;
      return true;
    }
  }
  for (AuxiliaryImgArchive &archive : auxiliaryImages_) {
    if (!archive.source)
      continue;
    if (const img::Entry *auxiliary = archive.index.find(memberName)) {
      source = archive.source.get();
      entry = auxiliary;
      return true;
    }
  }
  return false;
}

bool GameDataRuntime::loadImgCollisionMember(std::string_view memberName,
                                             std::string &error) {
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang";
    return false;
  }
  img::FileDescriptorByteSource *source = nullptr;
  const img::Entry *entry = nullptr;
  if (!findMountedImgMember(memberName, source, entry) || !source || !entry ||
      !endsWith(entry->name, ".col")) {
    error = "member IMG yang dipilih bukan file .col";
    return false;
  }
  // The source already owns the resolved entry. Reuse its canonical indexed
  // name rather than trusting the caller's casing/path spelling.
  const img::ArchiveIndex *index = &imageIndex_;
  if (source != imageSource_.get()) {
    index = nullptr;
    for (const AuxiliaryImgArchive &archive : auxiliaryImages_) {
      if (archive.source.get() == source) {
        index = &archive.index;
        break;
      }
    }
  }
  if (!index) {
    error = "sumber IMG collision tidak konsisten";
    return false;
  }
  auto candidateCatalog = std::make_unique<world::CollisionCatalog>(
      collisionCatalog_ ? *collisionCatalog_ : world::CollisionCatalog{});
  if (!world::loadCollisionImgMember(*source, *index, entry->name,
                                     *candidateCatalog, error)) {
    return false;
  }
  world::CollisionWorld candidateWorld;
  world::CollisionWorldBuildReport candidateReport;
  if (!rebuildWorld(*candidateCatalog, placements_, candidateWorld,
                    candidateReport, error))
    return false;
  collisionCatalog_ = std::move(candidateCatalog);
  collisionWorld_ = std::move(candidateWorld);
  worldReport_ = candidateReport;
  return true;
}

bool GameDataRuntime::loadAllImgCollisionMembers(std::string &error) {
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang";
    return false;
  }

  struct CollisionMember {
    img::FileDescriptorByteSource *source = nullptr;
    const img::ArchiveIndex *index = nullptr;
    const img::Entry *entry = nullptr;
    // Primary is loaded last when a model name collides, matching the primary-
    // first DFF/TXD lookup policy without relying on IMG directory order.
    size_t sourcePriority = 0;
  };
  std::vector<CollisionMember> members;
  members.reserve(kMaxRuntimeImgCollisionMembers);
  uint64_t totalBytes = 0;
  const auto collect = [&](img::FileDescriptorByteSource *source,
                           const img::ArchiveIndex *index,
                           size_t sourcePriority) -> bool {
    if (!source || !index)
      return false;
    for (const img::Entry &entry : index->entries()) {
      if (!endsWith(entry.name, ".col"))
        continue;
      if (entry.sizeBytes == 0 || entry.sizeBytes > img::kMaxExtractBytes ||
          members.size() >= kMaxRuntimeImgCollisionMembers ||
          entry.sizeBytes > kMaxRuntimeImgCollisionBytes - totalBytes) {
        error = "catalog member COL IMG melebihi batas aman";
        return false;
      }
      totalBytes += entry.sizeBytes;
      members.push_back({source, index, &entry, sourcePriority});
    }
    return true;
  };
  // Auxiliary archives are gathered first and the primary is applied last, so
  // a duplicate model follows the same primary-first resolution contract as
  // DFF/TXD assets. All mounted archives share one member/byte budget.
  for (size_t index = 0; index < auxiliaryImages_.size(); ++index) {
    const AuxiliaryImgArchive &archive = auxiliaryImages_[index];
    if (!collect(archive.source.get(), &archive.index, index + 1u))
      return false;
  }
  if (!collect(imageSource_.get(), &imageIndex_, 0u))
    return false;
  // Some valid roots keep collision in external COLFILE paths; absence of IMG
  // chunks is therefore a safe no-op, not a failed tree import.
  if (members.empty())
    return true;
  // Use a stable lexical order rather than physical IMG directory order. Sort
  // lower-precedence auxiliaries before the primary; archives supplied later
  // also load before earlier auxiliaries, preserving a deterministic override.
  std::sort(members.begin(), members.end(),
            [](const CollisionMember &left, const CollisionMember &right) {
              if (left.sourcePriority != right.sourcePriority)
                return left.sourcePriority > right.sourcePriority;
              const std::string_view leftName = left.entry ? left.entry->name : std::string_view{};
              const std::string_view rightName = right.entry ? right.entry->name : std::string_view{};
              return leftName < rightName;
            });

  auto candidateCatalog = std::make_unique<world::CollisionCatalog>(
      collisionCatalog_ ? *collisionCatalog_ : world::CollisionCatalog{});
  for (const CollisionMember &member : members) {
    if (!member.source || !member.index || !member.entry ||
        !world::loadCollisionImgMember(*member.source, *member.index, member.entry->name,
                                       *candidateCatalog, error)) {
      return false;
    }
  }

  world::CollisionWorld candidateWorld;
  world::CollisionWorldBuildReport candidateReport;
  if (!rebuildWorld(*candidateCatalog, placements_, candidateWorld,
                    candidateReport, error))
    return false;
  collisionCatalog_ = std::move(candidateCatalog);
  collisionWorld_ = std::move(candidateWorld);
  worldReport_ = candidateReport;
  return true;
}

bool GameDataRuntime::loadImgDffMember(std::string_view memberName,
                                       std::string &error) {
  return loadImgDffMemberInternal(memberName, true, error);
}

bool GameDataRuntime::loadImgDffMemberInternal(std::string_view memberName,
                                               bool selectLatest,
                                               std::string &error) {
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang";
    return false;
  }
  img::FileDescriptorByteSource *source = nullptr;
  const img::Entry *entry = nullptr;
  if (!findMountedImgMember(memberName, source, entry) || !source || !entry ||
      !endsWith(entry->name, ".dff")) {
    error = "member IMG yang dipilih bukan file .dff";
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!img::readEntry(*source, *entry, img::kMaxExtractBytes, bytes, error))
    return false;
  std::vector<dff::Mesh> meshes;
  if (!dff::parsePortableMeshes(bytes, meshes, error))
    return false;

  size_t newMeshes = 0;
  size_t newVertices = 0;
  size_t newIndices = 0;
  if (!meshTotals(meshes, newMeshes, newVertices, newIndices)) {
    error = "jumlah mesh DFF overflow";
    return false;
  }
  const std::string modelName = entry->name.substr(0, entry->name.size() - 4);
  if (modelName.empty()) {
    error = "nama model member DFF tidak valid";
    return false;
  }
  // Allocate the replacement display key before catalog mutation so selecting
  // the latest manual preview cannot leave a partially committed catalog.
  std::string nextLatestModel;
  if (selectLatest)
    nextLatestModel.assign(modelName);
  const auto existing = dffModels_.find(modelName);
  size_t oldMeshes = 0;
  size_t oldVertices = 0;
  size_t oldIndices = 0;
  if (existing != dffModels_.end() &&
      !meshTotals(existing->second, oldMeshes, oldVertices, oldIndices)) {
    error = "catalog mesh DFF saat ini tidak konsisten";
    return false;
  }
  if (oldMeshes > dffMeshes_ || oldVertices > dffVertices_ ||
      oldIndices > dffIndices_) {
    error = "counter catalog mesh DFF tidak konsisten";
    return false;
  }
  const size_t baseMeshes = dffMeshes_ - oldMeshes;
  const size_t baseVertices = dffVertices_ - oldVertices;
  const size_t baseIndices = dffIndices_ - oldIndices;
  if ((existing == dffModels_.end() &&
       dffModels_.size() >= kMaxRuntimeDffModels) ||
      baseMeshes > kMaxRuntimeDffMeshes ||
      baseVertices > kMaxRuntimeDffVertices ||
      baseIndices > kMaxRuntimeDffIndices ||
      newMeshes > kMaxRuntimeDffMeshes - baseMeshes ||
      newVertices > kMaxRuntimeDffVertices - baseVertices ||
      newIndices > kMaxRuntimeDffIndices - baseIndices) {
    error = "catalog mesh DFF melebihi batas aman";
    return false;
  }

  if (existing == dffModels_.end()) {
    dffModels_.emplace(modelName, std::move(meshes));
  } else {
    existing->second = std::move(meshes);
  }
  dffMeshes_ = baseMeshes + newMeshes;
  dffVertices_ = baseVertices + newVertices;
  dffIndices_ = baseIndices + newIndices;
  unavailableDffModels_.erase(modelName);
  if (selectLatest)
    latestDffModel_.swap(nextLatestModel);
  return true;
}

bool GameDataRuntime::loadImgTxdMember(std::string_view memberName,
                                       std::string &error) {
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang";
    return false;
  }
  img::FileDescriptorByteSource *source = nullptr;
  const img::Entry *entry = nullptr;
  if (!findMountedImgMember(memberName, source, entry) || !source || !entry ||
      !endsWith(entry->name, ".txd")) {
    error = "member IMG yang dipilih bukan file .txd";
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!img::readEntry(*source, *entry, img::kMaxExtractBytes, bytes, error))
    return false;
  std::vector<txd::Texture> textures;
  if (!txd::parsePortableTextures(bytes, textures, error))
    return false;

  size_t newTextures = 0;
  size_t newPixels = 0;
  if (!textureTotals(textures, newTextures, newPixels)) {
    error = "jumlah texture TXD overflow";
    return false;
  }
  const std::string dictionaryName =
      entry->name.substr(0, entry->name.size() - 4);
  if (dictionaryName.empty()) {
    error = "nama dictionary TXD tidak valid";
    return false;
  }
  const auto existing = txdDictionaries_.find(dictionaryName);
  size_t oldTextures = 0;
  size_t oldPixels = 0;
  if (existing != txdDictionaries_.end() &&
      !textureTotals(existing->second, oldTextures, oldPixels)) {
    error = "catalog texture TXD saat ini tidak konsisten";
    return false;
  }
  if (oldTextures > textures_ || oldPixels > texturePixels_) {
    error = "counter catalog texture TXD tidak konsisten";
    return false;
  }
  const size_t baseTextures = textures_ - oldTextures;
  const size_t basePixels = texturePixels_ - oldPixels;
  if ((existing == txdDictionaries_.end() &&
       txdDictionaries_.size() >= kMaxRuntimeTxdDictionaries) ||
      baseTextures > kMaxRuntimeTextures ||
      basePixels > kMaxRuntimeTexturePixels ||
      newTextures > kMaxRuntimeTextures - baseTextures ||
      newPixels > kMaxRuntimeTexturePixels - basePixels) {
    error = "catalog texture TXD melebihi batas aman";
    return false;
  }

  if (existing == txdDictionaries_.end()) {
    txdDictionaries_.emplace(dictionaryName, std::move(textures));
  } else {
    existing->second = std::move(textures);
  }
  textures_ = baseTextures + newTextures;
  texturePixels_ = basePixels + newPixels;
  unavailableTxdDictionaries_.erase(dictionaryName);
  return true;
}

bool GameDataRuntime::resolveTextureWithParents(
    const std::string &dictionaryName, const std::string &textureName,
    size_t &lazyLoads, const txd::Texture *&out, std::string &error) {
  out = nullptr;
  error.clear();
  if (dictionaryName.empty() || textureName.empty())
    return true;

  std::unordered_set<std::string> visited;
  visited.reserve(kMaxRuntimeTxdParentLookupDepth + 1u);
  std::string current = dictionaryName;
  for (size_t depth = 0; depth <= kMaxRuntimeTxdParentLookupDepth; ++depth) {
    if (!visited.insert(current).second) {
      error = "siklus parent TXD saat resolusi texture";
      return false;
    }

    auto dictionary = txdDictionaries_.find(current);
    if (dictionary == txdDictionaries_.end() &&
        unavailableTxdDictionaries_.find(current) ==
            unavailableTxdDictionaries_.end()) {
      // One texture overlay is strictly optional, but IMG work must remain
      // bounded per UI refresh. Do not cache a cap skip as an unavailable TXD:
      // a later refresh can legitimately try it again.
      if (lazyLoads >= kMaxNearbyWorldTxdLoadsPerBatch)
        return true;
      ++lazyLoads;
      std::string loadError;
      if (!loadImgTxdMember(current + ".txd", loadError)) {
        unavailableTxdDictionaries_.insert(current);
      }
      dictionary = txdDictionaries_.find(current);
    }
    if (dictionary != txdDictionaries_.end()) {
      if (const txd::Texture *texture =
              findTextureByName(dictionary->second, textureName)) {
        out = texture;
        return true;
      }
    }

    const auto parent = txdParentByChild_.find(current);
    if (parent == txdParentByChild_.end())
      return true;
    current = parent->second;
  }
  error = "rantai parent TXD melebihi batas resolusi";
  return false;
}

bool GameDataRuntime::copyLatestDffPreview(size_t maxVertices,
                                           std::vector<float> &out,
                                           std::string &error) const {
  out.clear();
  error.clear();
  if (maxVertices == 0)
    maxVertices = kMaxDffPreviewVertices;
  maxVertices = std::min(maxVertices, kMaxDffPreviewVertices);
  if (maxVertices < 3 || latestDffModel_.empty()) {
    error = "belum ada model DFF portable untuk preview";
    return false;
  }
  const auto model = dffModels_.find(latestDffModel_);
  if (model == dffModels_.end()) {
    error = "catalog model DFF preview tidak konsisten";
    return false;
  }

  size_t expandedVertices = 0;
  for (const dff::Mesh &mesh : model->second) {
    if (mesh.indices.empty() || mesh.indices.size() % 3u != 0 ||
        (!mesh.colors.empty() && mesh.colors.size() != mesh.vertices.size()) ||
        (!mesh.triangleMaterials.empty() &&
         (mesh.triangleMaterials.size() != mesh.indices.size() / 3u ||
          mesh.materials.empty()))) {
      error = "mesh DFF preview tidak konsisten";
      return false;
    }
    if (mesh.indices.size() > maxVertices - expandedVertices) {
      error = "model DFF melebihi batas vertex preview";
      return false;
    }
    expandedVertices += mesh.indices.size();
  }
  if (expandedVertices == 0 ||
      expandedVertices >
          std::numeric_limits<size_t>::max() / kDffPreviewFloatsPerVertex) {
    error = "jumlah vertex DFF preview tidak valid";
    return false;
  }
  out.reserve(expandedVertices * kDffPreviewFloatsPerVertex);

  for (const dff::Mesh &mesh : model->second) {
    for (size_t triangle = 0; triangle < mesh.indices.size() / 3u; ++triangle) {
      // Meshes without a material table remain a valid color-only fallback.
      dff::Rgba materialColor{255u, 255u, 255u, 255u};
      if (!mesh.triangleMaterials.empty()) {
        const size_t materialIndex = mesh.triangleMaterials[triangle];
        if (materialIndex >= mesh.materials.size()) {
          out.clear();
          error = "slot material DFF preview tidak valid";
          return false;
        }
        materialColor = mesh.materials[materialIndex].color;
      }
      for (size_t corner = 0; corner < 3; ++corner) {
        const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
        if (vertexIndex >= mesh.vertices.size()) {
          out.clear();
          error = "index vertex DFF preview tidak valid";
          return false;
        }
        const dff::Vec3 &vertex = mesh.vertices[vertexIndex];
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z)) {
          out.clear();
          error = "vertex DFF preview tidak finite";
          return false;
        }
        float red = static_cast<float>(materialColor.r) / 255.0f;
        float green = static_cast<float>(materialColor.g) / 255.0f;
        float blue = static_cast<float>(materialColor.b) / 255.0f;
        float alpha = static_cast<float>(materialColor.a) / 255.0f;
        if (!mesh.colors.empty()) {
          const dff::Rgba &prelit = mesh.colors[vertexIndex];
          red *= static_cast<float>(prelit.r) / 255.0f;
          green *= static_cast<float>(prelit.g) / 255.0f;
          blue *= static_cast<float>(prelit.b) / 255.0f;
          alpha *= static_cast<float>(prelit.a) / 255.0f;
        }
        // GTA's Z-up coordinates map to this GLES surface's Y-up display.
        out.insert(out.end(),
                   {vertex.x, vertex.z, -vertex.y, red, green, blue, alpha});
      }
    }
  }
  return true;
}

bool GameDataRuntime::copyLatestTexturedDffPreview(size_t maxVertices,
                                                   size_t maxTexturePixels,
                                                   TexturedDffPreview &out,
                                                   std::string &error) const {
  out = {};
  error.clear();
  if (maxVertices == 0)
    maxVertices = kMaxDffPreviewVertices;
  maxVertices = std::min(maxVertices, kMaxDffPreviewVertices);
  if (maxTexturePixels == 0)
    maxTexturePixels = kMaxDffTexturedPreviewPixels;
  maxTexturePixels = std::min(maxTexturePixels, kMaxDffTexturedPreviewPixels);
  if (maxVertices < 3 || maxTexturePixels == 0 || latestDffModel_.empty()) {
    error = "belum ada model DFF portable untuk preview texture";
    return false;
  }
  const auto model = dffModels_.find(latestDffModel_);
  if (model == dffModels_.end()) {
    error = "catalog model DFF preview texture tidak konsisten";
    return false;
  }

  const auto findTextureIn =
      [](const std::vector<txd::Texture> &dictionary,
         const std::string &name) -> const txd::Texture * {
    for (const txd::Texture &texture : dictionary) {
      if (texture.name == name)
        return &texture;
    }
    return nullptr;
  };
  const auto resolveTexture = [&](const std::string &name,
                                  const txd::Texture *&resolved) -> bool {
    resolved = nullptr;
    // GTA commonly pairs foo.dff with foo.txd. Prefer that explicit catalog
    // relation when present; otherwise only accept a globally unique name.
    const auto preferred = txdDictionaries_.find(latestDffModel_);
    if (preferred != txdDictionaries_.end()) {
      resolved = findTextureIn(preferred->second, name);
      if (resolved)
        return true;
    }
    for (const auto &dictionary : txdDictionaries_) {
      if (dictionary.first == latestDffModel_)
        continue;
      const txd::Texture *candidate = findTextureIn(dictionary.second, name);
      if (!candidate)
        continue;
      if (resolved != nullptr) {
        error = "nama texture material DFF ambigu antar dictionary TXD";
        return false;
      }
      resolved = candidate;
    }
    return true;
  };

  for (const dff::Mesh &mesh : model->second) {
    if (mesh.indices.size() % 3u != 0 ||
        mesh.triangleMaterials.size() != mesh.indices.size() / 3u ||
        mesh.texCoords.size() != mesh.vertices.size() ||
        (!mesh.colors.empty() && mesh.colors.size() != mesh.vertices.size())) {
      error = "mesh DFF texture preview tidak konsisten";
      return false;
    }
    for (const uint16_t materialIndex : mesh.triangleMaterials) {
      if (materialIndex >= mesh.materials.size()) {
        error = "slot material DFF texture preview tidak valid";
        return false;
      }
    }
  }

  const txd::Texture *selectedTexture = nullptr;
  std::string selectedTextureName;
  std::unordered_map<std::string, const txd::Texture *> resolutionCache;
  for (const dff::Mesh &mesh : model->second) {
    for (const uint16_t materialIndex : mesh.triangleMaterials) {
      const std::string &textureName =
          mesh.materials[materialIndex].textureName;
      if (textureName.empty())
        continue;
      const auto cached = resolutionCache.find(textureName);
      const txd::Texture *resolved = nullptr;
      if (cached != resolutionCache.end()) {
        resolved = cached->second;
      } else {
        if (resolutionCache.size() >= kMaxDffPreviewTextureNames ||
            !resolveTexture(textureName, resolved)) {
          if (error.empty())
            error = "terlalu banyak nama texture material untuk preview";
          return false;
        }
        resolutionCache.emplace(textureName, resolved);
      }
      if (resolved != nullptr) {
        selectedTexture = resolved;
        selectedTextureName = textureName;
        break;
      }
    }
    if (selectedTexture != nullptr)
      break;
  }
  if (selectedTexture == nullptr) {
    error = "belum ada texture TXD yang cocok dengan material DFF terakhir";
    return false;
  }
  const size_t texturePixels =
      static_cast<size_t>(selectedTexture->width) * selectedTexture->height;
  if (selectedTexture->width == 0 || selectedTexture->height == 0 ||
      texturePixels == 0 || texturePixels > maxTexturePixels ||
      texturePixels > std::numeric_limits<size_t>::max() / 4u ||
      selectedTexture->rgba.size() != texturePixels * 4u) {
    error = "texture TXD untuk preview tidak valid atau melebihi batas";
    return false;
  }

  size_t expandedVertices = 0;
  for (const dff::Mesh &mesh : model->second) {
    for (size_t triangle = 0; triangle < mesh.triangleMaterials.size();
         ++triangle) {
      const dff::Material &material =
          mesh.materials[mesh.triangleMaterials[triangle]];
      if (material.textureName != selectedTextureName)
        continue;
      if (expandedVertices > maxVertices - 3u) {
        error = "batch texture DFF melebihi batas vertex preview";
        return false;
      }
      expandedVertices += 3u;
    }
  }
  if (expandedVertices == 0 ||
      expandedVertices > std::numeric_limits<size_t>::max() /
                             kDffTexturedPreviewFloatsPerVertex) {
    error = "jumlah vertex batch texture DFF tidak valid";
    return false;
  }

  TexturedDffPreview candidate;
  candidate.textureWidth = selectedTexture->width;
  candidate.textureHeight = selectedTexture->height;
  candidate.textureRgba = selectedTexture->rgba;
  candidate.vertices.reserve(expandedVertices *
                             kDffTexturedPreviewFloatsPerVertex);
  for (const dff::Mesh &mesh : model->second) {
    for (size_t triangle = 0; triangle < mesh.triangleMaterials.size();
         ++triangle) {
      const dff::Material &material =
          mesh.materials[mesh.triangleMaterials[triangle]];
      if (material.textureName != selectedTextureName)
        continue;
      for (size_t corner = 0; corner < 3; ++corner) {
        const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
        if (vertexIndex >= mesh.vertices.size()) {
          error = "index vertex batch texture DFF tidak valid";
          return false;
        }
        const dff::Vec3 &vertex = mesh.vertices[vertexIndex];
        const dff::Vec2 &uv = mesh.texCoords[vertexIndex];
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z) || !std::isfinite(uv.u) ||
            !std::isfinite(uv.v)) {
          error = "vertex atau UV batch texture DFF tidak finite";
          return false;
        }
        float red = static_cast<float>(material.color.r) / 255.0f;
        float green = static_cast<float>(material.color.g) / 255.0f;
        float blue = static_cast<float>(material.color.b) / 255.0f;
        float alpha = static_cast<float>(material.color.a) / 255.0f;
        if (!mesh.colors.empty()) {
          const dff::Rgba &prelit = mesh.colors[vertexIndex];
          red *= static_cast<float>(prelit.r) / 255.0f;
          green *= static_cast<float>(prelit.g) / 255.0f;
          blue *= static_cast<float>(prelit.b) / 255.0f;
          alpha *= static_cast<float>(prelit.a) / 255.0f;
        }
        candidate.vertices.insert(candidate.vertices.end(),
                                  {vertex.x, vertex.z, -vertex.y, red, green,
                                   blue, alpha, uv.u, 1.0f - uv.v});
      }
    }
  }
  out = std::move(candidate);
  return true;
}

bool GameDataRuntime::copyTexturePreview(std::string_view dictionaryName,
                                         std::string_view textureName,
                                         size_t maxPixels, TexturePreview& out,
                                         std::string& error) {
  out = {};
  error.clear();
  std::string dictionary;
  std::string texture;
  if (!normalizeTextureLookupName(dictionaryName, dictionary) ||
      !normalizeTextureLookupName(textureName, texture)) {
    error = "nama sprite TXD tidak aman";
    return false;
  }
  if (maxPixels == 0) maxPixels = kMaxTextDrawSpritePixels;
  maxPixels = std::min(maxPixels, kMaxTextDrawSpritePixels);
  if (maxPixels == 0) {
    error = "batas pixel sprite tidak valid";
    return false;
  }

  // A HUD request is intentionally smaller than dynamic-world material work:
  // it may lazy-load at most its direct dictionary and one txdp parent, never
  // scan/select an arbitrary global texture name, and uses the existing
  // cycle-safe resolver.
  constexpr size_t kMaxTextDrawSpriteTxdLoads = 2u;
  static_assert(kMaxNearbyWorldTxdLoadsPerBatch >= kMaxTextDrawSpriteTxdLoads,
                "sprite TXD load cap must fit runtime cap");
  size_t lazyLoads = kMaxNearbyWorldTxdLoadsPerBatch - kMaxTextDrawSpriteTxdLoads;
  const txd::Texture* resolved = nullptr;
  if (!resolveTextureWithParents(dictionary, texture, lazyLoads, resolved, error)) {
    return false;
  }
  if (!resolved) {
    error = "sprite TXD tidak tersedia";
    return false;
  }
  const size_t width = resolved->width;
  const size_t height = resolved->height;
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<size_t>::max() / height) {
    error = "dimensi sprite TXD tidak valid";
    return false;
  }
  const size_t pixels = width * height;
  if (pixels == 0 || pixels > maxPixels ||
      pixels > std::numeric_limits<size_t>::max() / 4u ||
      resolved->rgba.size() != pixels * 4u) {
    error = "sprite TXD melampaui budget HUD";
    return false;
  }
  out.width = resolved->width;
  out.height = resolved->height;
  out.rgba = resolved->rgba;
  return true;
}

bool GameDataRuntime::copyNearbyWorldRenderBatch(
    float anchorX, float anchorY, float anchorZ, float radius, size_t maxInstances,
    size_t maxVertices, NearbyWorldRenderBatch &out, std::string &error, int32_t interior,
    const std::vector<removal::Directive>* removals) {
  out = {};
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang untuk renderer map";
    return false;
  }
  if (!std::isfinite(anchorX) || !std::isfinite(anchorY) ||
      !std::isfinite(anchorZ) || !std::isfinite(radius) || radius <= 0.0f) {
    error = "anchor atau radius renderer map tidak valid";
    return false;
  }
  if (interior < 0 || interior > 255) {
    error = "interior renderer map tidak valid";
    return false;
  }
  radius = std::min(radius, kMaxNearbyWorldRenderRadius);
  if (maxInstances == 0 || maxInstances > kMaxNearbyWorldRenderInstances) {
    maxInstances = kMaxNearbyWorldRenderInstances;
  }
  if (maxVertices == 0 || maxVertices > kMaxNearbyWorldRenderVertices) {
    maxVertices = kMaxNearbyWorldRenderVertices;
  }
  if (maxVertices < 3u) {
    error = "batas vertex renderer map terlalu kecil";
    return false;
  }
  if (maxVertices >
      std::numeric_limits<size_t>::max() / kDffPreviewFloatsPerVertex) {
    error = "batas vertex renderer map overflow";
    return false;
  }

  struct Candidate {
    const map::Instance *placement = nullptr;
    float distanceSquared = 0.0f;
    size_t sourceOrder = 0;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(
      std::min(placements_.size(), kMaxNearbyWorldRenderCandidates));
  const float radiusSquared = radius * radius;
  for (size_t index = 0; index < placements_.size(); ++index) {
    const map::Instance &placement = placements_[index];
    // A server-authoritative interior is part of the snapshot boundary.
    // Never mix a retained IPL placement from another interior into this map
    // batch merely because its GTA-space coordinates happen to be nearby.
    if (placement.interior != interior || placement.modelName.empty() ||
        removal::removesAny(removals, placement.modelId, placement.position.x,
                            placement.position.y, placement.position.z) ||
        !std::isfinite(placement.position.x) ||
        !std::isfinite(placement.position.y) ||
        !std::isfinite(placement.position.z)) {
      continue;
    }
    const float dx = placement.position.x - anchorX;
    const float dy = placement.position.y - anchorY;
    const float dz = placement.position.z - anchorZ;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSquared) || distanceSquared > radiusSquared)
      continue;
    candidates.push_back({&placement, distanceSquared, index});
  }
  const auto nearerFirst = [](const Candidate &left, const Candidate &right) {
    if (left.distanceSquared != right.distanceSquared)
      return left.distanceSquared < right.distanceSquared;
    return left.sourceOrder < right.sourceOrder;
  };
  if (candidates.size() > kMaxNearbyWorldRenderCandidates) {
    std::nth_element(candidates.begin(),
                     candidates.begin() + kMaxNearbyWorldRenderCandidates,
                     candidates.end(), nearerFirst);
    candidates.resize(kMaxNearbyWorldRenderCandidates);
  }
  std::sort(candidates.begin(), candidates.end(), nearerFirst);
  out.consideredPlacements = candidates.size();
  out.vertices.reserve(maxVertices * kDffPreviewFloatsPerVertex);
  std::unordered_set<std::string> renderedModelNames;
  size_t lazyLoads = 0;
  size_t writtenVertices = 0;

  for (const Candidate &candidate : candidates) {
    if (out.renderedPlacements >= maxInstances)
      break;
    const map::Instance &placement = *candidate.placement;
    if (unavailableDffModels_.find(placement.modelName) !=
        unavailableDffModels_.end()) {
      ++out.skippedModels;
      continue;
    }
    auto model = dffModels_.find(placement.modelName);
    if (model == dffModels_.end()) {
      if (lazyLoads >= kMaxNearbyWorldModelLoadsPerBatch) {
        ++out.skippedModels;
        continue;
      }
      std::string loadError;
      if (!loadImgDffMemberInternal(placement.modelName + ".dff", false,
                                    loadError)) {
        unavailableDffModels_.insert(placement.modelName);
        ++out.skippedModels;
        continue;
      }
      ++lazyLoads;
      model = dffModels_.find(placement.modelName);
      if (model == dffModels_.end()) {
        unavailableDffModels_.insert(placement.modelName);
        ++out.skippedModels;
        continue;
      }
    }

    map::Quaternion rotation;
    if (!normalizedPlacementRotation(placement.rotation, rotation)) {
      ++out.skippedModels;
      continue;
    }
    size_t requiredVertices = 0;
    bool validModel = true;
    for (const dff::Mesh &mesh : model->second) {
      if (mesh.indices.empty() || mesh.indices.size() % 3u != 0u ||
          (!mesh.colors.empty() &&
           mesh.colors.size() != mesh.vertices.size()) ||
          (!mesh.triangleMaterials.empty() &&
           (mesh.triangleMaterials.size() != mesh.indices.size() / 3u ||
            mesh.materials.empty())) ||
          mesh.indices.size() > maxVertices - requiredVertices) {
        validModel = false;
        break;
      }
      for (const uint32_t vertexIndex : mesh.indices) {
        if (vertexIndex >= mesh.vertices.size() ||
            !finiteDffVertex(mesh.vertices[vertexIndex])) {
          validModel = false;
          break;
        }
      }
      if (!validModel)
        break;
      requiredVertices += mesh.indices.size();
    }
    if (!validModel || requiredVertices == 0u ||
        requiredVertices > maxVertices - writtenVertices) {
      ++out.skippedModels;
      continue;
    }

    // The prevalidation above guarantees each complete placement fits before
    // writes begin, so the JNI boundary never receives a partial triangle.
    for (const dff::Mesh &mesh : model->second) {
      for (size_t triangle = 0; triangle < mesh.indices.size() / 3u;
           ++triangle) {
        // Missing material tables render as deterministic white/prelit meshes.
        dff::Rgba materialColor{255u, 255u, 255u, 255u};
        if (!mesh.triangleMaterials.empty()) {
          const size_t materialIndex = mesh.triangleMaterials[triangle];
          if (materialIndex >= mesh.materials.size()) {
            validModel = false;
            break;
          }
          materialColor = mesh.materials[materialIndex].color;
        }
        for (size_t corner = 0; corner < 3u; ++corner) {
          const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
          const dff::Vec3 rotated =
              rotateDffVertex(rotation, mesh.vertices[vertexIndex]);
          const float worldX = placement.position.x + rotated.x;
          const float worldY = placement.position.y + rotated.y;
          const float worldZ = placement.position.z + rotated.z;
          if (!std::isfinite(worldX) || !std::isfinite(worldY) ||
              !std::isfinite(worldZ)) {
            validModel = false;
            break;
          }
          float red = static_cast<float>(materialColor.r) / 255.0f;
          float green = static_cast<float>(materialColor.g) / 255.0f;
          float blue = static_cast<float>(materialColor.b) / 255.0f;
          float alpha = static_cast<float>(materialColor.a) / 255.0f;
          if (!mesh.colors.empty()) {
            const dff::Rgba &prelit = mesh.colors[vertexIndex];
            red *= static_cast<float>(prelit.r) / 255.0f;
            green *= static_cast<float>(prelit.g) / 255.0f;
            blue *= static_cast<float>(prelit.b) / 255.0f;
            alpha *= static_cast<float>(prelit.a) / 255.0f;
          }
          out.vertices.insert(out.vertices.end(),
                              {worldX - anchorX, worldZ - anchorZ,
                               -(worldY - anchorY), red, green, blue, alpha});
        }
        if (!validModel)
          break;
      }
      if (!validModel)
        break;
    }
    if (!validModel) {
      // This should be unreachable after prevalidation, but no malformed DFF
      // is allowed to leave a partial placement at the render boundary.
      out.vertices.resize(writtenVertices * kDffPreviewFloatsPerVertex);
      unavailableDffModels_.insert(placement.modelName);
      ++out.skippedModels;
      continue;
    }
    writtenVertices += requiredVertices;
    ++out.renderedPlacements;
    if (renderedModelNames.insert(placement.modelName).second)
      ++out.renderedModels;
  }
  return true;
}

bool GameDataRuntime::copyServerObjectRenderBatch(
    float anchorX, float anchorY, float anchorZ, float radius,
    const std::vector<ServerObjectRenderInput> &objects, size_t maxVertices,
    NearbyWorldRenderBatch &out, std::string &error) {
  out = {};
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang untuk objek server";
    return false;
  }
  if (!std::isfinite(anchorX) || !std::isfinite(anchorY) ||
      !std::isfinite(anchorZ) || !std::isfinite(radius) || radius <= 0.0f) {
    error = "anchor atau radius objek server tidak valid";
    return false;
  }
  if (objects.size() > kMaxServerObjectRenderInputs) {
    error = "jumlah objek server melebihi batas aman";
    return false;
  }
  radius = std::min(radius, kMaxNearbyWorldRenderRadius);
  if (maxVertices == 0 || maxVertices > kMaxNearbyWorldRenderVertices) {
    maxVertices = kMaxNearbyWorldRenderVertices;
  }
  if (maxVertices < 3u || maxVertices > std::numeric_limits<size_t>::max() /
                                            kDffPreviewFloatsPerVertex) {
    error = "batas vertex objek server tidak valid";
    return false;
  }

  struct Candidate {
    const ServerObjectRenderInput *input = nullptr;
    float distanceSquared = 0.0f;
    size_t sourceOrder = 0;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(objects.size());
  for (size_t index = 0; index < objects.size(); ++index) {
    const ServerObjectRenderInput &input = objects[index];
    // Ped skin 0 (CJ) is a valid GTA model ID. Unknown IDs still fail closed
    // at the loaded IDE lookup below, so accepting zero here cannot fabricate
    // geometry from a packet alone.
    if (input.modelId > 65535u || !std::isfinite(input.x) ||
        !std::isfinite(input.y) || !std::isfinite(input.z) ||
        input.x <= -20000.0f || input.x >= 20000.0f || input.y <= -20000.0f ||
        input.y >= 20000.0f || input.z <= -1000.0f || input.z >= 200000.0f ||
        !validServerObjectAngles(input.rotationX, input.rotationY,
                                 input.rotationZ) ||
        !std::isfinite(input.drawDistance) || input.drawDistance < 0.0f ||
        input.drawDistance > 50000.0f ||
        !validServerObjectScale(input.scaleX, input.scaleY, input.scaleZ)) {
      error = "transform objek server tidak valid";
      return false;
    }
    for (const ServerObjectMaterialOverrideInput& materialOverride : input.materialOverrides) {
      if (!validServerObjectMaterialOverrideInput(materialOverride)) {
        error = "material objek server tidak valid";
        return false;
      }
    }
    const float dx = input.x - anchorX;
    const float dy = input.y - anchorY;
    const float dz = input.z - anchorZ;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    const float effectiveDistance = input.drawDistance > 0.0f
                                        ? std::min(radius, input.drawDistance)
                                        : radius;
    if (!std::isfinite(distanceSquared) ||
        distanceSquared > effectiveDistance * effectiveDistance)
      continue;
    candidates.push_back({&input, distanceSquared, index});
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &left, const Candidate &right) {
              if (left.distanceSquared != right.distanceSquared)
                return left.distanceSquared < right.distanceSquared;
              return left.sourceOrder < right.sourceOrder;
            });
  out.consideredPlacements = candidates.size();
  out.vertices.reserve(maxVertices * kDffPreviewFloatsPerVertex);
  std::unordered_set<std::string> renderedModelNames;
  size_t lazyLoads = 0;
  size_t writtenVertices = 0;

  for (const Candidate &candidate : candidates) {
    const ServerObjectRenderInput &input = *candidate.input;
    const auto ide = ideObjects_.find(input.modelId);
    if (ide == ideObjects_.end() || ide->second.modelName.empty()) {
      ++out.skippedModels;
      continue;
    }
    const std::string &modelName = ide->second.modelName;
    if (unavailableDffModels_.find(modelName) != unavailableDffModels_.end()) {
      ++out.skippedModels;
      continue;
    }
    auto model = dffModels_.find(modelName);
    if (model == dffModels_.end()) {
      if (lazyLoads >= kMaxNearbyWorldModelLoadsPerBatch) {
        ++out.skippedModels;
        continue;
      }
      std::string loadError;
      if (!loadImgDffMemberInternal(modelName + ".dff", false, loadError)) {
        unavailableDffModels_.insert(modelName);
        ++out.skippedModels;
        continue;
      }
      ++lazyLoads;
      model = dffModels_.find(modelName);
      if (model == dffModels_.end()) {
        unavailableDffModels_.insert(modelName);
        ++out.skippedModels;
        continue;
      }
    }

    size_t requiredVertices = 0;
    bool validModel = true;
    for (const dff::Mesh &mesh : model->second) {
      if (mesh.indices.empty() || mesh.indices.size() % 3u != 0u ||
          (!mesh.colors.empty() &&
           mesh.colors.size() != mesh.vertices.size()) ||
          (!mesh.triangleMaterials.empty() &&
           (mesh.triangleMaterials.size() != mesh.indices.size() / 3u ||
            mesh.materials.empty()))) {
        validModel = false;
        break;
      }
      for (const uint16_t materialIndex : mesh.triangleMaterials) {
        if (materialIndex >= mesh.materials.size()) {
          validModel = false;
          break;
        }
      }
      if (!validModel)
        break;
      for (const uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size() ||
            !finiteDffVertex(mesh.vertices[index])) {
          validModel = false;
          break;
        }
      }
      if (!validModel)
        break;
      if (mesh.indices.size() > maxVertices - requiredVertices) {
        validModel = false;
        break;
      }
      requiredVertices += mesh.indices.size();
    }
    if (!validModel || requiredVertices == 0u ||
        requiredVertices > maxVertices - writtenVertices) {
      ++out.skippedModels;
      continue;
    }

    const size_t before = out.vertices.size();
    out.vertices.reserve((writtenVertices + requiredVertices) *
                         kDffPreviewFloatsPerVertex);
    size_t flattenedMaterialBase = 0;
    for (const dff::Mesh &mesh : model->second) {
      for (size_t triangle = 0; triangle < mesh.indices.size() / 3u;
           ++triangle) {
        // A color-only mesh can omit a material table. Keep a deterministic
        // white base instead of ever emitting uninitialized vertex modulation;
        // textured overlays independently require a concrete material slot.
        dff::Rgba materialColor{255u, 255u, 255u, 255u};
        bool overrideColour = false;
        if (!mesh.triangleMaterials.empty()) {
          const size_t materialIndex = mesh.triangleMaterials[triangle];
          materialColor = mesh.materials[materialIndex].color;
          const ServerObjectMaterialOverrideInput* materialOverride =
              materialOverrideAt(input, flattenedMaterialBase + materialIndex);
          // SA-MP's zero material colour means preserve the existing DFF
          // modulation; a nonzero ABGR replacement also disables the DFF's
          // vertex-lighting multiplication (the documented material policy).
          overrideColour = materialOverride != nullptr &&
                           (materialOverride->textActive || materialOverride->colourAbgr != 0u);
          if (overrideColour) {
            materialColor = materialOverride->textActive
                ? dff::Rgba{255u, 255u, 255u, 255u}
                : rgbaFromAbgr(materialOverride->colourAbgr);
          }
        }
        float red = static_cast<float>(materialColor.r) / 255.0f;
        float green = static_cast<float>(materialColor.g) / 255.0f;
        float blue = static_cast<float>(materialColor.b) / 255.0f;
        float alpha = static_cast<float>(materialColor.a) / 255.0f;
        for (size_t corner = 0; corner < 3u; ++corner) {
          const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
          const dff::Vec3 rotated = rotateServerObjectEuler(
              input.rotationX, input.rotationY, input.rotationZ,
              scaledServerObjectVertex(input, mesh.vertices[vertexIndex]));
          const float worldX = input.x + rotated.x;
          const float worldY = input.y + rotated.y;
          const float worldZ = input.z + rotated.z;
          if (!std::isfinite(worldX) || !std::isfinite(worldY) ||
              !std::isfinite(worldZ)) {
            validModel = false;
            break;
          }
          float vertexRed = red;
          float vertexGreen = green;
          float vertexBlue = blue;
          float vertexAlpha = alpha;
          if (!overrideColour && !mesh.colors.empty()) {
            const dff::Rgba &prelit = mesh.colors[vertexIndex];
            vertexRed *= static_cast<float>(prelit.r) / 255.0f;
            vertexGreen *= static_cast<float>(prelit.g) / 255.0f;
            vertexBlue *= static_cast<float>(prelit.b) / 255.0f;
            vertexAlpha *= static_cast<float>(prelit.a) / 255.0f;
          }
          out.vertices.insert(out.vertices.end(),
                              {worldX - anchorX, worldZ - anchorZ,
                               -(worldY - anchorY), vertexRed, vertexGreen,
                               vertexBlue, vertexAlpha});
        }
        if (!validModel)
          break;
      }
      if (!validModel)
        break;
      // SA-MP material slots are flattened over the portable DFF geometry
      // lists in deterministic file order; a multi-mesh model never aliases
      // slot zero across every mesh.
      flattenedMaterialBase += mesh.materials.size();
    }
    if (!validModel ||
        out.vertices.size() !=
            before + requiredVertices * kDffPreviewFloatsPerVertex) {
      out.vertices.resize(before);
      unavailableDffModels_.insert(modelName);
      ++out.skippedModels;
      continue;
    }
    writtenVertices += requiredVertices;
    ++out.renderedPlacements;
    if (renderedModelNames.insert(modelName).second)
      ++out.renderedModels;
  }
  return true;
}

bool GameDataRuntime::copyServerObjectTexturedRenderBatches(
    float anchorX, float anchorY, float anchorZ, float radius,
    const std::vector<ServerObjectRenderInput> &objects, size_t maxBatches,
    size_t maxVertices, size_t maxTexturePixels,
    NearbyWorldTexturedRenderBatches &out, std::string &error) {
  out = {};
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang untuk texture objek server";
    return false;
  }
  if (!std::isfinite(anchorX) || !std::isfinite(anchorY) ||
      !std::isfinite(anchorZ) || !std::isfinite(radius) || radius <= 0.0f) {
    error = "anchor atau radius texture objek server tidak valid";
    return false;
  }
  if (objects.size() > kMaxServerObjectRenderInputs) {
    error = "jumlah objek texture server melebihi batas aman";
    return false;
  }
  radius = std::min(radius, kMaxNearbyWorldRenderRadius);
  if (maxBatches == 0 || maxBatches > kMaxNearbyWorldTexturedRenderBatches) {
    maxBatches = kMaxNearbyWorldTexturedRenderBatches;
  }
  if (maxVertices == 0 || maxVertices > kMaxNearbyWorldRenderVertices) {
    maxVertices = kMaxNearbyWorldRenderVertices;
  }
  if (maxTexturePixels == 0 ||
      maxTexturePixels > kMaxDffTexturedPreviewPixels) {
    maxTexturePixels = kMaxDffTexturedPreviewPixels;
  }
  if (maxVertices < 3u || maxTexturePixels == 0u ||
      maxVertices > std::numeric_limits<size_t>::max() /
                        kDffTexturedPreviewFloatsPerVertex) {
    error = "batas batch texture objek server tidak valid";
    return false;
  }

  struct Candidate {
    const ServerObjectRenderInput *input = nullptr;
    float distanceSquared = 0.0f;
    size_t sourceOrder = 0;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(objects.size());
  for (size_t index = 0; index < objects.size(); ++index) {
    const ServerObjectRenderInput &input = objects[index];
    if (input.modelId > 65535u || !std::isfinite(input.x) ||
        !std::isfinite(input.y) || !std::isfinite(input.z) ||
        input.x <= -20000.0f || input.x >= 20000.0f || input.y <= -20000.0f ||
        input.y >= 20000.0f || input.z <= -1000.0f || input.z >= 200000.0f ||
        !validServerObjectAngles(input.rotationX, input.rotationY,
                                 input.rotationZ) ||
        !std::isfinite(input.drawDistance) || input.drawDistance < 0.0f ||
        input.drawDistance > 50000.0f ||
        !validServerObjectScale(input.scaleX, input.scaleY, input.scaleZ)) {
      error = "transform texture objek server tidak valid";
      return false;
    }
    for (const ServerObjectMaterialOverrideInput& materialOverride : input.materialOverrides) {
      if (!validServerObjectMaterialOverrideInput(materialOverride)) {
        error = "material objek server tidak valid";
        return false;
      }
    }
    const float dx = input.x - anchorX;
    const float dy = input.y - anchorY;
    const float dz = input.z - anchorZ;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    const float effectiveDistance = input.drawDistance > 0.0f
                                        ? std::min(radius, input.drawDistance)
                                        : radius;
    if (!std::isfinite(distanceSquared) ||
        distanceSquared > effectiveDistance * effectiveDistance)
      continue;
    candidates.push_back({&input, distanceSquared, index});
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &left, const Candidate &right) {
              if (left.distanceSquared != right.distanceSquared)
                return left.distanceSquared < right.distanceSquared;
              return left.sourceOrder < right.sourceOrder;
            });

  // One GLES slot represents one immutable resolved texture. Several nearby
  // transforms can append their matching triangles into that slot, preventing
  // a crowd sharing one ped/vehicle texture from consuming all four slots.
  std::vector<const txd::Texture *> batchTextures;
  // Text-material overlays share the same GLES budget but are generated RGBA
  // textures rather than pointers into a mounted TXD. A parallel bounded key
  // lets equal signs coalesce without conflating them with a real TXD texture.
  std::vector<std::string> batchTextKeys;
  batchTextures.reserve(maxBatches);
  batchTextKeys.reserve(maxBatches);
  size_t lazyDffLoads = 0;
  size_t lazyTextureLoads = 0;
  size_t writtenVertices = 0;
  size_t usedTexturePixels = 0;
  for (const Candidate &candidate : candidates) {
    const ServerObjectRenderInput &input = *candidate.input;
    const auto ide = ideObjects_.find(input.modelId);
    if (ide == ideObjects_.end() || ide->second.modelName.empty()) {
      continue;
    }
    const std::string &modelName = ide->second.modelName;
    if (unavailableDffModels_.find(modelName) != unavailableDffModels_.end())
      continue;
    auto model = dffModels_.find(modelName);
    if (model == dffModels_.end()) {
      if (lazyDffLoads >= kMaxNearbyWorldModelLoadsPerBatch)
        continue;
      std::string loadError;
      if (!loadImgDffMemberInternal(modelName + ".dff", false, loadError)) {
        unavailableDffModels_.insert(modelName);
        continue;
      }
      ++lazyDffLoads;
      model = dffModels_.find(modelName);
      if (model == dffModels_.end()) {
        unavailableDffModels_.insert(modelName);
        continue;
      }
    }

    struct ResolvedMaterial {
      size_t flattenedMaterialIndex = 0;
      const txd::Texture *texture = nullptr;
      const ServerObjectMaterialOverrideInput *textMaterial = nullptr;
    };
    std::vector<ResolvedMaterial> resolvedMaterials;
    std::vector<size_t> consideredMaterialSlots;
    std::vector<std::string> resolutionKeys;
    std::vector<const txd::Texture *> resolutionValues;
    size_t flattenedMaterialBase = 0;
    for (const dff::Mesh &mesh : model->second) {
      if (mesh.indices.size() % 3u != 0u ||
          mesh.triangleMaterials.size() != mesh.indices.size() / 3u) {
        flattenedMaterialBase += mesh.materials.size();
        continue;
      }
      for (const uint16_t materialIndex : mesh.triangleMaterials) {
        if (materialIndex >= mesh.materials.size()) continue;
        const size_t flattenedMaterialIndex = flattenedMaterialBase + materialIndex;
        if (std::find(consideredMaterialSlots.begin(), consideredMaterialSlots.end(),
                      flattenedMaterialIndex) != consideredMaterialSlots.end()) {
          continue;
        }
        if (consideredMaterialSlots.size() >= kMaxDffPreviewTextureNames) break;
        consideredMaterialSlots.push_back(flattenedMaterialIndex);
        const ServerObjectMaterialOverrideInput* materialOverride =
            materialOverrideAt(input, flattenedMaterialIndex);
        // Text=2 replaces this exact flattened material slot with a generated
        // RGBA sign texture. It has no TXD lookup and intentionally bypasses
        // model -1's Default-only colour sentinel handling below.
        if (materialOverride != nullptr && materialOverride->textActive) {
          resolvedMaterials.push_back({flattenedMaterialIndex, nullptr, materialOverride});
          continue;
        }
        // API model -1 arrives as the u16 all-ones sentinel and changes only
        // base colour. It must not replace the DFF texture or consume a TXD
        // lazy-load slot; the color batch already renders that material slot.
        if (materialOverride != nullptr && materialOverride->sourceModel == 0xFFFFu) {
          continue;
        }
        const std::string& dictionary = materialOverride != nullptr
            ? materialOverride->textureDictionary : ide->second.textureDictionary;
        const std::string& textureName = materialOverride != nullptr
            ? materialOverride->textureName : mesh.materials[materialIndex].textureName;
        if (dictionary.empty() || textureName.empty()) continue;
        const std::string resolutionKey = dictionary + "\x1F" + textureName;
        const auto cached = std::find(resolutionKeys.begin(), resolutionKeys.end(), resolutionKey);
        const txd::Texture* resolved = nullptr;
        if (cached != resolutionKeys.end()) {
          resolved = resolutionValues[static_cast<size_t>(cached - resolutionKeys.begin())];
        } else {
          std::string resolveError;
          if (!resolveTextureWithParents(dictionary, textureName, lazyTextureLoads, resolved,
                                         resolveError)) {
            error = std::move(resolveError);
            return false;
          }
          resolutionKeys.push_back(resolutionKey);
          resolutionValues.push_back(resolved);
        }
        if (resolved != nullptr) {
          resolvedMaterials.push_back({flattenedMaterialIndex, resolved});
        }
      }
      flattenedMaterialBase += mesh.materials.size();
    }
    for (const ResolvedMaterial &resolvedMaterial : resolvedMaterials) {
      const txd::Texture *selectedTexture = resolvedMaterial.texture;
      const ServerObjectMaterialOverrideInput *selectedText = resolvedMaterial.textMaterial;
      const bool generatedText = selectedText != nullptr;
      const size_t selectedMaterialIndex = resolvedMaterial.flattenedMaterialIndex;
      const std::string selectedTextKey = generatedText ? textMaterialCacheKey(*selectedText)
                                                        : std::string();
      size_t existingBatch = out.batches.size();
      for (size_t index = 0; index < batchTextures.size(); ++index) {
        if ((!generatedText && batchTextures[index] == selectedTexture) ||
            (generatedText && batchTextures[index] == nullptr &&
             batchTextKeys[index] == selectedTextKey)) {
          existingBatch = index;
          break;
        }
      }
      const bool createsBatch = existingBatch == out.batches.size();
      if (createsBatch && out.batches.size() >= maxBatches) {
        // Do not stop scanning here: a later material or candidate can still
        // reuse one accepted slot without another texture allocation.
        continue;
      }

      uint16_t textureWidth = 0;
      uint16_t textureHeight = 0;
      std::vector<uint8_t> generatedTexture;
      if (generatedText) {
        if (createsBatch) {
          if (!rasterizeTextMaterial(*selectedText, textureWidth, textureHeight,
                                     generatedTexture)) {
            continue;
          }
        } else {
          textureWidth = out.batches[existingBatch].textureWidth;
          textureHeight = out.batches[existingBatch].textureHeight;
        }
      } else if (selectedTexture != nullptr) {
        textureWidth = selectedTexture->width;
        textureHeight = selectedTexture->height;
      } else {
        continue;
      }
      const size_t texturePixels = static_cast<size_t>(textureWidth) * textureHeight;
      const size_t rgbaBytes = texturePixels <= std::numeric_limits<size_t>::max() / 4u
          ? texturePixels * 4u : 0u;
      const bool rgbaValid = generatedText
          ? (!createsBatch || generatedTexture.size() == rgbaBytes)
          : (selectedTexture != nullptr && selectedTexture->rgba.size() == rgbaBytes);
      if (textureWidth == 0 || textureHeight == 0 || texturePixels == 0 ||
          texturePixels > maxTexturePixels || rgbaBytes == 0u || !rgbaValid ||
          (createsBatch && texturePixels > kMaxNearbyWorldTexturedTotalPixels -
                                               usedTexturePixels)) {
        continue;
      }

      const size_t availableVertices = maxVertices - writtenVertices;
      size_t requiredVertices = 0;
      bool valid = true;
      size_t selectedMaterialBase = 0;
      for (const dff::Mesh &mesh : model->second) {
        if (mesh.indices.size() % 3u != 0u ||
            mesh.triangleMaterials.size() != mesh.indices.size() / 3u) {
          valid = false;
          break;
        }
        for (size_t triangle = 0; triangle < mesh.triangleMaterials.size();
             ++triangle) {
          const size_t materialIndex = mesh.triangleMaterials[triangle];
          if (materialIndex >= mesh.materials.size()) {
            valid = false;
            break;
          }
          if (selectedMaterialBase + materialIndex != selectedMaterialIndex)
            continue;
          if (mesh.texCoords.size() != mesh.vertices.size() ||
              (!mesh.colors.empty() &&
               mesh.colors.size() != mesh.vertices.size()) ||
              availableVertices < 3u ||
              requiredVertices > availableVertices - 3u) {
            valid = false;
            break;
          }
          for (size_t corner = 0; corner < 3u; ++corner) {
            const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
            if (vertexIndex >= mesh.vertices.size() ||
                !finiteDffVertex(mesh.vertices[vertexIndex]) ||
                !std::isfinite(mesh.texCoords[vertexIndex].u) ||
                !std::isfinite(mesh.texCoords[vertexIndex].v)) {
              valid = false;
              break;
            }
          }
          if (!valid)
            break;
          requiredVertices += 3u;
        }
        if (!valid)
          break;
        selectedMaterialBase += mesh.materials.size();
      }
      if (!valid || requiredVertices == 0u)
        continue;

      NearbyWorldTexturedRenderBatch batch;
      batch.textureWidth = textureWidth;
      batch.textureHeight = textureHeight;
      if (generatedText) {
        batch.textureRgba = std::move(generatedTexture);
      } else {
        batch.textureRgba = selectedTexture->rgba;
      }
      batch.vertices.reserve(requiredVertices *
                             kDffTexturedPreviewFloatsPerVertex);
      size_t outputMaterialBase = 0;
      for (const dff::Mesh &mesh : model->second) {
        for (size_t triangle = 0; triangle < mesh.triangleMaterials.size();
             ++triangle) {
          const size_t materialIndex = mesh.triangleMaterials[triangle];
          const dff::Material &material = mesh.materials[materialIndex];
          if (outputMaterialBase + materialIndex != selectedMaterialIndex)
            continue;
          for (size_t corner = 0; corner < 3u; ++corner) {
            const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
            const dff::Vec3 rotated = rotateServerObjectEuler(
                input.rotationX, input.rotationY, input.rotationZ,
                scaledServerObjectVertex(input, mesh.vertices[vertexIndex]));
            const float worldX = input.x + rotated.x;
            const float worldY = input.y + rotated.y;
            const float worldZ = input.z + rotated.z;
            if (!std::isfinite(worldX) || !std::isfinite(worldY) ||
                !std::isfinite(worldZ)) {
              valid = false;
              break;
            }
            dff::Rgba materialColor = material.color;
            const ServerObjectMaterialOverrideInput* materialOverride =
                materialOverrideAt(input, outputMaterialBase + materialIndex);
            const bool overrideColour = materialOverride != nullptr &&
                (materialOverride->textActive || materialOverride->colourAbgr != 0u);
            if (overrideColour) {
              materialColor = materialOverride->textActive
                  ? dff::Rgba{255u, 255u, 255u, 255u}
                  : rgbaFromAbgr(materialOverride->colourAbgr);
            }
            float red = static_cast<float>(materialColor.r) / 255.0f;
            float green = static_cast<float>(materialColor.g) / 255.0f;
            float blue = static_cast<float>(materialColor.b) / 255.0f;
            float alpha = static_cast<float>(materialColor.a) / 255.0f;
            if (!overrideColour && !mesh.colors.empty()) {
              const dff::Rgba &prelit = mesh.colors[vertexIndex];
              red *= static_cast<float>(prelit.r) / 255.0f;
              green *= static_cast<float>(prelit.g) / 255.0f;
              blue *= static_cast<float>(prelit.b) / 255.0f;
              alpha *= static_cast<float>(prelit.a) / 255.0f;
            }
            const dff::Vec2 &uv = mesh.texCoords[vertexIndex];
            batch.vertices.insert(batch.vertices.end(),
                                  {worldX - anchorX, worldZ - anchorZ,
                                   -(worldY - anchorY), red, green, blue, alpha,
                                   uv.u, 1.0f - uv.v});
          }
          if (!valid)
            break;
        }
        if (!valid)
          break;
        outputMaterialBase += mesh.materials.size();
      }
      if (valid && batch.vertices.size() ==
                       requiredVertices * kDffTexturedPreviewFloatsPerVertex) {
        writtenVertices += requiredVertices;
        if (createsBatch) {
          usedTexturePixels += texturePixels;
          out.batches.push_back(std::move(batch));
          // TXD pointers are immutable after commit; generated text uses a
          // parallel key and never aliases user-owned archive memory.
          batchTextures.push_back(selectedTexture);
          batchTextKeys.push_back(generatedText ? selectedTextKey : std::string());
        } else {
          std::vector<float> &destination = out.batches[existingBatch].vertices;
          destination.insert(destination.end(), batch.vertices.begin(),
                             batch.vertices.end());
        }
      }
    }
  }
  error.clear();
  return true;
}

bool GameDataRuntime::copyNearbyWorldTexturedRenderBatches(
    float anchorX, float anchorY, float anchorZ, float radius,
    size_t maxBatches, size_t maxVertices, size_t maxTexturePixels,
    NearbyWorldTexturedRenderBatches &out, std::string &error, int32_t interior,
    const std::vector<removal::Directive>* removals) {
  out = {};
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang untuk texture map";
    return false;
  }
  if (!std::isfinite(anchorX) || !std::isfinite(anchorY) ||
      !std::isfinite(anchorZ) || !std::isfinite(radius) || radius <= 0.0f) {
    error = "anchor atau radius texture map tidak valid";
    return false;
  }
  if (interior < 0 || interior > 255) {
    error = "interior texture map tidak valid";
    return false;
  }
  radius = std::min(radius, kMaxNearbyWorldRenderRadius);
  if (maxBatches == 0 || maxBatches > kMaxNearbyWorldTexturedRenderBatches) {
    maxBatches = kMaxNearbyWorldTexturedRenderBatches;
  }
  if (maxVertices == 0 || maxVertices > kMaxNearbyWorldRenderVertices) {
    maxVertices = kMaxNearbyWorldRenderVertices;
  }
  if (maxTexturePixels == 0 ||
      maxTexturePixels > kMaxDffTexturedPreviewPixels) {
    maxTexturePixels = kMaxDffTexturedPreviewPixels;
  }
  if (maxVertices < 3u || maxTexturePixels == 0u ||
      maxVertices > std::numeric_limits<size_t>::max() /
                        kDffTexturedPreviewFloatsPerVertex) {
    error = "batas batch texture map tidak valid";
    return false;
  }

  struct Candidate {
    const map::Instance *placement = nullptr;
    float distanceSquared = 0.0f;
    size_t sourceOrder = 0;
  };
  std::vector<Candidate> candidates;
  const float radiusSquared = radius * radius;
  for (size_t index = 0; index < placements_.size(); ++index) {
    const map::Instance &placement = placements_[index];
    if (placement.interior != interior || placement.modelName.empty() ||
        removal::removesAny(removals, placement.modelId, placement.position.x,
                            placement.position.y, placement.position.z))
      continue;
    if (dffModels_.find(placement.modelName) == dffModels_.end())
      continue;
    const auto ide = ideObjects_.find(placement.modelId);
    if (ide == ideObjects_.end() || ide->second.textureDictionary.empty())
      continue;
    const float dx = placement.position.x - anchorX;
    const float dy = placement.position.y - anchorY;
    const float dz = placement.position.z - anchorZ;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSquared) || distanceSquared > radiusSquared)
      continue;
    candidates.push_back({&placement, distanceSquared, index});
  }
  const auto nearerFirst = [](const Candidate &left, const Candidate &right) {
    if (left.distanceSquared != right.distanceSquared)
      return left.distanceSquared < right.distanceSquared;
    return left.sourceOrder < right.sourceOrder;
  };
  if (candidates.size() > kMaxNearbyWorldRenderCandidates) {
    std::nth_element(candidates.begin(),
                     candidates.begin() + kMaxNearbyWorldRenderCandidates,
                     candidates.end(), nearerFirst);
    candidates.resize(kMaxNearbyWorldRenderCandidates);
  }
  std::sort(candidates.begin(), candidates.end(), nearerFirst);
  // A texture batch is a material slot. Multiple IPL placements resolved to
  // the same immutable TXD texture append into it instead of duplicating GPU
  // memory and starving nearby distinct materials.
  std::vector<const txd::Texture *> batchTextures;
  batchTextures.reserve(maxBatches);
  size_t lazyTextureLoads = 0;
  size_t writtenVertices = 0;
  size_t usedTexturePixels = 0;

  for (const Candidate &candidate : candidates) {
    const map::Instance &placement = *candidate.placement;
    const auto model = dffModels_.find(placement.modelName);
    const auto ide = ideObjects_.find(placement.modelId);
    if (model == dffModels_.end() || ide == ideObjects_.end())
      continue;
    const std::string &dictionaryName = ide->second.textureDictionary;
    struct ResolvedMaterial {
      std::string textureName;
      const txd::Texture *texture = nullptr;
    };
    std::vector<ResolvedMaterial> resolvedMaterials;
    std::vector<std::string> consideredTextureNames;
    for (const dff::Mesh &mesh : model->second) {
      if (mesh.indices.size() % 3u != 0u ||
          mesh.triangleMaterials.size() != mesh.indices.size() / 3u) {
        continue;
      }
      for (const uint16_t materialIndex : mesh.triangleMaterials) {
        if (materialIndex >= mesh.materials.size())
          continue;
        const std::string &textureName =
            mesh.materials[materialIndex].textureName;
        if (textureName.empty() ||
            std::find(consideredTextureNames.begin(),
                      consideredTextureNames.end(),
                      textureName) != consideredTextureNames.end()) {
          continue;
        }
        if (consideredTextureNames.size() >= kMaxDffPreviewTextureNames)
          break;
        consideredTextureNames.push_back(textureName);
        const txd::Texture *resolved = nullptr;
        std::string resolveError;
        if (!resolveTextureWithParents(dictionaryName, textureName,
                                       lazyTextureLoads, resolved,
                                       resolveError)) {
          error = std::move(resolveError);
          return false;
        }
        if (resolved != nullptr) {
          resolvedMaterials.push_back({textureName, resolved});
        }
      }
    }
    for (const ResolvedMaterial &resolvedMaterial : resolvedMaterials) {
      const txd::Texture *selectedTexture = resolvedMaterial.texture;
      const std::string &selectedTextureName = resolvedMaterial.textureName;
      size_t existingBatch = out.batches.size();
      for (size_t index = 0; index < batchTextures.size(); ++index) {
        if (batchTextures[index] == selectedTexture) {
          existingBatch = index;
          break;
        }
      }
      const bool createsBatch = existingBatch == out.batches.size();
      if (createsBatch && out.batches.size() >= maxBatches) {
        // A later material or placement can still use an accepted slot.
        continue;
      }
      const size_t texturePixels =
          static_cast<size_t>(selectedTexture->width) * selectedTexture->height;
      if (selectedTexture->width == 0 || selectedTexture->height == 0 ||
          texturePixels == 0 || texturePixels > maxTexturePixels ||
          texturePixels > std::numeric_limits<size_t>::max() / 4u ||
          selectedTexture->rgba.size() != texturePixels * 4u ||
          (createsBatch && texturePixels > kMaxNearbyWorldTexturedTotalPixels -
                                               usedTexturePixels)) {
        continue;
      }

      map::Quaternion rotation;
      if (!normalizedPlacementRotation(placement.rotation, rotation))
        continue;
      const size_t availableVertices = maxVertices - writtenVertices;
      size_t requiredVertices = 0;
      bool valid = true;
      for (const dff::Mesh &mesh : model->second) {
        if (mesh.indices.size() % 3u != 0u ||
            mesh.triangleMaterials.size() != mesh.indices.size() / 3u) {
          valid = false;
          break;
        }
        for (size_t triangle = 0; triangle < mesh.triangleMaterials.size();
             ++triangle) {
          const size_t materialIndex = mesh.triangleMaterials[triangle];
          if (materialIndex >= mesh.materials.size()) {
            valid = false;
            break;
          }
          if (mesh.materials[materialIndex].textureName != selectedTextureName)
            continue;
          // A multi-mesh DFF may legitimately mix untextured collision/detail
          // geometry with a textured mesh. Those unrelated meshes must not hide
          // one safe overlay, but every triangle we export still needs complete
          // UV/prelit arrays and finite indexed data.
          if (mesh.texCoords.size() != mesh.vertices.size() ||
              (!mesh.colors.empty() &&
               mesh.colors.size() != mesh.vertices.size()) ||
              availableVertices < 3u ||
              requiredVertices > availableVertices - 3u) {
            valid = false;
            break;
          }
          for (size_t corner = 0; corner < 3u; ++corner) {
            const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
            if (vertexIndex >= mesh.vertices.size() ||
                !finiteDffVertex(mesh.vertices[vertexIndex]) ||
                !std::isfinite(mesh.texCoords[vertexIndex].u) ||
                !std::isfinite(mesh.texCoords[vertexIndex].v)) {
              valid = false;
              break;
            }
          }
          if (!valid)
            break;
          requiredVertices += 3u;
        }
        if (!valid)
          break;
      }
      if (!valid || requiredVertices == 0u)
        continue;

      NearbyWorldTexturedRenderBatch batch;
      batch.textureWidth = selectedTexture->width;
      batch.textureHeight = selectedTexture->height;
      batch.textureRgba = selectedTexture->rgba;
      batch.vertices.reserve(requiredVertices *
                             kDffTexturedPreviewFloatsPerVertex);
      for (const dff::Mesh &mesh : model->second) {
        for (size_t triangle = 0; triangle < mesh.triangleMaterials.size();
             ++triangle) {
          const dff::Material &material =
              mesh.materials[mesh.triangleMaterials[triangle]];
          if (material.textureName != selectedTextureName)
            continue;
          for (size_t corner = 0; corner < 3u; ++corner) {
            const size_t vertexIndex = mesh.indices[triangle * 3u + corner];
            const dff::Vec3 rotated =
                rotateDffVertex(rotation, mesh.vertices[vertexIndex]);
            const float worldX = placement.position.x + rotated.x;
            const float worldY = placement.position.y + rotated.y;
            const float worldZ = placement.position.z + rotated.z;
            if (!std::isfinite(worldX) || !std::isfinite(worldY) ||
                !std::isfinite(worldZ)) {
              valid = false;
              break;
            }
            float red = static_cast<float>(material.color.r) / 255.0f;
            float green = static_cast<float>(material.color.g) / 255.0f;
            float blue = static_cast<float>(material.color.b) / 255.0f;
            float alpha = static_cast<float>(material.color.a) / 255.0f;
            if (!mesh.colors.empty()) {
              const dff::Rgba &prelit = mesh.colors[vertexIndex];
              red *= static_cast<float>(prelit.r) / 255.0f;
              green *= static_cast<float>(prelit.g) / 255.0f;
              blue *= static_cast<float>(prelit.b) / 255.0f;
              alpha *= static_cast<float>(prelit.a) / 255.0f;
            }
            const dff::Vec2 &uv = mesh.texCoords[vertexIndex];
            batch.vertices.insert(batch.vertices.end(),
                                  {worldX - anchorX, worldZ - anchorZ,
                                   -(worldY - anchorY), red, green, blue, alpha,
                                   uv.u, 1.0f - uv.v});
          }
          if (!valid)
            break;
        }
        if (!valid)
          break;
      }
      if (valid && batch.vertices.size() ==
                       requiredVertices * kDffTexturedPreviewFloatsPerVertex) {
        writtenVertices += requiredVertices;
        if (createsBatch) {
          usedTexturePixels += texturePixels;
          out.batches.push_back(std::move(batch));
          batchTextures.push_back(selectedTexture);
        } else {
          std::vector<float> &destination = out.batches[existingBatch].vertices;
          destination.insert(destination.end(), batch.vertices.begin(),
                             batch.vertices.end());
        }
      }
    }
  }
  // Texture overlays are optional: the caller still draws its color/prelit
  // placement batch if no safe DFF/TXD relation is presently available.
  error.clear();
  return true;
}

bool GameDataRuntime::copyNearbyWorldTexturedRenderBatch(
    float anchorX, float anchorY, float anchorZ, float radius,
    size_t maxVertices, size_t maxTexturePixels,
    NearbyWorldTexturedRenderBatch &out, std::string &error, int32_t interior,
    const std::vector<removal::Directive>* removals) {
  out = {};
  NearbyWorldTexturedRenderBatches batches;
  if (!copyNearbyWorldTexturedRenderBatches(anchorX, anchorY, anchorZ, radius,
                                            1u, maxVertices, maxTexturePixels,
                                            batches, error, interior, removals)) {
    return false;
  }
  if (!batches.batches.empty())
    out = std::move(batches.batches.front());
  return true;
}

bool GameDataRuntime::loadGtaDatFileDescriptor(int fileDescriptor,
                                               std::string &error) {
  std::string text;
  if (!readDescriptorText(fileDescriptor, text, error))
    return false;
  map::AssetManifest candidate;
  if (!map::parseGtaDat(text, candidate, error))
    return false;
  // A manifest identifies the selected GTA source tree. A newly accepted one
  // must not retain handling coefficients from an earlier root even if callers
  // have not yet replaced the IMG; the Android mount flow reloads handling.cfg
  // explicitly after this manifest commits.
  assetManifest_ = std::move(candidate);
  handlingProfiles_.clear();
  return true;
}

bool GameDataRuntime::loadExternalCollisionFileDescriptor(int fileDescriptor,
                                                          std::string &error) {
  return loadExternalCollisionFileDescriptors(std::vector<int>{fileDescriptor},
                                              error);
}

bool GameDataRuntime::loadExternalCollisionFileDescriptors(
    const std::vector<int> &fileDescriptors, std::string &error) {
  error.clear();
  if (fileDescriptors.empty() ||
      fileDescriptors.size() > kMaxRuntimeBatchDescriptors) {
    error = "jumlah descriptor COL batch tidak valid";
    return false;
  }
  auto candidateCatalog = std::make_unique<world::CollisionCatalog>(
      collisionCatalog_ ? *collisionCatalog_ : world::CollisionCatalog{});
  for (const int fileDescriptor : fileDescriptors) {
    if (fileDescriptor < 0) {
      error = "descriptor COL tidak valid";
      return false;
    }
    std::vector<uint8_t> bytes;
    if (!readDescriptorBytes(fileDescriptor, img::kMaxExtractBytes, bytes,
                             error) ||
        !world::loadCollisionArchiveBytes(bytes, *candidateCatalog, error)) {
      return false;
    }
  }
  world::CollisionWorld candidateWorld;
  world::CollisionWorldBuildReport candidateReport;
  if (!rebuildWorld(*candidateCatalog, placements_, candidateWorld,
                    candidateReport, error))
    return false;
  collisionCatalog_ = std::move(candidateCatalog);
  collisionWorld_ = std::move(candidateWorld);
  worldReport_ = candidateReport;
  return true;
}

bool GameDataRuntime::loadIdeFileDescriptor(int fileDescriptor,
                                            std::string &error) {
  return loadIdeFileDescriptors(std::vector<int>{fileDescriptor}, error);
}

bool GameDataRuntime::loadIdeFileDescriptors(
    const std::vector<int> &fileDescriptors, std::string &error) {
  error.clear();
  if (fileDescriptors.empty() ||
      fileDescriptors.size() > kMaxRuntimeBatchDescriptors) {
    error = "jumlah descriptor IDE batch tidak valid";
    return false;
  }
  auto candidateIde = ideObjects_;
  auto candidateTxdParents = txdParentByChild_;
  for (const int fileDescriptor : fileDescriptors) {
    if (fileDescriptor < 0) {
      error = "descriptor IDE tidak valid";
      return false;
    }
    std::string text;
    map::IdeParseResult parsed;
    if (!readDescriptorText(fileDescriptor, text, error) ||
        !map::parseIdeFile(text, parsed, error)) {
      return false;
    }
    for (const map::IdeObject &object : parsed.objects) {
      const auto existing = candidateIde.find(object.modelId);
      if (existing != candidateIde.end() &&
          !sameIdeDefinition(existing->second, object)) {
        error = "model ID IDE bertentangan antar file";
        return false;
      }
      if (existing == candidateIde.end() &&
          candidateIde.size() >= map::kMaxIdeObjects) {
        error = "jumlah object IDE gabungan melebihi batas aman";
        return false;
      }
      candidateIde.emplace(object.modelId, object);
    }
    for (const map::TxdParentRelation &relation : parsed.txdParents) {
      const auto existing = candidateTxdParents.find(relation.child);
      if (existing != candidateTxdParents.end()) {
        if (existing->second != relation.parent) {
          error = "parent dictionary TXD bertentangan antar file IDE";
          return false;
        }
        continue;
      }
      if (candidateTxdParents.size() >= kMaxRuntimeTxdParentRelations) {
        error = "jumlah parent dictionary TXD gabungan melebihi batas aman";
        return false;
      }
      candidateTxdParents.emplace(relation.child, relation.parent);
    }
  }
  if (!validateTxdParentGraph(candidateTxdParents, error))
    return false;
  // A text IPL's name is advisory in SA. If definitions arrive after a text
  // placement, rebind its lookup name by ID and rebuild collision atomically;
  // this also makes decompiled `dummy` lines safe without accepting ambiguity.
  std::vector<map::Instance> candidatePlacements = placements_;
  for (map::Instance &placement : candidatePlacements) {
    const auto object = candidateIde.find(placement.modelId);
    if (object != candidateIde.end())
      placement.modelName = object->second.modelName;
  }
  const world::CollisionCatalog emptyCatalog;
  const world::CollisionCatalog &catalog =
      collisionCatalog_ ? *collisionCatalog_ : emptyCatalog;
  world::CollisionWorld candidateWorld;
  world::CollisionWorldBuildReport candidateReport;
  if (!rebuildWorld(catalog, candidatePlacements, candidateWorld,
                    candidateReport, error))
    return false;
  ideObjects_ = std::move(candidateIde);
  txdParentByChild_ = std::move(candidateTxdParents);
  placements_ = std::move(candidatePlacements);
  collisionWorld_ = std::move(candidateWorld);
  worldReport_ = candidateReport;
  return true;
}

bool GameDataRuntime::loadHandlingFileDescriptor(int fileDescriptor,
                                                 std::string &error) {
  error.clear();
  if (fileDescriptor < 0) {
    error = "descriptor handling.cfg tidak valid";
    return false;
  }
  std::string text;
  if (!readDescriptorText(fileDescriptor, text, error)) return false;
  std::unordered_map<std::string, handling::Profile> candidate;
  if (!handling::parseHandlingCfg(text, candidate, error)) return false;
  handlingProfiles_ = std::move(candidate);
  return true;
}

bool GameDataRuntime::vehicleMotionTuning(
    uint32_t modelId, vehiclemotion::VehicleMotionTuning &out) const {
  const auto ide = ideObjects_.find(modelId);
  if (ide == ideObjects_.end() || ide->second.handlingName.empty()) return false;
  const auto profile = handlingProfiles_.find(ide->second.handlingName);
  if (profile == handlingProfiles_.end()) return false;
  return handling::makeMotionTuning(profile->second, out);
}

bool GameDataRuntime::loadIplFileDescriptor(int fileDescriptor,
                                            std::string &error) {
  return loadIplFileDescriptors(std::vector<int>{fileDescriptor}, error);
}

bool GameDataRuntime::loadIplFileDescriptors(
    const std::vector<int> &fileDescriptors, std::string &error) {
  error.clear();
  if (fileDescriptors.empty() ||
      fileDescriptors.size() > kMaxRuntimeBatchDescriptors) {
    error = "jumlah descriptor IPL batch tidak valid";
    return false;
  }
  std::vector<map::Instance> candidatePlacements = placements_;
  for (const int fileDescriptor : fileDescriptors) {
    if (fileDescriptor < 0) {
      error = "descriptor IPL tidak valid";
      return false;
    }
    // Text IPL and the SA streaming `bnry` variant share this descriptor API.
    // Read one bounded buffer, then choose only an explicit magic signature;
    // arbitrary binary bytes never fall through to the text parser.
    std::vector<uint8_t> bytes;
    if (!readDescriptorBytes(fileDescriptor, kMaxMapTextBytes, bytes, error))
      return false;
    std::vector<map::Instance> parsed;
    const bool binaryIpl = hasBinaryIplMagic(bytes);
    if (binaryIpl) {
      if (!map::parseBinaryIplInstances(bytes, parsed, error))
        return false;
    } else {
      if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) {
        error =
            "IPL text mengandung byte NUL; bukan binary IPL bnry yang didukung";
        return false;
      }
      size_t offset = 0;
      if (bytes.size() >= 3u && bytes[0] == 0xEFu && bytes[1] == 0xBBu &&
          bytes[2] == 0xBFu) {
        offset = 3u;
      }
      const std::string text(
          reinterpret_cast<const char *>(bytes.data() + offset),
          bytes.size() - offset);
      if (!map::parseIplInstances(text, parsed, error))
        return false;
    }
    if (candidatePlacements.size() > world::kMaxCollisionWorldInstances ||
        parsed.size() >
            world::kMaxCollisionWorldInstances - candidatePlacements.size()) {
      error = "jumlah placement IPL gabungan melebihi batas aman";
      return false;
    }
    for (map::Instance &placement : parsed) {
      const auto object = ideObjects_.find(placement.modelId);
      if (placement.modelName.empty()) {
        // Binary INST records carry an ID only. An IDE definition is therefore
        // mandatory before collision/model lookup can safely name the asset.
        if (object == ideObjects_.end()) {
          error = "binary IPL membutuhkan definisi model IDE yang sudah dimuat";
          return false;
        }
        placement.modelName = object->second.modelName;
      } else if (object != ideObjects_.end()) {
        // SA resolves placement assets by model ID; the text model-name field
        // is historically ignored and can legitimately be `dummy`. Prefer the
        // loaded IDE mapping instead of rejecting a valid stock/decompiled IPL.
        placement.modelName = object->second.modelName;
      }
    }
    candidatePlacements.insert(candidatePlacements.end(),
                               std::make_move_iterator(parsed.begin()),
                               std::make_move_iterator(parsed.end()));
  }
  const world::CollisionCatalog emptyCatalog;
  const world::CollisionCatalog &catalog =
      collisionCatalog_ ? *collisionCatalog_ : emptyCatalog;
  world::CollisionWorld candidateWorld;
  world::CollisionWorldBuildReport candidateReport;
  if (!rebuildWorld(catalog, candidatePlacements, candidateWorld,
                    candidateReport, error)) {
    return false;
  }
  placements_ = std::move(candidatePlacements);
  collisionWorld_ = std::move(candidateWorld);
  worldReport_ = candidateReport;
  return true;
}

bool GameDataRuntime::loadImgStreamingIpls(std::string &error) {
  error.clear();
  if (!imageSource_ || imageIndex_.empty()) {
    error = "IMG GTA belum dipasang untuk IPL streaming";
    return false;
  }
  std::unordered_set<std::string> parentBases;
  for (const std::string &path : assetManifest_.iplPaths) {
    const std::string base = manifestIplBaseName(path);
    if (!base.empty())
      parentBases.insert(base);
  }
  if (parentBases.empty())
    return true;

  std::vector<const img::Entry *> streamingEntries;
  for (const img::Entry &entry : imageIndex_.entries()) {
    if (!isStreamingIplMemberFor(entry.name, parentBases))
      continue;
    if (streamingEntries.size() >= kMaxRuntimeImgStreamingIpls) {
      error = "jumlah IPL streaming IMG melebihi batas aman";
      return false;
    }
    streamingEntries.push_back(&entry);
  }
  if (streamingEntries.empty())
    return true;
  std::sort(streamingEntries.begin(), streamingEntries.end(),
            [](const img::Entry *left, const img::Entry *right) {
              return left->name < right->name;
            });

  std::vector<map::Instance> candidatePlacements = placements_;
  for (const img::Entry *entry : streamingEntries) {
    std::vector<uint8_t> bytes;
    if (!entry ||
        !img::readEntry(*imageSource_, *entry, kMaxMapTextBytes, bytes, error))
      return false;
    if (!hasBinaryIplMagic(bytes)) {
      error = "member IPL streaming IMG bukan binary bnry yang didukung";
      return false;
    }
    std::vector<map::Instance> parsed;
    if (!map::parseBinaryIplInstances(bytes, parsed, error))
      return false;
    if (candidatePlacements.size() > world::kMaxCollisionWorldInstances ||
        parsed.size() >
            world::kMaxCollisionWorldInstances - candidatePlacements.size()) {
      error = "jumlah placement IPL streaming gabungan melebihi batas aman";
      return false;
    }
    for (map::Instance &placement : parsed) {
      const auto object = ideObjects_.find(placement.modelId);
      if (object == ideObjects_.end()) {
        error = "IPL streaming binary membutuhkan definisi model IDE yang "
                "sudah dimuat";
        return false;
      }
      placement.modelName = object->second.modelName;
    }
    candidatePlacements.insert(candidatePlacements.end(),
                               std::make_move_iterator(parsed.begin()),
                               std::make_move_iterator(parsed.end()));
  }

  const world::CollisionCatalog emptyCatalog;
  const world::CollisionCatalog &catalog =
      collisionCatalog_ ? *collisionCatalog_ : emptyCatalog;
  world::CollisionWorld candidateWorld;
  world::CollisionWorldBuildReport candidateReport;
  if (!rebuildWorld(catalog, candidatePlacements, candidateWorld,
                    candidateReport, error)) {
    return false;
  }
  placements_ = std::move(candidatePlacements);
  collisionWorld_ = std::move(candidateWorld);
  worldReport_ = candidateReport;
  return true;
}

} // namespace samp::gta::runtime
