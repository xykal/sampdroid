# Arsitektur port native SampDroid

```text
Android shell (Kotlin)
 ├─ server browser / native HUD / Android native input
 └─ JNI boundary
       │
Native client core (C++17, ARMv7 + ARM64)
 ├─ SA-MP query protocol
 ├─ RakNet 2.x codec                         ← deterministic test vectors
 │   ├─ bitstream / ACK / reliability
 │   └─ reliable receipt + split reassembly + inbound reliable-ordered gates
 ├─ Persistent session + RPC/world state
 ├─ Player + vehicle sync encoder/remote broadcast decoder
 ├─ bounded dialog StringCompressor decoder + reliable responder
 ├─ user-owned SAF fd runtime: IMG v2 → COL catalog / DFF mesh + TXD texture → IPL world
 └─ Android world/game bridge
       │
Native Android renderer backend
 ├─ Canvas radar + HUD + dialog presenter
 ├─ GLES 2.0 proxy geometry + copied IPL/dynamic DFF batches + bounded TXD overlays
 └─ user-owned game data only; no Windows/x86 runtime
```

## Prinsip port

- Client core tidak memanggil `samp.exe`, Wine, Winlator, atau plugin Windows.
- Network state mengubah packet server menjadi state aman (`player`, `spawn`,
  `vehicle`, `object`, `dialog`, dst.), bukan langsung menulis memory binary PC.
- Input native menghasilkan semantic action (`move`, `jump`, `aim`, `fire`) yang
  diterjemahkan oleh Android client sendiri.
- Renderer/world bridge menerima state native dan data milik pengguna; APK tidak
  membundel asset atau binary game. Android SAF dapat memilih folder GTA yang
sudah diekstrak, menemukan `data/gta.dat`/GTA3 IMG utama dan sampai empat
companion `models/*.img` read-only untuk lookup DFF/TXD, lalu batch COL/IDE/IPL
text serta resolver dinamis stock opsional `data/vehicles.ide`/`data/peds.ide`
yang tidak tercantum di `gta.dat`, `parent_streamN.ipl` `bnry` relevan, dan saat
pengguna meminta sync, catalog `.col` IMG yang dibatasi 256 member/32 MiB dengan
descriptor bounded;
native menduplikasi descriptor dan merakit runtime random-access/read-only.
Installer Kotlin yang terpisah dapat membaca ZIP lokal atau URL HTTPS milik
pemain ke staging SAF tujuan, memverifikasi/menginspeksi hasil ekstrak, lalu
baru meminta mount native read-only. Runtime native sendiri bukan extractor
RAR/EXE, repacker, atau host katalog/download asset GTA.

## Session yang sudah ada di `main`

`raknet_session` mempertahankan satu socket UDP yang dipilih pengguna setelah
`ClientJoin`, membawa message/order counter, gate receipt reliable bounded,
split reassembler, gate inbound `RELIABLE_ORDERED`, `WorldState`, ACK inbound,
ACK outbound, dan retry queue bounded. Saat idle lima detik tanpa
reliable outbound yang pending, session mengirim reliable transport keepalive
`ID_DETECT_LOST_CONNECTIONS`; inbound `ID_INTERNAL_PING` dibalas dengan
unreliable `ID_CONNECTED_PONG`. Android shell memanggil JNI ini dari Kotlin IO
coroutine; selama detail server terbuka, poll UDP berjalan otomatis dan
berhenti/disconnect saat dialog ditutup.

Fondasi encoder `OnFootSync` sudah ada: payload client→server yang diuji,
`UNRELIABLE_SEQUENCED` channel 0 (counter sequenced tetap terpisah dari
counter RPC reliable-ordered), rate gate `PlayerInit`, dan spawn/session safety gate. Spawn gate sekarang mencakup `RequestSpawn` → respons → `PlayerSpawn`
(52) → transport ACK, sementara `WorldState` menahan template spawn/posisi/vitals
lokal terverifikasi. Reducer juga menyimpan `ResetPlayerWeapons`/`GivePlayerWeapon`/
`SetPlayerAmmo`/`SetPlayerArmedWeapon` sebagai raw weapon-ID/ammo + armed fact bounded. Armed fact itu mengisi weapon
low-six-bits preview OnFoot/driver InCar dan, bila AIM ditahan dengan
weapon/ammo active known, dapat membangun Packet-203 `PlayerAimSync` source-verified dari transform
collision-grounded + heading/pitch native bounded. Packet-203 punya rate `WeaponRate`, gate
spawn/freeze/on-foot/kamera-server, dan tidak memilih senjata atau mengurangi
ammo. Ketika kandidat itu lolos, snapshot renderer boleh menampilkan view aim
orang-pertama/reticle dari yaw/pitch yang sama; itu tetap presentasi tanpa target.
Saat **AIM+FIRE** ditahan, bridge juga dapat membangun Packet-206 client 41-byte
`HitType=None` yang rate-capped dari origin kamera sudah tervalidasi dan weapon/ammo
server-known. Form tanpa `PlayerID` ini diaudit terhadap reader + generator NPC
open.mp commit `3d20f3dc`; `PlayerInit.LagCompensation=0` juga menahan sender,
sedangkan kompatibilitas live server legacy/anticheat tetap belum terbukti. Ia
memakai ID/offset netral; `HitPos` hanya boleh nol atau contact absolut dari
ray COL statik dekat, tanpa memilih target entity/raycast remote, mengklaim
damage/reward, atau mengurangi ammo. Server tetap menjadi otoritas aksi no-target
tersebut. Kebalikannya, decoder read-only Packet 203 menerima broadcast camera direction
34-byte hanya untuk entity yang sudah `PlayerStreamIn`, memvalidasi mode/front/
posisi/AimZ, lalu meneruskannya singkat sebagai segmen arah renderer tanpa
mengklaim projectile atau target. Packet 206 server-relay 43-byte juga hanya
menjadi cue arah pendek sementara setelah hit/weapon/vector tervalidasi; hit ID,
target, vital, ammo, dan damage tetap tidak masuk state renderer. Outbound lokal
hanya memiliki Packet-206 `HitType=None` dengan sentinel nol atau contact COL
world-only dekat—tidak ada cue target/entity impact. Decoder Packet 207 juga menerima broadcast
server→client hanya untuk entity yang sudah `PlayerStreamIn`; ia memvalidasi
bit-packed quaternion/percent/velocity serta field surfing/animation opsional lalu
mereduksinya ke state remote bounded. RPC `ApplyPlayerAnimation`/`ClearPlayerTasks`
(86/87) dan NPC `ApplyActorAnimation`/clear (173/174) juga punya reducer exact:
target `u16`, DynStr8 library/name cap source `15/23` byte, delta, empat bit,
dan time. Snapshot hanya membawa cue `ANIM` bounded ke Canvas/GLES; tidak ada
IFP/skeleton execution, mutation input/collision, atau sync lokal dari directive.
`SetPlayerSkin` (153) non-DL juga diparse
exact `u32 PlayerID | u32 Skin` dan hanya mengganti model retained (remote atau
lokal yang sudah punya template spawn), bukan transform/lifecycle atau actor
baru. `PlayerStreamOut`, ID lokal, dan sync orphan diabaikan. Vehicle mengikuti
lifecycle yang sama: RPC `StreamInVehicle` (164)
menciptakan proxy bounded; Packet 200 driver dan Packet 209 unoccupied hanya
memperbarui proxy yang sudah stream-in. Occupancy menyimpan `driverKnown`
terpisah dari `driverId`, sebab ID player `0` sah dan bukan sentinel kosong.
Packet-200 mengikat actor driver remote yang memang sudah streamed ke vehicle
itu; Packet-211 `PlayerPassengerSync` yang source-verified melakukan hal sama
untuk passenger dan memakai `VEC3 Position` wire sebagai anchor, tanpa menebak
offset kursi. Packet-207 on-foot, PlayerStreamOut/Quit, vehicle stream-out,
atau occupancy baru pemain melepas ikatan lama fail-closed. Packet-209 melepas
**hanya driver**—passenger Packet-211 yang masih valid tidak dihapus. Renderer
menahan ped proxy/DFF saat actor adalah driver atau passenger (nametag, bubble,
dan marker radar tetap beranchor pada transform tervalidasi), sehingga tidak ada
ped ganda menembus mobil. Local passenger yang sudah diberi RPC 70 seat nonzero
mengirim Packet-211 raw source-verified pada `InCarRate`, tetapi hanya memakai
transform vehicle server sebagai anchor—tidak ada offset kursi, collision,
atau seat prediction. Held AIM dapat memasang bit drive-by Packet-211 hanya
untuk Uzi/MP5/Tec-9 yang armed+berammo dari server dan passenger non-cuffed;
FIRE tetap key input dan tidak ada BulletSync target/damage. Gate
`UNRELIABLE_SEQUENCED` per channel membuang index stale (termasuk wrap 16-bit)
sebelum state disentuh. Reader
IMG v2/COL2-COL3, query lantai lokal, side sweep silinder, dan raycast statik
COL bounded (box/sphere/mesh, interior exact, jarak ≤256, kandidat/primitive
budget) sudah ada. Raycast itu belum diberi target entity; hanya contact statik
dekat yang dapat mengisi `HitPos` Packet-206 `HitType=None`, sehingga tidak
menjadi authority damage. Bersama parser
directive `gta.dat`, IDE placement-capable `objs`/`tobj`/`anim`
serta resolver model `cars`/`peds` untuk vehicle streamed, skin player remote,
dan NPC `ShowActor`, serta IPL text + strict `bnry`. NPC server memakai map ID
terpisah dari player-pool (maksimal 1.000 retained/64 export) dan tetap hanya
fakta visual—tidak dapat menjadi collision, interaction, target, atau authority
damage lokal. Karena root stock tidak memasukkan dua table dinamis itu
ke `gta.dat`, picker SAF secara read-only juga mencoba `data/vehicles.ide` dan
`data/peds.ide`; file yang tidak ada tetap no-op untuk root minimal.
Android memberi descriptor sebagai source; picker folder SAF memakai manifest
untuk mencari `data/gta.dat`/IMG utama lalu batch COL/IDE/IPL tree secara
transaksional, menemukan hanya member IMG
`parent_streamN.ipl` yang relevan, dan saat sinkronisasi eksplisit memuat catalog
`.col` IMG bounded untuk jalur stock yang tidak mendeklarasikan `COLFILE`.
`GameDataRuntime` merakit IMG member atau COLFILE eksternal, mapping IDE, dan
placement IPL menjadi catalog/spatial collision world. Ia juga memilih placement exterior dekat anchor, lazy-load DFF
portable dari IMG, menerapkan transform IPL, dan menyalin batch prelit ke GLES.
Sampai empat overlay material/TXD IDE yang cocok dapat ikut disalin pada
placement IPL, termasuk beberapa material berbeda dari satu DFF portable,
dengan vertex + total RGBA masing-masing jalur dibatasi 1 MiB pixel. Jalur
transform serupa juga menerima input object server, vehicle streamed, skin
player remote/local, serta NPC `ShowActor`, lalu mengembalikan sampai empat
overlay dinamis tersendiri. Instance
yang resolve ke texture immutable yang sama digabung dalam satu slot, sehingga
empat slot adalah cap material, bukan cap empat entity; absence/format TXD yang
tidak portable hanya meninggalkan batch prelit/proxy.
Kotlin memvalidasi dan membuat direct buffer sebelum GL, tiap jalur punya slot
texture sendiri, texture stale dihapus di thread GL, dan RGBA identik di-refresh
tanpa upload GPU ulang. Resolver `txdp` mencoba dictionary
child lalu parent eksplisit dengan graph cycle-safe, maksimal 16 hop dan empat
lazy-read TXD per refresh. Transform static DFF `FrameList`/`Atomic` portable
diterapkan bounded sebelum copy batch. Primary GTA3 IMG lalu maksimal empat
auxiliary IMG asset read-only diperiksa berurutan; ini membuat relasi
`txdcut.ide` stock cutscene→gta3 dapat resolve tanpa copy file. Full-COL sync
bounded kini juga menggabungkan member `.col` primary + auxiliary dalam satu
cap, dengan primary menang jika nama model bertabrakan; discovery `parent_stream`
tetap primary-only. Binding material override/arbitrary DFF→TXD, skin/animasi,
LOD/occlusion, dan map draw bertexture penuh masih berikutnya. Preview DFF manual tetap mendukung
satu texture TXD yang ter-resolve unik secara aman. `local_gameplay_bridge`
meminjam `CollisionWorld` read-only selama satu tick surface dunia Android
(radar/GPU), reset dari SetSpawnInfo/SetPlayerPos authoritative, lalu hanya
menghasilkan Packet-207 bila floor COL pada interior authoritative yang tepat valid.
Gravity `PlayerInit`/`SetPlayerGravity` (146) yang source-verified ikut diskalakan
pada pilot vertikal default 0,008 hanya dalam envelope 0,001–0,020; nilai server
lain atau gravity belum known menghentikan prediction/on-foot sync fail-closed,
bukan diganti gravity lokal tebak. Untuk seat driver 0 yang
sudah diotorisasi `PutPlayerInVehicle`, bridge terpisah mereset dari transform
`StreamInVehicle`, menjalankan pilot kinematic gas/mundur-steer-rem terhadap
floor COL dan sweep statik, lalu membangun Packet-200 client-originated tepat
64 byte. Session tetap menjadi satu-satunya sender USeq channel-0 rate-gated
(dengan counter sequenced terpisah dari RPC ordered; `OnFootRate`/`InCarRate`); freeze, world collision kosong, server velocity,
stream vehicle hilang, dan input/state invalid berhenti fail-closed. Passenger
menggunakan sender Packet-211 terpisah tanpa prediction; koreksi posisi/velocity
atau mutator vehicle server membuang prediction driver tanpa memutasi
`WorldState`. RPC `SetPlayerInterior` kini masuk ke snapshot: batch IPL/DFF
statis serta reset/floor/sweep collision memilih ID interior yang tepat
(exterior = 0). Setelah transisi, tick on-foot/driver hanya lanjut bila floor
COL matching benar-benar ada; geometry dinamis tanpa metadata interior tetap
disembunyikan fail-closed. `SetWorldBounds` (17) receiver-specific juga
menahan hanya prediction exterior pada rectangle maxX/minX/maxY/minY server;
ia tidak menjadi force push-back, perubahan COL, atau batas interior. `SetPlayerVirtualWorld` (48) non-DL juga disimpan
sebagai `i32` server-routing/HUD fact, tetapi sengaja tidak disamakan dengan
interior atau dijadikan filter collision/renderer lokal: server-lah yang
menentukan entity mana yang di-stream untuk recipient tersebut. Packet-210
remote sekarang boleh memperbarui transform/attitude visual trailer yang sudah
streamed, tetapi tidak memberi coupling atau sender/pilot trailer lokal.
Handling/suspension/damage GTA, drive-by, outbound/local trailer,
eviction/streaming COL sekitar player untuk budget perangkat rendah, dan
validasi server nyata masih belum ada.

`WorldSnapshot` menyalin maksimal 64 player remote (termasuk cue RPC-166
`WASTED` observer-only), 64 NPC server `ShowActor`, dan 64 vehicle, 128 object
server/pickup, 64 label 3D, 64 map icon, 64 GangZone,
sample endpoint kamera server, marker, vitals, chat tail, lingkungan
waktu/cuaca (termasuk clock recipient RPC 29/30), directive audio RPC 41/42,
state interior server, dan dialog server ke JNI. `NativeAudioStreamController`
memvalidasi ulang HTTPS dan mengikat `MediaPlayer` ke lifecycle dialog/activity;
ia tidak memiliki socket atau write path gameplay. NPC snapshot
memakai ID namespace terpisah, stable-order, dan hanya model/transform/health/
invulnerable yang telah divalidasi—bukan state player atau interaksi. Semua adalah copy immutable/bounded; tidak ada pointer
entity atau socket yang menyeberang ke Android. Reducer menerima layout legacy/extended `CreateObject`
(44), `SetObjectPos` (45), `SetObjectRotation` (46), `MoveObject` (99),
`StopObject` (122), dan `DestroyObject` (47) secara bounded. Layout Move/Stop
42/2-byte diaudit terhadap writer open.mp; Move diinterpolasi hanya ke snapshot
monotonic client. Attachment `CreateObject` modern dipertahankan dengan urutan
parent vehicle→object yang diaudit writer lalu di-resolve ke frame secara
bounded (maksimal 8 level, yaw parent yang tersedia, `SyncRotation`, tanpa
mengarang child bila parent hilang/stream-out/siklik). `AttachObjectToPlayer`
(75) juga hanya mengikat object global existing ke parent player local/remote
streamed yang punya transform; fidelity pitch/roll vehicle/ped penuh dan material
override tidak ditebak. `CreatePickup` (95) / `DestroyPickup` (63) menyimpan record global
24/4-byte bounded; tap **USE** yang eksplisit saja boleh memilih pickup dalam
radius 2,15 unit lalu mengantrikan `PickedUpPickup` (131) reliable. Gate spawn
ACK, controllable, exterior/interior, jarak, serta interval 450 ms semuanya
diperiksa native; tidak ada reward/inventory yang dipalsukan lokal. Canvas
Android menggambar radar/peta top-down; GLES 2.0 menggambar grid perspektif,
proxy kubus actor dan prisma vehicle, marker checkpoint/pickup, sky generated
dari waktu/cuaca, HUD, kamera orbit/zoom, batch DFF prelit placement exterior,
dan bila ID objek server, vehicle streamed, skin player remote/local
(cetak spawn `SetSpawnInfo` terlebih dahulu untuk local; `SetPlayerSkin` kemudian
boleh mengganti modelnya), atau NPC `ShowActor` cocok dengan IDE/IMG user, batch
DFF prelit static/dynamic dapat ditampilkan; sampai empat material TXD portable
untuk subset dinamis itu juga dapat overlay pada transform yang sama. Batch
object/vehicle/player/NPC dinamis refresh terpisah dan dibatasi, sementara proxy
authority tetap fallback bila asset atau material tidak tersedia. Renderer tidak membaca socket atau descriptor/asset game
pada GL thread; semua snapshot dan vertex batch tiba sebagai copy bounded.
Proxy, GLES tanpa
texture-map penuh, dan actor/vehicle generated geometry tetap bukan klaim visual
GTA PC lengkap.

Presentation RPC tambahan tetap melewati reducer native sebelum renderer:
`Show/Hide/FlashGangZone` menyimpan rectangle ABGR dengan cap 1.024/64 export;
Canvas meng-clip rectangle dan hanya memphase flash yang sudah diperintah server—
tidak ada capture/ownership lokal. `PlayerShow/HideTextLabel` mendecode default
RakNet StringCompressor secara bounded, lalu Kotlin resolve offset attachment
terhadap actor/vehicle pada frame yang sama; attachment tak dikenal dibuang,
bukan diposisikan di origin. Scene GLES menghitung proyeksi anchor, dan HUD Canvas
menggambar teks portable di overlay yang sama. `Set/RemovePlayerMapIcon` menyimpan
100 slot RGBA dan menggambar diamond radar/beacon GLES tanpa mengklaim atlas
sprite GTA. `SetPlayerCameraPosition`, `LookAt`, `InterpolateCamera`, dan
`BehindPlayer` menjadi `CameraView` sampled read-only: endpoint GTA dikonversi
ke koordinat GLES dan memenangi fallback touch hanya selama endpoint server
known. Tidak satu pun jalur presentation ini mengubah collision, posisi lokal,
atau sender Packet-207/200. LOS label dan font/billboard/sprite GTA penuh tetap
belum ada.

Vehicle stream/proxy tetap server-authoritative, tetapi edge **USE** dapat
membangun request `OnPlayerEnterVehicle` 26 atau `OnPlayerExitVehicle` 154 dari
vehicle streamed terdekat; state seat baru berubah dari RPC server
`PutPlayerInVehicle` 70 / `RemovePlayerFromVehicle` 71. Saat `inVehicle`,
Packet-207 dihentikan fail-closed. Seat 0 saja yang dapat menghasilkan
`PlayerVehicleSync`/Packet-200 raw sesudah recheck session, stream, seat, dan
spawn ACK. Seat nonzero menghasilkan `PlayerPassengerSync`/Packet-211 raw yang
terpisah pada `InCarRate`, memakai anchor vehicle server serta tanpa prediction;
passenger tidak pernah mengirim packet driver. Prediction snapshot milik driver
dapat mengganti transform proxy lokal sampai koreksi server datang, tetapi ini
masih pilot kinematic, bukan vehicle physics GTA. `ShowDialog` (61)
didecode native dengan tree default RakNet
`StringCompressor` yang bounded sebelum melewati JNI. Presenter Android dapat
mengirim response 62 lewat reliable queue hanya bila ID **dan revision** dialog
masih cocok; row Tablist tetap mempertahankan tab untuk `inputText` kompatibel.
Touch pad runtime memetakan intent Android ke input/heading `OnFoot` lokal dan
pitch AimSync bounded; scene GPU **dan radar fallback** menjalankan tick maksimal 50 ms. Keyboard
Bluetooth dan controller Android masuk router semantic yang sama (router
menggabungkan hold touch/hardware, dead-zone stick, dan look relatif tanpa
menulis RakNet); `T`/`/`/Start dan Tab hanya membuka chat/score UI native.
Editor LAYOUT hanya menyimpan koordinat/scale overlay lokal dan
menyalin JSON data-only; ia tidak mengubah action semantics atau membuka loader
file/script. Tick itu tidak menerima posisi dari input: ia hanya mengirim
kandidat collision-grounded melalui gate spawn/ACK dan rate server.
`TogglePlayerControllable` menghentikan serta mengosongkan semua source input.
Gameplay loop ini masih pilot exterior/driver/passenger yang belum
live-validated; reconnect/full congestion handling, map/asset bridge penuh,
TextDraw/GameText sprite/model/font/animasi penuh, physics/handling kendaraan
GTA, dan gameplay penuh masih belum ada.
Transport keepalive bukan player sync. Karena itu session surface tetap diberi
label eksperimen dan tidak boleh dipasarkan sebagai renderer/playable client.

`ShowTextDraw` / `HideTextDraw` / `TextDrawSetString` sekarang direduksi dari
header wire source-audited ke maksimal 2304 record dan maksimal 32 copy per
frame. HUD Canvas Android hanya menggambar proxy text/box/warna/alignment dasar,
bukan font atau sprite GTA; waktu server mengaktifkan selection, hanya hit-box
record yang masih selectable dapat mengirim RPC 83 melalui recheck session
native. Ini menutup jalur UI server dasar tanpa mempercayai ID dari UI, tetapi
belum kompatibilitas TextDraw visual penuh. `SendGameText` (73) juga direduksi
secara exact dari `u32 style | u32 time | DynStr32`, dibatasi 64 byte dan waktu
monotonic native; Android menampilkan banner Canvas portable sampai durasi
berakhir. Gaya, font, colour animation, dan efek frontend GTA asli tidak
diklaim.

## Batas layer

| Layer | Boleh | Tidak boleh |
|---|---|---|
| `raknet_codec` | parse/serialize bytes, bounds checks, test vector | UI, asset IO |
| `rpc_decoder` / `sync_decoder` / `rpc_state` | decode RPC/broadcast menjadi state | hook/execute binary Windows |
| session | lifecycle, ACK/retry, ordered RPC, sync | block UI thread |
| Android UI | HUD, browser, input event | socket blocking di main thread |
| world bridge | render state dengan backend Android | bundle/download proprietary game asset |
| native input layout | posisi/action tombol internal | script/plugin/executable loader |

## Kontribusi berdampak besar

1. Persistent session, class/spawn response, player sync, dan teardown.
2. Decoder/state player, vehicle, object, pickup, dialog, textdraw, HUD.
3. Native touch/gamepad/keyboard/gyro mapper.
4. Renderer/world bridge yang portable dan ramah ARMv7.
5. Capture test yang disanitasi, docs, performance profiling, dan accessibility.
