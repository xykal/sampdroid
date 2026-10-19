# Roadmap port native SampDroid

Tujuan roadmap ini jelas: mengganti kebutuhan menjalankan client SA-MP PC lewat
Winlator dengan APK Android native. Tidak ada milestone launcher modpack PC.

## Milestone 0.6 — protocol/state observability

- [x] Open connection, cookie, auth, accepted, ClientJoin.
- [x] ACK reliable, multi-packet datagram, split reassembly.
- [x] `PlayerInit`, PlayerJoin/Quit/Name/Color state reducer.
- [x] Host regression tests untuk wire vector dan lifecycle player state.
- [x] Client `PlayerRequestClass` / `PlayerRequestSpawn` / `PlayerSpawn` builder,
  response decoder dual legacy/open.mp, dan transport-ACK gate.
- [x] Retain socket setelah ClientJoin dan kirim class/spawn flow lewat session native.
- [ ] Live interoperability capture untuk class/spawn di beberapa server nyata.
- [x] Decode player StreamIn/StreamOut, exact non-DL `SetPlayerSkin` (153), dan
  broadcast remote `PlayerFootSync` (207) ke state entity bounded; model snapshot
  menuju renderer DFF/proxy bounded ada, tetapi animasi/capture nyata belum.
- [~] Decode vehicle/global-object/global-pickup state: remote vehicle proxy,
  global object lifecycle + source-audited Move/Stop motion, dan `Create/DestroyPickup` +
  bounded render state ada; player-object/player-pickup variants dan captures
  authoritative belum.
- [ ] Sanitized capture replay test dari server nyata.

## Milestone 0.7 — persistent native session

- [x] Explicit connect/disconnect dan clean UDP teardown dari Android shell.
- [x] Socket retained setelah ClientJoin; peer IP/port filter dan bounded state.
- [x] `RELIABLE_ORDERED` channel-2 class request, ACK consumption, bounded retry queue.
- [x] Class selection, SetSpawnInfo, local-player spawn/position/vitals, dan spawn response state reducer.
- [x] Lifecycle-aware automatic poll saat detail session terlihat; setiap poll
  memakai paling banyak satu receive blocking pendek (40 ms), lalu—hanya bila
  receive pertama tidak timeout—menguras paling banyak 15 datagram yang sudah
  siap secara nonblocking. Jadi satu poll maksimal 16 attempt receive tanpa
  mengunci backlog sync/RPC menjadi satu UDP frame per poll.
- [x] Transport keepalive reliable 5 detik + reply internal ping.
- [~] ACK range bounded kini canonical outbound (sort/dedupe, contiguous non-wrap
  coalesce, span/record cap) serta decoder inbound membatasi range; reconnect,
  ping/timeout metrics, dan resend window penuh masih berikutnya.
- [x] Bounded `SendClientMessage` chat state.
- [x] Normal + race checkpoint native state.
- [~] Dialog native lengkap untuk style umum + response reliable. Legacy menu
  source-verified `Init/Show/Hide` (`76/77/78`) kini juga menjadi Android row
  presenter dengan select/exit revision-bound (`132/140`). TextDraw
  `Show/Hide/SetString` bounded ke Canvas text/box proxy dan selection tap →
  RPC 83 revalidated; `SendGameText` 73 juga menjadi banner timed portable.
  RPC 41/42 juga punya playback `MediaPlayer` HTTPS-only yang lifecycle-bound;
  HTTP/endpoint tidak aman sengaja tidak difetch. Font-4 TextDraw sprite `dictionary:texture` kini dapat memakai satu TXD RGBA user-owned yang bounded/cached (bukan Android drawable); font-5 juga dapat menampilkan mesh DFF prelit user-owned bounded bila model resolve. Kamera/material/font GTA, animasi, dan full style masih belum.
- [x] Fondasi `OnFootSync` packet 207: vector regresi client→server,
  `UNRELIABLE_SEQUENCED` channel 1, ordering counter terpisah, rate gate dari
  `PlayerInit`, serta spawn/session gate.
- [x] `PlayerAimSync` packet 203 terbatas: writer client→server fixed 32 byte
  vector-tested, packed `CamZoom:6|WeaponState:2` source-verified, USeq
  channel-1 + `WeaponRate` sendiri. Hanya dapat memakai transform
  collision-grounded, yaw/pitch native bounded, dan weapon+ammo authoritative
  saat AIM ditahan/on-foot; spawn/ACK/freeze/vehicle/kamera-server gate fail-closed.
  Kandidat yang lolos juga memasok view aim orang-pertama + reticle renderer dari
  yaw/pitch sama, tanpa memakai target marker. Saat AIM+FIRE, Packet-206 client
  41-byte `HitType=None` rate-capped dapat dikirim dari pose yang sama dengan
  weapon/ammo server-known; form tanpa `PlayerID` sudah diaudit pada reader +
  generator NPC open.mp commit `3d20f3dc`, ditahan bila `LagCompensation=0`,
  tetapi belum diuji live terhadap legacy server/anticheat. Formnya netral untuk
  ID/offset dan tidak punya target entity/raycast remote; `HitPos` hanya dapat
  berasal dari ray COL statik dekat atau sentinel nol, tanpa hit/damage/reward
  atau decrement ammo lokal. Broadcast remote Packet-203 34-byte
  juga didecode ke segmen arah kamera sementara pada scene untuk actor yang sudah
  stream-in. Packet-206 remote yang sudah direlay server juga hanya menjadi cue arah
  pendek sementara tanpa hit ID/target/damage state; no-impact sentinel tidak
  diproyeksikan ke world origin.
- [x] Touch semantic mapper bounded + local-player → OnFoot preview, tanpa packet
  movement otomatis atau posisi/collision palsu.
- [x] Decoder Packet 207 server→client dengan quaternion/percent/velocity
  compressed, opsi surfing/animation, guards malformed, dan lifecycle remote entity.
- [~] `Reset/Give/SetPlayerArmedWeapon` plus `SetPlayerAmmo` server kini menjadi
  state ammo/armed bounded (`SetPlayerAmmo` compact `u8|u16` source-verified);
  armed weapon authoritative masuk byte Packet-207/200, Packet-203 saat AIM, dan
  Packet-206 `HitType=None` saat AIM+FIRE tanpa touch menebak selection. Resolver
  target/raycast/hit/damage, animasi tembak lengkap, inventory slot GTA, dan ammo
  depletion lokal sengaja belum ada.
- [~] Encoder OnFoot kini dihubungkan ke tick surface dunia Android (radar/GPU)
  melalui bridge collision-grounded: hanya setelah PlayerInit/spawn/ACK, `TogglePlayerControllable`,
  state lokal, gravity `PlayerInit`/RPC 146 dalam envelope aman, dan floor `CollisionWorld` valid. Ia mengirim Packet-207 pada rate
  server, memakai partition floor/sweep COL pada interior authoritative yang cocok dan fail-closed bila belum ada atau gravity server eksotis belum termodelkan. `SetPlayerHealth(0)` juga sekarang memutus held input/prediksi serta semua sync lokal sampai server memulihkan health atau respawn native yang disetujui server; tetapi belum capture/live interoperability.

## Milestone 0.8 — native Android controls + HUD

- [x] Runtime touch pad v1: joystick, drag-kanan yaw/pitch look,
  jump/run/fire/aim/use → bounded semantic state + OnFoot preview/AimSync pitch.
- [~] Scene GPU **dan radar fallback** memakai overlay joystick/look/buttons
  yang sama untuk tick gerak collision-grounded + sync rate-gated. **USE** kini juga edge eksplisit:
  pickup server terdekat (gate spawn/posisi/interior/radius/rate native lalu
  RPC 131 reliable, tanpa reward lokal), lalu request enter/exit vehicle yang
  baru mengubah seat setelah RPC server 70/71. Seat driver 0 kini menjalankan
  steer/gas/mundur/rem kinematic collision + Packet-200 `InCarRate`; passenger
  Packet-211 anchor/input dasar kini juga boleh memasang drive-by untuk SMG
  server-armed saat AIM (tanpa target/damage), Packet-203 aim on-foot, serta
  Packet-206 no-target AIM+FIRE terbatas sudah ada. Drive-by bullet/full vehicle
  physics/handling GTA, target/raycast/hit penuh, dan animasi/weapon gameplay
  lengkap masih berikutnya;
  gyro opt-in sudah dicatat pada poin kontrol berikutnya.
- [~] Keyboard Bluetooth (`WASD`/arrow + action keys) dan gamepad (stick/D-pad,
  buttons, shoulder/trigger) memetakan ke semantic input yang sama di scene GPU
  maupun radar fallback; `T`/`/`/Start membuka composer chat native dan Tab
  membuka roster tanpa menjadi state gameplay. Overlay touch scene/radar sekarang juga membawa **DUCK**
  yang sama dengan key crouch dan dapat diposisikan lewat LAYOUT. Gyro look kini
  opt-in per layout: sensor hanya mengeluarkan delta yaw/pitch bounded ke mixer
  semantic yang sama, berhenti saat freeze/spectate/dead/dismiss, dan tidak
  membawa posisi/target/packet sendiri. Semua perubahan UI ini belum
  compile/package Android atau divalidasi perangkat/sensor nyata.
- [x] Editor **LAYOUT** in-scene: drag joystick/tombol, scale per perangkat,
  reset, persistence internal, dan salin JSON v1 data-only ke clipboard.
  Dynamic file import sengaja belum dibuka.
- [~] Accessibility source-only: LAYOUT sekarang mengizinkan tombol sampai 172%,
  menyimpan toggle kontras tinggi, serta memberi preset satu tangan kiri/kanan
  yang masih bebas digeser untuk overlay in-scene/radar. TalkBack/device/sensor
  validation dan APK validation masih berikutnya.

## Milestone 0.9 — world/game bridge

- [x] Bounded world snapshot → Canvas radar Android: local/remote player,
  NPC `ShowActor` server terpisah, dan checkpoint terlihat tanpa Windows runtime;
  kamera drag/zoom lokal.
- [x] Scene GLES 2.0 Android tanpa asset: grid perspektif, proxy actor,
  checkpoint beacon, orbit/zoom camera, dan update dari snapshot immutable.
  Transform remote on-foot dan vehicle dynamic kini diblend maksimum 90 ms
  melalui dua redraw on-demand (≈30 fps saat snapshot 10 Hz; teleport/seat/static
  snap) agar marker gameplay tidak patah tanpa permanent continuous GPU load.
  Ini renderer geometri visual, bukan map/model GTA atau simulasi.
- [~] Spectator server lifecycle: guarded legacy-layout reducer untuk
  `TogglePlayerSpectating`/`PlayerSpectatePlayer`/`PlayerSpectateVehicle`
  (`124/126/127`) ada bersama snapshot immutable. Saat aktif, native memutus
  prediction dan memblokir seluruh OnFoot/InCar/Aim/Passenger lokal; radar,
  GLES camera, nearby asset anchor, label, dan nametag hanya mengikuti target
  jika entity stream target ada. Target tidak pernah menjadi local authority,
  dan target belum streamed tetap memakai anchor aman. Layout perlu diverifikasi
  lagi terhadap writer current/open.mp dan belum diuji di APK/server nyata.
- [~] Presentation server bounded: reducer + JNI sekarang membawa GangZone
  `Show/Hide/Flash/Stop` (radar clipped/flash visual), `PlayerShow/HideTextLabel`
  dengan attachment immutable (radar + Canvas overlay yang diproyeksikan GLES),
  `Set/RemovePlayerMapIcon` (diamond/beacon portable), waktu recipient
  `SetPlayerTime`/`TogglePlayerClock`, dan endpoint kamera
  `SetCamera*`/`Interpolate`/`BehindPlayer`. C++ regression dan audit syntax JNI
  sudah lulus; Kotlin/GLES belum melalui compile/package Android atau capture
  server nyata. Semua tetap renderer-only: tidak ada capture turf, LOS/font/sprite
  GTA penuh, atau mutasi collision/OnFootSync dari kamera.
- [~] Fondasi data game milik pengguna (lokal, belum validasi Android): indeks
  IMG v2 bounded + parser COL2/COL3 + floor/side-sweep serta raycast statik
  box/sphere/mesh bounded (interior exact, ≤256 unit, budget kerja), parser
  `gta.dat`, layout IDE `objs`/`tobj`/`anim`/`cars`/`peds`, serta IPL text/strict
  `bnry` capped dan fail-closed. Picker SAF memasang IMG utama, batch COL/IDE/IPL,
  dan opsional memuat `data/vehicles.ide`/`data/peds.ide` stock (dua table dinamis
  yang tidak dicantumkan `gta.dat`), lalu menemukan `parent_streamN.ipl` yang
  parent-nya ada di manifest serta pada sync eksplisit membangun catalog `.col` IMG
  stock yang dibatasi 256 member/32 MiB. Hingga empat companion `models/*.img`
  juga terpasang read-only untuk lookup DFF/TXD; eviction/streaming COL sekitar
  player, collision/placement multi-IMG penuh, dan validasi Android tetap
  berikutnya.
- [~] Local grounding pilot: reset posisi authoritative + walk/run/jump bounded
  di spatial collision world kini terhubung ke session scene dan Packet-207
  rate-gated. Floor + sweep silinder sisi terhadap primitive/mesh wall dasar
  tersedia; raycast statik COL juga kini memberi paling banyak `HitPos` world-only
  dekat untuk Packet-206 `HitType=None`, tanpa target entity atau damage authority.
  Koreksi SetSpawnInfo/SetPlayerPos membuang prediction; freeze, data collision
  kosong, server velocity, serta transisi `SetPlayerInterior` mereset prediction.
  State interior memilih placement IPL GLES dan floor/sweep COL yang
  ber-ID sama; bila floor belum termuat, tick tetap fail-closed. `SetWorldBounds`
  server juga kini menahan prediction on-foot/driver hanya di exterior tanpa
  mengarang push-back. Capsule/mesh physics penuh, handling/suspension vehicle GTA, streaming collision sekitar
  player, dan live validation belum.
- [~] Renderer asset path: parser DFF RenderWare portable vertex/triangle/UV/
  prelit/material reference dan decoder TXD RGBA/DXT PC sudah bounded. Scene
  GLES memuat batch DFF prelit placement IPL dekat player pada interior server
  yang cocok (exterior = 0) dari IMG user dan sampai empat overlay TXD/material
  IDE independen yang aman, termasuk
  beberapa material resolvable dari satu DFF portable. Jalur transform sama kini
  juga memberi sampai empat overlay untuk subset static object, vehicle streamed,
  skin player/local, dan NPC `ShowActor` dinamis; vertex serta RGBA
  dibatasi bersama per jalur (total 1 MiB pixel), slot texture stale dibersihkan
  di thread GL, dan upload byte RGBA identik dapat dipakai ulang antar refresh.
  `txdp` child→parent dictionary yang cycle-safe juga dipakai secara bounded
  dalam IMG terpasang, dan sibling mesh tanpa UV yang tidak memakai material itu
  tidak menutup overlay aman. Preview manual juga mendukung satu TXD unik.
  Transform static `FrameList`/`Atomic` portable sudah diterapkan sebelum mesh
  masuk batch. Primary GTA3 + maksimal empat auxiliary IMG asset dapat memberi
  lookup DFF/TXD lintas archive (termasuk `txdcut.ide` cutscene→gta3). Full-COL
  sync bounded kini mencakup primary + companion IMG dalam satu cap dan primary
  menang pada nama model duplikat; parent-stream IPL tetap primary-only, jadi
  streaming collision dinamis/multi-IMG penuh belum. Binding material override/
  arbitrary texture, skin/animasi, LOD/occlusion, dan streaming world penuh
  masih berikutnya.
- [~] User-owned game data runtime berbasis file descriptor Android tanpa copy
  atau download asset: SAF dapat memilih root GTA hasil ekstrak, resolver
  `gta.dat` memasang IMG utama dan menyusun catalog + spatial collision world
  secara transaksional. Root + status sinkronisasi map dipersist untuk rebuild
  setelah process death, termasuk SD card; seluruh asset streaming dan resolver
  hubungan DFF/IDE skala map masih berikutnya.
- [~] World/entity bridge: remote player state/radar + proxy scene ada; RPC
  `Create/Set/Move/Stop/DestroyObject` masuk snapshot/radar; transform static
  maupun `MoveObject` sampled monotonic dapat resolve ke batch DFF prelit lewat
  IDE/IMG user. `Create/DestroyPickup` tampil sebagai marker
  radar/GLES dan **USE** mengirim `PickedUpPickup` hanya setelah validasi native.
  USE juga request enter/exit vehicle; local seat datang hanya dari Put/Remove
  RPC server dan menghentikan OnFoot. Seat driver 0 dapat memakai pilot
  kinematic floor/sweep dan Packet-200 client exact; correction/mutator vehicle
  server mereset prediction. Hingga empat overlay TXD material bounded kini
  memakai transform yang sama untuk subset static object/vehicle/actor DFF.
  Attachment object→vehicle/object/player kini di-resolve renderer-only secara
  bounded (parent hilang/stream-out/siklik disembunyikan; heading parent tersedia saja).
  `PlayerBeginObjectSelect`/`OnPlayerSelectObject` RPC 27 kini punya picker daftar
  bounded dan sender native yang kembali memeriksa object/model/origin/mode server;
  setiap Begin/Cancel/mode replacement juga memajukan revision native sehingga tap
  daftar terlambat ditolak, dan satu choice queued adalah terminal sampai lifecycle
  server baru. `PlayerBeginObjectEdit`/`OnPlayerEditObject` RPC 117 juga punya fallback step
  transform touch yang revision/tuple/kind-nya native-gated (baseline editor
  khusus tetap attachment-resolved bahkan bila snapshot renderer 128 object
  sudah cap), dan `PlayerBeginAttachedObjectEdit`/`OnPlayerEditAttachedObject` RPC 116 kini punya
  editor offset/rotasi/scale attachment-local terminal dengan slot/model/bone/warna
  tetap native-owned (ABGR inbound→ARGB outbound). Keduanya bukan gizmo PC atau
  mutasi optimistis. Belum raycast mesh/GTA click point penuh. Fidelity pitch/roll vehicle, override
  material object, binding semua material/texture vehicle, player-pickup variant,
  drive-by/trailer sync, vehicle handling/fisika penuh, HUD GTA penuh, dan
  gameplay loop masih berikutnya. Remote
  Packet-211 passenger inbound kini punya anchor renderer source-verified dan
  tidak lagi diinferensi dari offset kursi. RPC 166 juga hanya memberi cue
  `WASTED` observer remote sementara (tidak pernah death lokal). `cars`/`peds` IDE kini memetakan model vehicle streamed, skin player remote/local
  yang sudah `SetSpawnInfo`, serta NPC `ShowActor` ke DFF user untuk batch visual
  bounded. NPC memakai lifecycle/ID map terpisah (`Show/Hide`, posisi, sudut,
  health; 1.000 retained/64 render), dan tetap visual-only tanpa collision,
  target/hit, atau interaction authority. RPC 86/87 player dan 173/174 NPC kini
  menyimpan directive animasi source-layout (lib/name 15/23 byte, delta, four
  packed flags, time) lalu menampilkannya sebagai cue/ring `ANIM`; belum ada
  eksekusi IFP/skeleton, dan flags tidak memodifikasi input/physics/sync.
  Proxy tetap fallback dan refresh model dinamis dibatasi terpisah dari streaming IPL.
- [~] Budget renderer source-only per `ActivityManager.memoryClass`/low-RAM:
  Lite/Hemat/Penuh menurunkan radius IPL, instance/vertex DFF, model dinamis,
  batch texture, dan pixel preview sebelum JNI pada perangkat kecil; ceiling
  aman tetap sama. Perilaku ARMv7/ARM64 dan low-memory Android belum dipackage
  atau diukur di perangkat.

## Milestone 1.0 — native client loop

- [ ] Join → class → spawn → sync → world/HUD/input berjalan sebagai APK native.
- [ ] Tidak membutuhkan Winlator, `samp.exe`, atau modpack PC.
- [ ] Reproducible tagged APK draft releases GitHub Actions untuk kedua ABI, lalu publish manual setelah review.

## Label issue

`good first issue`, `help wanted`, `protocol`, `android`, `native-port`,
`controls`, `accessibility`, `armv7`, dan `documentation`.
