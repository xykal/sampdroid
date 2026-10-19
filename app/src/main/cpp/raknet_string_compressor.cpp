// Bounded default-language RakNet StringCompressor decoder.
//
// The wire format is interoperable with the public RakNet StringCompressor
// format: a compressed u16 bit count followed by an MSB-first Huffman stream.
// This implementation builds the fixed default tree locally and links no
// RakNet source or library into SampDroid.
#include "raknet_string_compressor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace samp::raknet::stringcompressor {
namespace {

struct Node {
  uint32_t weight = 0;
  int left = -1;
  int right = -1;
  int value = -1;
};

struct HuffmanTree {
  std::vector<Node> nodes;
  int root = -1;
};

// The default-language frequency data is part of the published RakNet wire
// behavior. Zero entries deliberately become weight one when the tree is
// constructed, matching the protocol's requirement that all byte values stay
// decodable.
constexpr std::array<uint32_t, 256> kDefaultFrequencies = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 722, 0, 0, 2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    11084, 58, 63, 1, 0, 31, 0, 317, 64, 64, 44, 0, 695, 62, 980, 266,
    69, 67, 56, 7, 73, 3, 14, 2, 69, 1, 167, 9, 1, 2, 25, 94,
    0, 195, 139, 34, 96, 48, 103, 56, 125, 653, 21, 5, 23, 64, 85, 44,
    34, 7, 92, 76, 147, 12, 14, 57, 15, 39, 15, 1, 1, 1, 2, 3,
    0, 3611, 845, 1077, 1884, 5870, 841, 1057, 2501, 3212, 164, 531, 2019, 1330, 3056, 4037,
    848, 47, 2586, 2919, 4771, 1707, 535, 1106, 152, 1243, 100, 0, 2, 0, 10, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

void insertByWeight(std::vector<int>& queue, const std::vector<Node>& nodes, int nodeIndex) {
  // Equal weights go before older entries. That deterministic tie rule is
  // necessary for a byte-for-byte compatible default tree.
  const auto at = std::find_if(queue.begin(), queue.end(), [&](int existing) {
    return nodes[existing].weight >= nodes[nodeIndex].weight;
  });
  queue.insert(at, nodeIndex);
}

HuffmanTree buildDefaultTree() {
  HuffmanTree tree;
  tree.nodes.reserve(511);
  std::vector<int> queue;
  queue.reserve(511);

  for (int value = 0; value < 256; ++value) {
    Node leaf;
    leaf.weight = kDefaultFrequencies[static_cast<size_t>(value)];
    if (leaf.weight == 0) leaf.weight = 1;
    leaf.value = value;
    tree.nodes.push_back(leaf);
    insertByWeight(queue, tree.nodes, value);
  }

  while (queue.size() > 1) {
    const int lesser = queue.front();
    queue.erase(queue.begin());
    const int greater = queue.front();
    queue.erase(queue.begin());

    Node branch;
    branch.weight = tree.nodes[lesser].weight + tree.nodes[greater].weight;
    branch.left = lesser;
    branch.right = greater;
    const int branchIndex = static_cast<int>(tree.nodes.size());
    tree.nodes.push_back(branch);
    insertByWeight(queue, tree.nodes, branchIndex);
  }
  if (!queue.empty()) tree.root = queue.front();
  return tree;
}

const HuffmanTree& defaultTree() {
  static const HuffmanTree tree = buildDefaultTree();
  return tree;
}

}  // namespace

bool decodeDefaultString(BitReader& reader, size_t maxBytes, std::string& output) {
  output.clear();
  const uint16_t encodedBitCount = reader.readCompressedU16();
  if (!reader.ok()) return false;
  if (encodedBitCount == 0) return true;

  const HuffmanTree& tree = defaultTree();
  if (tree.root < 0) return false;
  int node = tree.root;
  for (uint16_t bit = 0; bit < encodedBitCount; ++bit) {
    const bool one = reader.readBit();
    if (!reader.ok()) {
      output.clear();
      return false;
    }
    node = one ? tree.nodes[node].right : tree.nodes[node].left;
    if (node < 0 || static_cast<size_t>(node) >= tree.nodes.size()) {
      output.clear();
      return false;
    }
    const Node& current = tree.nodes[node];
    if (current.left == -1 && current.right == -1) {
      if (output.size() >= maxBytes) {
        output.clear();
        return false;
      }
      output.push_back(static_cast<char>(current.value));
      node = tree.root;
    }
  }

  // RakNet pads EncodeArray to a byte boundary and includes those padding
  // bits in the advertised count. The decoder intentionally ignores a final
  // partial codeword: requiring `node == root` would reject valid strings
  // whose alignment padding ends inside a Huffman path.
  return true;
}

}  // namespace samp::raknet::stringcompressor
