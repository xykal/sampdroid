# Native RakNet session lifecycle

Dokumen ini menjelaskan session client Android native SampDroid setelah `ClientJoin`.
Ini bukan wrapper `samp.exe`, bukan Wine/Winlator, dan bukan runtime modpack PC.

## Batas implementasi saat ini

```text
user pilih server
  → native UDP socket
  → open/cookie/auth/accepted
  → NEW_INCOMING_CONNECTION + ClientJoin
  → socket + message counters + split reassembler TETAP hidup
  → poll datagram / ACK reliable / reply internal ping / reduce RPC world state
  → reliable transport keepalive otomatis saat idle
  → user memilih class lalu meminta spawn secara eksplisit
  → respons spawn nonzero → core kirim RPC PlayerSpawn (52) → tunggu ACK
  → state spawn/posisi lokal direduce dari RPC server
  → touch/keyboard/gamepad → semantic input lokal
  → scene GPU atau radar menjalankan gameplay tick collision-gated
  → core mengirim OnFoot/driver/passenger/AimSync hanya pada rate dan gate server yang valid
```

Tombol **Spawn** sekarang menjalankan handshake native eksplisit, bukan spawn
otomatis saat connect. Ia tetap bukan kontrol gameplay atau renderer.

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

Packet memakai `UNRELIABLE_SEQUENCED` (reliability `7`), ordering channel `1`,
dan counter ordering channel-1 yang **terpisah** dari RPC reliable-ordered
channel-2. Nilai rate `0`/abnormal dibatasi client ke 20–1000 ms (default 30 ms)
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
`UNRELIABLE_SEQUENCED` pada channel `1`, menggunakan ordering counter sync yang
sama tetapi cadence tersendiri dari `WeaponRate` PlayerInit (dibatasi 20–1000
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
**41 byte / 328 bit** berikut pada channel-1
`UNRELIABLE_SEQUENCED`. Layout client ini sudah diaudit terhadap
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
client → PlayerRequestSpawn (RPC 129, RELIABLE_ORDERED channel 2)
server → RequestSpawn response (legacy u8 atau open.mp u32, nilai nonzero)
client → PlayerSpawn (RPC 52 kosong, RELIABLE_ORDERED channel 2)
server → transport ACK untuk RPC 52
client → eligible untuk OnFootSync channel 1
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
`TogglePlayerControllable`, dismiss scene, dan state button tersangkut ditangani
dengan clear fail-closed. Tidak ada key/controller Android yang langsung menulis
RakNet atau mengarang transform dunia.

## Ownership dan keamanan

- Hanya **satu** session eksplisit disimpan pada satu waktu.
- Membuka session baru menutup socket lama secara aman.
- Menutup dialog/detail session juga mengirim disconnect best-effort dan menutup
  UDP socket; client tidak meninggalkan koneksi pemain tersembunyi.
- Datagram diterima hanya dari IP + port yang dipilih pengguna.
- Split reassembly, player state, ACK record, pending reliable queue, dan retry
  semuanya dibatasi agar server/packet malformed tidak dapat mengalokasikan tanpa
  batas.
- Network call harus dari `Dispatchers.IO`, bukan UI thread.

## Reliable ordered client RPC

Class/spawn request dibungkus seperti ini:

```text
RakNet RELIABLE_ORDERED (9)
ordering channel: 2 (SyncRPC)
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

## Android diagnostic surface sementara

Di detail server terdapat:

- **Sesi native** — membuka socket dan memulai poll lifecycle-aware otomatis
  selama dialog masih terlihat;
- **Class n/m** — setelah `PlayerInit`, tap meminta class berikutnya dan tahan
  meminta sebelumnya dari `SetSpawnInfoCount`; native menolak ID di luar count,
  menjaga satu request class pending, lalu mengizinkan retry setelah timeout
  bounded agar response lama tidak dipakai sebagai izin spawn;
- **Spawn** — user-triggered `PlayerRequestSpawn`; respons izin memicu
  `PlayerSpawn` native dan diagnostic menunggu ACK-nya;
- **Scoreboard** — meminta RPC 155 body kosong dengan throttle satu detik;
  respons roster score/ping hanya ditampilkan dari state server bounded;
- **Kontrol touch** — membuka semantic multi-touch pad dan scene/radar dapat
  menjalankan tick OnFoot collision-grounded serta driver Packet-200 terbatas
  bila world data GTA valid. Saat AIM ditahan dan kandidat Packet-203 lolos gate native, scene memakai view aim orang-pertama dari yaw/pitch yang sama; HUD menunjukkan reticle/pitch dan status kandidat (tanpa target/hit marker). Tap **USE** tetap memilih pickup dulu lalu meminta
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
  renderer. Saat on-foot dan AIM ditahan, Packet 203 terbatas dapat mengikuti
  `WeaponRate` dari transform collision + weapon/ammo server; AIM+FIRE juga
  dapat mengirim Packet-206 no-target rate-capped. Target/hit/damage dan fire
  gameplay GTA lengkap tetap belum dikirim;
- **Putus sesi** — menghentikan poll lalu menutup socket native.

Loop memanggil native poll di `Dispatchers.IO` (timeout UDP 40 ms). Setiap poll
melakukan paling banyak satu receive blocking, lalu hanya bila itu tidak timeout
menguras maksimal 15 datagram siap secara nonblocking (16 attempt total); presentation
JSON/UI hanya diperbarui sekitar 10 Hz. Saat dialog ditutup, coroutine dibatalkan dan
disconnect best-effort dikirim. Poll juga menjalankan keepalive transport reliable
dan reply internal ping. Dunia radar maupun scene GPU menjalankan gameplay tick
periodik; encoder OnFoot/driver/passenger/AimSync tetap fail-closed bila
collision, spawn/ACK, vehicle stream, camera endpoint, atau authority server
tidak memenuhi gate.

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
