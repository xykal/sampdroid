# Tabel ID RPC SA-MP / open.mp

Diekstrak langsung dari header `Shared/NetCode/*.hpp` di
[openmultiplayer/open.mp](https://github.com/openmultiplayer/open.mp)
(`NetworkPacketBase<ID, NetworkPacketType::RPC, OrderingChannel_SyncRPC>`).

Format wire RPC legacy:

```
[0x14 = ID_RPC][rpc id (u8)][payload bit-packed]
```

Semua RPC dikirim pada ordering channel 2 (`OrderingChannel_SyncRPC`),
reliability `RELIABLE_ORDERED` (9).

## Client -> Server

| ID | Nama | Keterangan |
|----|------|------------|
| 25 | PlayerConnect / ClientJoin | lihat `core.hpp` |
| 26 | OnPlayerEnterVehicle | `u16 VehicleID, u8 passenger`; tap USE membawa intent driver, tahan USE intent passenger; attempt saja, server tetap menentukan seat |
| 27 | OnPlayerSelectObject | current open.mp reader: `u32 SelectType, u16 ObjectID, u32 Model, VEC3 hit position`; hanya setelah `PlayerBeginObjectSelect`; Android memberi native ID object + revision lifecycle, lalu native kembali mengambil model/origin current, menolak tap Begin/Cancel stale, dan queue maksimal satu choice per lifecycle—bukan raycast hit GTA penuh |
| 116 | OnPlayerEditAttachedObject | current open.mp reader: `u32 response, u32 slot, u32 model, u32 bone, VEC3 attachment offset, VEC3 rotation, VEC3 scale, u32 colour1 ARGB, u32 colour2 ARGB`; hanya active local attachment edit server. Android mengizinkan Cancel=0/Save=1 dan native re-samples slot/model/bone/colour (RPC 113 ABGR→RPC 116 ARGB) |
| 117 | OnPlayerEditObject | `bit PlayerObject, u16 ObjectID, u32 response, VEC3 position, VEC3 rotation`; hanya tuple edit server aktif, transform touch bounded/revalidated native; Cancel=0, Final=1, Update=2 |
| 54 | NPCConnect | |
| 50 | PlayerCommandMessage | chat diawali `/` |
| 101 | PlayerChatMessage | chat biasa |
| 62 | OnPlayerDialogResponse | `i16 dialog ID, u8 response, i16 list item, DynStr8 text`; builder + reliable sender bounded |
| 83 | OnPlayerSelectTextDraw | `u16 raw wire ID`; hanya bila selection + selectable state native masih valid |
| 132 | OnPlayerSelectedMenuRow | exact `u8 row`; only current enabled server-menu revision/row may queue it |
| 140 | OnPlayerExitedMenu | body kosong; only current server-menu revision may queue it |
| 128 | PlayerRequestClass | `u16 ClassID` — builder + wire test ada |
| 129 | PlayerRequestSpawn | payload kosong — builder + wire test ada |
| 131 | PickedUpPickup | `i32 PickupID`; hanya dari tap USE setelah native checks/rate limit |
| 154 | OnPlayerExitVehicle | `u16 VehicleID`; attempt keluar, bukan state lokal langsung |
| 155 | OnPlayerRequestScoresAndPings | body kosong; request roster score/ping cache server, reliable rate-limited |
| 103 | ClientCheck | jawaban anti-cheat |

### Layout PlayerConnect (25)

```
u32   VersionNumber        // 0.3.7 = 4057
u8    Modded
u8    NameLen  + char[] Name
u32   ChallengeResponse    // token dari ACCEPTED ^ 4057
u8    KeyLen   + char[] Key        // 40 char hex (serial)
u8    VerLen   + char[] VersionString
u32   (opsional) IsUsingOfficialClient / OmpVersion
```

## Server -> Client (layout yang didecode saat ini)

| ID | Nama | Layout |
|----|------|--------|
| 11 | SetPlayerName | |
| 15 | TogglePlayerControllable | `u8 Enable`; reducer menghentikan preview/OnFootSync saat server freeze |
| 17 | SetWorldBounds | `VEC4 maxX,minX,maxY,minY`; reducer menahan prediction on-foot/driver **exterior** pada rectangle server tanpa mengarang force push-back atau menyentuh interior/COL |
| 18 | GivePlayerMoney | `i32 delta`; menambah/mengurangi counter money HUD server-authoritative |
| 20 | ResetPlayerMoney | payload kosong; reset counter money HUD ke nol |
| 21 | ResetPlayerWeapons | payload kosong; clear ammo server-known dan mengarm unarmed |
| 22 | GivePlayerWeapon | `u32 Weapon, u32 Ammo`; state ammo server-known per raw weapon ID |
| 27 | PlayerBeginObjectSelect | body kosong; membuka picker object bounded native dan memajukan revision pilihan (bukan geometry hit-test GTA penuh) sampai lifecycle cancel/edit server |
| 28 | PlayerCancelObjectEdit | body kosong; menutup mode pilih, edit object (117), **dan** edit attachment (116) Android; juga menginvalidasi revision picker saat server mengakhiri lifecycle |
| 29 | SetPlayerTime | `u8 Hour,u8 Minute`; waktu recipient-specific untuk HUD/sky proxy, tidak memengaruhi cadence/movement |
| 30 | TogglePlayerClock | `u8 Enable` (`0/1`); hanya menyembunyikan/menampilkan clock HUD lokal, tidak menghapus fakta waktu server |
| 32 | PlayerStreamIn | reducer actor remote non-DL: ID, team, skin, VEC3, angle, color, fighting style, 11 skill level |
| 36 | PlayerShowTextLabel | `u16 wire ID, u32 RGBA, VEC3/offset, float draw distance, u8 LOS, u16 player/vehicle attachment, StringCompressor`; label portable bounded |
| 37 | DisableCheckpoint | payload kosong |
| 38 | SetRaceCheckpoint | u8 type, VEC3 pos, VEC3 next pos, float size |
| 39 | DisableRaceCheckpoint | payload kosong |
| 41 | PlayAudioStream | `DynStr8 URL` (writer `HybridString<128>`, max 127 byte), `VEC3,float distance,u8 usepos`; reducer hanya menyimpan directive bounded. Android memutar **HTTPS** yang lolos validasi lifecycle; HTTP/URL tidak aman diblokir lokal tanpa response server |
| 42 | StopAudioStream | payload kosong; menghentikan/release media Android lewat revision directive |
| 44 | CreateObject | legacy transform atau writer modern + attachment; `u8 materialCount`, lalu type/slot default/text record source-verified, max 16 slot |
| 45 | SetObjectPos | `u16 ObjectID, VEC3` |
| 46 | SetObjectRotation | `u16 ObjectID, VEC3 rot` |
| 47 | DestroyObject | `u16 ObjectID` |
| 48 | SetPlayerVirtualWorld | exact non-DL `i32 worldId`; retained/displayed as server routing state only, never guessed as GTA interior/collision/entity filter |
| 56 | SetPlayerMapIcon | `u8 slot, VEC3, u8 type, u32 RGBA, u8 style`; radar/GLES marker portable bounded |
| 58 | PlayerHideTextLabel | `u16 raw wire ID`; idempoten menghapus label 3D retained |
| 59 | SetPlayerChatBubble | `u16 PlayerID, u32 RGBA, float draw distance, u32 expiry ms, DynStr8`; expiry-filtered snapshot bubble bounded |
| 61 | ShowDialog | i16 ID, u8 style, 3 DynStr8, default RakNet `StringCompressor` body; bounded native decoder + Android dialog presenter |
| 63 | DestroyPickup | `i32 PickupID`; menghapus record global bounded/idempoten |
| 65 | LinkVehicleToInterior | `u16 VehicleID, u8 interior`; memperbarui proxy streamed |
| 67 | SetPlayerArmedWeapon | `u32 Weapon`; satu-satunya authority weapon aktif lokal |
| 70 | PutPlayerInVehicle | `u16 VehicleID, u8 seat`; seat authoritative; seat 0 dapat membuka pilot driver Packet-200 collision-grounded terbatas |
| 71 | RemovePlayerFromVehicle | payload kosong; keluar authoritative |
| 72 | SetPlayerColor | u16 PlayerID, u32 Color |
| 73 | SendGameText | `u32 style, u32 time ms, DynStr32`; bounded timed Android banner proxy, bukan font/animasi GTA penuh |
| 74 | ForcePlayerClassSelection | payload kosong; mencabut class/spawn handshake stale dan menghentikan prediction/sync lokal sampai class/spawn server berikutnya |
| 75 | AttachObjectToPlayer | `u16 object, u16 player, VEC3 offset, VEC3 rotation`; only existing client-object slot may follow a valid local/streamed player parent in renderer |
| 76 | PlayerInitMenu | current open.mp writer: `u8 ID, u32 two columns, StaticString<32>, VEC2, float width(s), u32 enabled, 12*u32 row enabled, fixed headers/items`; bounded native Android menu definition |
| 77 | PlayerShowMenu | exact `u8 MenuID`; presents only a retained source-verified definition |
| 78 | PlayerHideMenu | exact `u8 MenuID`; idempotently hides matching active Android menu |
| 80 | ShowPlayerNameTagForPlayer | `u16 PlayerID, u8 show`; policy visibilitas nametag per remote actor |
| 82 | InterpolateCamera | bit selector + two VEC3 + u32 duration + u8 cut; sampled renderer camera authority saja |
| 83 | TextDrawSelection | bit enable + u32 RGBA; enables bounded selectable hit surface |
| 84 | SetPlayerObjectMaterial | `u16 ObjectID, u8 type, u8 slot`, lalu record default/text; hanya slot object client-shared existing, 0–15 |
| 85 | StopFlashGangZone | `i16 gang zone ID`; menghentikan fase flash visual |
| 86 | ApplyPlayerAnimation | `u16 PlayerID, DynStr8 lib, DynStr8 name, float delta, 4 packed bit flags, u32 time`; retained bounded visual directive untuk local/remote streamed saja |
| 87 | ClearPlayerTasks | `u16 PlayerID`; clears only retained RPC-86 visual directive |
| 93 | SendClientMessage | u32 color, u32 len, char[] — reducer caps 144 bytes / 100 lines |
| 94 | SetWorldTime | u8 hour `0..23`; drives generated scene ambience |
| 95 | CreatePickup | `i32 PickupID, i32 model, i32 spawn type, VEC3`; visual/interaction record bounded |
| 99 | MoveObject | u16 ObjectID, VEC3 pos, VEC3 target, float speed, VEC3 rot |
| 101 | PlayerChatMessage | u16 sender, DynStr8; bounded HUD history |
| 105 | PlayerTextDrawSetString | `u16 wire ID, DynStr16`; update only draw retained sebelumnya |
| 107 | SetCheckpoint | VEC3 pos, float size |
| 108 | ShowGangZone | `i16 ID, VEC2 min, VEC2 max, u32 ABGR`; radar rect visual bounded |
| 113 | SetPlayerAttachedObject | current writer: `u16 PlayerID, u32 slot, bit Create`, lalu conditional model/bone/VEC3 offset/rotation/scale + 2 ABGR; retained 10-slot renderer accessory, dan slot lokal current dapat menjadi fakta sumber untuk editor RPC 116 |
| 116 | PlayerBeginAttachedObjectEdit | exact `u32 slot` (`0..9`); membuka editor transform **attachment-local** hanya bila slot attachment pemain lokal masih retained/valid |
| 117 | PlayerBeginObjectEdit | `bit PlayerObject, u16 ObjectID` (17 meaningful bits / 3 body bytes); membuka editor transform Android hanya untuk tuple server ini |
| 120 | HideGangZone | `i16 ID`; idempoten menghapus gang zone |
| 121 | FlashGangZone | `i16 ID, u32 ABGR`; warna/fase flash visual server-authoritative |
| 122 | StopObject | `u16 ObjectID`; menghentikan interpolasi lokal bounded |
| 124 | TogglePlayerSpectating | current open.mp writer: exact `u32 Enable` boolean `0/1`; active state immediately blocks local on-foot/vehicle/aim/passenger sync |
| 126 | PlayerSpectatePlayer | current open.mp writer: exact `u16 PlayerID, u8 SpecCamMode`; copied visual target only, never aliases local player authority |
| 127 | PlayerSpectateVehicle | current open.mp writer: exact `u16 VehicleID, u8 SpecCamMode`; copied visual target only, never aliases local vehicle authority |
| 128 | PlayerRequestClassResponse | selectable, team, model, spawn, angle, 3 weapon/ammo slots |
| 129 | PlayerRequestSpawnResponse | u32 Allow |
| 133 | SetPlayerWantedLevel | `u8 0..6`; star HUD server-authoritative, tanpa police/reward simulation lokal |
| 134 | ShowTextDraw | fixed visual header + DynStr16 text; bounded Android Canvas text/box proxy |
| 135 | HideTextDraw | `u16 raw wire ID` |
| 137 | PlayerJoin | |
| 138 | PlayerQuit | u16 PlayerID, u8 Reason |
| 139 | **PlayerInit** | ~212 byte (lihat bawah) |
| 144 | RemovePlayerMapIcon | `u8 slot`; idempoten menghapus marker map icon |
| 145 | SetPlayerAmmo | exact compact `u8 Weapon, u16 Ammo`; hanya memperbarui ammo server-known raw weapon ID |
| 146 | SetPlayerGravity | exact `float gravity`; environment/pilot on-foot memakai value GTA server bila berada dalam envelope aman, selain itu prediction fail-closed |
| 147 | SetVehicleHealth | `u16 VehicleID, float`; memperbarui proxy streamed |
| 152 | SetWeather | u8 weather ID; drives generated scene haze only |
| 153 | SetPlayerSkin | non-DL exact `u32 PlayerID, u32 Skin`; mengganti model actor remote streamed atau model local yang sudah punya spawn template; custom-skin DL field sengaja tidak diminta |
| 155 | SendPlayerScoresAndPings | payload sampai habis berupa tuple `u16 ID, i32 score, u32 ping`; roster UI bounded |
| 156 | SetPlayerInterior | `u8 interior`; snapshot mereset prediksi lalu memilih batch IPL/DFF dan query COL dengan ID interior tepat; tick tetap fail-closed bila floor matching tidak tersedia |
| 157 | SetPlayerCameraPosition | `VEC3`; endpoint kamera scene server-authoritative |
| 158 | SetPlayerCameraLookAt | `VEC3, u8 cut`; endpoint target kamera scene server-authoritative |
| 159 | SetVehiclePosition | `u16 VehicleID, VEC3`; memperbarui proxy streamed |
| 160 | SetVehicleZAngle | `u16 VehicleID, float`; memperbarui proxy streamed |
| 162 | SetCameraBehindPlayer | body kosong; menghapus endpoint server dan mengembalikan fallback touch/player-follow |
| 163 | PlayerStreamOut | `u16 PlayerID`; reducer clears remote sync eligibility |
| 164 | StreamInVehicle | full legacy initial vehicle layout; bounded proxy lifecycle/state |
| 165 | StreamOutVehicle | `u16 VehicleID`; clears proxy sync eligibility |
| 166 | PlayerDeath | exact observer body `u16 PlayerID`; hanya cue `WASTED` remote sementara. Tidak pernah dipakai sebagai bukti death/respawn player lokal |
| 171 | ShowActor | non-DL exact `u16 ActorID, u32 Skin, VEC3, float angle, float health, u8 invulnerable`; creates/updates separate visual NPC |
| 172 | HideActor | `u16 ActorID`; idempotently removes separate visual NPC |
| 173 | ApplyActorAnimation | `u16 ActorID, DynStr8 lib, DynStr8 name, float delta, 4 packed bit flags, u32 time`; retained bounded visual directive untuk NPC existing |
| 174 | ClearActorAnimation | `u16 ActorID`; clears only retained RPC-173 visual directive |
| 175 | SetActorFacingAngle | `u16 ActorID, float angle`; only updates an existing ShowActor NPC |
| 176 | SetActorPosition | `u16 ActorID, VEC3`; only updates an existing ShowActor NPC |
| 178 | SetActorHealth | `u16 ActorID, float health`; only updates an existing ShowActor NPC |

### Guarded spectator lifecycle (124 / 126 / 127)

The retained wire layouts are now verified against current open.mp
[`Shared/NetCode/core.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/core.hpp):
RPC 124 writes `u32 Enable` (only `0`/`1` is accepted here), while RPC 126/127
write exactly `u16 PlayerID/VehicleID | u8 SpecCamMode`. The reducer accepts no
short or trailing bodies.

While server spectator state is active, local prediction/control is invalidated and every
outbound local OnFoot, InCar, Aim, and Passenger path is fail-closed. Target IDs are retained
in a separate copied namespace/type and may anchor camera/radar/rendering only after the
corresponding streamed player/vehicle snapshot exists. They never become local identity,
position, control, collision, or sync authority. Disabling spectator clears the retained target.

### Lifecycle server NPC actor (171 / 172 / 175 / 176 / 178)

`ShowActor` is **not** `PlayerStreamIn`: it retains a server NPC in an
independent actor-ID namespace, capped at 1,000 retained records and 64
stable-ID-ordered snapshot copies. The non-DL body is accepted only at its exact
27-byte size; model, finite world coordinates, angle, health, and invulnerable
flag are all bounded before state changes. `HideActor` is idempotent. Position,
angle, and health mutations use their exact 14/6/6-byte bodies and are ignored
when the matching NPC does not exist, so a mutator cannot fabricate an entity.

The Android radar/GLES path may draw the copied model/transform and resolve a
portable `peds` DFF from user-owned GTA data. This remains visual presentation
only: NPC actors never become remote-player identities, collision obstacles,
interaction/use targets, local animation authority, raycast targets, hit claims,
or damage authority.

### Forced animation directive (86 / 87 / 173 / 174)

Current open.mp writes RPC 86/173 as `u16 target`, `DynStr8 lib`, `DynStr8
name`, `float delta`, four consecutive `BIT` flags (`loop`, `lockX`, `lockY`,
`freeze`), then unaligned `u32 time`; only up to four zero wire-pad bits may
remain. RPC 87/174 are exact `u16` clears. The audited SDK `AnimationData`
stores the source tokens in `HybridString<16>` / `HybridString<24>`, so native
accepts at most 15 library bytes and 23 name bytes, printable identifier
characters only. Malformed/trailing data preserves the prior directive.

A directive is retained only for the initialized local player, an existing
streamed remote player, or an existing NPC actor; it never fabricates a player
or actor. Fresh remote StreamIn/StreamOut clears its remote directive. It
reaches Canvas/GLES as an explicit `ANIM` cue/ring, **not** false skeletal
playback: no IFP is executed yet, and `lockX`, `lockY`, `freeze`, delta, or
duration never mutate native collision, input, local physics, or outbound sync.
A future user-owned GTA IFP/skeleton renderer may consume the bounded same
state without widening the protocol boundary.

### Lifecycle remote player

`PlayerStreamIn` (32) adalah batas identitas/render entity: untuk client non-DL
layoutnya `u16 id | u8 team | u32 skin | VEC3 pos | float angle | u32 RGBA |
u8 fightingStyle | u16 skill[11]`. SampDroid menyimpan actor ini secara bounded.
`PlayerStreamOut` (163) menonaktifkan actor tersebut dan menolak Packet 207
berikutnya sampai `StreamIn` baru datang. Sementara actor masih retained,
`SetPlayerSkin` (153) non-DL mengubah modelnya lewat layout source-verified
`u32 PlayerID | u32 Skin`; snapshot renderer berikutnya memakai model baru tanpa
mengubah transform atau lifecycle stream. Bila ID adalah pemain lokal, reducer
hanya mengganti model retained setelah/ketika template `SetSpawnInfo` ada—tidak
menciptakan actor lokal di origin. Field `CustomSkin` yang hanya ada pada client
0.3.DL sengaja tidak diparse/diminta oleh jalur client saat ini.

### Nametag dan chat bubble (59 / 80 / PlayerInit)

`PlayerInit` membawa `SetNameTagDrawDistance`, `DisableNameTagLOS`, dan
`ShowNameTags`. Reducer meneruskan policy itu bersama override RPC 80 `u16
PlayerID | u8 show`; override boleh datang sebelum `PlayerJoin` dan dipasang saat
actor dikenal. Renderer hanya menunjukkan nametag remote bila policy global,
override, dan draw-distance mengizinkan. Flag LOS dipertahankan, tetapi belum
ada klaim raycast LOS GTA yang identik PC.

`SetPlayerChatBubble` 59 memakai layout tepat `u16 PlayerID | u32 RGBA | float
DrawDistance | u32 ExpireTime | DynStr8 Text`. Text dibatasi 128 byte, float
harus finite/positif dalam envelope renderer, dan expiry dibaca sebagai durasi
steady-clock lokal (maksimum defensif satu jam). Nilai kosong/nol membersihkan
bubble. Snapshot Android tidak menerima deadline mentah dan hanya melihat bubble
aktif; bubble tidak mengubah chat, entity transform, seat, atau combat.

### Checkpoint / race checkpoint

```text
SetCheckpoint (107):      float X | float Y | float Z | float Size
SetRaceCheckpoint (38):   u8 Type | VEC3 Position | VEC3 NextPosition | float Size
DisableCheckpoint (37):   empty
DisableRaceCheckpoint(39): empty
```

Reducer menolak float non-finite dan ukuran negatif, menyimpan marker aktif
terakhir, lalu meng-clear flag saat RPC disable datang. Marker ini sudah keluar
sebagai snapshot bounded ke radar Canvas dan scene GPU generated (bukan asset
atau collision GTA).

### Pilih object server (27 / 28)

Current open.mp [`Shared/NetCode/object.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/object.hpp)
menulis `PlayerBeginObjectSelect` (27) dan `PlayerCancelObjectEdit` (28) tanpa
body. Saat mode 27 aktif, Android menampilkan maksimal 24 object yang memang
sudah berada di snapshot renderer bounded. Tap mengirim arah sebaliknya dengan
layout reader `OnPlayerSelectObject`: `u32 SelectType | u16 ObjectID | u32 Model
| VEC3 Position`, melalui `RELIABLE_ORDERED` channel 2.

Java hanya menyerahkan ID u16. Tepat sebelum enqueue, native memeriksa lagi mode
server, lifecycle object, rantai parent yang masih resolve, model, dan origin
sampled; ID/model/posisi mentah dari UI tidak pernah menjadi authority. Current
open.mp pool men-reserve ID `0` dan sentinel wire `0xFFFF`; keduanya ditolak.
`SelectType` emit nilai legacy netral `0`; component open.mp current resolving object shared
pool berdasarkan ID/model. Posisi yang dikirim sementara adalah origin object,
bukan klaim raycast atau impact point mesh GTA—full mesh hit-testing tetap pekerjaan
renderer berikutnya. Throttle 120 ms menghindari flood tap. RPC 28 menutup picker.

### Edit object server (117 / 28)

Current open.mp [`Shared/NetCode/object.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/object.hpp)
menulis `PlayerBeginObjectEdit` server→client sebagai `bit PlayerObject | u16
ObjectID`; final byte membawa tujuh padding transport. Reducer menyimpan tuple
`global/player object + ID` dan revision baru setiap Begin, lalu menghapusnya pada
RPC 28 atau `DestroyObject` target. Begin tidak mengizinkan Kotlin memilih type,
ID, ataupun merekonstruksi object yang belum tersedia.

Tombol `OBJ` membuka fallback editor langkah angka untuk layar touch (0.10/1.00
meter dan 5/15 derajat), bukan mengklaim gizmo mouse PC atau raycast mesh. Untuk
setiap tap, Kotlin memberikan transform **beserta revision snapshot**, bukan
ID/kind. Tepat sebelum sender membangun arah balik `OnPlayerEditObject`, C++ di
bawah session mutex memeriksa accepted + PlayerInit, kesamaan revision (tap lama
fail-closed setelah Begin baru), tuple server yang masih aktif, object saat ini dan rantai
attachment yang masih resolve, respons `Cancel=0`/`Final=1`/`Update=2`, finite
world/rotasi envelope, cadence Update 90 ms, serta terminal Final/Cancel satu kali
per revision. Paket kemudian `RELIABLE_ORDERED` channel 2. Baseline editor
disalinkan sebagai snapshot native attachment-resolved khusus, bukan dicari dari
list renderer 128 object; jadi `PlayerObject=true` dan ID yang berada di luar
cap/sort frame tetap tidak bisa mewarisi transform object lain atau menjadi
false-positive. Server tetap memutuskan hasil callback/mutasi; Android tidak
mengubah lifecycle atau transform reducer secara optimistis. RPC 28 tetap satu-satunya cancel lifecycle server untuk mode
selection maupun editor.

### Edit player attachment server (116 / 113 / 28)

Current open.mp `PlayerBeginAttachedObjectEdit` (116) membawa tepat satu `u32`
slot `0..9`, yaitu namespace attachment pemain lokal—bukan ID global object dan
bukan pilihan bebas UI. Reducer menyimpan `slot + revision` terpisah dari edit
object 117. Ia hanya mengekspos editor ketika `SetPlayerAttachedObject` (113)
pada pemain lokal masih memiliki slot aktif yang finite/bounded. Begin 116,
Begin 117, dan Begin selection saling menggantikan mode editor; RPC 28 menutup
semuanya. Penghapusan slot lokal aktif juga menutup gate sebelum respons dapat
dikirim.

Dialog touch mengubah hanya sembilan nilai attachment-local: offset, rotation,
dan scale. Kotlin tidak pernah menyerahkan slot, model, bone, atau warna; hanya
mengembalikan revision snapshot untuk equality gate. Pada SAVE=1/CANCEL=0,
sender native di bawah session mutex me-resolve ulang lifecycle revision dan
slot yang sama (tap lama fail-closed), lalu menyalin model/bone dan dua warna. Layout
`SetPlayerAttachedObject` memakai warna **ABGR** server→client, sedangkan reader
`OnPlayerEditAttachedObject` memakai `Colour::FromARGB`; sender secara eksplisit
menukar R/B ke ARGB sebelum membangun RPC 116. Offset dibatasi ±10.000, rotasi
±360.000°, scale nonzero |0.001..100|, bone 0..18, model 1..65535. Satu respons
terminal reliable-ordered channel 2 boleh queue per revision. Tidak ada mutasi
optimistis pada renderer: server tetap menentukan callback dan attachment
berikutnya.

### Object server (44–47, 99, 122)

Reducer menyimpan maksimal 4096 object global, tetapi snapshot renderer menyalin
maksimal 128 object dengan rantai parent yang benar-benar dapat di-resolve.
`CreateObject` menerima short legacy (34 byte) atau header SA-MP/open.mp modern
(NoCameraCol, **vehicle parent lalu object parent**, optional offset/rotation
attachment + `SyncRotation`, kemudian `u8 MaterialsCount`) hingga 32 KiB. Writer
saat ini di [`Shared/NetCode/object.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/object.hpp)
menulis maksimal 16 record sebagai `u8 type, u8 slot` (`None=0`, `Default=1`,
`Text=2`): `Default` berisi `u16 source model, DynStr8 TXD, DynStr8 texture,
u32 ABGR`; `Text` berisi exact `u8 ObjectMaterialSize`, DynStr8 font,
`u8 font size`, `u8 bold`, `u32 font ARGB`, `u32 background ARGB`, `u8
alignment`, lalu `StringCompressor` text. Reducer mempertahankan kedua varian:
Default hanya bila nama TXD/texture lookup key portable (case dinormalisasi),
sedangkan Text dibatasi enum size source `10..140`, alignment `0..2`, dan
HybridString writer `31` byte. RPC 84 (`SetPlayerObjectMaterial`)
memakai record yang sama setelah `u16 ObjectID` untuk mengganti satu slot object
existing; source component menjamin global/player object tidak bertabrakan dalam
pool ID client yang sama. Trailing hanya
boleh padding bit akhir (maksimum tujuh bit); slot duplikat, path-like key,
record malformed, object orphan, atau opaque payload ditolak fail-closed.

Bila tepat satu parent ada, reducer mempertahankan relasi/offset/rotasi itu; saat membuat frame ia
mencari parent vehicle yang masih streamed atau parent object secara rekursif,
men-sample `MoveObject` parent dengan clock monotonic, dan membatasi rantai ke
8 level. RPC 75 `AttachObjectToPlayer` yang source-writer-verified juga boleh
mengikat **object global yang sudah ada** ke transform local player atau remote
player yang masih streamed; RPC orphan tidak membuat object baru. Bila native
sudah punya prediksi local yang collision-grounded, snapshot renderer boleh
memakai **copy anchor prediksi itu saja** untuk child pada player lokal; ia tidak
mengubah `WorldState`, RPC, atau authority server. Parent tidak ada/stream-out,
rantai siklik/terlalu dalam, parent ganda, atau data attachment malformed membuat
child **tidak dirender**—bukan dibekukan di koordinat
`CreateObject` mentah. Offset memakai heading parent yang memang tersedia;
`SyncRotation=false` hanya ada pada attachment CreateObject dan mempertahankan
rotasi child dari `CreateObject`. Client belum punya pitch/roll vehicle/ped penuh,
jadi tidak mengklaim fidelity matriks 3D GTA/suspensi untuk attachment.

`MoveObject` memakai body tepat 42 byte: `u16 ID | VEC3 current | VEC3 target |
float speed | VEC3 target rotation`; seluruh vector rotasi `-1000` berarti tanpa
rotasi akhir. Layout itu diaudit terhadap writer `MoveObject`/`StopObject` di
[open.mp `Shared/NetCode/object.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/object.hpp),
bukan lagi tebakan 30-byte. View diinterpolasi dengan clock monotonic lokal;
`StopObject`, SetPos, SetRotation, dan Destroy membatalkan/menutup motion itu.
Snapshot renderer membawa paling banyak 16 material per object. Jalur GLES
DFF/TXD user-owned dapat mengganti slot DFF flattened yang sama, memakai lookup
TXD/parent bounded, dan memakai ABGR nonzero sebagai warna base sambil
menonaktifkan vertex lighting sesuai dokumentasi API (nol mempertahankan warna
DFF). `modelid=-1`/u16 `65535` color-only boleh membawa kedua nama TXD kosong,
tetap bergantung pada base mesh, dan tidak memicu lookup texture.

Text slot yang sama sekarang membuat RGBA overlay native bounded memakai ukuran
source exact (`32x32` sampai `512x512`), ARGB font/background, alignment,
newline, dan bold. Karena GTA font proprietary tidak dibundel, glyph adalah
fallback bitmap asset-free—bukan klaim fidelity Arial/Pricedown PC—dan font face
source dipertahankan hanya sebagai metadata. Text tanpa UV/material table,
input invalid, atau yang melewati cap texture/vertex tetap fail-closed. Collision
object dan feedback movement object ke server tetap belum diklaim sebagai
fidelity PC penuh.

### Player-attached object (113)

Writer current open.mp di
[`Shared/NetCode/object.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/object.hpp)
menulis tepat `u16 PlayerID | u32 Index | bit Create`. Jika `Create=1`, sisa
body adalah `u32 model | u32 bone | VEC3 offset | VEC3 rotation | VEC3 scale |
u32 colour1.ABGR | u32 colour2.ABGR`; `Create=0` menghapus slot dan tidak
membawa field tambahan. Reducer menerima hanya slot `0..9`, bone `0..18`, model
`1..65535`, float finite/bounded, dan hanya untuk pemain lokal atau ID player
yang sudah dikenal—RPC tidak boleh membuat identitas player baru. Maksimum 64
sample aksesori keluar ke snapshot per frame; quit membersihkan seluruh slot
pemain tersebut.

Scene meneruskan transform, scale, dan dua warna source ke DFF user-owned
sebagai visual accessory; bila asset tidak bisa di-resolve, GLES/radar tetap
menggambar marker kecil dari transform/color source agar state server tidak
hilang. Karena client native ini belum memiliki ped
skeleton/animasi GTA, bone di-anchor dengan tinggi **heuristic** portable (head,
spine, limb) dan heading parent saja; tidak ada klaim pose tulang, pitch/roll,
collision, hitbox, target, atau outbound sync yang presisi. Warna dipakai sebagai
bounded color-only material overlay untuk flattened material slot 0/1 bila mesh
mendukungnya. Jadi kompatibilitas wire/state sudah source-shaped, sementara
fidelity visual aktual masih harus melewati deferred native/Android validation.

### Pickup global (63, 95, 131)

`CreatePickup` dibaca sebagai body tepat 24 byte: `i32 ID | i32 model | i32
spawn type | float X/Y/Z`; `DestroyPickup` tepat satu `i32 ID`. Struktur dan
arah RPC 131 diaudit terhadap [open.mp
`Shared/NetCode/pickup.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/pickup.hpp).
Keduanya dibatasi 4096 record retained / 128 snapshot marker, menolak
ID/model/type/posisi yang invalid, dan tidak mengarang virtual-world atau
interior karena field tersebut tidak ada pada wire record ini. Marker radar/GLES
hanyalah proxy interaksi, bukan model pickup GTA. Tombol **USE** Android
menghasilkan satu edge ke native; hanya setelah PlayerSpawn sudah di-ACK, posisi
lokal + controllable valid, interior zero, pickup dalam radius 2,15, dan interval
450 ms, client mengantrikan `PickedUpPickup` (131) `RELIABLE_ORDERED` channel 2.
Ia tidak memberikan item/uang/health lokal dan tidak menghapus pickup sebelum
server mengirim DestroyPickup.

### TextDraw portable (83, 105, 134, 135)

Layout source-audited dari [open.mp
`Shared/NetCode/textdraw.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/textdraw.hpp):
`ShowTextDraw` memakai raw `u16` wire ID, flag, VEC2/warna/size header tetap,
dan `DynStr16` maksimal 256 byte. SampDroid menyimpan maksimal 2304 draw
(global raw ID `0..2047`, player `2048..2303`) tetapi hanya menyalin 32 ke HUD
per frame. Android Canvas menggambar text/box screen-space dasar, alpha/warna
ABGR, alignment, shadow/outline kecil. Font-4 sprite dengan token literal
`dictionary:texture` sekarang dapat meminta satu RGBA TXD user-owned yang
benar-benar didecode (nama strict ≤31 byte, no path, max 512² pixel, cache 16
sprite, retry throttled) dan merender bitmap itu pada rect TextDraw; miss tetap
jelas ditandai `SPRITE`, bukan diganti drawable Android. Font GTA 0–3, model
preview font-5, dan GameText/font/animasi penuh masih belum setara GTA. Tetapi
font-5 kini meneruskan exact `model`, rotation, zoom, dan colour1/colour2 ke
snapshot lalu, bila model dapat dipetakan dari data user-owned, menggambar copy
mesh DFF prelit bounded (maks. 3.600 vertex / cache 8) ke rect Canvas. Itu adalah
mesh nyata, **bukan** klaim kamera frontend GTA, material/TXD/vehicle-colour,
lighting, atau skeletal fidelity; miss tetap transparan bertuliskan preview belum
ada. `SetString` tidak boleh membuat draw baru;
`Hide` idempoten menghapus record. Ketika RPC 83 server mengaktifkan selection,
hanya draw yang masih visible dan `selectable` mendapat hit-box berwarna RGBA
dari server; tap mengirim raw wire ID lewat RPC 83 reliable setelah native
memeriksa state lagi.
Tidak ada klik otomatis atau raw ID dari Android yang dipercaya.

### Dialog (61 / 62)

`ShowDialog` memakai body default RakNet `StringCompressor`, bukan raw DynStr.
Reducer membangun tree Huffman default secara independen, membaca compressed
`u16` bit count, menolak output/bitstream yang melebihi batas, lalu mensanitasi
UTF-8 sebelum teks melewati JNI. Batas saat ini: title 64 byte, button 48 byte,
dan body 1024 byte. ID `-1` menutup dialog aktif.

Android presenter mendukung message, input, password, list, tablist, dan
header-tablist dalam UI native. Saat user menekan tombol, sender mencocokkan ID
**dan revision** dialog aktif lalu mengirim RPC 62 lewat `RELIABLE_ORDERED`
channel 2. Untuk Tablist, row sumber yang dipilih tetap mempertahankan tab pada
`inputText` (selain `listItem`) agar kompatibel. Dialog hanya disembunyikan
lokal setelah packet berhasil masuk antrean reliable.

### Legacy menu (76 / 77 / 78 / 132 / 140)

Current open.mp [`menu.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/menu.hpp)
verifies `PlayerInitMenu` as `u8 MenuID`, `u32 HasTwoColumns`, fixed
`StaticString<32>` title/header/item arrays, position/width floats, `u32`
menu/row booleans, and at most 12 rows per column. The reducer requires the
exact variable body through its final fixed row, bounds IDs to the current
legacy 128-menu pool, retains a hidden definition, and only presents it after
RPC 77. RPC 78 hides only the matching active menu.

Android renders bounded native row buttons; two columns stay one server row.
Selecting a row sends exact RPC 132 `u8`, while cancellation sends empty RPC
140. Both calls are `RELIABLE_ORDERED` channel 2 and must match the active
menu ID **and revision**; disabled/missing rows, stale UI, unknown menu IDs,
and failed sends leave server authority intact. Menu position/font/GTA frontend
styling is intentionally not claimed.

### GameText (73)

Layout sumber-audited dari
[`SendGameText`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/core.hpp)
adalah `u32 Style | u32 TimeMs | DynStr32 Text`. Reducer menerima maksimal 64
byte text, style `0..255`, dan durasi sampai satu jam; payload harus habis tepat.
Durasi nol atau text kosong menonaktifkan banner. Native menyimpan waktu monotonic
agar snapshot otomatis tidak lagi aktif setelah durasi server berakhir. Android
Canvas hanya menampilkan banner teks/shadow portable dan menghapus markup warna
umum; gaya/font/animasi GameText GTA asli tidak diemulasi.

### Money HUD server (18 / 20)

`GivePlayerMoney` (18) dibaca sebagai tepat satu `i32` delta signed dan
`ResetPlayerMoney` (20) sebagai body kosong. Reducer menjumlahkan delta secara
saturating ke counter `int64`, lalu mengeksposnya ke HUD hanya ketika server
sudah mengirim salah satu RPC ini. Tidak ada tombol Android yang menambah,
mengurangi, atau membelanjakan money; tahap ini bukan inventory/economy client.

### Scoreboard server (155)

Client mengirim `OnPlayerRequestScoresAndPings` (155) dengan body kosong lewat
`RELIABLE_ORDERED` channel 2 dan native membatasi request ke satu per detik.
Respons memakai ID yang sama, tetapi writer compatible mengirim roster cache
lengkap sebagai tuple berulang sampai payload habis: `u16 player ID | i32 score |
u32 ping`, tanpa count. Reducer menolak payload yang bukan kelipatan 10 byte,
ID duplikat, atau lebih dari 1.000 entry, kemudian mengganti roster lama secara
atomik. Baris yang tiba sebelum `PlayerJoin` ditahan per-ID secara bounded sampai
nama tersedia; UI hanya menampilkan salinan server ini (maks. 64), bukan query
web atau ping Android.

### Weapon authority dasar (21 / 22 / 67 / 145)

`ResetPlayerWeapons` (21) memiliki body kosong. `GivePlayerWeapon` (22) adalah
`u32 Weapon | u32 Ammo`; `SetPlayerArmedWeapon` (67) adalah `u32 Weapon`; dan
`SetPlayerAmmo` (145) yang source-verified memakai **`u8 Weapon | u16 Ammo`**,
bukan layout GiveWeapon. Reducer menerima ID legacy `0..46`, menyimpan ammo
menurut **raw weapon ID** tanpa menebak slot GTA, dan hanya mengubah armed
weapon dari RPC 67 (Reset mengembalikan unarmed 0). Snapshot HUD menampilkan
armed weapon/ammo bila server memang sudah memberikannya. Ketika OnFoot atau
driver InCar yang sudah lolos semua gate lain dikirim, byte weapon low-six-bits
mengambil nilai armed server tersebut; ia tidak berasal dari tombol touch dan
tidak memilih/mengurangi ammo secara lokal.

Saat on-foot, semantic AIM ditahan, collision/heading lokal valid, dan ammo
weapon aktif known, armed/ammo server ini juga dapat mengisi Packet-203 AimSync
terbatas. Ia hanya menulis camera mode/front/position dari state lokal yang
dimiliki + packed weapon state dari ammo server, dan ditahan saat kamera server
aktif. AIM+FIRE kini juga dapat mengirim Packet-206 client `HitType=None`
41-byte dari origin pose ini dengan ID/offset netral serta cadence bounded per
weapon. `HitPos` tetap nol atau hanya contact absolut ray COL statik dekat, bukan
entity target. Layout form tanpa `PlayerID` ini telah diaudit terhadap
reader + generator NPC open.mp commit `3d20f3dc`; sender juga ditahan saat
`PlayerInit.LagCompensation=0`, namun belum diuji live terhadap server
legacy/anticheat. Ini masih belum mengimplementasikan inventory/slot GTA
penuh, animasi senjata, target/raycast, hit detection, damage, reward, atau
ammo decrement lokal; Android tidak dapat mengirim peluru/hit bertarget yang
dibuat-buat.

### Label 3D portable (36 / 58)

`PlayerShowTextLabel` (36) memakai body bitstream:

```text
u16 WireID | u32 RGBA | VEC3 PositionOrAttachmentOffset | float DrawDistance |
u8 LOS | u16 PlayerAttachID | u16 VehicleAttachID | default StringCompressor text
```

`WireID 0..1023` adalah label global dan `1024..2047` label per-player;
`PlayerHideTextLabel` (58) membawa tepat satu `u16` yang sama. Reducer menerima
maksimal 2.048 slot, membatasi string hasil decompression menjadi 64 byte,
menolak posisi/distance non-finite dan trailing non-padding bitstream, lalu
menyalin maksimal 64 label ke snapshot. Attachment bukan pointer: Android
hanya menjumlahkan offset dengan actor/vehicle dari frame immutable; attachment
yang belum streamed dihilangkan, bukan diletakkan di origin palsu. Radar dan
scene menampilkan anchor yang berada dalam `DrawDistance`; scene memproyeksikan
teks Canvas di atas GLES. `LOS` dipertahankan sebagai fakta server, tetapi belum
diuji terhadap collision renderer. Font/billboard/occlusion GTA asli tidak
diklaim.

### Map icon (56 / 144)

Writer compatible `SetPlayerMapIcon` adalah body tepat 19 byte:

```text
u8 Slot | float X | float Y | float Z | u8 Type | u32 RGBA | u8 Style
```

SampDroid membatasi slot legacy `0..99`; Set mengganti slot lama dan Remove (144)
berisi tepat satu `u8` serta idempoten. Snapshot hanya menyalin 64 marker dalam
urutan slot. Canvas radar menggambar diamond berwarna dan GLES menggambar beacon
pada posisi server. `Type`/`Style` sengaja tidak dipetakan ke atlas sprite GTA:
marker ini menampilkan koordinat, warna, dan lifecycle server yang nyata tanpa
mengaku telah mereproduksi seluruh radar sprite PC.

### Gang zone (85 / 108 / 120 / 121)

`ShowGangZone` (108) adalah `i16 ID | float minX/minY/maxX/maxY | u32 ABGR`,
dengan namespace `0..1023`. Rectangle harus finite, ordered, non-empty, dan
berada dalam envelope world sebelum mengganti slot lama. `HideGangZone` (120)
menghapus id secara idempoten. `FlashGangZone` (121) hanya berlaku pada zone yang
visible dan menyimpan warna ABGR; `StopFlashGangZone` (85) menghapus state flash.
Snapshot deterministic dibatasi 64 zone. Radar Canvas melakukan clipping dan
menggunakan clock presentasi hanya untuk fase blink dari state flash yang sudah
ditetapkan server. Ini **bukan** implementasi capture/ownership turf atau
simulasi gameplay gang-zone.

### Kamera server (82 / 157 / 158 / 162)

`SetPlayerCameraPosition` (157) menerima tepat satu `VEC3`; `SetPlayerCameraLookAt`
(158) menerima `VEC3 | u8 cut`; `SetCameraBehindPlayer` (162) hanya body kosong
dan clear dua endpoint. `InterpolateCamera` (82) memakai satu selector bit
(`position` atau `look-at`), dua `VEC3`, `u32 duration`, lalu `u8 cut`—bukan
layout byte-aligned tebakan. Reducer membatasi durasi sampai satu jam, membuat
zero-duration menjadi cut target, dan mengambil sample read-only dengan clock
monotonic. Scene GLES mengonversi endpoint GTA `(x,y,z)` ke local `(x,z,-y)` dan
memberi endpoint known server prioritas di atas fallback touch/player-follow.
Kamera ini **tidak** mengubah collision, posisi lokal, atau outbound OnFootSync;
SetCameraBehindPlayer mengembalikan fallback pada snapshot berikutnya.

### Vehicle lifecycle dan broadcast sync

`StreamInVehicle` (164) adalah satu-satunya pembuat entity vehicle bounded;
Packet 200 (`PlayerVehicleSync`) dan Packet 209 (`PlayerUnoccupiedSync`) tidak
boleh membuat vehicle baru. Keduanya hanya memperbarui posisi/health proxy yang
sudah stream-in. RPC server `LinkVehicleToInterior` 65, `SetVehicleHealth` 147,
`SetVehiclePosition` 159, dan `SetVehicleZAngle` 160 juga mengganti field proxy
hanya jika lifecycle stream masih aktif; semua ukuran/layout diverifikasi dengan
writer open.mp. Packet 200 juga memberi heading dari quaternion terkompresi;
Packet 209 mempertahankan heading terakhir karena vector direction-nya bukan
Euler angle. Snapshot renderer kini boleh mengganti transform **vehicle driver
lokal** dengan prediksi collision-grounded yang terpisah; remote vehicle tetap
hanya memakai transform authoritative tanpa interpolasi.

Packet 211 (`PlayerPassengerSync`) server→client juga sudah dibaca dari
[`PlayerPassengerSync::write` open.mp](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/vehicle.hpp)
yang diverifikasi: `u8 211 | u16 PlayerID | u16 VehicleID | u16
packed(Seat low-6, DriveBy, Cuffed, Weapon/Additional) | u8 Health | u8 Armour
| u16 LeftRight | u16 UpDown | u16 Keys | VEC3 Position` (fixed 27 byte).
Reducer menuntut player **dan** vehicle sudah stream-in, menyimpan anchor posisi
Packet-211 tanpa offset kursi GTA tebakan, lalu menahan ped proxy/DFF untuk
passenger seperti driver agar tidak muncul menembus kendaraan. Nametag, bubble,
dan radar tetap memakai anchor yang tervalidasi. `driverKnown` tetap state
terpisah karena player ID 0 valid; Packet 209 hanya melepas driver lama dan
tidak menghapus passenger Packet-211 yang masih berlaku. Packet 207 on-foot,
stream-out/quit, vehicle stream-out, atau occupancy lebih baru untuk player
tersebut melepas attachment lama fail-closed. Ini tidak mengklaim asset/model
GTA atau physics kendaraan GTA penuh.

### Interaksi vehicle (26, 70, 71, 154)

Writer/readers [open.mp `Shared/NetCode/vehicle.hpp`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/vehicle.hpp)
menetapkan bahwa request client enter hanya `u16 VehicleID | u8 Passenger`
(RPC 26), sedangkan exit hanya `u16 VehicleID` (154); tidak ada PlayerID karena
server sudah tahu pengirim. Edge **USE** native memilih pickup dahulu, lalu
vehicle streamed dengan interior yang cocok dalam radius 3,25; ia meminta kursi
pengemudi (`Passenger=0`) dan menunggu server. Hanya `PutPlayerInVehicle` 70
(`u16 ID,u8 seat`) atau `RemovePlayerFromVehicle` 71 (kosong) yang mengubah
state local. Saat state itu `inVehicle`, Packet-207 on-foot dihentikan dan
USE berikutnya meminta keluar. Untuk **seat 0 saja**, SampDroid sekarang
membangun Packet 200 client-originated (`ID 200 | u16 vehicle | u16 left/right |
u16 up/down | u16 keys | raw GTA quaternion | VEC3 pos | VEC3 velocity |
float health | u8 health | u8 armour | u8 weapon/additional | u8 siren |
u8 gear | u16 trailer | u32 hydra`; 64 byte) mengikuti
[`PlayerVehicleSync::read`](https://github.com/openmultiplayer/open.mp/blob/master/Shared/NetCode/vehicle.hpp).
Transportnya `UNRELIABLE_SEQUENCED` channel 1 dengan rate `PlayerInit.InCarRate`
yang dibatasi. Pilot kinematic native hanya memakai gas/mundur, steer, rem,
COL ground, dan sweep statik; ia berhenti pada geometri/ground yang tidak valid
dan dapat diganti correction server kapan saja. Untuk seat nonzero dari RPC 70,
SampDroid juga membangun Packet 211 **client→server** fixed 25-byte tanpa
PlayerID (`211 | vehicle | packed seat/flags/weapon | health | armour | input |
VEC3 vehicle anchor`) lalu mengirim USeq channel 1 pada `InCarRate`. Ia hanya
aktif jika vehicle masih streamed, spawn/ACK siap, dan server tidak freeze;
seat/posisi tidak diprediksi. Bit cuffed hanya mengikuti `SetPlayerSpecialAction`
server (Cuffed=24). Berdasarkan reader `PlayerPassengerSync` dan vehicle update
open.mp commit `3d20f3dc`, held AIM boleh memasang drive-by hanya untuk Uzi/MP5/
Tec-9 yang armed+berammo dari server dan passenger tidak cuffed; FIRE tetap
masuk key bit, tanpa Packet-206 target/damage. Packet 211 **inbound remote**
tetap menjadi anchor passenger renderer. Trailer, siren/gear/Hydra aktif, damage, handling/suspension, AI, dan
physics GTA penuh belum diimplementasi. Tidak ada jaminan server menerima attempt.

### Class selection / spawn response

Untuk client non-DL seperti SampDroid saat ini, server membalas RPC 128 dengan:

```text
u8 Selectable | u8 TeamID | u32 ModelID | u8 Unknown1 |
float SpawnX | float SpawnY | float SpawnZ | float ZAngle |
u32 Weapon[3] | u32 Ammo[3]
```

RPC 129 dari server berisi `u32 Allow`; `0` berarti spawn ditolak.
Keduanya sudah memiliki decoder/state + regression test. `PlayerRequestClass`
dan `PlayerRequestSpawn` dikirim explicit lewat retained native session; respons
allow memicu RPC 52 kosong lalu gate ACK sebelum gameplay sync dapat berjalan.
Jika `SetPlayerHealth(0)` authoritative diterima, HUD Android mematikan input
lokal dan menawarkan request respawn yang sama—satu request pending saja,
dengan retry explicit minimum 8 detik bila respons server hilang. Tombol itu
bukan bukti/claim spawn, death, damage, atau hasil server.

### SendClientMessage (93)

```text
u32 RGBA | u32 TextLength | char[TextLength] Text
```

Reducer menyimpan maksimal 100 line, masing-masing maksimal 144 byte. Payload
chat tidak dimasukkan mentah ke summary diagnostic agar log tetap bounded; future
native HUD mengambilnya dari `WorldState`.

### Layout PlayerInit (139) — paket terbesar saat join

```
1 bit  EnableZoneNames
1 bit  UsePlayerPedAnims
1 bit  AllowInteriorWeapons
1 bit  UseLimitGlobalChatRadius
float  LimitGlobalChatRadius
1 bit  EnableStuntBonus
float  SetNameTagDrawDistance
1 bit  DisableInteriorEnterExits
1 bit  DisableNameTagLOS
1 bit  ManualVehicleEngineAndLights
u32    SetSpawnInfoCount
u16    PlayerID
1 bit  ShowNameTags
u32    ShowPlayerMarkers
u8     SetWorldTime
u8     SetWeather
float  SetGravity
1 bit  LanMode
u32    SetDeathDropAmount
1 bit  Instagib
u32    OnFootRate, InCarRate, WeaponRate, Multiplier, LagCompensation
u8     ServerNameLen + char[] ServerName
u8[212] VehicleModels
u32    EnableVehicleFriendlyFire
```

Catatan: `VEC3` / `float` di open.mp ditulis apa adanya (32 bit per komponen),
bukan float terkompresi seperti client 0.3.7 legacy.
