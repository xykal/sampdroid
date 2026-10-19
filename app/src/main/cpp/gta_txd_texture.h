// Bounded portable PC TXD decoder for user-owned GTA SA texture dictionaries.
// It emits only decoded RGBA mip level zero for the Android renderer; it does
// not extract, write, download, or redistribute any texture asset.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace samp::gta::txd {

struct Texture {
  std::string name;  // Lowercase RenderWare texture name (no filesystem path).
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4, top-level mip only.
};

constexpr size_t kMaxTexturesPerDictionary = 4096;
constexpr size_t kMaxTextureDimension = 2048;
constexpr size_t kMaxTexturePixels = 4u * 1024u * 1024u;
constexpr size_t kMaxTexturePixelsTotal = 16u * 1024u * 1024u;

// Reads Direct3D 8/9 PC Raster chunks from an SA TXD transactionally. Supports
// raw 8888/888/565/4444 and DXT1/DXT3/DXT5 top-level mipmaps. Paletted, PS2,
// XBOX, cube, and unsupported native layouts fail closed rather than guessing.
bool parsePortableTextures(const std::vector<uint8_t>& bytes, std::vector<Texture>& out,
                           std::string& error,
                           size_t maxTextures = kMaxTexturesPerDictionary,
                           size_t maxPixels = kMaxTexturePixelsTotal);

}  // namespace samp::gta::txd
