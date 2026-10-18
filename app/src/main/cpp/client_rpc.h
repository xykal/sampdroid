// Client-to-server SA-MP/open.mp RPC payload builders.
// A live session wraps these payloads in RakNet RELIABLE_ORDERED channel 2.
#pragma once

#include <cstdint>
#include <vector>

namespace samp::clientrpc {

std::vector<uint8_t> buildRequestClass(uint16_t classId);
std::vector<uint8_t> buildRequestSpawn();

}  // namespace samp::clientrpc
