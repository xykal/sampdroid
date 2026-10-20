# Native RakNet session lifecycle

Dokumen ini menjelaskan session client Android native SampDroid setelah `ClientJoin`.
Ini bukan wrapper `samp.exe`, bukan Wine/Winlator, dan bukan runtime modpack PC.

## Batas implementasi saat ini

```text
user pilih server
  → native UDP socket
  → open/cookie/auth/accepted
  → NEW_INCOMING_CONNECTION → internal ping → empty static-data → ClientJoin
  → socket + message counters + gate receipt reliable + split reassembler + gate reliable-ordered TETAP hidup
  → poll datagram / ACK reliable / retry bounded / reply ping/static-data request / reduce RPC world state
  → reliable transport keepalive otomatis saat idle
  → `PlayerInit` valid + class tersedia → core meminta class awal `#0` secara otomatis
  → user dapat memilih class lain lalu meminta spawn secara eksplisit
  → respons spawn nonzero → core kirim RPC PlayerSpawn (52) → tunggu ACK
  → state spawn/posisi lokal direduce dari RPC server
  → touch/keyboard/gamepad → semantic input lokal
  → scene GPU atau radar menjalankan gameplay tick collision-gated
  → core mengirim OnFoot/driver/passenger/AimSync hanya pada rate dan gate server yang valid
```

Tombol **Spawn** sekarang menjalankan handshake native eksplisit, bukan spawn
otomatis saat connect. Ia tetap bukan kontrol gameplay atau renderer.

### Kontrol RakPeer sesudah accept

Untuk parity source offline dengan jalur client RakPeer 2.52 yang diaudit,
setelah `ID_CONNECTION_REQUEST_ACCEPTED` core mengirim, dalam urutan transport,
`ID_NEW_INCOMING_CONNECTION` reliable, `ID_INTERNAL_PING` unreliable dengan
RakNetTime lokal, lalu `ID_RECEIVED_STATIC_DATA` reliable dengan payload lokal
kosong. Core lalu menunggu `ID_INTERNAL_PING` atau `ID_RECEIVED_STATIC_DATA` dari
server sebagai bukti source-defined bahwa server sudah memproses NEW_INCOMING,
baru mengirim `ClientJoin` pada SyncRPC channel 2. Semua reliable sejak
`CONNECTION_REQUEST` dicatat pada antrean kecil ACK/retry (750 ms, maksimal lima
retransmit), dan record yang belum di-ACK dipindahkan ke session retained—tidak
hilang di batas probe/session.
`ID_REQUEST_STATIC_DATA` dari peer dibalas payload kosong yang sama; static data
peer tidak diperlakukan sebagai data GTA/world. Ini berasal dari perilaku
`RakPeer::PingInternal`/`SendStaticDataInternal`, bukan tebakan format payload.

Parity ini sudah punya vector regresi host. Probe native SampDroid mengonfirmasi
accepted, readiness post-accept, dan ACK transport untuk ClientJoin UIF; probe
RakNet referensi yang read-only dengan `ClientJoin` R2/channel 2 menerima
`PlayerInit` UIF tanpa class/spawn. Pada 2026-10-20, satu sesi SampDroid native
baru yang dibatasi (tanpa spawn, sync, chat, command, atau input gameplay)
berakhir pada `unexpected reply` sebelum accepted/`PlayerInit`; sesi lokal
kemudian dibersihkan dan tidak ada klaim class/spawn. Hasil itu bukan regresi yang
dapat disimpulkan sendiri maupun bukti gameplay SampDroid: drain bootstrap,
class/spawn, sync, dan perilaku client-facing masih perlu validasi APK/device
yang terukur.

## Static GTA world filter dari server (RPC 43)

Sesudah reducer menerima `RemoveBuildingForPlayer` (`RPC 43`), directive
`u32 model | VEC3 center | float radius` yang exact dan bounded diterapkan
kepada **placement static** GTA milik user untuk recipient session itu. `u32`
`0xFFFFFFFF` mempertahankan arti Pawn `modelid = -1`: hapus semua model static
di radius, bukan model ID baru. Record pendek/bertail, float non-finite, atau
range tidak aman ditolak; duplikat tidak tumbuh dan maksimum 1.000 directive
retained.

Satu filter yang sama berjalan di renderer IPL/DFF prelit, overlay IPL/TXD,
dan collision world COL: query floor grounding, sweep on-foot/driver, dan ray
static Packet-206 no-target semuanya mengabaikan placement yang matching.
Saat directive berubah, retained session juga membuang prediksi on-foot/driver
sebelum tick berikutnya; client tidak boleh mengirim langkah yang baru saja
tervalidasi terhadap lantai/dinding yang sudah dihapus server. Artinya native
tidak akan memperlihatkan gedung yang sudah server remove sambil masih
membenturkannya ke player. Ini **tidak** menghapus/mengedit data GTA di
SD card, dynamic `CreateObject`, vehicle, player, pickup, atau asset lain; bila
model/placement belum ada di data user, path fail-closed tanpa proxy geometry.
Directive bersifat session-local dan ikut hilang saat teardown/reset `WorldState`.

## Recipient-local vehicle parameters (RPC 24)

`SetVehicleParams` memakai RPC `24` dengan body fixed **18 byte**:
`VehicleID:u16` diikuti enam belas byte parameter legacy dalam urutan `engine,
lights, alarm, doors, bonnet, boot, objective, siren, driver/passenger/back-left/
back-right door, driver/passenger/back-left/back-right window`. Byte wire dibaca
sebagai signed `int8`: `0xFF` berarti `-1` (**unset**), sementara hanya `0` dan
`1` yang valid sebagai nilai lain. Body pendek/bertail, ID nol, atau byte lain
ditolak atomik.

Audit writer open.mp ([vehicle.cpp](https://github.com/openmultiplayer/open.mp/blob/master/Server/Components/Vehicles/vehicle.cpp)) menetapkan `this->params = params` lalu selalu menulis
**seluruh** struct `VehicleParams`; [Pawn vehicle natives](https://github.com/openmultiplayer/open.mp/blob/master/Server/Components/Pawn/Scripting/Vehicle/Natives.cpp)
memperlihatkan `SetVehicleParamsEx` mengisi setiap field dari argumennya dan
`SetVehicleParamsForPlayer` mengirim full struct global dengan override recipient.
Karena itu satu RPC 24 adalah replacement snapshot:
field `-1` membersihkan state retained ke unset, bukan delta/no-change. Ini
memastikan engine/lock lama tidak bertahan setelah reset server. Reducer hanya
menerima target `StreamInVehicle`; ia tidak dapat membuat vehicle atau menebak
raw ID. State parameter juga dibersihkan pada `StreamOutVehicle` **dan**
`StreamInVehicle` baru untuk raw ID yang sama. Perubahan snapshot menerbitkan
event `VehicleChanged` bertanda parameter; karena RPC ini tidak membawa
transform/velocity, pilot driver tidak disnap ulang—tick berikutnya saja
mengonsumsi gate engine manual yang baru.

UI driver menampilkan hanya fakta `MESIN`, `LAMPU`, dan `SIREN` yang eksplisit
dari server. Bit `ManualVehicleEngineAndLights` PlayerInit juga diretain: hanya
bila bit itu aktif pilot kinematic menghormati `engine=0` dengan menahan throttle
lokal/outbound sambil membolehkan coast, steer, dan brake. Pada server automatic
bit-off, engine/lights tetap fakta visual tetapi tidak menjadi pembatas drivetrain
Android yang ditebak. `siren=1` disalin ke Packet-200 outbound hanya untuk exact
current streamed driver vehicle. Tidak ada toggle Android untuk engine/lights/siren,
tidak ada door/alarm/lampu visual GTA atau unlock/damage/system kendaraan lokal
yang diinvent; door-lock eksplisit server hanya boleh menggate request masuk
seperti dijelaskan di bawah. Field lain tetap retained sebagai state kompatibilitas dan dapat
mendukung renderer/physics nyata kelak bila ada kontrak GTA user-data yang
terverifikasi.

### Legacy recipient objective/door lock (RPC 161)

Legacy server paths use separate RPC `161` for `SetVehicleParamsForPlayer`,
with exact body `VehicleID:u16 | objective:u8 | doorsLocked:u8`. (Current
open.mp instead emits a full recipient RPC 24 snapshot; both forms are accepted
for compatibility.) Both RPC-161 trailing fields are strict booleans `0/1`,
never a signed unset value. It updates only the retained `objective` and `doors`
slots for a currently streamed vehicle, preserving any engine/lights/siren facts
established by the most recent RPC 24. A duplicate produces no fake
transform update; malformed/unknown/unstreamed targets fail closed, and normal
stream lifecycle clears it with all other parameter facts.

The Android radar renders a portable yellow diamond and `OBJ` label for an
explicit objective; GLES renders a small yellow marker above the vehicle rather
than claiming an exact GTA arrow asset. An explicit `doorsLocked=1` also blocks
the local USE → `EnterVehicle` request for this recipient, matching server
lock authority. It does not synthesize an unlock, seat, animation, or optimistic
entry: server RPC 70 remains the sole seat authority.

## Recipient-local vehicle velocity (RPC 91)

`SetVehicleVelocity` memakai RPC `91` dengan body exact **13 byte**:
`Type:u8 | Velocity:VEC3 raw-f32`. Writer open.mp menetapkan `Type=0` sebagai
linear dan `Type=1` sebagai angular; tidak ada `VehicleID` pada wire. Karena
itu reducer hanya menerima directive ketika recipient lokal sudah diberi
`PutPlayerInVehicle` seat `0` dan proxy ID tersebut masih `StreamIn`. Ia tidak
memilih kendaraan terdekat/terakhir, tidak menarget passenger atau vehicle
unoccupied, dan menolak keluarga rail GTA (`449, 537, 538, 569, 570, 590`) sesuai
semantik SA-MP bahwa native velocity tidak memengaruhi train. Selector selain
`0/1`, payload bertail/pendek, NaN/Inf, vector melewati envelope sync, atau
context yang tidak cocok ditolak atomik tanpa mengubah state.

Channel linear dan angular disimpan sendiri beserta revision monotonik; command
angular tidak menghapus vektor linear sebelumnya. Stream-out atau `StreamIn`
baru untuk raw ID yang sama membersihkan directive sehingga ID reuse tidak dapat
mewarisi force lama. Perubahan valid juga membuang prediksi driver yang sedang
berjalan pada batas reducer, sebelum tick berikutnya dapat membuat Packet-200
stale.

Untuk channel **linear** yang wajar bagi pilot saat ini (magnitudo sampai `3.0`
dalam unit CVector GTA), bridge mengonsumsi masing-masing revision satu kali
setelah reseed transform authoritative. Nilai CVector GTA dikonversi pada batas
terpusat 50-Hz ke world-unit/detik, lalu horizontal/lateral dan launch-Z berjalan
melalui floor/sweep COL yang sama dengan driver. Packet-200 berikutnya
mengonversi kembali velocity ke CVector GTA. Bila COL tidak cukup, body bernilai
ekstrem untuk pilot, atau bounds server dilanggar, path fail-closed alih-alih
membuat rute/teleport bebas. Reset `SetVehiclePosition` untuk vehicle yang sama
tidak memutar ulang revision lama.

Channel **angular** tetap direduce dan disimpan sebagai fakta server, tetapi
belum diubah menjadi yaw/pitch/roll: port ini belum mempunyai physics/orientasi
kendaraan GTA tiga-sumbu. Ini disengaja agar `turn` tidak menjadi rotasi lokal
palsu. Jadi increment ini mendukung jump/boost linear collision-bounded tanpa
mengklaim suspension, damage, air/boat/train, atau rotasi GTA lengkap.

Untuk throttle/steer biasa, pilot driver sekarang dapat memakai **envelope
kinematic data-informed** bila user sudah memasang `data/handling.cfg` valid dan
sinkronisasi `data/vehicles.ide` menemukan token handling untuk model vehicle
server saat ini. Reader hanya mengizinkan profile bounded lalu menurunkan
maksimum speed, accel/reverse, brake/handbrake, coast/lateral settle, dan steering
rate ke envelope Android; profile disalin dari runtime read-only dan session
mencocokkan model lagi tepat sebelum dipakai. Missing/invalid/stale profile selalu
kembali ke constants generic dan tidak pernah memblokir seat server atau
memaksa transform. Ini **bukan** full GTA handling: tidak ada wheel/suspension,
gearbox, traction/contact physics, tyre/damage, boat/air/train behavior, atau
angular vehicle simulation. Semua outcome masih melewati same COL/bounds/rate
Packet-200/correction server gate.

Terpisah dari RPC 91, **Packet-200 inbound** membawa quaternion compressed
untuk transform vehicle remote. Decoder menormalisasi dan mendekomposisi inverse
quaternion ke Euler GTA `X → Y → Z` sebelum DFF user-owned dirender, sehingga
roll/pitch pesawat atau kendaraan yang terbalik tidak lagi dibuang menjadi yaw
proxy. Konvensi inverse mempertahankan heading kompatibel yang sudah ada
(`rotationZ == angle` untuk vehicle level). Packet-209 tidak punya quaternion
melainkan vector roll/direction; ia mempertahankan attitude Packet-200 terakhir,
dan `StreamInVehicle`/`SetVehicleZAngle` kembali ke fallback yaw-only. Semua ini
**visual remote transform**: tidak memberi pilot lokal suspension, angular
physics, collision baru, atau authority Packet-200 outbound.

## Fondasi outbound OnFootSync

Core mempunyai `raknet_session_send_onfoot()` untuk state lokal tervalidasi.
Pad diagnostik saja hanya mengisi **preview** melalui
`raknet_session_update_controls()` dan tidak memanggil API kirim. Di scene GPU
atau radar, gameplay tick native yang sama baru dapat membangun/mengirim
kandidat grounded secara periodik. Ia hanya boleh mengirim setelah semua syarat
berikut terpenuhi:

1. UDP session aktif dan `CONNECTION_REQUEST_ACCEPTED` sudah diterima;
2. `PlayerInit` valid sudah diterima;
3. server sudah mengirim respons spawn nonzero, client sudah mengirim RPC
   `PlayerSpawn` (52), **dan** transport sudah menerima ACK-nya;
4. posisi, quaternion, velocity, dan surfing offset lolos batas finite/world
   yang sama dengan reader server;
5. `TogglePlayerControllable` tidak sedang freeze, dan jika health sudah known
   nilainya **bukan** nol; `SetPlayerHealth(0)` langsung membuang held input,
   prediction, dan semua kandidat Packet-207/200/203/211 sampai server
   mengembalikan health valid atau handshake respawn berikutnya selesai;
6. world collision user-owned pada ID interior server yang sama menghasilkan
   langkah grounded yang valid;
7. interval `OnFootRate` dari `PlayerInit` sudah jatuh tempo.

Packet memakai `UNRELIABLE_SEQUENCED` (reliability `7`), ordering
**SyncPacket channel `1`**, dan counter sequenced channel-1 yang **terpisah**
dari counter **SyncRPC reliable-ordered channel `2`**. RakNet memang menyimpan
write-index ordered dan sequenced pada array berbeda; nilai rate `0`/abnormal
dibatasi client ke 20–1000 ms (default 30 ms)
agar server malformed tidak dapat membuat client mem-flood atau freeze terlalu lama.

Penting: layout client→server packet `207` berbeda dari layout broadcast
server→recipient. Encoder mengikuti reader `PlayerFootSync` server: body fixed
69 byte (ID + left/right/up/down/keys raw, position, quaternion WXYZ raw,
health/armour raw, weapon/action, velocity, surfing data, animation). Ia sengaja
**tidak** memakai writer broadcast yang berisi player ID, field conditional, dan
kompresi berbeda. Vector regresi host mengunci kedua layout transport dan payload.

## Outbound PlayerAimSync (Packet 203) terbatas

Saat player **on-foot**, tombol semantic **AIM** sedang ditahan, collision
native sudah menghasilkan transform grounded, dan server sudah memberi
`SetPlayerArmedWeapon` **serta** ammo untuk weapon aktif, tick gameplay juga
boleh membangun Packet `203` (`PlayerAimSync`). Ini dikirim
`UNRELIABLE_SEQUENCED` pada SyncPacket channel `1`, menggunakan ordering counter
sequenced yang sama tetapi tetap terpisah dari counter SyncRPC ordered channel `2`
serta memiliki cadence tersendiri dari `WeaponRate` PlayerInit (dibatasi 20–1000
ms, default 30 ms). Ia tetap memerlukan gate session/spawn/ACK yang sama dengan
OnFootSync dan berhenti saat freeze, masuk vehicle, atau kamera endpoint sedang
dikendalikan RPC server.

Wire writer adalah fixed **32 byte / 256 bit**, source-verified terhadap writer
NPC open.mp yang kompatibel:

```text
203 | CamMode:u8 | CamFront:vec3 raw f32 | CamPos:vec3 raw f32 |
      AimZ:f32 | (CamZoom:6 | WeaponState:2):u8 | AspectRatio:u8
```

`CamZoom` memakai bit 0–5 dan `WeaponState` bit 6–7. Builder memvalidasi front
vector nonzero dengan kuadrat panjang `< 1.5`, finite/world camera position,
mode kamera compatible, pitch, dan field packed sebelum serialisasi. Mode
normal adalah 53; sniper/camera/rocket/heat-seeker memakai mode 7/46/8/51
sesuai pemetaan compatible. Weapon state (`0/1/2`) berasal dari ammo server
known saja—tidak dari tombol FIRE atau counter lokal.

Posisi kamera kandidat berasal dari transform on-foot collision-grounded yang
memang dimiliki port ini plus eye-height presentasi tetap. Drag kanan/tongkat
kanan memberi yaw dan pitch **relatif** yang dibatasi: heading tetap sumber arah
horizontal Packet-207, sedangkan pitch native `-75..75°` menghasilkan front-Z
dan `AimZ` compatible (`AimZ = -pitch` radians). Zoom dan aspect ratio masih
netral (0), karena belum ada bridge zoom/aspect camera authoritative. Dengan
Path aim ini tidak memilih target, raycast, mengurangi ammo, atau mengklaim
hit/damage/reward. Saat semantic **AIM+FIRE** sama-sama held, ia dapat menjadi
basis Packet-206 no-target terbatas yang dijelaskan di bawah; endpoint kamera
server tetap menahan seluruh send fail-closed sampai bridge kamera authoritative
tersedia.

## Outbound local PlayerBulletSync no-target (Packet 206)

Saat dan hanya saat on-foot collision bridge, spawn ACK, `AIM`, `FIRE`, weapon,
ammo, freeze/death/vehicle, kamera-server, **dan** `PlayerInit.LagCompensation`
server nonzero gate semuanya valid, native dapat mengirim form client
**41 byte / 328 bit** berikut pada SyncPacket channel-1
`UNRELIABLE_SEQUENCED` (counter sequenced tetap terpisah dari SyncRPC channel-2). Layout client ini sudah diaudit terhadap
`PlayerBulletSync::read` open.mp commit `3d20f3dc0e26917264cdc8585c65913e080a143c`
dan generator NPC-nya; handler open.mp yang sama menolak Packet-206 bila
`LagCompensation == 0`, sehingga Android fail-closed dan HUD menyatakan
`lagcomp server OFF`. Interoperabilitas live dengan server legacy/anticheat lain
tetap belum divalidasi:

```text
206 | HitType=0:u8 | HitID=0:u16 | Origin:vec3 raw f32 |
      HitPos=zero-or-close-static-COL:vec3 raw f32 | Offset=(0,0,0):vec3 raw f32 |
      WeaponID:u8
```

Tidak ada `PlayerID` dalam form client (reader server mendapatkan peer dari
session); form relay server→recipient tetap 43 byte karena writer relay membawa
ID itu. Pada open.mp audited, `HitType=0` masuk jalur callback shot-missed,
bukan damage target. Android selalu mengirim `HitID` dan `Offset` netral; `HitPos`
bernilai nol bila tidak ada contact, atau hanya dapat menjadi koordinat absolut
dari ray COL statik dekat (maksimal 20 unit) pada data GTA user. Resolver itu tidak melihat
player/vehicle/server object dan tidak boleh menentukan target entity, damage,
reward, atau decrement ammo. Origin berasal dari pose kamera Packet-203 yang
collision-grounded, weapon/ammo hanya dari RPC server, dan cadence dibatasi per
keluarga weapon. Jadi ini adalah aksi tembak yang masih divalidasi server,
**bukan** implementasi hit/damage GTA lengkap. Jika relay remote juga membawa
sentinel no-impact, renderer tidak menggambar garis palsu menuju world origin.
HUD lokal hanya mem-pulse `COL dekat (lokal)` setelah socket-send menerima form
dengan contact COL tersebut; pulse itu bukan ACK server, hit entity, atau damage.

## Inbound remote PlayerAimSync (Packet 203)

Packet `203` broadcast server→recipient memakai player-ID prefix sehingga tepat
**34 byte / 272 bit**:

```text
203 | PlayerID:u16 | CamMode:u8 | CamFront:vec3 raw f32 | CamPos:vec3 raw f32 |
      AimZ:f32 | (CamZoom:6 | WeaponState:2):u8 | AspectRatio:u8
```

Native hanya menerima frame lengkap dengan mode compatible, camera position
finite di envelope dunia, `AimZ` `[-pi, pi]`, dan front vector nonzero dengan
kuadrat panjang `< 1.5`. Ia juga menolak ID lokal serta actor yang belum
`PlayerStreamIn`, jadi Packet-203 tidak dapat membuat entity baru atau mengubah
posisi/vehicle state. Snapshot renderer membawa arah kamera itu selama maksimum
1,2 detik dan scene hanya menggambar segmen arah pendek dari posisi kamera
server—bukan peluru, raycast, target, hit marker, atau damage feedback.

## Inbound remote PlayerBulletSync relay (Packet 206)

Packet `206` yang **sudah direlay server** juga didecode sebagai fixed **43 byte /
344 bit**:

```text
206 | PlayerID:u16 | HitType:u8 | HitID:u16 | Origin:vec3 raw f32 |
      HitPos:vec3 raw f32 | Offset:vec3 raw f32 | WeaponID:u8
```

Reader menerima hanya hit type legacy `0..4`, weapon bullet compatible
`22..34` atau `38`, origin/offset finite dengan envelope server, serta hit-position
finite yang aman untuk renderer. Reducer mensyaratkan remote player sudah
stream-in, menolak ID lokal/orphan, dan **tidak menyimpan atau meneruskan HitID,
HitType, vital, ammo, target, atau damage** ke UI. Hanya sebuah arah dari origin
ke `HitPos` disimpan maksimal 220 ms lalu GLES menggambar segmen pendek tanpa
impact endpoint. Jadi ini visual cue dari packet server-relay, bukan BulletSync
lokal, raycast, hit claim, atau mekanisme damage port ini.

Path lokal hanya dapat mengirim form no-target yang dijelaskan di atas; tidak
ada Packet-206 target/raycast/hit/damage yang dipalsukan oleh Android.

## Handshake spawn dan state lokal

Setelah user menekan **Spawn**, urutan yang dipakai adalah:

```text
client → PlayerRequestSpawn (RPC 129, RELIABLE_ORDERED SyncRPC channel 2)
server → RequestSpawn response (legacy u8 atau open.mp u32, nilai nonzero)
client → PlayerSpawn (RPC 52 kosong, RELIABLE_ORDERED SyncRPC channel 2)
server → transport ACK untuk RPC 52
client → eligible untuk OnFootSync SyncPacket channel 1 (counter USeq terpisah dari RPC)
```

Nilai respons `2` juga diperlakukan sebagai izin spawn, sehingga jalur
`SpawnPlayer` server tetap mengikuti konfirmasi `PlayerSpawn` yang sama. Pending
RPC 52 diberi purpose terpisah; ACK lama tidak boleh membuka request spawn baru.

Request pertama juga diberi gate `spawnRequestPending`: tap **RESPAWN** di HUD
hanya dapat mengantrikan satu RPC 129 sampai respons server tiba. Jika respons
hilang setelah transport reliable sudah ACK, retry eksplisit baru boleh setelah
8 detik. Tombol tidak pernah menganggap request sebagai hidup/spawn berhasil;
ia hanya menampilkan menunggu respons server. Saat `SetPlayerHealth(0)` diketahui,
HUD menghentikan touch/hardware gameplay dan menawarkan request ini secara
explicit (kecuali spectator), tanpa mengirim death report, animasi, atau respawn
otomatis.

`WorldState` juga mereduksi data local-player yang aman untuk bridge berikutnya:
`SetSpawnInfo` (team, skin, spawn, weapon/ammo; loadout raw juga mengisi ammo
known tanpa memilih weapon aktif), `SetPlayerAmmo` compact, `SetPlayerPosition`/FindZ,
facing angle, health, armour, `Give/ResetPlayerMoney`, special action, dan velocity.
`SetPlayerSkin` non-DL (153) juga didecode tepat sebagai `u32 PlayerID | u32 Skin`:
ia mengganti model actor remote retained atau model lokal yang sudah memiliki template
spawn, tanpa menciptakan actor lokal di origin. Semua float finite, posisi dunia,
dan velocity dibatasi sebelum masuk state renderer/input. `SetPlayerVirtualWorld`
(48) juga diterima tepat sebagai `i32` state routing/HUD; ia **bukan** ID interior
GTA sehingga tidak mengubah partition collision/IPL atau menyaring entity secara
lokal—server tetap authority stream visibility. `PlayerInit` dan RPC
`SetPlayerGravity` (146) juga memberikan gravity GTA authoritative ke pilot
on-foot; default 0,008 dan custom 0,001–0,020 dipakai sebagai skala vertikal
bounded, sedangkan gravity unknown/di luar envelope menghentikan Packet-207
prediction aman. RPC 29 `SetPlayerTime(u8 hour,u8 minute)` dan RPC 30
`TogglePlayerClock(u8)` juga dipertahankan sebagai presentasi recipient-specific:
HUD/sky proxy memakai waktu lokal bila ada, sementara toggle hanya menyembunyikan
clock HUD—bukan physics/cadence. RPC 41/42 audio stream juga direduksi lebih dulu
sebagai directive/revision bounded; hanya URL HTTPS tanpa kredensial/host literal
lokal umum yang boleh mencapai `MediaPlayer`, lalu playback berhenti saat dialog/app
background ditutup. URL HTTP atau malformed tidak mengirim response ke server.
RPC 17 `SetWorldBounds` juga direduksi sebagai
`maxX,minX,maxY,minY`; hanya prediction exterior on-foot/driver yang ditahan pada
rectangle itu, tanpa memalsukan force push-back server, mengubah COL, atau
memaksakan bounds ke interior. Ini state native saja — belum menggambar ped atau
mengemulasi fisika GTA penuh. RPC 166 `PlayerDeath(u16 PlayerID)` dipakai hanya
sebagai cue `WASTED` **remote** maksimum 8 detik; broadcast normal open.mp
bukan konfirmasi death lokal, jadi tidak pernah menghentikan/respawn client
lokal dari RPC itu.

## Server NPC actor visual (RPC 171–178)

Jalur non-DL juga menerima `ShowActor` exact: `u16 actorId | u32 skin | VEC3 |
float angle | float health | u8 invulnerable`. Ini membuat record NPC server
terpisah dari `PlayerStreamIn`, maksimal 1.000 record retained dan 64 copy
render stable-ID. `HideActor` (`u16`) aman dipanggil berulang; mutator sudut,
posisi, dan health masing-masing memakai body exact 6/14/6 byte dan hanya boleh
menyentuh actor yang sudah muncul. Float/model/koordinat tetap tervalidasi dan
mutator orphan tidak membuat actor baru.

Snapshot JNI membawa NPC ini ke radar dan scene GLES, yang dapat memakai proxy
atau `peds` DFF user-owned bila resolusi model tersedia. Ini **murni visual**:
actor NPC tidak menjadi player identity, seat, collision, target/hit raycast,
interaksi USE, damage, atau source animation lokal.

RPC 86/87 untuk player dan RPC 173/174 untuk NPC kini mereduksi directive
animasi exact (`u16 target`, dua DynStr8 bounded, float delta, empat `BIT`,
`u32 time`) dan clear `u16`. SDK open.mp yang diaudit membatasi `AnimationData`
menjadi library 15 byte dan name 23 byte; native/Kotlin memeriksa ulang batas
serta token identifier sebelum snapshot. Fresh remote StreamIn/Out juga menghapus
directive lama. Radar/GLES menunjukkan cue/ring `ANIM`, bukan animasi skeleton:
IFP belum dieksekusi, dan `lockX/lockY/freeze` tidak membekukan input, collision,
physics, atau sync lokal.

## Nametag dan chat bubble server

`PlayerInit` sekarang mempertahankan `ShowNameTags`, `SetNameTagDrawDistance`,
dan `DisableNameTagLOS`. RPC `ShowPlayerNameTagForPlayer` (80) mengubah
visibilitas per remote player, termasuk bila RPC datang sebelum Join/StreamIn.
Snapshot renderer kemudian hanya meneruskan nametag yang diizinkan server dan
berada dalam jarak tersebut. `DisableNameTagLOS` diteruskan sebagai policy, tetapi
client belum mengklaim raycast LOS GTA yang akurat; nametag/label tetap Canvas
portable sampai evaluator collision-visibility yang benar ada.

RPC `SetPlayerChatBubble` (59) didecode persis sebagai `u16 playerId`, RGBA,
float draw-distance, `u32` expiry duration, dan DynStr8 maks. 128 byte. Native
menyimpan salinan per-player/lokal, memakai steady clock hanya untuk menghapus
bubble setelah waktu habis (lifetime defensif maksimum satu jam), lalu snapshot
hanya menyalurkan bubble aktif yang masih dalam draw-distance. Bubble tidak
mengubah chat, position, combat, atau authority gameplay apa pun.

RPC `SendDeathMessage` (55) juga memiliki surface HUD global yang terpisah
dari RPC `PlayerDeath` (166). Wire exact-nya `u16 KillerID, u16 PlayerID, u8
reason`; `0xFFFF` pada killer adalah sentinel no-killer dari writer server.
Reducer menerima body 5 byte saja, menolak victim sentinel/trailing data, dan
menyimpan maksimal 16 row selama delapan detik steady-clock. Snapshot Android
mengirim paling banyak delapan row live dengan nama known atau label `#ID`; HUD
menampilkan paling banyak empat row dan reason raw `R<n>`. Feed tidak mengubah
HP, ammo, damage, respawn, atau menyatakan Android telah memahami weapon GTA.

## Touch input → preview aman

`Kontrol touch` adalah pad multi-touch v1 di Android: stick bergerak, drag
kanan horizontal/vertikal untuk yaw/pitch look, dan tombol tahan
jump/run/fire/aim/use/crouch. JNI mengirim semantic action yang dibatasi ke
mapper C++; mapper mengonversinya ke `LeftRight`, `UpDown`, `Keys`, heading
quaternion WXYZ sesuai `GTAQuat` legacy, dan pitch AimSync terpisah. Preview
mengambil **hanya** posisi/vital/velocity server yang sudah tervalidasi.

Pad diagnostik **sendiri** tidak mengirim packet OnFoot. Tanpa renderer/collision
bridge, memajukan posisi dari joystick saja akan membuat client mengarang posisi
dan berisiko pada validasi server. Namun pad in-scene Dunia native meneruskan
intent yang sama ke gameplay tick native; tick itu hanya mengirim Packet-207
setelah collision world, spawn/ACK, controllable, dan state lokal tervalidasi.
Semua kontrol diclear saat dialog ditutup.

## Keyboard dan gamepad → semantic input yang sama

Scene GPU dan radar fallback juga memasang router hardware Android. Keyboard
Bluetooth memakai `WASD` atau panah untuk gerak; `Space`, `Shift`, `C`, `F`, `Q`,
`R`, dan `E`/Enter berturut-turut menjadi jump, sprint, crouch, fire, aim,
secondary, dan USE. `T` membuka composer chat, `/` membukanya dengan slash
terisi untuk command, dan `Tab` membuka score/ping roster; ketiganya merupakan
shortcut UI lokal dan tidak menjadi held gameplay state. Gamepad memakai stick
kiri/D-pad untuk gerak, stick kanan untuk look yaw/pitch relatif, `A/B/X/Y`
untuk jump/USE/fire/sprint, shoulder/trigger untuk aim/fire, serta **Start**
untuk composer chat. Tahan USE non-driver selama 500 ms tetap membawa intent
passenger; tap biasa tidak pernah mengubah seat lokal.

Router hanya menyatukan state hardware dengan pad touch menjadi `ControlSample`
terbatas lalu melewati JNI mapper yang sama. Sebelum shortcut membuka modal ia
mengosongkan held hardware; composer sendiri masih memakai sender chat native
ordered dan native yang memvalidasi sesi/pesan. Dead-zone controller, freeze
`TogglePlayerControllable`, app masuk background, dismiss scene, dan state button
tersangkut ditangani dengan clear fail-closed. Saat activity tidak `RESUMED`,
radar/GPU menghentikan gameplay tick, render/asset refresh, dan playback stream,
radar mengirim sample input nol sekali, sedangkan surface GLES juga menerima
`onPause()` eksplisit untuk menjeda thread GL; tidak ada key/controller Android
yang langsung menulis RakNet atau mengarang transform dunia.

## Ownership dan keamanan

- Hanya **satu** session eksplisit disimpan pada satu waktu.
- Membuka session baru menutup socket lama secara aman.
- Menutup dialog/detail session juga mengirim disconnect best-effort dan menutup
  UDP socket; client tidak meninggalkan koneksi pemain tersembunyi.
- RPC `PlayerClose` (40) body tepat kosong dan `ConnectionRejected` (130)
  `u8 reason 1..4` juga terminal: native membersihkan hold input/prediksi lalu
  menutup socket dan memberi status server-ended/rejection yang terverifikasi.
  Byte tambahan/reason asing ditolak; keduanya bukan alasan untuk
  mereset/mengarang world lokal. Jalur sebelum `PlayerInit` juga menghentikan
  hand-off retained agar rejection eksplisit tidak menjadi status menunggu init.
  Saat frame berikutnya inactive, radar/GLES menghentikan tick gameplay, asset
  refresh, audio, dialog/menu server, dan visual world stale; hanya alasan
  terminal yang tetap ditampilkan.
- Datagram diterima hanya dari IP + port yang dipilih pengguna.
- Split reassembly, player state, ACK record, pending reliable queue, dan retry
  semuanya dibatasi agar server/packet malformed tidak dapat mengalokasikan tanpa
  batas. Receipt gate reliable mengingat maksimum 4.096 message number dalam
  window `u16`, sehingga retransmit tetap di-ACK tetapi tidak mengulang control,
  split fragment, atau reducer. Split membatasi 64 sequence pending, 256 KiB payload retained total,
  serta total payload yang masih representable oleh bit-length `u16`; fragment
  dengan split-ID yang sama juga wajib mempertahankan count, reliability, dan
  metadata ordering yang sama—identitas berbeda mengganti state ID lama agar
  body dua packet tidak pernah tersambung. Overflow menutup session sesudah ACK
  transport, bukan mempertahankan stream rusak.
- `RELIABLE_ORDERED` inbound baru masuk reducer setelah gate urutan per channel:
  frame lebih awal dibuffer bounded, lalu hanya run index kontigu yang dirilis.
  Jika batas packet/byte terlampaui, session ditutup jujur—bukan meng-ACK lalu
  menghilangkan RPC ordered yang tidak mungkin dipulihkan.
- Network call harus dari `Dispatchers.IO`, bukan UI thread.

## Reliable ordered client RPC

Class/spawn request dibungkus seperti ini:

```text
RakNet RELIABLE_ORDERED (9)
ordering channel: 2 (SyncRPC client stream; open.mp source contract)
RPC payload: [0x14][rpc id][bit-packed fields]
```

Session menyimpan nomor message dan ordering index setelah `ClientJoin`. Packet
reliable outbound di-track sampai server meng-ACK-nya. Retry setiap 750 ms,
maximum 5 retry, dan queue dibatasi 32 packet.

Saat tidak ada reliable outbound yang menunggu ACK selama 5 detik, session juga
mengirim `ID_DETECT_LOST_CONNECTIONS` (23) sebagai `RELIABLE` (8). Ini mengikuti
perilaku transport RakNet 2.52 kompatibel SA-MP. `ID_INTERNAL_PING` (6) inbound
dijawab dengan `ID_CONNECTED_PONG` (9) yang unreliable; count keduanya terlihat
di diagnostic. Ini transport keepalive, **bukan** player/gameplay sync atau
mekanisme reconnect. Full congestion control/resend window tetap pekerjaan
session/sync berikutnya.

## Alur pemain Android

Detail server menempatkan alur utama di bagian atas, bukan menjadikan log
transport sebagai cara bermain:

- **Hubungkan native** — membuka socket dan memulai poll lifecycle-aware otomatis
  selama dialog masih terlihat. Tap ganda tidak membuat handshake paralel;
  koneksi yang sedang dibuka/ditutup diberi status yang jelas;
- **Class awal** — segera setelah `PlayerInit` valid dengan class tersedia, native
  meminta class `#0` pada SyncRPC channel 2 agar server legacy tidak menutup peer
  sebelum UI sempat bereaksi. Setelah respons, tombol **Class** aktif: tap meminta
  class berikutnya dan tahan meminta sebelumnya dari `SetSpawnInfoCount`; native
  menolak ID di luar count, menjaga satu request pending, lalu mengizinkan retry
  setelah timeout bounded agar respons lama tidak dipakai sebagai izin spawn;
- **Spawn** — baru aktif setelah class selectable diterima. Respons izin memicu
  `PlayerSpawn` native dan UI menunggu ACK transportnya;
- **Buka dunia** — memilih scene GLES sebagai jalur utama untuk HUD/kontrol/scene
  entity; perangkat tanpa OpenGL ES 2.0 secara otomatis jatuh ke radar Canvas
  native yang memakai session dan tick collision yang sama. Ini bukan klaim
  bahwa renderer GTA/physics penuh sudah selesai;
- Bagian bawah tetap menyimpan query, probe, chat, touch layout, dan kontrol
  lanjutan untuk troubleshooting tanpa menggeser alur pemain utama.
- **Scoreboard** — meminta RPC 155 body kosong dengan throttle satu detik;
  respons roster score/ping hanya ditampilkan dari state server bounded;
- **Kontrol touch** — membuka semantic multi-touch pad dan scene/radar dapat
  menjalankan tick OnFoot collision-grounded serta driver Packet-200 terbatas
  bila world data GTA valid; driver memakai profile `handling.cfg` model-matched
  bila tersedia atau fallback generic aman. Saat AIM ditahan dan kandidat
  Packet-203 lolos gate native, scene memakai view aim orang-pertama dari
  yaw/pitch yang sama; HUD menunjukkan reticle/pitch dan status kandidat (tanpa
  target/hit marker). Tap **USE** tetap memilih pickup dulu lalu meminta
  driver/exit; tahan USE mengirim intent passenger untuk vehicle dekat. Keduanya
  hanya request RPC 26/154—seat lokal tidak berubah sampai RPC 70/71 server.
  Setelah server men-seat player dan vehicle masih streamed, snapshot dunia boleh
  menjadikan transform vehicle server sebagai **anchor kamera/proxy** passenger
  tanpa mengarang offset kursi GTA. Saat RPC 70 server memberi seat nonzero dan
  vehicle tetap stream-in, tick native juga mengirim Packet 211 passenger raw
  pada `InCarRate`, memakai anchor vehicle authoritative + input/vitals server;
  ia tidak melakukan physics atau pindah seat. Held AIM hanya dapat mengaktifkan
  bit drive-by bila passenger non-cuffed memegang Uzi/MP5/Tec-9 dengan ammo
  server-known; FIRE tetap key input dan tidak mengirim target/damage BulletSync.
  Untuk remote, Packet 211 inbound yang source-verified membawa anchor passenger sendiri ke
  renderer. Packet 210 inbound juga menerima transform trailer exact 57-byte
  (`player, vehicle, VEC3, raw w/x/y/z quaternion, linear/turn VEC3`) hanya
  untuk vehicle yang sudah streamed; state ini mengubah transform DFF/proxy
  tetapi tidak membuat cab relation, health/seat, local trailer simulation, atau
  Packet-210 outbound. Saat on-foot dan AIM ditahan, Packet 203 terbatas dapat mengikuti
  `WeaponRate` dari transform collision + weapon/ammo server; AIM+FIRE juga
  dapat mengirim Packet-206 no-target rate-capped. Target/hit/damage dan fire
  gameplay GTA lengkap tetap belum dikirim;
- **Putus sesi** — menghentikan poll lalu menutup socket native. Jika handshake
  masih berada di thread IO, teardown menunggunya selesai lebih dulu agar socket
  terlambat tidak tertinggal setelah dialog ditutup.

Loop memanggil native poll di `Dispatchers.IO` (timeout UDP foreground 40 ms).
Setiap poll melakukan paling banyak satu receive blocking, lalu hanya bila itu
tidak timeout menguras maksimal 15 datagram siap secara nonblocking (16 attempt
total); presentation JSON/UI hanya diperbarui sekitar 10 Hz. Saat activity tidak
`RESUMED`, socket eksplisit hanya mendapat cadence maintenance 200 ms/100 ms
agar ACK/retry bounded tidak langsung putus, sementara snapshot, dialog/menu
server, dan renderer tidak dipresentasikan; controller presentasi dibersihkan
saat transisi background. Saat dialog ditutup, polling dibatalkan dan teardown
socket diserialkan di belakang handshake IO yang mungkin masih berjalan. Poll
juga menjalankan keepalive transport reliable dan reply internal ping. Dunia
radar maupun scene GPU menjalankan gameplay tick periodik; encoder
OnFoot/driver/passenger/AimSync tetap fail-closed bila collision, spawn/ACK,
vehicle stream, camera endpoint, atau authority server tidak memenuhi gate.

Surface ini masih **eksperimen** dan belum renderer/simulasi GTA lengkap. Ia
menjalankan path gameplay native yang terbatas dan tervalidasi, bukan mengklaim
APK sudah playable penuh.

## Berikutnya

1. Full ACK range behavior, reconnect, ping/timeout metrics, dan resend window
   yang benar di atas keepalive transport yang sudah ada.
2. Capture/live validation untuk handshake spawn lengkap, `OnFootSync`, dan
   PlayerAimSync Packet-203 yang sudah punya encoder/rate gate.
3. Player/vehicle/object state → native HUD + renderer/world bridge.
4. Capture validation untuk loop touch/gamepad/keyboard OnFoot + vehicle yang
   sudah digate, lalu gyro dan physics/gameplay vehicle lebih lengkap.
