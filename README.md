# SampDroid

[![Build APK](https://github.com/xykal/sampdroid/actions/workflows/build-apk.yml/badge.svg)](https://github.com/xykal/sampdroid/actions/workflows/build-apk.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-0b8f5a.svg)](LICENSE)
[![ABIs](https://img.shields.io/badge/ABI-armeabi--v7a%20%7C%20arm64--v8a-2563eb.svg)](#kompatibilitas)

**SampDroid** adalah proyek port client SA-MP ke Android sebagai **APK native**.
Target akhirnya: pemain dapat masuk server SA-MP/open.mp dari Android tanpa
Winlator, tanpa Wine/x86 emulation, tanpa menjalankan `samp.exe`, dan tanpa
menjadikan modpack PC sebagai syarat client.

> **Port di sini berarti kompatibilitas client PC dibangun ulang secara native
> untuk ARM Android.** `samp.exe` adalah binary Windows/x86, jadi tidak bisa
> sekadar di-compile ulang menjadi APK; yang dikerjakan adalah client Android
> native dengan protocol, session, sync, HUD, input, dan world bridge yang setara
> dari sisi pemain.

**Distribusi APK:** setiap tag dibuild GitHub Actions sebagai **draft release**
lebih dulu. Draft direview maintainer sebelum dipublikasikan.

---

## Target produk

```text
Install APK SampDroid → pilih server → connect/login/spawn → main native Android
```

Bukan ini:

```text
Android → Winlator/Wine → Windows/x86 → samp.exe → server
```

SampDroid tidak didesain sebagai launcher modpack PC. Kontrol touch/gamepad dan
keyboard Bluetooth adalah bagian dari client Android sendiri.

Detail kontrak port: [docs/NATIVE_PORT_TARGET.md](docs/NATIVE_PORT_TARGET.md).

## Alur pemain saat ini

1. Dari kartu **Data GTA**, hubungkan folder GTA yang sudah diekstrak di
   penyimpanan/SD card, pilih ZIP lokal milikmu, atau masukkan URL ZIP HTTPS yang memang kamu kuasai.
   Grant SAF permanen diperlukan; ZIP ditahan di staging SD/provider, diperiksa,
   lalu baru dicoba mount read-only—tidak ada asset game yang dibundel.
2. Setelah folder terhubung, pilih **Siapkan world & collision** untuk memuat
   source map/collision yang tersedia dari data tersebut.
3. Query server, lalu buka detailnya dan tekan **Hubungkan native**. Setelah
   `PlayerInit`, native meminta class awal server (`#0`) agar handshake legacy
   tidak kedaluwarsa; kamu tetap dapat memilih class lain lalu tekan **Spawn**
   mengikuti respons server.
4. **Buka dunia** memilih scene GLES sebagai jalur utama (dengan radar Canvas
   fallback untuk perangkat tanpa GLES 2.0). HUD, chat, touch/gamepad/keyboard,
   dan tick gameplay hanya memakai state/session native yang sudah digate.

Alur ini sudah lebih dari launcher/log diagnostik, tetapi status eksperimental
di bawah tetap berlaku: belum ada klaim renderer GTA, physics, atau kompatibilitas
live-server penuh setara PC.

## Status implementasi saat ini

| Modul client | Status | Bukti / catatan |
|---|---:|---|
| Browser + query server | ✅ | info, rules, player list, ping native |
| RakNet 2.x open/cookie/auth | ✅ | diverifikasi terhadap server live |
| `CONNECTION_REQUEST_ACCEPTED` + ClientJoin | 🧪 | frame ClientJoin msg=5/SyncRPC channel 2 **dan** burst post-accept 2/3/4/5 70-byte dalam satu datagram no-ACK byte-identical dengan BitStream legacy FakeBots; reliable tetap punya ACK/retry masing-masing; peer loopback memastikan empat frame itu keluar bersama-sama dan, saat burst pertama sengaja dibuang, msg 2/4/5 diretry terpisah dengan identitas asli. UIF belum memberi `PlayerInit`/class/spawn; coalescing baru tervalidasi host, bukan receipt semantik/live admission |
| ACK reliable + multi-packet datagram | ✅ | retransmisi tetap di-ACK lalu gate receipt `u16` bounded membuang duplikat sebelum split/reducer |
| Split reassembly | ✅ | fragment disusun sebelum RPC didecode |
| State pemain dasar | ✅ | `PlayerInit`, Join/Quit/Name/Color, StreamIn/Out, roster score/ping RPC 155, policy nametag `ShowNameTags`/jarak + RPC 80 per-player, dan local spawn/posisi/vitals → model native bounded |
| Native chat state | 🧪 | chat broadcast/HUD tail bounded + composer chat/`/command` reliable native (tombol CHAT di scene/radar, keyboard `T` atau `/`, dan controller Start) dengan IME **Kirim**, serta `SetPlayerChatBubble` server dengan expiry native bounded; bukan log saja |
| Checkpoint state | ✅ | normal + race checkpoint reducer dengan marker di radar + scene GPU |
| Class/spawn packet core | ✅ | `PlayerInit` memicu RequestClass #0 bootstrap native; class lain → RequestSpawn → PlayerSpawn confirmation + ACK gate native |
| Persistent native session | 🧪 | connect open/cookie/auth/bootstrap dibatasi total 1–12 dtk. Sesudah accept, urutan source RakPeer 2/3/4/5 (`NEW_INCOMING`/ping/static-data/ClientJoin) dikirim bersama sebagai satu datagram no-ACK lalu maksimal delapan read awal sebelum socket retained; auto-poll foreground 40 ms, peer filter, satu receive blocking lalu—bila tidak timeout—maksimal 15 receive ready nonblocking (16 attempt total), ACK/retry bounded per reliable frame, gate receipt reliable window 4.096 nomor sebelum split, inbound `RELIABLE_ORDERED` per-channel gate (buffer bounded/overflow menutup sesi), keepalive transport reliable + internal pong. Saat app background, maintenance dibatasi 200/100 ms tanpa presentasi dialog/menu server; reconnect belum |
| OnFootSync core | 🧪 | encoder packet-207 client→server vector-tested, USeq channel-1 dengan counter sequenced terpisah dari SyncRPC channel-2, rate/spawn gate; surface dunia radar/GPU menjalankan tick gerak **hanya** dari collision GTA yang telah tersedia lalu rate-gated send. Exterior grounding + sweep silinder sisi dasar terhadap COL box/sphere/mesh wall sudah ada, bersama raycast statik box/sphere/mesh bounded (interior exact, ≤256 unit) yang hanya dapat memberi `HitPos` absolut dekat pada Packet-206 `HitType=None` tanpa target/damage; RPC `SetPlayerInterior` memilih batch IPL dan query COL dengan ID interior yang persis sama, sedangkan RPC 17 `SetWorldBounds` menahan prediction on-foot/driver **exterior** di rectangle server tanpa menebak push-back. Transisi/ubah envelope mereset prediksi lalu tick hanya lanjut bila floor matching tersedia. Renderer placement interior bounded sudah ada bila IPL user termuat; physics GTA penuh dan capture/live validation tetap belum |
| AimSync + no-target fire core | 🧪 | Packet-203 `PlayerAimSync` client→server fixed 32-byte, vector-tested, USeq channel-1 dengan counter sequenced terpisah dari SyncRPC channel-2 dan rate `WeaponRate` sendiri. Hanya aktif on-foot saat semantic **AIM** ditahan, transform collision-grounded + heading/yaw dan pitch native bounded valid, serta armed weapon/ammo diketahui dari RPC server. Saat **AIM+FIRE**, Packet-206 client 41-byte `HitType=None` rate-capped juga dapat dikirim dari pose kamera yang sama. Layout no-`PlayerID` ini diaudit terhadap reader + generator NPC open.mp commit `3d20f3dc`; `LagCompensation=0` menahan sender fail-closed dan server legacy/anticheat live belum divalidasi. Ia tidak dapat membawa target ID, entity raycast/impact, offset, damage, reward, atau ammo decrement lokal; hanya ray COL statik dekat dari data user yang boleh mengisi `HitPos` absolut tetap dengan `HitType=None`. Camera mode compatible, packed `CamZoom:6 | WeaponState:2`, serta gate spawn/ACK/freeze/vehicle/kamera-server tetap diverifikasi |
| Remote on-foot bridge | 🧪 | decoder broadcast packet-207 server→client + lifecycle StreamIn/Out → state entity native bounded; RPC 166 hanya memberi cue `WASTED` remote singkat, bukan death/respawn lokal |
| Dunia/radar + scene GPU native | 🧪 | Canvas radar + GLES 2.0 menampilkan local/remote actor, nametag/bubble server berjarak, checkpoint, weather/waktu (termasuk `SetPlayerTime`/clock recipient), proxy kendaraan, state `Create/Set/Move/Stop/DestroyObject`, pickup `Create/DestroyPickup`, GangZone visual, map-icon server, dan label 3D portable. Endpoint kamera server (`SetCamera*`/`InterpolateCamera`) diprioritaskan renderer tanpa memutasi gerak/sync lokal; `SetCameraBehindPlayer` kembali ke fallback touch/player-follow. GangZone hanya rectangle/flash visual—bukan capture turf—dan map icon/label memakai marker/Canvas portable, bukan sprite/font/LOS GTA penuh. Bila IDE/IMG user memuat `cars`/`peds`, model vehicle streamed, skin actor remote, serta NPC `ShowActor` dapat ikut masuk batch DFF prelit dinamis bounded; reducer NPC (`Show/HideActor`, posisi, sudut, health) memakai namespace ID terpisah, maksimal 1.000 retained/64 render copy, dan visual-only—bukan player identity, hit target, collision, atau interaction authority. `SetPlayerSkin` non-DL juga mengganti model remote/local dari server (actor lokal tetap perlu template `SetSpawnInfo` agar tidak dipalsukan di origin); proxy authority tetap fallback. Tap **USE** memilih pickup valid dulu (RPC 131 tanpa reward lokal), lalu request enter vehicle driver (26); tahan **USE** membawa intent passenger untuk vehicle yang sama, dan USE saat sudah diset-seat server meminta exit (154). RPC 70/71 tetap satu-satunya authority seat; spawn/posisi/interior/radius/rate-limit dicek native dan state kendaraan tak pernah diubah sebelum RPC server. Objek server yang model ID-nya cocok dengan IDE/IMG user—termasuk `MoveObject` dengan transform sampled monotonic—bisa masuk batch DFF prelit; yang belum resolve tetap marker/proxy jujur. Placement IPL pada interior server yang terpilih (exterior = 0) serta subset objek/vehicle/skin dinamis kini dapat membawa sampai empat overlay material/TXD independen yang dibatasi (vertex + total 1 MiB pixel RGBA per jalur); resolver `txdp` child→parent dapat melewati GTA3 IMG utama dan sampai empat companion `models/*.img` read-only. Overlay disalin dulu, texture GL stale dibersihkan pada thread GL, dan proxy tetap fallback—ini belum binding semua material, map stream, atau visual GTA lengkap |
| Class/spawn native | 🧪 | browser class dari `PlayerInit.SetSpawnInfoCount` (tap class berikutnya, tahan untuk sebelumnya), request reliable satu-per-satu + timeout bounded, response/ACK, local vitals/freeze server, dan state spawn authoritative |
| Vehicle visual + occupant sync | 🧪 | `StreamIn/OutVehicle` + broadcast Packet 200 driver / Packet 209 unoccupied mengubah proxy kendaraan yang sudah stream-in. Packet 210 inbound `PlayerTrailerSync` yang fixed/raw source-verified juga mengganti transform + attitude trailer streamed saja tanpa mengarang cab attachment, seat, health, atau trailer physics lokal. Occupancy memakai flag eksplisit (`driverKnown`), jadi ID player `0` tetap driver valid, bukan sentinel kosong. Packet 211 inbound `PlayerPassengerSync` juga diparse dari layout writer open.mp yang source-verified: passenger remote memakai anchor posisi wire dan dapat coexist dengan driver; Packet 209 hanya melepas driver, bukan passenger aktif. Driver maupun passenger remote yang sudah stream menahan ped geometry/DFF supaya tidak dobel/tembus mobil; nametag, bubble, serta marker radar tetap tampil pada anchor tervalidasi. Packet on-foot, stream-out/quit, driver baru, atau stream-out vehicle melepas anchor lama. Jika IDE `cars` + DFF/TXD user tersedia, batch GLES juga dapat resolve model vehicle terdekat read-only dengan overlay material bounded (proxy authority tetap fallback). Tap **USE** request enter driver/exit dan tahan USE request intent passenger; 70/71 tetap satu-satunya authority seat. Passenger lokal kini mengirim Packet-211 client→server raw 25-byte hanya setelah RPC 70 seat nonzero + vehicle masih streamed + spawn/ACK; posisinya disalin dari transform vehicle authoritative, tanpa physics atau offset kursi lokal. Input/vitals/armed weapon server ikut pada `InCarRate`; passenger non-cuffed dengan Uzi/MP5/Tec-9 yang memang diarm+berammo dari server dapat mengaktifkan bit drive-by Packet-211 saat AIM ditahan (FIRE tetap key input), tanpa BulletSync target/damage. Cuffed tetap hanya dari `SetPlayerSpecialAction` server. Untuk seat driver 0, Packet-200 client→server raw 64-byte kini vector-tested dan dikirim USeq channel-1 (counter sequenced tetap terpisah dari SyncRPC channel-2) mengikuti `InCarRate`; pilot kinematic memakai gas/mundur, steer, rem, floor COL, dan sweep geometri statik. Coupling/physics trailer outbound-lokal, siren/gear/Hydra aktif, handling/suspension/damage, physics GTA penuh, serta binding semua material/skin/animasi vehicle belum |
| Player/vehicle/object sync penuh | ⏳ | remote on-foot + driver Packet-200/passenger Packet-211/trailer Packet-210 visual bounded, inbound Packet-203 remote camera-direction serta Packet-206 server-relay direction cue transient, dan outbound AimSync Packet-203 + BulletSync no-target `HitType=None` dari AIM+FIRE, global object lifecycle/motion, global pickup display + user-confirmed RPC 131, attempt enter/exit + seat lifecycle authoritative, serta driver Packet-200 kinematic terbatas sudah ada; player-object/pickup variant, drive-by dan trailer sync/coupling **outbound-lokal**, **local** target/raycast/hit/damage authority, handling/fisika kendaraan penuh, inventory/reward authority, dan gameplay penuh belum |
| HUD + textdraw/dialog native | 🧪 | HUD vitals/chat/entity, money dari `Give/ResetPlayerMoney`, TAB-style score/ping roster yang meminta RPC 155 server, serta armed weapon/ammo yang benar-benar didorong RPC server, `SendGameText` timed banner proxy, ShowDialog + respons reliable native, serta stream audio RPC 41/42 yang lifecycle-bound dan HTTPS-only tersedia (URL HTTP/unsafe sengaja diblokir lokal). Saat AIM ditahan dan kandidat Packet-203 native benar-benar lolos gate, scene beralih ke view aim orang-pertama dari yaw/pitch yang sama; HUD scene menampilkan reticle + pitch serta status kandidat tanpa target/hit marker. Saat AIM+FIRE, HUD menampilkan counter datagram Packet-206 no-target dan pulse `COL dekat (lokal)` bila socket-send native membawa contact COL statik dekat—keduanya bukan receipt server, hit entity, damage, atau ammo lokal. `Show/Hide/SetString` TextDraw punya Canvas text/box proxy bounded dan tap selectable → RPC 83 setelah native recheck; font-4 sprite literal `dictionary:texture` dapat memakai TXD RGBA user-owned yang dibatasi/cache, sedangkan font-5 dapat memakai mesh DFF prelit user-owned bounded bila model resolvable. Keduanya belum mengklaim font/kamera/material GTA penuh; animasi TextDraw/GameText penuh juga belum. |
| Touch/gamepad/keyboard/gyro native | 🧪 | touch pad runtime → semantic native action → scene gameplay tick collision-grounded; tap **USE** meminta pickup/driver, tahan USE meminta intent passenger tanpa mengubah seat lokal; drag kanan mengubah yaw/heading dan pitch look relative native (pitch hanya untuk AimSync, bukan transform dunia). Scene GPU **dan radar fallback** memakai overlay touch yang sama, memakai tinggi dialog penuh + menjaga layar tetap menyala selama sesi, dan menaruh aksi utilitas pada toolbar horizontal yang bisa digeser agar tombol tidak mengecil di ponsel sempit; lalu saat activity tidak `RESUMED` menghentikan tick/render/refresh/audio, menjeda surface GLES, dan mengirim input nol fail-closed, serta menerima keyboard Bluetooth (`WASD`/arrow, Space/Shift/C/F/Q/R/E; `T` chat, `/` command, Tab scores) dan gamepad (stick/D-pad, A/B/X/Y, shoulder/trigger, Start chat); gameplay tetap melewati mapper semantic yang sama, sementara shortcut chat/score hanya membuka UI native. Gyro look opt-in di **LAYOUT** mengubah kecepatan sudut perangkat menjadi delta yaw/pitch bounded saja dan berhenti saat freeze/spectate/dead, app background, atau dialog dismiss—bukan posisi/target/packet mandiri. LAYOUT juga menyimpan drag/scale, kontras, dan preset satu tangan kiri/kanan per perangkat serta dapat menyalin JSON data-only v1; validasi sensor/perangkat Android nyata masih berikutnya |
| User-owned GTA data runtime | 🧪 | menu **Data GTA** dapat memilih folder GTA hasil ekstrak via Android SAF, memilih ZIP lokal milik pemain, **atau** menerima URL ZIP HTTPS yang pemain miliki/berhak pakai. Kedua jalur ZIP hanya membaca input lalu menyalinnya ke staging di folder pilihan (termasuk SD card), menghitung SHA-256 opsional, menolak redirect non-HTTPS/ZIP traversal/bomb/struktur salah, mengekstrak sementara, memeriksa `data/gta.dat` + header `models/gta3.img`, lalu baru mencoba mount native read-only. APK tidak membundel/host asset GTA, tidak memodifikasi arsip sumber, dan tidak menyalin data akhir ke internal. Root tervalidasi parse `gta.dat`, mount GTA3 IMG utama serta sampai empat companion `models/*.img` read-only untuk lookup DFF/TXD, batch COL/IDE/IPL text, serta mencoba resolver dinamis stock opsional `data/vehicles.ide` dan `data/peds.ide` (karena tidak terdaftar di `gta.dat`). Lalu ia menemukan hanya `parent_streamN.ipl` `bnry` relevan dan (saat sinkronisasi eksplisit) seluruh member `.col` IMG utama yang dibatasi 256 member/32 MiB; native merakit catalog/spatial collision world dari fd read-only tanpa storage permission lebar, copy, atau asset bundling |
| World/game bridge Android | 🧪 | radar/entity surface, scene GPU, HUD, dialog, touch overlay, runtime collision, parser mesh DFF/material, decoder TXD, placement-aware DFF batch per-ID interior, sampai empat overlay TXD bounded untuk placement/dynamic model, serta resolver `cars`/`peds` stock → streamed vehicle/skin DFF bounded sudah ada; session hanya mengirim gerak yang tervalidasi oleh floor + side sweep COL pada interior server yang cocok. Resolver map/texture/LOD penuh, material/texture vehicle, collision/streaming multi-IMG penuh, physics GTA lengkap, dan gameplay server lengkap belum |

Suite regresi C++ host terbaru lulus untuk codec/state sekaligus builder handshake
legacy: frame ClientJoin SyncRPC-2 msg=5, fixture burst 2/3/4/5 70-byte yang
byte-identical dengan BitStream FakeBots, ACK/retry bounded per reliable frame,
dan peer loopback yang membuktikan jalur session mengirim keempat frame bootstrap
dalam **satu** datagram, lalu bila burst itu sengaja hilang meretry reliable msg
2/4/5 terpisah dengan identitas asli, sebelum `PlayerInit` memicu RequestClass
`#0` pada channel yang benar. Peer itu juga mengirim 256 RPC ordered valid sebelum `PlayerInit`
lalu `PlayerInit` duplikat untuk mengunci hand-off retained dan memastikan
bootstrap class tetap tepat satu kali, termasuk penolakan server penuh pre-cookie
serta deadline connect saat cookie berhenti. Build GitHub Actions
`39475012329` lulus untuk Kotlin/GLES, NDK packaging, dan kedua ABI; artifact
debug internalnya lolos cek ZIP/ELF `armeabi-v7a` + `arm64-v8a`.

Itu **bukan** bukti kompatibilitas gameplay/live UIF. Dua observasi native yang
berbeda pada 2026-10-20 dicatat agar tidak tercampur: (1) sebelum koreksi urutan
source 2/3/4/5, sesi sembilan detik accepted/aktif menerima 36 packet/34 RPC
broadcast tanpa `PlayerInit`, class, atau spawn; (2) satu konfirmasi bounded
sesudah koreksi urutan itu—**sebelum** implementasi coalescing saat ini—menerima
38 packet/33 RPC, satu pong, serta tidak melihat retransmit/error, namun tetap
tanpa `PlayerInit`, class, atau spawn. Tidak ada traffic UIF lagi setelah
coalescing ditambahkan. Tidak satu pun observasi membuktikan ACK/receipt semantik
ClientJoin, class/spawn, atau gameplay UIF; tidak ada sync, chat, command, atau
input gameplay dikirim. Coalescing hanya bukti fidelity source + regresi host,
bukan perbaikan admission UIF. Validasi APK/device terukur tetap pending.
Workspace ini tidak memiliki Gradle/Kotlin/Android SDK lokal; build APK tetap
hanya boleh lewat GitHub Actions sesuai gate rilis.

Tombol **Probe RakNet** di detail server memberi observability alur transport,
bukan bukti gameplay:

```text
open → cookie → auth → accepted → ClientJoin → stream RPC (bila server mengirim)
```

State pemain hanya direduce setelah RPC valid benar-benar diterima; accepted atau
broadcast saja tidak dipresentasikan sebagai world playable.

## Kompatibilitas

SampDroid dibangun native untuk:

- **`armeabi-v7a`** — perangkat Android ARM 32-bit;
- **`arm64-v8a`** — perangkat Android ARM 64-bit.

Minimum Android: **API 24 / Android 7.0**. Dua ABI berarti APK tidak memerlukan
Winlator, tetapi performa akhir tetap akan bergantung pada RAM/GPU saat renderer
dunia selesai dibuat.

## Native Android input

Touch, gamepad, keyboard Bluetooth, gyro, dan HUD berada dalam scope internal
APK. File layout hanya mendeskripsikan posisi tombol native — tidak memuat
script, plugin PC, executable, atau asset eksternal.

- [Native input layout v1](docs/NATIVE_INPUT.md)
- [Schema input](schemas/native-input-layout.schema.json)
- [Contoh default landscape](examples/native-input-layouts/default-landscape.json)

## Open source untuk kontributor

- Kode SampDroid: **[MIT License](LICENSE)**.
- **Tidak ada CLA**; kontributor mempertahankan copyright-nya.
- Kontribusi memakai [DCO](DCO.txt), jadi masuk/keluar tetap MIT.
- Repo public, GitHub Discussions, issue tracker, CI, roadmap, dan APK release
  ada di `xykal/sampdroid`.
- Panduan: [CONTRIBUTING.md](CONTRIBUTING.md), [GOVERNANCE.md](GOVERNANCE.md),
  [SECURITY.md](SECURITY.md), [NOTICE.md](NOTICE.md).

Kontributor dibutuhkan untuk network/session, native renderer bridge, Android
input/HUD, ARMv7 optimization, test vector, accessibility, dokumentasi, dan
localization. Tidak ada kontribusi yang boleh membawa game assets, Windows binary,
crack, credential, cheat, atau bypass server.

## Arsitektur

```text
Kotlin Android shell
  ├─ server browser / native HUD / native input
  └─ JNI
       └─ C++17 native client core
            ├─ query protocol
            ├─ RakNet bitstream + ACK + split reassembly
            ├─ session + RPC/world state
            ├─ player sync encoder + remote decoder
            └─ Android game/world bridge
```

Detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) dan
[native session lifecycle](docs/NATIVE_SESSION.md).

Dokumentasi protocol:

- [RakNet + SA-MP](docs/protocol/raknet-samp.md)
- [Tabel RPC open.mp](docs/protocol/rpc-ids.md)

## Build resmi — GitHub Actions

Semua APK resmi di-compile oleh **GitHub Actions** dari repo publik, bukan build
lokal maintainer. Setiap push/PR menjalankan regression test C++ lalu build APK
untuk `armeabi-v7a` + `arm64-v8a`, kemudian memverifikasi kedua `.so` ada di APK.
Push tag `v*` hanya membuat **draft release** bila keystore resmi tersedia di
GitHub Secrets: CI menghasilkan APK `release` signed, bukan debug APK, lalu
maintainer mempublishnya manual setelah review. Lihat
[signing rilis](docs/RELEASE_SIGNING.md).

## Roadmap

Lihat [docs/ROADMAP.md](docs/ROADMAP.md). Jalur prioritas port sekarang:

1. persistent RakNet session, class selection, spawn, dan sync;
2. state world untuk player, vehicle, object, pickup, checkpoint, dialog, chat;
3. HUD + input Android native;
4. world/game bridge tanpa runtime Windows;
5. optimasi perangkat ARMv7 dan ARM64.

## Asset policy

APK tidak membundel GTA APK/OBB/data, binary Windows, atau asset pihak ketiga.
Backend dunia memakai data yang pengguna miliki sendiri; ini menjaga SampDroid
APK Android native, kecil, dan bukan repack game. Menu **Data GTA** menerima
folder GTA hasil ekstrak melalui Android Storage Access Framework—termasuk folder
SD card—atau URL **HTTPS ZIP yang pengguna sendiri masukkan**. Jalur URL tidak
menyediakan katalog/hotlink asset: ia membuat salinan unduhan sementara di folder
yang dipilih pengguna, mendukung SHA-256 pembanding, membatasi ukuran/entry dan
ZIP traversal/bomb, memeriksa `data/gta.dat` serta header `models/gta3.img`, lalu
baru mencoba mount native read-only. Jika parser native menolak, tree hasil
ekstrak tidak dipersist/integrasikan; archive sumber tidak diubah. Data final
selalu tetap di folder SAF/SD card, bukan dicopy ke internal. Setelah mount,
runtime membaca `gta.dat`, IMG, COL/IDE/IPL text, resolver dinamis stock opsional
`data/vehicles.ide`/`data/peds.ide`, binary `parent_streamN.ipl` relevan, dan
saat pengguna menjalankan sinkronisasi map, member `.col` IMG yang dibatasi
(maksimal 256 / 32 MiB) lewat descriptor read-only. RAR dan executable PC tidak
dieksekusi atau dibundel. Fondasi indeks IMG/COL, `gta.dat`/IDE/text+binary IPL,
dan urutan runtime dunia: [docs/GAME_DATA_RUNTIME.md](docs/GAME_DATA_RUNTIME.md).
