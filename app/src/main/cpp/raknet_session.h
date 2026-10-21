// Persistent native RakNet session API for the Android client shell.
// This owns no renderer and never executes a Windows/x86 client.
#pragma once

#include "client_sync.h"
#include "gta_building_removal.h"
#include "input_state.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace samp {

namespace gta::world {
class CollisionWorld;
}

namespace vehiclemotion {
struct VehicleMotionTuning;
}

// Bounded server Apply*Animation directive carried only for visual/UI handoff.
// It is intentionally not a local movement or collision instruction: actual
// skeletal execution requires a later user-owned GTA IFP asset path.
struct WorldAnimationSnapshot {
  bool active = false;
  uint64_t revision = 0;
  std::string library;
  std::string name;
  float delta = 0.0f;
  bool loop = false;
  bool lockX = false;
  bool lockY = false;
  bool freeze = false;
  uint32_t durationMs = 0;
};

struct WorldActorSnapshot {
  uint16_t id = 0;
  uint32_t rgba = 0;
  uint32_t model = 0;
  bool onFootKnown = false;
  // Short-lived renderer-only copy of a remote Packet-203 camera direction.
  // It is never a target, raycast, projectile, or damage assertion.
  bool aimKnown = false;
  uint8_t aimCameraMode = 0;
  uint8_t aimWeaponState = 0;
  float aimCameraFrontX = 0.0f;
  float aimCameraFrontY = 0.0f;
  float aimCameraFrontZ = 0.0f;
  float aimCameraPositionX = 0.0f;
  float aimCameraPositionY = 0.0f;
  float aimCameraPositionZ = 0.0f;
  // Short-lived server-relayed Packet-206 direction cue only. The snapshot
  // excludes hit IDs/types so rendering cannot infer local damage authority.
  bool bulletKnown = false;
  uint8_t bulletWeaponId = 0;
  float bulletOriginX = 0.0f;
  float bulletOriginY = 0.0f;
  float bulletOriginZ = 0.0f;
  float bulletDirectionX = 0.0f;
  float bulletDirectionY = 0.0f;
  float bulletDirectionZ = 0.0f;
  // Brief RPC-166 observer-only wasted cue; never local death authority.
  bool wasted = false;
  WorldAnimationSnapshot forcedAnimation;
  bool inVehicle = false;
  bool vehicleDriver = false;
  uint16_t vehicleId = 0;
  uint8_t vehicleSeat = 0;
  bool nameTagVisible = true;
  bool chatBubbleActive = false;
  uint32_t chatBubbleRgba = 0;
  float chatBubbleDrawDistance = 0.0f;
  std::string chatBubbleText;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float health = 100.0f;
  float armour = 0.0f;
  std::string name;
};

// Server NPC actor copied from ShowActor/HideActor and exact transform
// mutators. It has a separate namespace from remote player IDs and remains
// visual-only: no local combat/collision/interaction authority crosses JNI.
struct WorldServerActorSnapshot {
  uint16_t id = 0;
  uint32_t model = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float health = 100.0f;
  bool invulnerable = false;
  WorldAnimationSnapshot forcedAnimation;
};

// Copied server chat suitable for a small in-world HUD. The retained WorldState
// history is larger, while this renderer boundary always exports a fixed small
// tail so a chat flood cannot amplify into UI allocations every frame.
struct WorldChatSnapshot {
  uint32_t rgba = 0;
  std::string text;
};

// Global RPC-55 feed copied after expiry filtering. Names are already resolved
// from local/known player facts so Android never has to infer identity from IDs.
struct WorldDeathMessageSnapshot {
  bool hasKiller = false;
  uint16_t killerId = 0xFFFFu;
  uint16_t victimId = 0xFFFFu;
  uint8_t reason = 0;
  std::string killerName;
  std::string victimName;
};

// One score/ping roster row; it has no transform and is kept separately from
// streamed render actors so TAB-style UI can show distant joined players.
struct WorldScoreSnapshot {
  uint16_t id = 0;
  uint32_t rgba = 0;
  bool npc = false;
  int32_t score = 0;
  uint32_t ping = 0;
  std::string name;
};

// Bounded, decoded ShowDialog hand-off. The Android layer never sees raw
// StringCompressor bytes or session-owned dialog state.
struct WorldDialogSnapshot {
  bool active = false;
  uint64_t revision = 0;
  int16_t id = -1;
  uint8_t style = 0;
  std::string title;
  std::string button1;
  std::string button2;
  std::string body;
};

// Current legacy menu hand-off. Fixed source strings were bounded/decoded in
// WorldState; Android sees only rows and revision-bound presentation facts.
struct WorldMenuSnapshot {
  bool active = false;
  uint64_t revision = 0;
  uint8_t id = 0;
  bool twoColumns = false;
  bool enabled = true;
  std::array<bool, 12> rowEnabled{};
  std::string title;
  std::string column1Header;
  std::string column2Header;
  std::array<std::string, 12> column1Items{};
  std::array<std::string, 12> column2Items{};
  uint8_t column1Count = 0;
  uint8_t column2Count = 0;
};

// Bounded vehicle hand-off. StreamInVehicle establishes entity identity and
// Packet 200/209 can update its authoritative visual transform; Packet 211
// remains a separate passenger-actor anchor. This is still not local driving,
// collision, or a full GTA vehicle simulation.
struct WorldVehicleSnapshot {
  uint16_t id = 0;
  bool dynamicSyncKnown = false;
  bool driverKnown = false;
  uint16_t driverId = 0;
  uint32_t model = 0;
  uint8_t colour1 = 0;
  uint8_t colour2 = 0;
  uint8_t paintjob = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  // Packet-200 quaternion-derived DFF attitude; false means use yaw fallback.
  bool attitudeKnown = false;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  // Source-owned RPC-24 values in wire order. `-1` remains the explicit
  // no-server-value sentinel, never a fabricated local switch state.
  bool parametersKnown = false;
  std::array<int8_t, 16> parameters{
      -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1};
  float health = 1000.0f;
};

// One bounded Default or Text material copied from an object RPC. Default
// names remain lookup keys, never Android paths. Text values stay bounded at
// the source writer's HybridString<32> capacity and retain ARGB colours.
struct WorldObjectMaterialSnapshot {
  bool active = false;
  uint16_t sourceModel = 0;
  uint32_t colourAbgr = 0xFFFFFFFFu;
  std::string textureDictionary;
  std::string textureName;
  bool textActive = false;
  uint8_t textMaterialSize = 0;
  uint8_t textFontSize = 0;
  bool textBold = false;
  uint32_t textColourArgb = 0;
  uint32_t textBackgroundColourArgb = 0;
  uint8_t textAlignment = 0;
  std::string textFont;
  std::string text;
};

// Server-owned object transform copied from Create/Set/Destroy/MoveObject RPC
// state. A bounded renderer-only vehicle/object/player attachment chain is
// resolved before this snapshot; absent, streamed-out, over-deep, or cyclic parents are
// omitted rather than fabricated at the child CreateObject coordinates.
struct WorldObjectSnapshot {
  uint16_t id = 0;
  uint32_t model = 0;
  bool moving = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float drawDistance = 0.0f;
  std::array<WorldObjectMaterialSnapshot, 16> materials{};
};

// Renderer-only sampled SetPlayerAttachedObject state. `bone` remains the raw
// server enum for diagnostics; its Android placement is an explicitly
// approximate presentation anchor, never a skeleton/collision fact.
struct WorldPlayerAttachedObjectSnapshot {
  uint16_t playerId = 0;
  uint8_t slot = 0;
  uint32_t model = 0;
  uint8_t bone = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float scaleZ = 1.0f;
  uint32_t colour1Abgr = 0xFFFFFFFFu;
  uint32_t colour2Abgr = 0xFFFFFFFFu;
};

// Raw source attachment facts made visible only while server RPC 116 opens a
// local editor. Unlike WorldPlayerAttachedObjectSnapshot, offsets stay in the
// protocol's attachment-local coordinate space so Android never reverse-solves
// a rendered bone/world transform to fabricate an outbound edit.
struct WorldAttachedObjectEditSnapshot {
  bool active = false;
  uint8_t slot = 0;
  uint64_t revision = 0;
  uint32_t model = 0;
  uint8_t bone = 0;
  float offsetX = 0.0f;
  float offsetY = 0.0f;
  float offsetZ = 0.0f;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float scaleZ = 1.0f;
};

// Server-created pickup hand-off. This is visual plus user-confirmed nearby
// interaction candidate data, not a local award/inventory simulation.
struct WorldPickupSnapshot {
  int32_t id = 0;
  int32_t model = 0;
  int32_t spawnType = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

// Server-visible 2D gang-zone territory, copied only for radar rendering.
// Colours retain their numeric ABGR wire representation so Android can map
// them without guessing an RGBA byte order; flashing phase is presentation-only.
struct WorldGangZoneSnapshot {
  uint16_t id = 0;
  float minX = 0.0f;
  float minY = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;
  uint32_t colourAbgr = 0;
  bool flashing = false;
  uint32_t flashColourAbgr = 0;
};

// Server-owned local spectator state. Target IDs remain foreign namespaces:
// Kotlin resolves them only against copied players/vehicles and never lets a
// target become local input, collision, or sync authority.
struct WorldSpectateSnapshot {
  bool known = false;
  bool active = false;
  bool targetKnown = false;
  bool targetVehicle = false;
  uint16_t targetId = 0;
  uint8_t mode = 0;
};

// Read-only sampled server camera RPC state. It is deliberately separate from
// local movement and collision state: SetPlayerCamera*/InterpolateCamera must
// never mutate a player transform or manufacture outbound gameplay input.
struct WorldCameraSnapshot {
  bool positionKnown = false;
  bool lookAtKnown = false;
  bool interpolatingPosition = false;
  bool interpolatingLookAt = false;
  float positionX = 0.0f;
  float positionY = 0.0f;
  float positionZ = 0.0f;
  float lookAtX = 0.0f;
  float lookAtY = 0.0f;
  float lookAtZ = 0.0f;
};

// Decoded PlayerShowTextLabel visual state. Position is static world-space or
// an attachment offset depending on non-sentinel player/vehicle IDs. Android
// resolves those IDs against its immutable frame; no pointer/lifecycle state
// crosses the JNI boundary.
struct WorldTextLabelSnapshot {
  uint16_t wireId = 0;
  bool playerTextLabel = false;
  uint32_t rgba = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float drawDistance = 0.0f;
  bool los = false;
  uint16_t playerAttachId = 0xFFFFu;
  uint16_t vehicleAttachId = 0xFFFFu;
  std::string text;
};

// Per-recipient SetPlayerMapIcon state. `type`/`style` remain raw legacy
// values because Android's portable marker renderer has no GTA radar atlas.
struct WorldMapIconSnapshot {
  uint8_t id = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint8_t type = 0;
  uint32_t rgba = 0;
  uint8_t style = 0;
};

// Bounded textdraw data for Android's screen-space HUD. Styles that require
// GTA frontend/model assets are labeled/proxied rather than emulated.
struct WorldTextDrawSnapshot {
  uint16_t wireId = 0;
  bool playerTextDraw = false;
  bool useBox = false;
  uint8_t alignment = 0;
  float letterWidth = 0.0f;
  float letterHeight = 0.0f;
  uint32_t letterColourAbgr = 0;
  float textWidth = 0.0f;
  float textHeight = 0.0f;
  uint32_t boxColourAbgr = 0;
  uint8_t shadow = 0;
  uint8_t outline = 0;
  uint32_t backgroundColourAbgr = 0;
  uint8_t style = 0;
  bool selectable = false;
  float x = 0.0f;
  float y = 0.0f;
  // Exact ShowTextDraw preview fields. They remain data-only until a proper
  // screen-space DFF/texture preview renderer is available.
  uint16_t model = 0;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float zoom = 0.0f;
  int16_t colour1 = 0;
  int16_t colour2 = 0;
  std::string text;
};

// Timed RPC-73 hand-off for Android's deliberately non-GTA GameText banner.
struct WorldGameTextSnapshot {
  bool active = false;
  uint64_t revision = 0;
  uint32_t style = 0;
  uint32_t durationMs = 0;
  std::string text;
};

// Read-only renderer/HUD hand-off. This is a small bounded view of authoritative
// session state, not a simulation, collision system, or transmit command.
struct WorldSnapshot {
  bool active = false;
  // Copy only native-owned lifecycle facts into the renderer so Android can
  // present an honest pre-game state. They are not UI authority: RequestClass,
  // RequestSpawn, PlayerSpawn, and its transport ACK remain native-gated.
  bool playerInit = false;
  bool classSelectionKnown = false;
  bool classRequestPending = false;
  bool spawnResponseKnown = false;
  bool spawnAllowed = false;
  // Native-session presentation gate for an explicit PlayerRequestSpawn. It
  // lets Android disable duplicate respawn taps; it does not assert a server
  // spawn outcome or mutate any WorldState fact.
  bool spawnRequestPending = false;
  bool spawnConfirmationSent = false;
  bool spawnConfirmationAcked = false;
  // Set only after an authoritative SetSpawnInfo template gives a model ID.
  // Position alone must not make Android guess CJ/model zero for every player.
  bool localSpawnInfoKnown = false;
  bool localPositionKnown = false;
  bool localFacingKnown = false;
  bool localHealthKnown = false;
  bool localArmourKnown = false;
  bool localMoneyKnown = false;
  bool localWantedLevelKnown = false;
  bool localScoreKnown = false;
  bool localArmedWeaponKnown = false;
  bool localArmedAmmoKnown = false;
  bool localControllableKnown = false;
  bool localControllable = true;
  bool localInteriorKnown = false;
  // Server RPC 48 virtual-world state is retained for HUD/session fidelity;
  // it does not infer an IPL/collision partition.
  bool localVirtualWorldKnown = false;
  bool localVehicleKnown = false;
  bool localInVehicle = false;
  // Derived only from the latest native Packet-211 candidate after server seat
  // authority; it is a passenger presentation state, not damage authority.
  bool localPassengerDriveByActive = false;
  uint8_t localInterior = 0;
  int32_t localVirtualWorld = 0;
  uint16_t localVehicleId = 0;
  uint8_t localVehicleSeat = 0;
  WorldSpectateSnapshot spectate;
  // True only while collision-grounded local prediction has accepted a step;
  // server position/spawn corrections replace it immediately.
  bool localPredictionActive = false;
  bool localGrounded = false;
  // Semantic AIM is renderer-visible only as a reticle/status. The native
  // session remains sole authority for whether its bounded Packet-203 candidate
  // has passed collision/weapon/camera gates. FIRE is renderer-visible only
  // for the neutral no-target shot affordance; it carries no target authority.
  bool localAimHeld = false;
  bool localFireHeld = false;
  bool localAimSyncReady = false;
  // PlayerInit lag-compensation mode is the server-advertised availability
  // gate for this conservative Packet-206 path; it does not assert a shot.
  bool localBulletSyncServerEnabled = false;
  // Short presentation pulse after native socket-send accepted a close static
  // COL HitPos. It is explicitly not server receipt, entity targeting, or hit.
  bool localStaticWorldContactActive = false;
  // Count of datagrams actually accepted by the native socket send path; this
  // is a local transport diagnostic, not server receipt or a hit/damage count.
  uint32_t outboundNoTargetBulletPackets = 0;
  float localAimPitchDegrees = 0.0f;
  // Render-only fallback: local camera anchor samples a streamed server vehicle
  // transform while seated and not driving a local kinematic prediction. This
  // is not an exact GTA seat transform and never changes WorldState/input sync.
  bool localVehicleAnchorKnown = false;
  bool worldTimeKnown = false;
  // Per-recipient RPC-29/30 visual clock state. Kotlin cannot alter it.
  bool playerTimeKnown = false;
  bool playerClockKnown = false;
  bool playerClockEnabled = true;
  // Retained RPC-41/42 directive. Java independently rejects non-HTTPS URLs
  // before media playback; these fields have no input or movement authority.
  bool audioStreamActive = false;
  bool audioStreamPositional = false;
  bool weatherKnown = false;
  bool gravityKnown = false;
  // Received RPC-17 rectangle, exposed for an honest HUD diagnostic only.
  // It does not grant Kotlin any movement or collision authority.
  bool worldBoundsKnown = false;
  bool showNameTags = false;
  bool disableNameTagLos = false;
  bool checkpointActive = false;
  bool raceCheckpointActive = false;
  uint16_t localPlayerId = 0;
  uint8_t worldTime = 12;
  uint8_t playerHour = 12;
  uint8_t playerMinute = 0;
  uint64_t audioStreamRevision = 0;
  float audioStreamX = 0.0f;
  float audioStreamY = 0.0f;
  float audioStreamZ = 0.0f;
  float audioStreamDistance = 0.0f;
  std::string audioStreamUrl;
  uint8_t weather = 0;
  float gravity = 0.008f;
  float worldMaxX = 20000.0f;
  float worldMinX = -20000.0f;
  float worldMaxY = 20000.0f;
  float worldMinY = -20000.0f;
  float nameTagDrawDistance = 0.0f;
  uint32_t localModel = 0;
  float localX = 0.0f;
  float localY = 0.0f;
  float localZ = 0.0f;
  float localAngle = 0.0f;
  float localHealth = 100.0f;
  float localArmour = 0.0f;
  int64_t localMoney = 0;
  uint8_t localWantedLevel = 0;
  int32_t localScore = 0;
  uint32_t localPing = 0;
  uint8_t localArmedWeapon = 0;
  uint32_t localArmedAmmo = 0;
  bool localChatBubbleActive = false;
  uint32_t localChatBubbleRgba = 0;
  float localChatBubbleDrawDistance = 0.0f;
  std::string localChatBubbleText;
  WorldAnimationSnapshot localForcedAnimation;
  float checkpointX = 0.0f;
  float checkpointY = 0.0f;
  float checkpointZ = 0.0f;
  float checkpointSize = 0.0f;
  float raceCheckpointX = 0.0f;
  float raceCheckpointY = 0.0f;
  float raceCheckpointZ = 0.0f;
  float raceCheckpointSize = 0.0f;
  int streamedPlayers = 0;
  int remoteOnFootPlayers = 0;
  int streamedVehicles = 0;
  int trackedServerActors = 0;
  int serverObjects = 0;
  int trackedPickups = 0;
  int trackedTextDraws = 0;
  int trackedTextLabels = 0;
  int trackedMapIcons = 0;
  int trackedGangZones = 0;
  WorldCameraSnapshot camera;
  bool textDrawSelectionActive = false;
  // RPC 27 selection is independent of screen textdraw selection. It only
  // enables a bounded object-ID picker; model/position stay native-owned.
  bool objectSelectionActive = false;
  // This monotonic native token changes on Begin/Cancel/mode replacement so a
  // delayed UI choice cannot be applied to a newer RPC-27 selection lifecycle.
  uint64_t objectSelectionRevision = 0;
  // RPC 117 editing is a separate server-owned mode. The tuple/revision is
  // presentation only; every outbound transform reply is still native-gated.
  bool objectEditActive = false;
  bool objectEditPlayerObject = false;
  uint16_t objectEditId = 0xFFFFu;
  uint64_t objectEditRevision = 0;
  // The active object editor gets its own attachment-resolved baseline even
  // when the normal 128-object renderer copy is full or sorted before its ID.
  // This avoids Java inferring a player-object transform from an unrelated
  // capped frame entry.
  bool objectEditTransformAvailable = false;
  float objectEditX = 0.0f;
  float objectEditY = 0.0f;
  float objectEditZ = 0.0f;
  float objectEditRotationX = 0.0f;
  float objectEditRotationY = 0.0f;
  float objectEditRotationZ = 0.0f;
  WorldAttachedObjectEditSnapshot attachedObjectEdit;
  // Native RPC 83 uses an RGBA colour for the server's selection affordance.
  uint32_t textDrawSelectionColourRgba = 0;
  WorldGameTextSnapshot gameText;
  WorldDialogSnapshot dialog;
  WorldMenuSnapshot menu;
  std::string serverName;
  std::string error;
  std::vector<WorldActorSnapshot> actors;
  std::vector<WorldServerActorSnapshot> serverActors;
  std::vector<WorldScoreSnapshot> scoreboard;
  std::vector<WorldVehicleSnapshot> vehicles;
  std::vector<WorldObjectSnapshot> objects;
  std::vector<WorldPlayerAttachedObjectSnapshot> playerAttachedObjects;
  std::vector<WorldPickupSnapshot> pickups;
  std::vector<WorldGangZoneSnapshot> gangZones;
  std::vector<WorldTextLabelSnapshot> textLabels;
  std::vector<WorldMapIconSnapshot> mapIcons;
  std::vector<WorldTextDrawSnapshot> textDraws;
  std::vector<WorldChatSnapshot> chat;
  std::vector<WorldDeathMessageSnapshot> deathMessages;
};

struct SessionSnapshot {
  bool active = false;
  bool accepted = false;
  // Per-record RakNet ACK telemetry for the source-style post-accept burst.
  // These receipts are deliberately separate from `joined`/`playerInit`: an
  // ACK proves only peer transport receipt, not SA-MP gameplay admission.
  bool newIncomingTransportAcked = false;
  bool staticDataTransportAcked = false;
  bool clientJoinTransportAcked = false;
  bool joined = false;
  bool playerInit = false;
  bool classSelectionKnown = false;
  bool classSelectionForced = false;
  bool classSelectable = false;
  bool classRequestPending = false;
  bool spawnResponseKnown = false;
  bool spawnAllowed = false;
  bool spawnRequestPending = false;
  bool spawnConfirmationSent = false;
  bool spawnConfirmationAcked = false;
  bool localSpawnInfoKnown = false;
  bool localPositionKnown = false;
  bool localControllableKnown = false;
  bool localControllable = true;
  bool localInteriorKnown = false;
  bool localVirtualWorldKnown = false;
  bool localVehicleKnown = false;
  bool localInVehicle = false;
  uint8_t localInterior = 0;
  int32_t localVirtualWorld = 0;
  uint16_t localVehicleId = 0;
  uint8_t localVehicleSeat = 0;
  bool nativeControlsKnown = false;
  bool onFootPreviewReady = false;
  bool inCarPreviewReady = false;
  bool aimPreviewReady = false;
  bool passengerPreviewReady = false;
  bool localPredictionActive = false;
  bool localGrounded = false;
  bool checkpointActive = false;
  bool raceCheckpointActive = false;
  uint16_t playerId = 0;
  uint16_t requestedClassId = 0;
  // Raw PlayerInit SetSpawnInfoCount, exposed only for bounded class browsing.
  uint32_t spawnClassCount = 0;
  uint32_t classModel = 0;
  uint32_t localModel = 0;
  float localX = 0.0f;
  float localY = 0.0f;
  float localZ = 0.0f;
  float localAngle = 0.0f;
  float controlMoveX = 0.0f;
  float controlMoveY = 0.0f;
  float controlHeading = 0.0f;
  float controlAimPitch = 0.0f;
  int controlLeftRight = 0;
  int controlUpDown = 0;
  uint16_t controlKeys = 0;
  // Raw OnFootRate retained from PlayerInit, plus the safe client-side
  // interval after malformed-rate bounds are applied.
  uint32_t onFootSyncRateMs = 0;
  uint32_t effectiveOnFootSyncRateMs = 0;
  uint32_t inCarSyncRateMs = 0;
  uint32_t effectiveInCarSyncRateMs = 0;
  uint32_t weaponSyncRateMs = 0;
  uint32_t effectiveWeaponSyncRateMs = 0;
  int receivedPackets = 0;
  int receivedRpcs = 0;
  // Local transport diagnostics (not a server score/ping authority): elapsed
  // time since any peer datagram, most recent reliable ACK round-trip from its
  // last send attempt, EWMA thereof, and bounded resend count.
  uint32_t inboundIdleMs = 0;
  uint32_t latestReliableRttMs = 0;
  uint32_t smoothedReliableRttMs = 0;
  int reliableResends = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  int trackedPlayers = 0;
  int streamedPlayers = 0;
  int remoteOnFootPlayers = 0;
  int trackedVehicles = 0;
  int streamedVehicles = 0;
  int trackedServerActors = 0;
  // Number of passive DL custom-object → stock-base catalog entries. This is
  // presentation/compatibility visibility only; it does not imply an asset was
  // downloaded, verified, or rendered as custom artwork.
  int dlObjectFallbacks = 0;
  int remoteOnFootPackets = 0;
  int remoteAimPackets = 0;
  int remoteBulletPackets = 0;
  int remoteVehiclePackets = 0;
  int chatMessages = 0;
  int trackedPickups = 0;
  int trackedTextLabels = 0;
  int trackedMapIcons = 0;
  int trackedGangZones = 0;
  int outboundPickupNotifications = 0;
  int outboundVehicleInteractions = 0;
  int outboundTextDrawSelections = 0;
  int outboundObjectSelections = 0;
  int outboundObjectEdits = 0;
  int outboundAttachedObjectEdits = 0;
  int outboundScoreRequests = 0;
  int reliableKeepalives = 0;
  int connectedPongs = 0;
  int outboundOnFootPackets = 0;
  int outboundInCarPackets = 0;
  int outboundAimPackets = 0;
  // Conservative client Packet-206 HitType=None attempts only; this is not a
  // target/damage counter and remains subject to server validation.
  int outboundNoTargetBulletPackets = 0;
  int outboundPassengerPackets = 0;
  int outboundChatMessages = 0;
  int outboundDialogResponses = 0;
  bool localOnFootStateKnown = false;
  bool localAimStateKnown = false;
  bool localPassengerStateKnown = false;
  // Exact bounded ClientJoin identity selected before the socket opens.
  // It reports transport compatibility only, not 0.3.DL artwork support.
  std::string clientProtocol;
  // `omp` means only that the OMP-cookie transport branch was selected; it is
  // neither an official-client version claim nor PlayerInit/game admission.
  std::string transportVariant;
  bool openMpServerEnvelope = false;
  std::string serverName;
  std::string rpcSummary;
  std::string syncSummary;
  std::string stateSummary;
  std::string error;
};

// Establishes and retains one explicit, user-initiated Android-native session.
// `nickname` is a user-selected legacy identity (3-24 printable ASCII bytes),
// never a bundled/shared bot name. Per-receive timeout is clamped 25–1500 ms;
// open/cookie/auth/post-accept is capped at 8x that value (1–12 s), then a
// quiet post-ClientJoin socket hands off to normal polling. The optional query
// version chooses DL only for an exact 0.3.DL-R1 value; its exact source-style
// `omp <major>.<minor>.<patch>.<build>` form chooses the OMP cookie first with
// a bounded SA-MP fallback. This changes transport negotiation only, never ClientJoin identity
// or PlayerInit/game admission. Always call off the UI thread.
SessionSnapshot raknet_session_connect(const std::string& ip, uint16_t port,
                                      int timeoutMs, const std::string& nickname,
                                      const std::string& advertisedServerVersion = "");

// Receives one timeout-bounded datagram, then only if it did not time out
// drains at most 15 immediately-ready datagrams, ACKs reliable packets, and
// reduces RPC state. Call from an IO coroutine while the user keeps the session.
SessionSnapshot raknet_session_poll(int timeoutMs = 200);

// Sends a RELIABLE_ORDERED SyncRPC-channel-2 PlayerRequestClass RPC. The caller
// must subsequently poll for the server reply before reading classSelectionKnown.
SessionSnapshot raknet_session_request_class(uint16_t classId);

// Starts the explicit spawn handshake. A nonzero server response automatically
// queues the required empty PlayerSpawn RPC; movement remains locked until its
// reliable transport ACK arrives.
SessionSnapshot raknet_session_request_spawn();

// Updates bounded Android-native semantic touch/controller input and rebuilds
// a local-only prospective OnFootSync preview. This function never transmits a
// gameplay packet; position/collision integration remains the world bridge's
// responsibility.
SessionSnapshot raknet_session_update_controls(const input::TouchIntent& input);

// Sends one bounded chat line or slash-prefixed command over the ordered RPC
// channel after PlayerInit. It never accepts control/newline bytes and retains
// no user text in diagnostic state.
SessionSnapshot raknet_session_send_chat(const std::string& message);

// Sends a response only for the currently visible server dialog revision. The
// dialog is hidden locally after the reliable packet queues successfully,
// preventing duplicate/stale Android taps while preserving failed-send recovery.
SessionSnapshot raknet_session_respond_dialog(int16_t dialogId, uint64_t dialogRevision,
                                              bool response, int16_t listItem,
                                              const std::string& text);

// Selects/exits only a currently active source-verified legacy menu revision.
// Both paths use RELIABLE_ORDERED SyncRPC channel 2 and clear Android
// presentation only after queuing succeeds, so stale/cross-menu taps cannot
// forge a response.
SessionSnapshot raknet_session_select_menu(uint8_t menuId, uint64_t menuRevision, uint8_t row);
SessionSnapshot raknet_session_exit_menu(uint8_t menuId, uint64_t menuRevision);

// User-confirmed USE action. While on foot it prefers a nearby pickup then a
// nearby streamed vehicle; while server-authoritatively seated it requests exit.
// A long-press may request passenger intent, but RPC 70/71 remain the sole seat
// authority. Every result is rate-limited against held taps.
SessionSnapshot raknet_session_use_nearby(bool passengerIntent = false);

// Requests the server's cached score/ping roster (empty RPC 155) once per
// bounded interval after acceptance/PlayerInit. The response remains UI-only.
SessionSnapshot raknet_session_request_scores();

// Sends an RPC 83 selection only for a current, server-selectable bounded
// textdraw while server selection mode is enabled.
SessionSnapshot raknet_session_select_textdraw(uint16_t wireId);

// Sends current open.mp-compatible OnPlayerSelectObject / RPC 27 only while
// the server has enabled object selection. Java supplies an ID and a copied
// native selection revision only; native resolves the current model and
// renderer-sampled origin from bounded state. Revision mismatch fails closed
// rather than letting a delayed list tap select under a newer Begin/Cancel.
SessionSnapshot raknet_session_select_object(uint16_t objectId, uint64_t expectedRevision);

// Sends an OnPlayerEditObject (RPC 117) reply only for the exact active server
// edit tuple. `response` is Cancel=0, Final=1, or Update=2; Java can never
// supply the global/player flag or object ID. Transform bounds, current object
// resolution, reliable ordered SyncRPC channel 2, and update cadence remain native.
// `expectedRevision` is a copied server revision, never an object identity.
// It prevents a delayed touch coroutine from applying an old transform to a
// newer BeginObjectEdit lifecycle that happened after the tap.
SessionSnapshot raknet_session_edit_object(uint32_t response, uint64_t expectedRevision,
                                           float x, float y, float z,
                                           float rotationX, float rotationY, float rotationZ);

// Sends an OnPlayerEditAttachedObject (RPC 116) reply only for the active
// server-owned attachment slot/revision. Java supplies offset/rotation/scale;
// native re-samples immutable model/bone/colours and emits one cancel/save
// reliable reply (response 0/1) under the session mutex.
SessionSnapshot raknet_session_edit_attached_object(
    uint32_t response, uint64_t expectedRevision,
    float offsetX, float offsetY, float offsetZ,
    float rotationX, float rotationY, float rotationZ,
    float scaleX, float scaleY, float scaleZ);

// Emits exactly one source-validated client PlayerFootSync packet only after
// the active session has PlayerInit, a server-approved spawn response, and a
// transport-ACKed PlayerSpawn confirmation. It uses UNRELIABLE_SEQUENCED on
// SyncPacket channel 1 and its own sequenced ordering counter, separate from
// the reliable SyncRPC stream, plus the server-advertised OnFootRate.
SessionSnapshot raknet_session_send_onfoot(const sync::OnFootState& state);

// Emits exactly one source-validated client PlayerVehicleSync / Packet 200
// only for the current server-authorized driver vehicle. It shares SyncPacket
// channel 1 with on-foot sync but has independent cadence state from
// PlayerInit InCarRate.
SessionSnapshot raknet_session_send_incar(const sync::InCarState& state);

// Advances the collision-grounded on-foot or driver-vehicle bridge once, then
// rate-gated-sends validated Packet-207/Packet-200 candidates. While on foot
// and holding semantic AIM it can additionally send gated Packet-203 using
// only grounded transform, native heading, and server-known weapon/ammo. A
// simultaneous FIRE may send only a rate-capped Packet-206 HitType=None
// attempt—never an Android-selected/entity target, hit/damage claim, or ammo
// mutation. A close read-only static COL contact may populate its absolute
// HitPos; it carries no entity ID/offset or local outcome. It refuses absent
// collision, server freezes, server
// velocity, incomplete spawn state, passengers,
// server-camera endpoints, and unknown/streamed-out vehicles instead of
// transmitting touch-only or fabricated state.
SessionSnapshot raknet_session_advance_gameplay(
    const gta::world::CollisionWorld& collisionWorld, float deltaSeconds,
    const vehiclemotion::VehicleMotionTuning* vehicleTuning = nullptr,
    uint32_t vehicleTuningModel = 0u);

// Returns the currently authoritative local driver vehicle model under the
// retained-session mutex, or zero when no streamed driver vehicle is eligible.
// It is only a lookup hint: advance_gameplay independently verifies the model
// before a user-data tuning can influence prediction.
uint32_t raknet_session_local_driver_vehicle_model();

// Sends a best-effort disconnect notification and releases the UDP socket.
SessionSnapshot raknet_session_disconnect();

// Returns the last retained session state without reading from the network.
SessionSnapshot raknet_session_status();

// Copies a renderer-neutral, bounded world/radar view without reading from the
// network. Callers must poll the session separately to receive fresh packets.
WorldSnapshot raknet_session_world_snapshot();

// Copies only validated RPC-43 static-world removals for a user-owned GTA map
// renderer. The vector is empty outside an active retained session and exposes
// no socket/world mutability to JNI.
std::vector<gta::removal::Directive> raknet_session_building_removals();

}  // namespace samp
