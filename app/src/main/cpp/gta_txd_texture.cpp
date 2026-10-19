// Bounded Direct3D PC TXD texture decoder for user-owned GTA SA data.
#include "gta_txd_texture.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

namespace samp::gta::txd {
namespace {

constexpr uint32_t kRwStruct = 0x01;
constexpr uint32_t kRwTextureNative = 0x15;
constexpr uint32_t kRwTextureDictionary = 0x16;
constexpr size_t kChunkHeaderBytes = 12;
constexpr size_t kD3dRasterHeaderBytes = 88;
constexpr uint32_t kPlatformD3d8 = 8;
constexpr uint32_t kPlatformD3d9 = 9;
constexpr uint32_t kFormat565 = 0x0200;
constexpr uint32_t kFormat4444 = 0x0300;
constexpr uint32_t kFormat8888 = 0x0500;
constexpr uint32_t kFormat888 = 0x0600;
constexpr uint32_t kFormatPaletteMask = 0x6000;
constexpr uint32_t kDxt1 = 0x31545844;  // MAKEFOURCC('D','X','T','1')
constexpr uint32_t kDxt3 = 0x33545844;
constexpr uint32_t kDxt5 = 0x35545844;

enum class Encoding : uint8_t { Raw8888, Raw888, Raw888x, Raw565, Raw4444, Dxt1, Dxt3, Dxt5 };

struct Chunk {
  uint32_t type = 0;
  size_t payloadOffset = 0;
  size_t payloadEnd = 0;
};

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 |
         static_cast<uint32_t>(bytes[3]) << 24;
}

bool rangeInside(size_t offset, size_t count, size_t total) {
  return offset <= total && count <= total - offset;
}

bool multiplySize(size_t left, size_t right, size_t& out) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left) return false;
  out = left * right;
  return true;
}

bool readChunk(const std::vector<uint8_t>& bytes, size_t& cursor, size_t end, Chunk& out) {
  if (!rangeInside(cursor, kChunkHeaderBytes, end)) return false;
  const uint8_t* header = bytes.data() + cursor;
  const size_t payloadOffset = cursor + kChunkHeaderBytes;
  const size_t payloadSize = readLe32(header + 4);
  if (!rangeInside(payloadOffset, payloadSize, end)) return false;
  out.type = readLe32(header);
  out.payloadOffset = payloadOffset;
  out.payloadEnd = payloadOffset + payloadSize;
  cursor = out.payloadEnd;
  return true;
}

// IMG v2 entries occupy whole sectors. Valid TXD streams may be followed by
// zero-fill; accept that suffix only at the outer stream boundary.
bool zeroFilledSuffix(const std::vector<uint8_t>& bytes, size_t offset) {
  return offset <= bytes.size() &&
         std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end(),
                     [](uint8_t value) { return value == 0; });
}

bool decodeName(const uint8_t* bytes, std::string& out) {
  constexpr size_t kNameBytes = 32;
  size_t length = 0;
  while (length < kNameBytes && bytes[length] != 0) ++length;
  if (length == 0 || length == kNameBytes) return false;
  out.clear();
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const unsigned char byte = bytes[i];
    // RenderWare dictionary keys may contain a literal stock space (for
    // example "new road"). They remain names only: separators and controls
    // are refused before deterministic lowercasing.
    if (byte < 0x20u || byte > 0x7Eu || byte == '/' || byte == '\\' || byte == ':') {
      out.clear();
      return false;
    }
    char character = static_cast<char>(byte);
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    out.push_back(character);
  }
  return true;
}

uint8_t expand5(uint8_t value) { return static_cast<uint8_t>((value * 255u + 15u) / 31u); }
uint8_t expand6(uint8_t value) { return static_cast<uint8_t>((value * 255u + 31u) / 63u); }
uint8_t expand4(uint8_t value) { return static_cast<uint8_t>(value * 17u); }

void decode565(uint16_t value, uint8_t& red, uint8_t& green, uint8_t& blue) {
  red = expand5(static_cast<uint8_t>((value >> 11) & 0x1Fu));
  green = expand6(static_cast<uint8_t>((value >> 5) & 0x3Fu));
  blue = expand5(static_cast<uint8_t>(value & 0x1Fu));
}

void writePixel(Texture& texture, size_t x, size_t y, const uint8_t rgba[4]) {
  if (x >= texture.width || y >= texture.height) return;
  const size_t destination = (y * texture.width + x) * 4u;
  texture.rgba[destination] = rgba[0];
  texture.rgba[destination + 1] = rgba[1];
  texture.rgba[destination + 2] = rgba[2];
  texture.rgba[destination + 3] = rgba[3];
}

void decodeColorBlock(const uint8_t* source, bool dxt1Alpha, uint8_t palette[4][4]) {
  const uint16_t first = readLe16(source);
  const uint16_t second = readLe16(source + 2);
  decode565(first, palette[0][0], palette[0][1], palette[0][2]);
  decode565(second, palette[1][0], palette[1][1], palette[1][2]);
  palette[0][3] = 255;
  palette[1][3] = 255;
  if (!dxt1Alpha || first > second) {
    for (int component = 0; component < 3; ++component) {
      palette[2][component] = static_cast<uint8_t>((2u * palette[0][component] + palette[1][component]) / 3u);
      palette[3][component] = static_cast<uint8_t>((palette[0][component] + 2u * palette[1][component]) / 3u);
    }
    palette[2][3] = 255;
    palette[3][3] = 255;
  } else {
    for (int component = 0; component < 3; ++component) {
      palette[2][component] = static_cast<uint8_t>((palette[0][component] + palette[1][component]) / 2u);
      palette[3][component] = 0;
    }
    palette[2][3] = 255;
    palette[3][3] = 0;
  }
}

bool decodeDxt(const uint8_t* source, size_t sourceSize, Encoding encoding, Texture& out) {
  const size_t blocksWide = (static_cast<size_t>(out.width) + 3u) / 4u;
  const size_t blocksHigh = (static_cast<size_t>(out.height) + 3u) / 4u;
  const size_t blockBytes = encoding == Encoding::Dxt1 ? 8u : 16u;
  size_t blockCount = 0;
  size_t expected = 0;
  if (!multiplySize(blocksWide, blocksHigh, blockCount) || !multiplySize(blockCount, blockBytes, expected) ||
      sourceSize < expected) {
    return false;
  }
  for (size_t blockY = 0; blockY < blocksHigh; ++blockY) {
    for (size_t blockX = 0; blockX < blocksWide; ++blockX) {
      const uint8_t* block = source + (blockY * blocksWide + blockX) * blockBytes;
      const uint8_t* colorBlock = block;
      uint8_t alpha[16];
      std::fill(std::begin(alpha), std::end(alpha), 255);
      if (encoding == Encoding::Dxt3) {
        for (size_t pixel = 0; pixel < 16; ++pixel) {
          alpha[pixel] = expand4(static_cast<uint8_t>((block[pixel / 2u] >> ((pixel % 2u) * 4u)) & 0x0Fu));
        }
        colorBlock = block + 8;
      } else if (encoding == Encoding::Dxt5) {
        const uint8_t first = block[0];
        const uint8_t second = block[1];
        uint8_t alphaPalette[8] = {first, second, 0, 0, 0, 0, 0, 0};
        if (first > second) {
          for (size_t i = 1; i <= 6; ++i) {
            alphaPalette[i + 1] = static_cast<uint8_t>(((7u - i) * first + i * second) / 7u);
          }
        } else {
          for (size_t i = 1; i <= 4; ++i) {
            alphaPalette[i + 1] = static_cast<uint8_t>(((5u - i) * first + i * second) / 5u);
          }
          alphaPalette[6] = 0;
          alphaPalette[7] = 255;
        }
        uint64_t selectors = 0;
        for (size_t i = 0; i < 6; ++i) selectors |= static_cast<uint64_t>(block[2 + i]) << (8u * i);
        for (size_t pixel = 0; pixel < 16; ++pixel) {
          alpha[pixel] = alphaPalette[(selectors >> (3u * pixel)) & 0x7u];
        }
        colorBlock = block + 8;
      }

      uint8_t palette[4][4] = {};
      decodeColorBlock(colorBlock, encoding == Encoding::Dxt1, palette);
      const uint32_t selectors = readLe32(colorBlock + 4);
      for (size_t pixelY = 0; pixelY < 4; ++pixelY) {
        for (size_t pixelX = 0; pixelX < 4; ++pixelX) {
          const size_t pixel = pixelY * 4u + pixelX;
          const uint8_t index = static_cast<uint8_t>((selectors >> (2u * pixel)) & 0x3u);
          uint8_t rgba[4] = {palette[index][0], palette[index][1], palette[index][2], palette[index][3]};
          if (encoding == Encoding::Dxt3 || encoding == Encoding::Dxt5) rgba[3] = alpha[pixel];
          writePixel(out, blockX * 4u + pixelX, blockY * 4u + pixelY, rgba);
        }
      }
    }
  }
  return true;
}

bool decodeRaw(const uint8_t* source, size_t sourceSize, Encoding encoding, Texture& out) {
  size_t bytesPerPixel = 0;
  if (encoding == Encoding::Raw8888 || encoding == Encoding::Raw888x) bytesPerPixel = 4;
  if (encoding == Encoding::Raw888) bytesPerPixel = 3;
  if (encoding == Encoding::Raw565 || encoding == Encoding::Raw4444) bytesPerPixel = 2;
  if (bytesPerPixel == 0) return false;
  const size_t unalignedRow = static_cast<size_t>(out.width) * bytesPerPixel;
  const size_t rowStride = (unalignedRow + 3u) & ~static_cast<size_t>(3u);
  size_t expected = 0;
  if (!multiplySize(rowStride, out.height, expected) || sourceSize < expected) return false;
  for (size_t y = 0; y < out.height; ++y) {
    const uint8_t* row = source + y * rowStride;
    for (size_t x = 0; x < out.width; ++x) {
      const uint8_t* pixel = row + x * bytesPerPixel;
      uint8_t rgba[4] = {0, 0, 0, 255};
      if (encoding == Encoding::Raw8888) {
        rgba[0] = pixel[2];
        rgba[1] = pixel[1];
        rgba[2] = pixel[0];
        rgba[3] = pixel[3];
      } else if (encoding == Encoding::Raw888 || encoding == Encoding::Raw888x) {
        rgba[0] = pixel[2];
        rgba[1] = pixel[1];
        rgba[2] = pixel[0];
      } else if (encoding == Encoding::Raw565) {
        decode565(readLe16(pixel), rgba[0], rgba[1], rgba[2]);
      } else {
        const uint16_t value = readLe16(pixel);
        rgba[0] = expand4(static_cast<uint8_t>((value >> 8) & 0x0Fu));
        rgba[1] = expand4(static_cast<uint8_t>((value >> 4) & 0x0Fu));
        rgba[2] = expand4(static_cast<uint8_t>(value & 0x0Fu));
        rgba[3] = expand4(static_cast<uint8_t>((value >> 12) & 0x0Fu));
      }
      writePixel(out, x, y, rgba);
    }
  }
  return true;
}

bool parseRasterStruct(const std::vector<uint8_t>& bytes, size_t begin, size_t end, Texture& out) {
  if (!rangeInside(begin, kD3dRasterHeaderBytes, end)) return false;
  const uint8_t* header = bytes.data() + begin;
  const uint32_t platform = readLe32(header);
  if (platform != kPlatformD3d8 && platform != kPlatformD3d9) return false;
  Texture candidate;
  if (!decodeName(header + 8, candidate.name)) return false;
  const uint32_t rasterFormat = readLe32(header + 72);
  const uint32_t d3dFormat = readLe32(header + 76);
  candidate.width = readLe16(header + 80);
  candidate.height = readLe16(header + 82);
  const uint8_t depth = header[84];
  const uint8_t declaredLevels = header[85];
  const uint8_t flags = header[87];
  if (candidate.width == 0 || candidate.height == 0 || candidate.width > kMaxTextureDimension ||
      candidate.height > kMaxTextureDimension || (flags & 0x02u) != 0 ||
      (rasterFormat & kFormatPaletteMask) != 0) {
    return false;
  }
  size_t pixels = 0;
  if (!multiplySize(candidate.width, candidate.height, pixels) || pixels > kMaxTexturePixels ||
      !multiplySize(pixels, 4, pixels)) {
    return false;
  }
  candidate.rgba.assign(pixels, 0);

  Encoding encoding;
  if (d3dFormat == kDxt1) {
    encoding = Encoding::Dxt1;
  } else if (d3dFormat == kDxt3) {
    encoding = Encoding::Dxt3;
  } else if (d3dFormat == kDxt5) {
    encoding = Encoding::Dxt5;
  } else {
    const uint32_t baseFormat = rasterFormat & 0x0F00u;
    if (baseFormat == kFormat8888 && depth == 32) {
      encoding = Encoding::Raw8888;
    } else if (baseFormat == kFormat888 && depth == 24) {
      encoding = Encoding::Raw888;
    } else if (baseFormat == kFormat888 && depth == 32) {
      encoding = Encoding::Raw888x;
    } else if (baseFormat == kFormat565 && depth == 16) {
      encoding = Encoding::Raw565;
    } else if (baseFormat == kFormat4444 && depth == 16) {
      encoding = Encoding::Raw4444;
    } else {
      return false;
    }
  }

  size_t cursor = begin + kD3dRasterHeaderBytes;
  const size_t levels = declaredLevels == 0 ? 1u : declaredLevels;
  if (levels > 16) return false;
  bool levelZeroDecoded = false;
  for (size_t level = 0; level < levels; ++level) {
    if (!rangeInside(cursor, 4, end)) return false;
    const size_t dataBytes = readLe32(bytes.data() + cursor);
    cursor += 4;
    if (!rangeInside(cursor, dataBytes, end)) return false;
    if (level == 0) {
      const bool decoded = encoding == Encoding::Dxt1 || encoding == Encoding::Dxt3 || encoding == Encoding::Dxt5
                               ? decodeDxt(bytes.data() + cursor, dataBytes, encoding, candidate)
                               : decodeRaw(bytes.data() + cursor, dataBytes, encoding, candidate);
      if (!decoded) return false;
      levelZeroDecoded = true;
    }
    cursor += dataBytes;
  }
  if (!levelZeroDecoded || cursor != end) return false;
  out = std::move(candidate);
  return true;
}

bool parseRaster(const std::vector<uint8_t>& bytes, const Chunk& raster, Texture& out) {
  size_t cursor = raster.payloadOffset;
  bool structFound = false;
  Texture candidate;
  Chunk child;
  while (cursor < raster.payloadEnd) {
    if (!readChunk(bytes, cursor, raster.payloadEnd, child)) return false;
    if (child.type == kRwStruct) {
      if (structFound || !parseRasterStruct(bytes, child.payloadOffset, child.payloadEnd, candidate)) {
        return false;
      }
      structFound = true;
    }
  }
  if (!structFound) return false;
  out = std::move(candidate);
  return true;
}

bool parseDictionary(const std::vector<uint8_t>& bytes, const Chunk& dictionary,
                     std::vector<Texture>& textures, size_t maxTextures, size_t maxPixels,
                     size_t& totalPixels) {
  size_t cursor = dictionary.payloadOffset;
  Chunk child;
  if (!readChunk(bytes, cursor, dictionary.payloadEnd, child) || child.type != kRwStruct ||
      child.payloadEnd - child.payloadOffset < 4) {
    return false;
  }
  const size_t declaredTextures = readLe16(bytes.data() + child.payloadOffset);
  // Empty dictionaries are valid stock placeholders. Keep their bounded,
  // structurally validated no-op semantics rather than treating them as a
  // malformed raster source.
  if (declaredTextures > maxTextures) return false;

  std::unordered_set<std::string> names;
  size_t parsed = 0;
  while (cursor < dictionary.payloadEnd) {
    if (!readChunk(bytes, cursor, dictionary.payloadEnd, child)) return false;
    if (child.type != kRwTextureNative) continue;
    if (textures.size() >= maxTextures) return false;
    Texture texture;
    if (!parseRaster(bytes, child, texture) || !names.insert(texture.name).second) return false;
    const size_t pixels = static_cast<size_t>(texture.width) * texture.height;
    if (totalPixels > maxPixels || pixels > maxPixels - totalPixels) return false;
    totalPixels += pixels;
    textures.push_back(std::move(texture));
    ++parsed;
  }
  return parsed == declaredTextures;
}

}  // namespace

bool parsePortableTextures(const std::vector<uint8_t>& bytes, std::vector<Texture>& out,
                           std::string& error, size_t maxTextures, size_t maxPixels) {
  out.clear();
  error.clear();
  if (maxTextures == 0 || maxTextures > kMaxTexturesPerDictionary) maxTextures = kMaxTexturesPerDictionary;
  if (maxPixels == 0 || maxPixels > kMaxTexturePixelsTotal) maxPixels = kMaxTexturePixelsTotal;
  if (bytes.size() < kChunkHeaderBytes) {
    error = "TXD terlalu pendek untuk header RenderWare";
    return false;
  }

  std::vector<Texture> candidate;
  size_t totalPixels = 0;
  size_t cursor = 0;
  Chunk top;
  bool dictionaryFound = false;
  while (cursor < bytes.size()) {
    // A zero chunk is not a portable TXD construct. It can only be the
    // all-zero sector tail supplied by IMG; reject mixed tails in one scan.
    if (bytes[cursor] == 0u &&
        (bytes.size() - cursor < 4u || readLe32(bytes.data() + cursor) == 0u)) {
      if (zeroFilledSuffix(bytes, cursor)) break;
      error = "chunk nol non-padding setelah stream RenderWare TXD";
      return false;
    }
    if (!readChunk(bytes, cursor, bytes.size(), top)) {
      error = "chunk RenderWare TXD terpotong atau melampaui batas";
      return false;
    }
    if (top.type != kRwTextureDictionary) continue;
    if (dictionaryFound || !parseDictionary(bytes, top, candidate, maxTextures, maxPixels, totalPixels)) {
      error = "hierarki atau raster portable TXD tidak didukung";
      return false;
    }
    dictionaryFound = true;
  }
  if (!dictionaryFound) {
    error = "TXD tidak memiliki dictionary RenderWare";
    return false;
  }
  out = std::move(candidate);
  return true;
}

}  // namespace samp::gta::txd
