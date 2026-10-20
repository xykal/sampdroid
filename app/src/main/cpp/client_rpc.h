// Client-to-server SA-MP/open.mp RPC payload builders.
// A live session wraps these payloads in RakNet RELIABLE_ORDERED channel 0.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace samp::clientrpc {

// Exact legacy-client ordering stream for every client-originated RPC,
// including ClientJoin. Keep this distinct from the USeq movement channel.
constexpr uint8_t kReliableOrderedChannel = 0u;

std::vector<uint8_t> buildRequestClass(uint16_t classId);
std::vector<uint8_t> buildRequestSpawn();
// Final empty RPC in the class-selection spawn handshake. It is emitted only
// after a server-approved RequestSpawn response arrives.
std::vector<uint8_t> buildPlayerSpawn();

// Builds legacy-compatible PlayerRequestChatMessage (RPC 101) and
// PlayerRequestCommandMessage (RPC 50) payloads. Empty output means malformed,
// too long, or unsafe user text; callers must not send it.
std::vector<uint8_t> buildChatMessage(const std::string& message);
std::vector<uint8_t> buildCommandMessage(const std::string& command);

// Builds OnPlayerDialogResponse (RPC 62). `response` maps to the first/second
// button, `listItem` is -1 for non-list dialogs, and text is bounded DynStr8.
std::vector<uint8_t> buildDialogResponse(int16_t dialogId, bool response, int16_t listItem,
                                         const std::string& text);

// Current open.mp menu writer/source verifies PlayerInitMenu (76), Show (77),
// Hide (78), and these client responses. The selected row is server-owned;
// callers must validate the active menu revision before queuing either RPC.
std::vector<uint8_t> buildMenuSelect(uint8_t row);
std::vector<uint8_t> buildMenuExit();

// Builds the explicit player-confirmed pickup notification (RPC 131). The
// caller must select a currently known nearby pickup and throttle repeats; this
// builder has no movement/world authority.
std::vector<uint8_t> buildPickedUpPickup(int32_t pickupId);

// Requests a nearby vehicle enter/exit event. These are attempts only; server
// PutPlayerInVehicle/RemovePlayerFromVehicle remains authoritative.
std::vector<uint8_t> buildEnterVehicle(uint16_t vehicleId, bool passenger);
std::vector<uint8_t> buildExitVehicle(uint16_t vehicleId);

// Sends the raw server-assigned global/player textdraw wire ID for a current
// selection session. State validation remains in the retained session layer.
std::vector<uint8_t> buildSelectTextDraw(uint16_t wireId);

// Current open.mp OnPlayerSelectObject inbound reader consumes RPC 27 as
// `u32 selectType | u16 objectId | u32 model | VEC3 hitPosition`. The Android
// path always uses the neutral ObjectSelectType_None value zero and a
// native-sampled object origin: it never
// accepts model/coordinates from Java as a target or a fabricated raycast hit.
std::vector<uint8_t> buildSelectObject(uint16_t objectId, uint32_t model,
                                       float hitX, float hitY, float hitZ);

// Current open.mp OnPlayerEditObject inbound reader consumes RPC 117 as
// `bit playerObject | u16 objectId | u32 response | VEC3 position | VEC3 rotation`.
// The session must verify the currently server-open edit mode before this
// builder is reached. Empty output rejects impossible response/object IDs and
// non-finite or unsafe transforms.
std::vector<uint8_t> buildObjectEditResponse(bool playerObject, uint16_t objectId,
                                             uint32_t response, float x, float y, float z,
                                             float rotationX, float rotationY, float rotationZ);

// Current open.mp OnPlayerEditAttachedObject reader consumes RPC 116 as
// `u32 response | u32 slot | u32 model | u32 bone | VEC3 offset | VEC3 rotation
// | VEC3 scale | u32 colour1 ARGB | u32 colour2 ARGB`. The active attachment
// slot/model/bone/colours must be re-sampled in native code; callers supply
// only the bounded editable offset/rotation/scale values.
std::vector<uint8_t> buildAttachedObjectEditResponse(
    uint32_t response, uint32_t slot, uint32_t model, uint32_t bone,
    float offsetX, float offsetY, float offsetZ,
    float rotationX, float rotationY, float rotationZ,
    float scaleX, float scaleY, float scaleZ,
    uint32_t colour1Argb, uint32_t colour2Argb);

// Requests the server's cached full score/ping roster (RPC 155, empty body).
// The response remains display-only and is bounded by the state reducer.
std::vector<uint8_t> buildRequestScoresAndPings();

}  // namespace samp::clientrpc
