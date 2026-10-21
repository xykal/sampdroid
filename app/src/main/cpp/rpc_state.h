// Small, renderer-independent state reducer for decoded player/chat/marker server RPCs.
#pragma once

#include "gta_building_removal.h"
#include "rpc_decoder.h"
#include "sync_decoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace samp::state {

// The received ClientJoin identity selects the server-to-client layouts that
// are legal for this reducer. This is not a feature toggle inferred from an
// arbitrary RPC: only the connection handshake may construct a DL R1 state.
enum class ClientWireProfile : uint8_t {
  Legacy037,
  DlR1,
};

enum class RpcEventKind : uint8_t {
  Ignored,
  PlayerInitialized,
  PlayerJoined,
  PlayerQuit,
  PlayerRenamed,
  PlayerColorChanged,
  PlayerSkinChanged,
  NameTagChanged,
  PlayerStreamedIn,
  PlayerStreamedOut,
  VehicleStreamedIn,
  VehicleStreamedOut,
  VehicleChanged,
  ObjectCreated,
  ObjectChanged,
  ObjectDestroyed,
  ObjectSelectionChanged,
  ObjectEditChanged,
  AttachedObjectEditChanged,
  PlayerAttachmentChanged,
  ServerActorCreated,
  ServerActorChanged,
  ServerActorDestroyed,
  // DL R1 ModelInfo catalog entry retained only to resolve a safe stock/base
  // visual fallback for a custom object ID. It is never download permission.
  DlModelCatalogChanged,
  PickupCreated,
  PickupDestroyed,
  TextDrawChanged,
  TextDrawSelectionChanged,
  TextLabelChanged,
  MapIconChanged,
  GangZoneChanged,
  CameraChanged,
  GameTextChanged,
  RemotePlayerSync,
  RemoteAimSync,
  RemoteBulletSync,
  RemoteVehicleSync,
  RemoteTrailerSync,
  RemotePassengerSync,
  ChatMessage,
  // Global RPC-55 kill-feed presentation, separate from RPC-166 per-remote
  // temporary wasted marker and never local death/respawn authority.
  DeathMessageChanged,
  ChatBubbleChanged,
  MoneyChanged,
  ScoreboardChanged,
  ClassSelection,
  SpawnInfoChanged,
  SpawnResponse,
  // Exact PlayerClose (40) and bounded ConnectionRejected (130) are
  // server-owned session-terminal instructions. The transport owns the actual
  // socket close after this event.
  SessionCloseRequested,
  LocalPlayerChanged,
  WeaponStateChanged,
  WorldEnvironmentChanged,
  // RPC 43 changes server-authoritative visibility/collision filtering for
  // static user-owned GTA IPL placements. It is intentionally distinct from
  // dynamic CreateObject state.
  BuildingRemovalChanged,
  DialogChanged,
  MenuChanged,
  CheckpointChanged,
  RaceCheckpointChanged,
  Malformed,
};

struct RpcEvent {
  RpcEventKind kind = RpcEventKind::Ignored;
  uint16_t playerId = 0;
  // Lets the native input bridge honor an authoritative facing correction
  // without treating unrelated local-player RPCs as a camera reset.
  bool localFacingUpdated = false;
  // Set only by authoritative spawn/position RPCs. The gameplay bridge uses
  // it to discard predicted coordinates rather than snapping back on a health,
  // armour, special-action, or velocity-only update.
  bool localPositionUpdated = false;
  // A nonzero SetPlayerVelocity needs a dedicated physics bridge; discard a
  // pedestrian prediction rather than rendering/syncing against stale motion.
  bool localVelocityUpdated = false;
  // The collision world filters every query by IPL interior. A server interior
  // transition therefore invalidates a prediction initialized in another map,
  // while unrelated local cosmetic/vital changes do not.
  bool localInteriorUpdated = false;
  // RPC 124/126/127 changes whether the local client may emit gameplay sync
  // and which copied stream target presentation follows.
  bool localSpectateUpdated = false;
  // PlayerInit/RPC 146 can change vertical acceleration. A current airborne
  // prediction must be dropped rather than continue using stale gravity.
  bool worldGravityUpdated = false;
  // RPC 17 changes a recipient-specific exterior movement envelope. Any old
  // prediction may already lie outside the newly server-authored rectangle.
  bool worldBoundsUpdated = false;
  // Set only by authoritative PutPlayerInVehicle/RemovePlayerFromVehicle so
  // native input can reset the correct movement mode without conflating a
  // health/armour/local cosmetic update with an enter/exit transition.
  bool localVehicleUpdated = false;
  // RPC 91 has no vehicle ID: a valid command can target only the already
  // streamed vehicle currently assigned to this recipient as driver. This bit
  // lets the session discard a stale scalar driver prediction before its next
  // Packet-200 candidate; it is never set for a passenger/unoccupied context.
  bool localVehicleVelocityUpdated = false;
  // RPC 24 changes retained engine/lights/siren/etc. facts without changing an
  // authoritative vehicle transform. The session uses this to preserve a safe
  // current driver pilot (which will consume engine mode next tick) rather
  // than snapping its position for a cosmetic/server-control update.
  bool vehicleParametersUpdated = false;
  // RPC 91 Type: zero selects linear velocity, one selects angular velocity.
  // It is meaningful only when localVehicleVelocityUpdated is true.
  bool localVehicleVelocityAngular = false;
  std::string summary;
};

// SetPlayerChatBubble is server-owned transient presentation state. Its
// deadline is a local steady-clock sample only; no client clock is ever sent
// back to a server or used for gameplay authority.
struct ChatBubble {
  bool active = false;
  uint32_t rgba = 0;
  float drawDistance = 0.0f;
  uint64_t expiresAtMs = 0;
  std::string text;
};

// RPC 41/42 recipient audio directive. This is deliberately only retained
// server presentation state: it is not a download, media decoder, gameplay
// timer, position authority, or permission for native code to open a URL.
// Kotlin has a separate HTTPS-only/lifecycle-gated playback boundary.
struct AudioStream {
  bool active = false;
  uint64_t revision = 0;
  bool positional = false;
  std::string url;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float distance = 0.0f;
};

// Server-forced ApplyAnimation/ApplyActorAnimation directive. It is retained
// separately from Packet-207's numeric animation ID/flags because the RPC
// carries source library/name and timing. Until a GTA IFP/skeleton renderer
// exists, this remains a bounded presentation cue only: it never changes local
// physics, collision, or outbound movement authority.
struct AnimationDirective {
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

// Atomic per-player score/ping facts from the server's complete RPC-155
// response. Values are display data only and never drive lag compensation.
struct ScorePing {
  bool known = false;
  int32_t score = 0;
  uint32_t ping = 0;
};

struct Player {
  uint16_t id = 0;
  uint32_t rgba = 0;
  bool npc = false;
  std::string name;
  // Defaults visible; PlayerInit global policy and RPC 80 are applied later
  // by the renderer snapshot without treating nametags as gameplay authority.
  bool nameTagVisible = true;
  ChatBubble chatBubble;
  ScorePing scorePing;

  // Renderer-neutral remote actor facts. Transform fields are updated by a
  // valid StreamIn RPC or decoded server-to-recipient PlayerFootSync packet;
  // the model may also be replaced by the exact SetPlayerSkin RPC.
  bool streamedIn = false;
  bool positionKnown = false;
  bool remoteOnFootKnown = false;
  // Packet-203 is an ephemeral remote camera-direction presentation fact. It
  // never grants an Android client target, damage, or firing authority.
  bool remoteAimKnown = false;
  uint64_t remoteAimUpdatedMs = 0;
  uint8_t aimCameraMode = 0;
  uint8_t aimWeaponState = 0;
  float aimCameraFrontX = 0.0f;
  float aimCameraFrontY = 0.0f;
  float aimCameraFrontZ = 0.0f;
  float aimCameraPositionX = 0.0f;
  float aimCameraPositionY = 0.0f;
  float aimCameraPositionZ = 0.0f;
  // A server-relayed Packet-206 cue lasts only briefly and retains no hit ID
  // or damage state in this client. It is visual feedback for another player's
  // already-broadcast shot, never local firing authority.
  bool remoteBulletKnown = false;
  uint64_t remoteBulletUpdatedMs = 0;
  uint8_t bulletWeaponId = 0;
  // RPC 166 is an observer-side death notice. It never proves local death;
  // retain a short remote-only wasted affordance rather than a fake respawn.
  bool remoteWastedKnown = false;
  uint64_t remoteWastedUpdatedMs = 0;
  float bulletOriginX = 0.0f;
  float bulletOriginY = 0.0f;
  float bulletOriginZ = 0.0f;
  float bulletDirectionX = 0.0f;
  float bulletDirectionY = 0.0f;
  float bulletDirectionZ = 0.0f;
  // Packet-200 identifies a driver and Packet-211 identifies a passenger.
  // This is presentation state only; no seat offset is invented by the client.
  bool remoteVehicleKnown = false;
  bool remoteVehicleDriver = false;
  uint16_t remoteVehicleId = 0;
  uint8_t remoteVehicleSeat = 0;
  uint8_t team = 0;
  // `model` remains the server-provided stock/base GTA model. DL R1 supplies
  // its optional custom-model reference separately, so a renderer can use a
  // lawful stock fallback without losing the source identity.
  uint32_t model = 0;
  uint32_t customModel = 0;
  uint8_t fightingStyle = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float quaternionW = 1.0f;
  float quaternionX = 0.0f;
  float quaternionY = 0.0f;
  float quaternionZ = 0.0f;
  float health = 100.0f;
  float armour = 0.0f;
  uint8_t weaponAdditionalKey = 0;
  uint8_t specialAction = 0;
  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;
  bool animationKnown = false;
  uint16_t animationId = 0;
  uint16_t animationFlags = 0;
  AnimationDirective forcedAnimation;
};

// Current open.mp/SA-MP SetPlayerAttachedObject uses exactly ten per-player
// slots (0..9). The state retains source transforms/colors, but a renderer only
// gets a sampled visual proxy; it must not infer a ped skeleton, collision, or
// player ownership from one accessory.
constexpr size_t kMaxPlayerAttachedObjectSlots = 10;
struct PlayerAttachedObject {
  bool active = false;
  // `model` is a renderable GTA stock/base fallback. `sourceModel` preserves
  // a signed DL custom object reference when the wire model is negative.
  uint32_t model = 0;
  int32_t sourceModel = 0;
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
  // Writer uses ABGR for these API-facing ARGB colours.
  uint32_t colour1Abgr = 0xFFFFFFFFu;
  uint32_t colour2Abgr = 0xFFFFFFFFu;
};

struct PlayerAttachedObjectView {
  uint16_t playerId = 0;
  uint8_t slot = 0;
  uint32_t model = 0;
  int32_t sourceModel = 0;
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

// Bounded copy handed to a renderer/HUD boundary. It intentionally contains
// no mutable map/socket state and is ordered by player ID when exported.
struct RemotePlayerView {
  uint16_t id = 0;
  uint32_t rgba = 0;
  uint32_t model = 0;
  uint32_t customModel = 0;
  bool onFootKnown = false;
  // Bounded, short-lived remote Packet-203 presentation data. No recipient
  // code may reinterpret this as a hit target or projectile endpoint.
  bool aimKnown = false;
  uint8_t aimCameraMode = 0;
  uint8_t aimWeaponState = 0;
  float aimCameraFrontX = 0.0f;
  float aimCameraFrontY = 0.0f;
  float aimCameraFrontZ = 0.0f;
  float aimCameraPositionX = 0.0f;
  float aimCameraPositionY = 0.0f;
  float aimCameraPositionZ = 0.0f;
  // Server-relayed visual shot direction only. No hit ID/type crosses this
  // renderer boundary, preventing it from being mistaken for damage authority.
  bool bulletKnown = false;
  uint8_t bulletWeaponId = 0;
  float bulletOriginX = 0.0f;
  float bulletOriginY = 0.0f;
  float bulletOriginZ = 0.0f;
  float bulletDirectionX = 0.0f;
  float bulletDirectionY = 0.0f;
  float bulletDirectionZ = 0.0f;
  // Brief server-observer death presentation only, never local death authority.
  bool wasted = false;
  AnimationDirective forcedAnimation;
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

// Bounded full-roster display copy. Unlike renderable players, it may include
// joined players outside stream range and deliberately has no world transform.
struct ScoreboardPlayerView {
  uint16_t id = 0;
  uint32_t rgba = 0;
  bool npc = false;
  int32_t score = 0;
  uint32_t ping = 0;
  std::string name;
};

// RPC 24 carries one signed int8 state per legacy vehicle parameter. `-1`
// means that field is explicitly unset in the server's full VehicleParams
// snapshot; 0/1 are the other accepted values. Keep the full parameter set
// source-owned even though the current Android presentation/pilot consumes
// only engine, lights, doors, objective, and siren.
constexpr size_t kVehicleParameterCount = 16;
enum class VehicleParameter : uint8_t {
  Engine = 0,
  Lights,
  Alarm,
  Doors,
  Bonnet,
  Boot,
  Objective,
  Siren,
  DoorDriver,
  DoorPassenger,
  DoorBackLeft,
  DoorBackRight,
  WindowDriver,
  WindowPassenger,
  WindowBackLeft,
  WindowBackRight,
};

struct VehicleParameters {
  bool known = false;
  std::array<int8_t, kVehicleParameterCount> values{
      -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1};

  int8_t value(VehicleParameter parameter) const {
    return values[static_cast<size_t>(parameter)];
  }
};

// Server-to-client vehicle facts. StreamInVehicle establishes identity and its
// initial transform; Packet 200/209 may replace that transform only with a
// decoded authoritative update. The renderer never invents in-between motion.
struct Vehicle {
  uint16_t id = 0;
  bool streamedIn = false;
  bool dynamicSyncKnown = false;
  // Driver ID 0 is valid on legacy servers, so never use the numeric ID as
  // an occupancy sentinel; driverKnown tracks Packet-200 vs Packet-209.
  bool driverKnown = false;
  uint16_t driverId = 0;
  uint32_t model = 0;
  uint8_t colour1 = 0;
  uint8_t colour2 = 0;
  uint8_t interior = 0;
  uint8_t paintjob = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  // Source Packet-200 only: decompressed full rigid-body attitude for the DFF
  // renderer. StreamIn/RPC-160 retain yaw-only fallback until a driver sync.
  bool attitudeKnown = false;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  // Exact recipient-local SetVehicleParams (RPC 24) state. It never grants
  // Android a local engine/lights/door toggle or vehicle ownership.
  VehicleParameters parameters;
  float health = 1000.0f;
  // RPC 91 is recipient-local and implicit: it has no VehicleID. Keep its
  // independently selected linear and angular CVector values on the exact
  // streamed, seat-zero local vehicle only. Revision counters make a direct
  // command observable exactly once by a later bounded driver bridge; neither
  // channel is guessed from Packet-200 or applied to another vehicle.
  bool linearVelocityKnown = false;
  bool angularVelocityKnown = false;
  uint64_t linearVelocityRevision = 0;
  uint64_t angularVelocityRevision = 0;
  float linearVelocityX = 0.0f;
  float linearVelocityY = 0.0f;
  float linearVelocityZ = 0.0f;
  float angularVelocityX = 0.0f;
  float angularVelocityY = 0.0f;
  float angularVelocityZ = 0.0f;
};

// Recipient-local door state is the only vehicle-parameter fact that can block
// Android from manufacturing an EnterVehicle request. Absent/unset (including
// an all-unset full RPC-24 snapshot) is deliberately permissive; server RPC 70
// remains the only seat authority either way.
inline bool isRecipientVehicleEntryLocked(const Vehicle& vehicle) {
  return vehicle.parameters.known &&
         vehicle.parameters.value(VehicleParameter::Doors) == 1;
}

struct RemoteVehicleView {
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
  bool attitudeKnown = false;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  VehicleParameters parameters;
  float health = 1000.0f;
};

// Per-recipient NPC actor established only by ShowActorForPlayer (171). Its
// transform/model are visual server facts, never a player-pool identity, hit
// target, collision obstacle, or local animation simulation.
struct ServerActor {
  uint16_t id = 0;
  uint32_t model = 0;
  uint32_t customModel = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float health = 100.0f;
  bool invulnerable = false;
  AnimationDirective forcedAnimation;
};

struct ServerActorView {
  uint16_t id = 0;
  uint32_t model = 0;
  uint32_t customModel = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float health = 100.0f;
  bool invulnerable = false;
  AnimationDirective forcedAnimation;
};

enum class ObjectAttachmentKind : uint8_t {
  None,
  Vehicle,
  Object,
  Player,
};

constexpr size_t kMaxServerObjectMaterialSlots = 16;

// One portable material replacement. Current open.mp ObjectMaterialData uses
// Default source-model/colour fields or Text fields. Default colour is ABGR on
// the wire (API callers provide ARGB before the server writer); text colours
// remain source ARGB. Each slot is exactly one variant, so a Text update clears
// a prior Default texture record rather than layering two server materials.
struct ServerObjectMaterialOverride {
  // Default texture/material replacement.
  bool active = false;
  uint16_t sourceModel = 0;
  // Zero is the documented preserve-existing-material-colour sentinel.
  uint32_t colourAbgr = 0xFFFFFFFFu;
  std::string textureDictionary;
  std::string textureName;

  // Text material retained independently of a Default lookup. `textActive`
  // is true only for the source SDK's Text=2 record; dimensions are the exact
  // ObjectMaterialSize enum (`10..140`), alignment is `0..2`, and text/font
  // strings are bounded by the writer-backed HybridString<32> capacity.
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

// Server-created world object. It retains validated static transforms, a
// bounded local-time MoveObject interpolation, plus source-writer attachment
// facts. Frame export resolves a bounded vehicle/object/player parent chain
// using the transform facts the client actually has and carries bounded
// Default texture or Text material overrides per source slot.
struct ServerObject {
  uint16_t id = 0;
  // For DL custom objects, sourceModel is the signed wire ID and model is the
  // resolved stock/base fallback (zero means catalog info is not known yet).
  uint32_t model = 0;
  int32_t sourceModel = 0;
  bool attached = false;
  ObjectAttachmentKind attachmentKind = ObjectAttachmentKind::None;
  uint16_t attachmentParentId = 0;
  float attachmentOffsetX = 0.0f;
  float attachmentOffsetY = 0.0f;
  float attachmentOffsetZ = 0.0f;
  float attachmentRotationX = 0.0f;
  float attachmentRotationY = 0.0f;
  float attachmentRotationZ = 0.0f;
  bool attachmentSyncRotation = false;
  std::array<ServerObjectMaterialOverride, kMaxServerObjectMaterialSlots> materials{};
  bool moving = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float drawDistance = 0.0f;
  // Monotonic client-side interpolation facts from MoveObject. They are never
  // sent back to a server and are cancelled by Set/Stop/Destroy lifecycle RPCs.
  uint64_t moveStartedMs = 0;
  float moveDurationSeconds = 0.0f;
  float moveTargetX = 0.0f;
  float moveTargetY = 0.0f;
  float moveTargetZ = 0.0f;
  bool moveHasTargetRotation = false;
  float moveTargetRotationX = 0.0f;
  float moveTargetRotationY = 0.0f;
  float moveTargetRotationZ = 0.0f;
};

// Server-owned ObjectEdit lifecycle. The begin RPC identifies whether this
// recipient is editing a global or a player object; IDs still share its visible
// object pool. Revision lets Android reset a stale local editor when a server
// starts another edit for the same numeric ID.
struct ObjectEditState {
  bool active = false;
  bool playerObject = false;
  uint16_t objectId = 0xFFFFu;
  uint64_t revision = 0;
};

// Server-owned player-attached-object edit lifecycle. The slot is the existing
// local recipient's 0..9 attachment namespace, not a global object ID. Native
// re-samples its model/bone/colours before sending a single save/cancel reply.
struct AttachedObjectEditState {
  bool active = false;
  uint8_t slot = 0;
  uint64_t revision = 0;
};

struct ServerObjectView {
  uint16_t id = 0;
  uint32_t model = 0;
  int32_t sourceModel = 0;
  bool moving = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float drawDistance = 0.0f;
  std::array<ServerObjectMaterialOverride, kMaxServerObjectMaterialSlots> materials{};
};

// An optional, copied local prediction anchor supplied only by the retained
// session when it already has a collision-grounded transform. It lets a
// renderer-only AttachObjectToPlayer child follow the same local visual frame
// without mutating WorldState's server authority.
struct LocalObjectAttachmentAnchor {
  bool valid = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
};

// CreatePickup has no global/player or interior discriminator on the wire.
// Current open.mp maps each stream-visible pickup to one client-local ID before
// sending it, so this is a bounded recipient record; it is never local ownership
// or a fabricated world/interior assertion.
struct Pickup {
  int32_t id = 0;
  int32_t model = 0;
  int32_t spawnType = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct PickupView {
  int32_t id = 0;
  int32_t model = 0;
  int32_t spawnType = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

// Server-authored textdraw facts are retained separately from Android font
// rendering. Raw wire IDs preserve the global (0..2047) / player (2048..2303)
// namespace split, so a selection response cannot silently target another draw.
struct TextDraw {
  uint16_t wireId = 0;
  bool playerTextDraw = false;
  bool useBox = false;
  uint8_t alignment = 0;
  bool proportional = true;
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
  uint16_t model = 0;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float zoom = 0.0f;
  int16_t colour1 = 0;
  int16_t colour2 = 0;
  std::string text;
};

struct TextDrawView {
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
  uint16_t model = 0;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float zoom = 0.0f;
  int16_t colour1 = 0;
  int16_t colour2 = 0;
  std::string text;
};

// Server 3D labels use a separate 1024-global + 1024-player wire namespace.
// `position` is the world position for unattached labels, or the authoritative
// offset from PlayerAttachID/VehicleAttachID for attached labels. LOS remains
// a renderer policy fact until native world collision can evaluate it.
struct TextLabel {
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

struct TextLabelView {
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

// Per-player SetPlayerMapIcon slot. Type/style are retained as raw compatible
// values so Android can render a portable marker without pretending it has all
// GTA radar sprite assets.
struct MapIcon {
  uint8_t id = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint8_t type = 0;
  uint32_t rgba = 0;
  uint8_t style = 0;
};

struct MapIconView {
  uint8_t id = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint8_t type = 0;
  uint32_t rgba = 0;
  uint8_t style = 0;
};

struct ChatLine {
  uint32_t rgba = 0;
  std::string text;
};

// Exact global SendDeathMessage (RPC 55) fact. `hasKiller=false` corresponds
// to INVALID_PLAYER_ID on wire; reason remains a raw server weapon/death code.
// It is intentionally presentation-only and has no local health/respawn effect.
struct DeathMessage {
  bool hasKiller = false;
  uint16_t killerId = 0xFFFFu;
  uint16_t victimId = 0xFFFFu;
  uint8_t reason = 0;
  uint64_t expiresAtMs = 0;
};

struct WorldEnvironment {
  // PlayerInit/SetWorldTime global hour. A later recipient-specific
  // SetPlayerTime can override only the local visual clock/environment sample.
  bool timeKnown = false;
  bool playerTimeKnown = false;
  bool playerClockKnown = false;
  bool playerClockEnabled = true;
  bool weatherKnown = false;
  // PlayerInit and RPC 146 use GTA gravity units (the usual server default is
  // 0.008). It is separate from weather/time and is consumed only by the
  // bounded on-foot predictor after a supported-value gate.
  bool gravityKnown = false;
  // Recipient-specific RPC 17 bounds apply only to exterior movement. They are
  // retained as maxX/minX/maxY/minY exactly as the compatible server writer
  // orders its Vector4; a missing RPC leaves local prediction unconstrained.
  bool worldBoundsKnown = false;
  uint8_t worldTime = 12;
  uint8_t playerHour = 12;
  uint8_t playerMinute = 0;
  uint8_t weather = 0;
  float gravity = 0.008f;
  float worldMaxX = 20000.0f;
  float worldMinX = -20000.0f;
  float worldMaxY = 20000.0f;
  float worldMinY = -20000.0f;
};

// Server-authoritative GameText/RPC 73 facts. Android renders only a portable
// text banner proxy; the original GTA timed style/font effects are not claimed.
struct GameText {
  bool active = false;
  uint64_t revision = 0;
  uint32_t style = 0;
  uint32_t durationMs = 0;
  uint64_t startedAtMs = 0;
  std::string text;
};

// A bounded server-authored dialog view. It contains only decoded UI text;
// response transport remains in the retained session layer.
struct Dialog {
  bool active = false;
  uint64_t revision = 0;
  int16_t id = -1;
  uint8_t style = 0;
  std::string title;
  std::string button1;
  std::string button2;
  std::string body;
};

// Legacy menu facts decoded from the current open.mp PlayerInitMenu writer.
// The renderer gets a copied active view only; selection remains revision-bound
// and reliable in the retained session layer. Fixed source fields are bounded
// to 32 bytes (including their terminal NUL) and 12 rows per column.
constexpr size_t kMaxMenuItems = 12;
struct Menu {
  bool active = false;
  uint64_t revision = 0;
  uint8_t id = 0;
  bool twoColumns = false;
  bool enabled = true;
  std::array<bool, kMaxMenuItems> rowEnabled{};
  std::string title;
  std::string column1Header;
  std::string column2Header;
  std::array<std::string, kMaxMenuItems> column1Items{};
  std::array<std::string, kMaxMenuItems> column2Items{};
  uint8_t column1Count = 0;
  uint8_t column2Count = 0;
};

struct Checkpoint {
  bool active = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float size = 0.0f;
};

struct RaceCheckpoint {
  bool active = false;
  uint8_t type = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float nextX = 0.0f;
  float nextY = 0.0f;
  float nextZ = 0.0f;
  float size = 0.0f;
};

// Per-recipient GangZone view. The legacy RPC only exposes visible zones, so
// hiding one removes it from this session-local render state rather than
// inventing a global zone lifecycle the client cannot observe.
struct GangZone {
  uint16_t id = 0;
  float minX = 0.0f;
  float minY = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;
  uint32_t colourAbgr = 0;
  bool flashing = false;
  uint32_t flashColourAbgr = 0;
};

struct GangZoneView {
  uint16_t id = 0;
  float minX = 0.0f;
  float minY = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;
  uint32_t colourAbgr = 0;
  bool flashing = false;
  uint32_t flashColourAbgr = 0;
};

// Server camera RPCs keep their source/target endpoints so an immutable world
// snapshot can sample a bounded interpolation without leaking mutable timing
// state to Android. `known` distinguishes a zero coordinate from no server
// camera override, and SetCameraBehindPlayer clears both endpoints.
struct CameraEndpoint {
  bool known = false;
  bool interpolating = false;
  uint64_t startedAtMs = 0;
  uint32_t durationMs = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float fromX = 0.0f;
  float fromY = 0.0f;
  float fromZ = 0.0f;
  float toX = 0.0f;
  float toY = 0.0f;
  float toZ = 0.0f;
};

struct CameraState {
  CameraEndpoint position;
  CameraEndpoint lookAt;
};

struct CameraView {
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

struct ClassSelection {
  bool valid = false;
  bool selectable = false;
  uint8_t team = 0;
  uint32_t model = 0;
  uint32_t customModel = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  std::array<uint32_t, 3> weapons{};
  std::array<uint32_t, 3> ammo{};
};

// Bounded local-player facts supplied by authoritative server RPCs. This is
// intentionally renderer-neutral: the Android world/input bridge consumes it
// later, while the network reducer owns validation and lifetime.
struct LocalPlayer {
  bool spawnInfoKnown = false;
  bool positionKnown = false;
  bool positionFindZ = false;
  bool facingKnown = false;
  bool healthKnown = false;
  bool armourKnown = false;
  bool moneyKnown = false;
  // Exact server wanted-level presentation (0..6); it has no local police,
  // wanted-star simulation, reward, or gameplay consequence.
  bool wantedLevelKnown = false;
  bool velocityKnown = false;
  bool specialActionKnown = false;
  bool controllableKnown = false;
  bool controllable = true;
  bool interiorKnown = false;
  // Spectator lifecycle is server-owned (RPC 124/126/127). It never turns the
  // local player into the target, and blocks locally-authored gameplay sync.
  bool spectatingKnown = false;
  bool spectating = false;
  bool spectateTargetKnown = false;
  bool spectateTargetVehicle = false;
  // RPC 48 is a server routing/presentation fact. Virtual worlds do not map
  // directly to GTA IPL interiors, so this value never re-partitions collision
  // or hides world geometry by a guessed local rule.
  bool virtualWorldKnown = false;
  // Authoritative PutPlayerInVehicle/RemovePlayerFromVehicle lifecycle only.
  // It is deliberately not a local driving/physics claim.
  bool vehicleKnown = false;
  bool inVehicle = false;
  ChatBubble chatBubble;
  ScorePing scorePing;

  uint8_t team = 0;
  uint8_t interior = 0;
  uint8_t wantedLevel = 0;
  int32_t virtualWorld = 0;
  uint16_t spectateTargetId = 0;
  // Retained raw `PlayerSpectateMode` byte for presentation only. The native
  // scene does not invent PC camera offsets for a mode it cannot reproduce.
  uint8_t spectateMode = 0;
  uint16_t vehicleId = 0;
  uint8_t vehicleSeat = 0;
  uint32_t model = 0;
  uint32_t customModel = 0;
  uint8_t spawnUnknown = 0;
  std::array<uint32_t, 3> spawnWeapons{};
  std::array<uint32_t, 3> spawnAmmo{};

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float health = 100.0f;
  float armour = 0.0f;
  // Money is maintained from exact Give/ResetPlayerMoney server RPCs. It is
  // display state only and is never spent/granted by an Android action.
  int64_t money = 0;
  // Server-owned weapon facts. GivePlayerWeapon can refresh ammo for a
  // weapon without selecting it; only SetPlayerArmedWeapon sets active state.
  // IDs are retained directly (0..46), never mapped through guessed slots.
  bool armedWeaponKnown = false;
  uint8_t armedWeapon = 0;
  std::array<uint32_t, 47> weaponAmmo{};
  std::array<bool, 47> weaponAmmoKnown{};
  uint8_t specialAction = 0;
  AnimationDirective forcedAnimation;
  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;
};

// This has no rendering or socket responsibility. It is the stable boundary
// between untrusted network bytes and a future HUD/world renderer.
class WorldState {
public:
  explicit WorldState(ClientWireProfile wireProfile = ClientWireProfile::Legacy037)
      : wireProfile_(wireProfile) {}

  // A terminal server lifecycle clears mutable world facts while retaining the
  // profile selected at ClientJoin; an active DL peer must never silently fall
  // back to legacy decoding after a disconnect/reject signal.
  void resetForSession() {
    const ClientWireProfile retainedProfile = wireProfile_;
    *this = WorldState(retainedProfile);
  }
  ClientWireProfile wireProfile() const { return wireProfile_; }
  bool usesDlR1WireLayout() const { return wireProfile_ == ClientWireProfile::DlR1; }

  bool applyRpc(const std::vector<uint8_t>& rpcPayload, RpcEvent& event);
  bool applyRemoteOnFoot(const syncdecode::RemoteOnFootSync& sync, RpcEvent& event);
  bool applyRemoteAim(const syncdecode::RemoteAimSync& sync, RpcEvent& event);
  bool applyRemoteBullet(const syncdecode::RemoteBulletSync& sync, RpcEvent& event);
  bool applyRemoteVehicle(const syncdecode::RemoteVehicleSync& sync, RpcEvent& event);
  bool applyRemoteTrailer(const syncdecode::RemoteTrailerSync& sync, RpcEvent& event);
  bool applyRemotePassenger(const syncdecode::RemotePassengerSync& sync, RpcEvent& event);

  bool initialized() const { return initialized_; }
  const rpc::PlayerInitData& playerInit() const { return playerInit_; }
  const WorldEnvironment& environment() const { return environment_; }
  // Copy-free immutable list used while the retained session mutex is held by
  // gameplay/render bridges. Entries are bounded and validated at RPC ingress.
  const std::vector<gta::removal::Directive>& buildingRemovals() const {
    return buildingRemovals_;
  }
  const AudioStream& audioStream() const { return audioStream_; }
  GameText gameText() const;
  const Dialog& dialog() const { return dialog_; }
  const Menu& menu() const { return activeMenu_; }
  size_t playerCount() const { return players_.size(); }
  size_t streamedPlayerCount() const;
  size_t remoteOnFootCount() const;
  size_t vehicleCount() const { return vehicles_.size(); }
  size_t streamedVehicleCount() const;
  size_t serverActorCount() const { return serverActors_.size(); }
  size_t serverObjectCount() const { return objects_.size(); }
  // A bounded passive catalog of DL custom-object → stock/base mappings.
  // This has no URL, file, byte, or download-completion state.
  size_t dlObjectFallbackCount() const { return dlObjectBaseModels_.size(); }
  size_t pickupCount() const { return pickups_.size(); }
  size_t textDrawCount() const { return textDraws_.size(); }
  size_t textLabelCount() const { return textLabels_.size(); }
  size_t mapIconCount() const { return mapIcons_.size(); }
  size_t gangZoneCount() const { return gangZones_.size(); }
  // Samples server SetPlayerCamera*/InterpolateCamera state at call time. It
  // is read-only and never changes local gameplay/input authority.
  CameraView camera() const;
  bool textDrawSelectionActive() const { return textDrawSelectionActive_; }
  uint32_t textDrawSelectionColourRgba() const { return textDrawSelectionColourRgba_; }
  bool isSelectableTextDraw(uint16_t wireId) const;
  // RPC 27 is a server-owned object-selection mode. An Android choice is
  // resolved back against this reducer immediately before sending so Java can
  // never nominate a raw model, coordinate, or stale object lifecycle record.
  bool objectSelectionActive() const { return objectSelectionActive_; }
  // A bodyless BeginObjectSelect still advances this native revision. It is not
  // a target identity; it only rejects a delayed Android list tap after the
  // server begins/cancels/replaces selection mode.
  uint64_t objectSelectionRevision() const { return objectSelectionRevision_; }
  bool selectedObjectView(uint16_t objectId, ServerObjectView& out) const;
  // RPC 117 is a distinct server-owned edit lifecycle. Its object kind/ID
  // tuple is rechecked again by the session immediately before an edit reply.
  const ObjectEditState& objectEdit() const { return objectEdit_; }
  bool editedObjectView(uint16_t objectId, bool playerObject, ServerObjectView& out) const;
  // RPC 116 targets an existing local-player attachment slot. Copying it into
  // a caller-owned value avoids exposing mutable map storage across sessions.
  const AttachedObjectEditState& attachedObjectEdit() const { return attachedObjectEdit_; }
  bool editedAttachedObject(uint8_t slot, PlayerAttachedObject& out) const;
  // Copies only non-attached objects. Moving global objects are sampled with
  // their bounded local-time facts; attachment hierarchy is never invented.
  std::vector<ServerObjectView> renderableObjects(
      size_t maxObjects = 128,
      const LocalObjectAttachmentAnchor* localAttachmentAnchor = nullptr) const;
  // Samples only an existing local/streamed parent. Bone placement is a
  // renderer heuristic, not a ped-skeleton or collision authority.
  std::vector<PlayerAttachedObjectView> renderablePlayerAttachedObjects(
      size_t maxAttachments = 64,
      const LocalObjectAttachmentAnchor* localAttachmentAnchor = nullptr) const;
  std::vector<PickupView> renderablePickups(size_t maxPickups = 128) const;
  std::vector<TextDrawView> renderableTextDraws(size_t maxTextDraws = 32) const;
  // Text labels are copied to Android after StringCompressor decoding. They
  // retain attachment IDs but no mutable player/vehicle pointers.
  std::vector<TextLabelView> renderableTextLabels(size_t maxTextLabels = 64) const;
  // Map-icon IDs are fixed slots, exported in deterministic ascending order.
  std::vector<MapIconView> renderableMapIcons(size_t maxMapIcons = 64) const;
  // Gang zones are already server-filtered for this player. Export a bounded,
  // deterministic ID-order list for a radar-compatible renderer.
  std::vector<GangZoneView> renderableGangZones(size_t maxGangZones = 64) const;
  // Returns the nearest known pickup within a caller-provided interaction
  // radius. It does not imply server permission or send anything itself.
  const Pickup* nearestPickup(float x, float y, float z, float radius) const;
  // Copies at most `maxPlayers` visible remote players for the renderer/HUD.
  std::vector<RemotePlayerView> renderablePlayers(size_t maxPlayers = 64) const;
  // Copies stable ID-ordered server NPC actor facts from RPC 171/172/175/176/
  // 178. This deliberately remains distinct from the player-pool snapshot.
  std::vector<ServerActorView> renderableServerActors(size_t maxActors = 64) const;
  // Copies a score/ping roster only after an exact RPC-155 response. This is
  // intentionally separate from stream range and has no gameplay authority.
  std::vector<ScoreboardPlayerView> scoreboardPlayers(size_t maxPlayers = 64) const;
  // Fixed cap/order protects the scene backend from a forged vehicle stream.
  std::vector<RemoteVehicleView> renderableVehicles(size_t maxVehicles = 64) const;
  // Picks only a streamed, known vehicle within the caller's interaction
  // radius. It has no authority to seat or locally simulate a player.
  const Vehicle* nearestVehicle(float x, float y, float z, float radius,
                                uint8_t localInterior, bool interiorKnown) const;
  // Direct bounded lookup for server-authorized local driver simulation. This
  // exposes immutable vehicle facts only; it cannot create/stream a vehicle.
  const Vehicle* findVehicle(uint16_t id) const;
  // Server-streamed vehicle transform used only as seated render/camera anchor.
  // It intentionally gives no seat offset or outbound passenger authority.
  const Vehicle* localVehiclePresentationAnchor() const;
  const Player* findPlayer(uint16_t id) const;
  size_t chatCount() const { return chatLines_.size(); }
  const ChatLine* latestChat() const {
    return chatLines_.empty() ? nullptr : &chatLines_.back();
  }
  // Returns only the newest bounded tail in chronological order. This isolates
  // the renderer/HUD from the larger retained diagnostic history.
  std::vector<ChatLine> recentChat(size_t maxLines = 6) const;
  // Returns current, unexpired global death-feed entries in chronological order.
  // The renderer receives copies only; no death message changes local gameplay.
  std::vector<DeathMessage> recentDeathMessages(size_t maxMessages = 5) const;
  const ClassSelection& classSelection() const { return classSelection_; }
  // True only after the server explicitly returned this client to class
  // selection; it clears when the user requests/receives a new class view.
  bool classSelectionForced() const { return classSelectionForced_; }
  const LocalPlayer& localPlayer() const { return localPlayer_; }
  // Samples a server bubble using local steady-clock time only; it never
  // contributes to local input or network authority.
  bool hasActiveLocalChatBubble() const;
  const Checkpoint& checkpoint() const { return checkpoint_; }
  const RaceCheckpoint& raceCheckpoint() const { return raceCheckpoint_; }
  bool spawnResponseKnown() const { return spawnResponseKnown_; }
  bool spawnAllowed() const { return spawnAllowed_; }

  // Called only after an outbound PlayerRequestClass is queued successfully.
  // It makes a stale class/spawn permission unusable while the server replies.
  void beginClassRequest();
  // Called only after an outbound PlayerRequestSpawn is queued successfully;
  // stale permission from a prior attempt must not unlock local movement.
  void beginSpawnRequest();

  // Called only after a matching dialog response is successfully queued on
  // the reliable session. Matching the revision as well as the server-visible
  // ID prevents a delayed Android dialog from acknowledging a newer dialog
  // that reused the same ID.
  bool acknowledgeDialogResponse(int16_t id, uint64_t revision);
  // Menu responses are similarly bound to the exact active server revision.
  // Selecting a disabled/nonexistent row is refused before state/UI changes.
  bool acknowledgeMenuSelection(uint8_t id, uint64_t revision, uint8_t row);
  bool acknowledgeMenuExit(uint8_t id, uint64_t revision);

private:
  // SA-MP/open.mp player IDs are small in normal use; bound state regardless
  // so an untrusted peer cannot grow the model indefinitely with fake joins.
  Player* getOrCreate(uint16_t id);
  Vehicle* getOrCreateVehicle(uint16_t id);
  ServerActor* getOrCreateServerActor(uint16_t id);
  ServerObject* getOrCreateObject(uint16_t id);
  // Removes a driver's slot as well as this actor's occupancy. `hideActor`
  // avoids reviving a stale foot pose while a newer vehicle/seat packet wins.
  void clearRemoteVehicleOccupancy(uint16_t playerId, bool hideActor);
  Pickup* getOrCreatePickup(int32_t id);

  ClientWireProfile wireProfile_ = ClientWireProfile::Legacy037;
  bool initialized_ = false;
  rpc::PlayerInitData playerInit_;
  WorldEnvironment environment_;
  std::vector<gta::removal::Directive> buildingRemovals_;
  AudioStream audioStream_;
  GameText gameText_;
  Dialog dialog_;
  // At most the legacy 128 menu IDs can be defined. `activeMenu_` is a copied
  // presentation snapshot, so later InitMenu updates cannot mutate UI under a
  // user tap without an explicit Show/revision transition.
  std::map<uint8_t, Menu> menus_;
  Menu activeMenu_;
  ClassSelection classSelection_;
  bool classSelectionForced_ = false;
  LocalPlayer localPlayer_;
  Checkpoint checkpoint_;
  RaceCheckpoint raceCheckpoint_;
  CameraState camera_;
  bool spawnResponseKnown_ = false;
  bool spawnAllowed_ = false;
  std::map<uint16_t, Player> players_;
  // Per-recipient RPC 80 may arrive before Join/StreamIn. Keep a bounded ID
  // override that getOrCreate applies once the remote player becomes known.
  std::map<uint16_t, bool> nameTagVisibilityOverrides_;
  // Score snapshots can arrive before PlayerJoin; retain at most one bounded
  // score/ping fact per ID until identity is announced.
  std::map<uint16_t, ScorePing> scorePingOverrides_;
  std::map<uint16_t, Vehicle> vehicles_;
  std::map<uint16_t, ServerActor> serverActors_;
  // Signed DL custom object IDs map to source-supplied GTA base IDs only. The
  // map deliberately carries no DFF/TXD URL/checksum/file payload.
  std::map<int32_t, uint32_t> dlObjectBaseModels_;
  std::map<uint16_t, ServerObject> objects_;
  // One array only for the local player or an already-known player ID; reducer
  // never creates a player identity from a SetPlayerAttachedObject RPC.
  std::map<uint16_t, std::array<PlayerAttachedObject, kMaxPlayerAttachedObjectSlots>>
      playerAttachedObjects_;
  std::map<int32_t, Pickup> pickups_;
  std::map<uint16_t, TextDraw> textDraws_;
  std::map<uint16_t, TextLabel> textLabels_;
  std::map<uint8_t, MapIcon> mapIcons_;
  std::map<uint16_t, GangZone> gangZones_;
  bool textDrawSelectionActive_ = false;
  uint32_t textDrawSelectionColourRgba_ = 0;
  // Set only by PlayerBeginObjectSelect (RPC 27) and cleared by the current
  // source writer's PlayerCancelObjectEdit (RPC 28). It gates all outbound
  // Android object-selection attempts.
  bool objectSelectionActive_ = false;
  uint64_t objectSelectionRevision_ = 0;
  // Set only by PlayerBeginObjectEdit (RPC 117) and the paired cancel RPC 28.
  // It does not mutate an object transform locally; native outbound responses
  // remain an explicit server-authorized editing interaction.
  ObjectEditState objectEdit_;
  // RPC 116 has a distinct mode from RPC 117 ObjectEdit; RPC 28 clears both.
  AttachedObjectEditState attachedObjectEdit_;
  std::deque<ChatLine> chatLines_;
  std::deque<DeathMessage> deathMessages_;
};

}  // namespace samp::state
