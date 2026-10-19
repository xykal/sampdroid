// Minimal, independently implemented decoder for RakNet's default
// StringCompressor wire format. It is used only for bounded server UI text.
#pragma once

#include "raknet_codec.h"

#include <cstddef>
#include <string>

namespace samp::raknet::stringcompressor {

// Reads one default-language StringCompressor value: compressed u16 bit length
// followed by MSB-first Huffman bits. `maxBytes` caps output and malformed or
// oversized strings fail closed without exposing a partial UI string.
bool decodeDefaultString(BitReader& reader, size_t maxBytes, std::string& output);

}  // namespace samp::raknet::stringcompressor
