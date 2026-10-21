# Runtime data dunia native

Dokumen ini menjelaskan jalur menuju dunia yang benar-benar bisa dimainkan di
SampDroid. Ini **bukan** fitur launcher modpack dan bukan mekanisme untuk
mendistribusikan game/data proprietary di APK.

## Kontrak data

APK hanya membawa kode open-source SampDroid. Saat world renderer nyata mulai
dipakai, pemain memilih data GTA San Andreas yang memang ia miliki sendiri.
SampDroid membaca data akhir **read-only** lewat storage Android dan tidak
menyertakan atau meng-host file game dalam APK/release GitHub.

Selain memilih folder hasil ekstrak, pemain boleh memilih **ZIP lokal** yang
sudah ia miliki atau memasukkan URL **HTTPS ZIP** miliknya sendiri, lalu memilih
folder tujuan melalui SAF (termasuk removable SD card). URL itu bukan katalog
asset SampDroid: tidak ada URL game yang dipaketkan, hotlink, atau bundle
proprietary. Baik input lokal maupun unduhan hanya **dibaca**, lalu disalin ke
folder sementara pada storage pilihan, dapat dibandingkan terhadap SHA-256 yang
pemain masukkan, dibatasi ukuran/entry/ratio, dilindungi dari traversal ZIP,
lalu dicek untuk `data/gta.dat` dan header `models/gta3.img`. Baru setelah itu
runtime mencoba parse/mount descriptor read-only. Jika validasi struktur atau
parser native gagal, data tidak dipersist sebagai root aplikasi; arsip input
tidak pernah diubah dan tidak ada tree GTA akhir yang disalin ke internal
storage.

Pemilih `ACTION_OPEN_DOCUMENT_TREE` wajib memberi grant SAF yang dapat
dipersist. Jalur folder membutuhkan read grant; jalur installer membutuhkan
read+write grant untuk staging di media pilihan. UI menolak grant session-only
sebelum unduhan/ekstraksi agar folder SD yang tampak terpasang tidak gagal diam-
diam saat aplikasi dibuka ulang. Untuk hasil installer, yang disimpan adalah URI
**tree asal** plus relative path root GTA yang bounded; proses berikutnya membuka
ulang tree asal lalu turun melalui path tersebut, bukan menyimpan URI child yang
tidak dapat diremount dengan benar oleh semua provider. Relative path dihitung
sebelum rename folder akhir dan leaf final direkonstruksi dari nama yang telah
divalidasi, sehingga provider SAF yang meninggalkan `DocumentFile` lama/cache
URI stale setelah `renameTo()` tidak dapat mempersist path pre-rename. Installer
lalu membuka ulang path final itu dari tree grant sebelum menyatakannya terpasang;
provider yang tak bisa meresolve path dibatalkan transaksional, bukan meninggalkan
preference root yang rusak. Mount, restore setelah process death, map sync, dan hand-off archive diserialkan lewat mutex runtime;
startup restore membawa epoch pilihan sehingga tidak boleh memasang tree lama
setelah pemain mulai memilih sumber pengganti.

```text
user-owned folder OR local ZIP OR user-supplied HTTPS ZIP
  └─ temporary SAF/SD-card validation + extraction (ZIP only)
       └─ IMG v2 / COL / IDE / IPL / DFF / TXD read-only root
            └─ loader native bounded
                 └─ collision + map streaming + renderer Android
                      └─ local movement/gameplay bridge + SA-MP sync
```

## Tahap awal yang sudah mulai dibangun

`gta_img_archive.{h,cpp}` adalah pembaca **indeks** GTA SA IMG v2 (`VER2`):

- tidak memuat archive multi-gigabyte ke RAM;
- memvalidasi header, directory, nama member, aritmetika sektor 2048 byte,
  range setiap entry, dan duplicate entry;
- punya `ByteSource` random-access dan implementasi `FileDescriptorByteSource`
  yang menduplikasi descriptor SAF lalu memakai `pread`, bukan buffer raksasa;
- menu **Data GTA** sudah memakai `ACTION_OPEN_DOCUMENT` untuk memilih IMG,
  COL, IDE, atau IPL text/`bnry` secara eksplisit; descriptor diduplikasi native
  dan URI grant IMG disimpan best-effort, tanpa storage permission lebar;
- extraction tetap dibatasi caller (`maxBytes`) dan fail-closed.

`gta_dff_mesh.{h,cpp}` sudah mulai membuka jalur renderer aset nyata: ia membaca
hierarki RenderWare GeometryList/Geometry portable yang bounded dan menghasilkan
vertex/triangle/UV/prelit color, slot material, warna diffuse, serta nama texture
lower-case tanpa memakai code/vendor library game. Nama dictionary stock dengan
spasi literal (mis. `new road`) tetap merupakan key, bukan path; separator dan
control byte tetap ditolak. Flag BinMesh triangle-strip stock tetap membaca array
`RpTriangle` kanonis yang bounded; extension tersebut tidak dipakai sebagai stream
indeks alternatif. `FrameList`/`Atomic` portable sekarang divalidasi bounded dan
mengaplikasikan transform parent statis ke mesh sebelum renderer menerimanya. UV
non-finite stock dikanonisasi ke origin texture agar tidak pernah mencapai GLES,
sementara posisi/normal/index rusak tetap fail-closed. Native platform geometry,
skin, animation, dan batching masih ditolak atau ditunda sampai diuji; parser
belum berarti map DFF sudah dirender.

`gta_txd_texture.{h,cpp}` melengkapi jalur material awal: ia membaca dictionary
Raster Direct3D PC dan mendecode top-level mip ke RGBA bounded untuk raw
8888/888/565/4444 serta DXT1/DXT3/DXT5. Normalisasi key TXD sama dengan DFF,
termasuk spasi literal stock, sehingga lookup material tidak berubah. Palette,
PS2/XBOX, cube texture, dan format native yang belum diuji ditolak fail-closed.
Dictionary stock valid tanpa raster diterima sebagai no-op struktural, bukan
kesalahan atau texture palsu. Renderer dapat membuat satu
preview batch bila nama material DFF menemukan texture TXD yang unik (atau ada di
dictionary bernama sama dengan DFF); batch placement nearby juga dapat meng-overlay
satu material yang cocok melalui dictionary IDE. Untuk section IDE `txdp`, resolver
nearby mencoba dictionary child lalu parent eksplisitnya saja; graph disimpan
transactional, konflik/siklus ditolak, chain dibatasi 16 hop, dan satu refresh
maksimal memicu empat baca TXD IMG lazy. Semua kasus ambigu ditolak. Primary
GTA3 IMG diperiksa lebih dulu lalu sampai empat IMG asset tambahan yang dipasang
read-only (misalnya `models/cutscene.img`), sehingga parent `txdcut.ide` stock
boleh berada lintas archive tanpa copy data; collision dan `parent_stream` tetap
otoritas IMG utama. Binding seluruh material/map, GPU cache, dan streaming masih
tahap berikutnya.

`gta_collision.{h,cpp}` adalah decoder read-only untuk archive `COL2` / `COL3`
yang umum di GTA SA PC:

- parser menangani model berurutan, bounds, sphere, axis-aligned box, compressed
  vertex, dan triangle mesh;
- offset COL harus berada dalam model yang sama; size/model/triangle dibatasi;
- `COLL`/`COL4` ditolak eksplisit sampai implementasinya diuji, tidak ditebak;
- kernel `findHighestFloor` sudah dapat mencari lantai lokal dari primitive atau
  mesh, sebagai dasar grounding/collision player nanti.

`gta_collision_catalog.{h,cpp}` menghubungkan keduanya tanpa melakukan scan atau
copy seluruh IMG: caller memilih satu member `.col` yang sudah diindeks, loader
membaca tepat member itu di bawah extraction cap, parse COL secara transaksional,
dan publish model collision bernama ke catalog bounded. Tahap spatial streaming
nanti yang menentukan member mana yang diminta di dekat player.

`gta_collision_world.{h,cpp}` lalu menerima placement IPL text maupun strict
binary yang collision model-nya sudah tersedia, menerapkan quaternion/posisi ke
sphere, box, dan mesh, dan membuat spatial grid 64m yang capped. Query lantai
world-space memfilter interior dan tidak bergantung pada proxy renderer. Selain
floor query, ada sweep silinder player horizontal yang bounded untuk box, sphere,
dan maksimal 4096 triangle wall per instance-query; ini konservatif agar input
Android tidak tembus dinding dasar, bukan physics GTA lengkap. Placement yang
model COL-nya belum dimuat dilaporkan/ditunda untuk streaming, bukan dipalsukan
sebagai ground.

`local_movement.{h,cpp}` adalah pilot deterministic yang reset dari posisi
authoritative, walk/run/jump di atas `CollisionWorld`, mengikuti floor, memakai
sweep silinder sisi dasar, dan menolak koordinat free-flight saat collision tidak
tersedia. `local_gameplay_bridge` memakainya dari tick surface dunia Android
(radar/GPU) lalu menghasilkan kandidat `PlayerFootSync` hanya jika floor pada
interior authoritative yang sama, input, vitals, dan state server valid; retained
session mengirimkannya sendiri pada rate server setelah spawn/ACK. SetSpawnInfo/
SetPlayerPosition, freeze, transisi SetPlayerInterior, source collision kosong,
dan server velocity membuang prediction atau menolak tick. Sweep ini belum
capsule/mesh physics GTA penuh; driver vehicle kini dapat memakai envelope
kinematic terbatas dari `handling.cfg` milik user (sesudah join `cars` IDE),
tetapi suspension/wheel contact/drivetrain/damage/animasi dan validasi server
nyata masih belum ada.

`gta_map_manifest.{h,cpp}` sekarang menangani map descriptor tanpa mengakses
filesystem secara langsung:

- `gta.dat` membaca directive `IDE`, `IPL`, `COLFILE`, dan `IMG`, lalu
  menormalkan path relatif lower-case dan menolak path absolut/traversal;
- section placement-capable `objs`, `tobj`, dan `anim` IDE menerima layout SA
  compact maupun mesh-count 1–3, menghasilkan model ID, nama model, TXD, draw
  distance pertama, dan flags. Resolver `cars`/`peds` membaca prefix
  model/TXD/type yang dibutuhkan renderer; untuk `cars`, token handling kelima
  juga disimpan sebagai key name-only untuk join profile `handling.cfg` yang
  sudah dipilih user. Token itu tidak menjalankan mod/skrip PC dan tidak
  mengubah authority server; root minimal empat-field tetap bisa merender dengan
  fallback generic. Kasus stock `peds` ID 0 `null` dipetakan eksplisit ke
  `player.dff`, dan tiga row `cars` stock yang menggabungkan nama DFF/TXD
  identik hanya diterima lewat repair token-duplikat sempit. Identifier stock
  yang memakai `+` atau tanda kurung tetap name-only (bukan path), sedangkan ID
duplikat ditolak agar mapping tidak ambigu. Section `txdp` menyimpan relasi
child→parent dictionary yang sudah dinormalisasi (duplikat identik no-op;
child bertentangan ditolak) untuk fallback texture terikat, bukan sebagai path
atau instruksi memuat asset;
- section text `inst` IPL menghasilkan placement model/interior/posisi/
  quaternion/LOD tervalidasi. Bila IDE tersedia, model ID adalah otoritas dan
  nama text historis seperti `dummy` dinormalisasi ke mapping IDE; `zone`,
  `occl`, dan section stock non-placement yang dikenal menjadi no-op bounded
  agar root stock tidak dipalsukan sebagai placement atau ditolak;
- strict binary IPL `bnry` membaca header 76-byte dan INST 40-byte secara
  little-endian, memvalidasi count/offset/float/quaternion, mengambil low byte
  interior dan signed LOD, lalu mewajibkan mapping IDE sebelum placement dipakai;
  CARS/section binary lain dibiarkan opaque, bukan ditebak;
- seluruh hasil bersifat transaksional dan capped sehingga data rusak tidak
  sebagian mengubah state runtime.

`gta_data_runtime.{h,cpp}` sekarang mengikat file yang dipilih menjadi satu
runtime read-only yang nyata, bukan sekadar inspector: primary IMG mount
mempertahankan fd duplikat dan hingga empat companion asset IMG dapat ditambah
secara transactional, COL dapat berasal dari member primary IMG atau `COLFILE`
eksternal, portable DFF member dapat masuk catalog mesh bounded, TXD member
didecode ke texture
catalog bounded, lalu IDE dan IPL text/binary digabung dan collision world
dibangun ulang secara transaksional. Sesudah manifest + IDE tersedia, runtime
mencari **hanya** member IMG bernama `parent_streamN.ipl` yang parent-nya ada
di `gta.dat`; setiap member wajib magic `bnry` dan seluruh set ditemukan
committed atau ditolak bersama. Pada aksi sinkronisasi map yang eksplisit,
runtime juga dapat mengindeks semua member `.col` IMG primary **dan** companion
yang sudah dipasang secara transaksional—maksimal 256 member dan 32 MiB entry
padded total untuk gabungan seluruh archive—lalu membangun collision world sekali.
Bila nama model bertabrakan, primary GTA3 menang konsisten dengan lookup asset
utama. Batas itu mencakup root GTA SA stock tanpa menjadikan setiap IMG/modded
archive sebagai scan atau alokasi tak terbatas. Tidak ada member yang dieksekusi,
ditulis, atau dipaketkan ke APK.

`gta_vehicle_handling.{h,cpp}` menambah reader bounded untuk `data/handling.cfg`
dari tree SAF yang sama. Ia hanya membaca profile whitespace stock-compatible
(`max velocity`, `engine acceleration`, `brake deceleration`, drag/traction, dan
`steering lock`), menolak ID duplikat, control/path-like byte, angka non-finite,
serta range tidak aman secara transaksional. Profile tidak berguna sendiri:
runtime harus menemukan `cars` IDE dengan model ID dan handling token yang sama.
Saat pemain benar-benar sudah server-authorized sebagai driver seat `0`, JNI
menyalin satu envelope kinematic tervalidasi untuk **model current** ke gameplay
tick; session mengecek model itu lagi di bawah mutex supaya stream/seat change
racy hanya jatuh ke fallback generic. Koefisien tetap dipersempit (speed,
accel/brake, coast, steering) dan seluruh floor/sweep COL, bounds, rate
Packet-200, serta koreksi server yang sudah ada tetap wajib lolos. Tidak ada
suspension, tyre/wheel, gearbox, boat/air/train handling, collision damage,
angular physics, atau authority outbound baru yang diklaim.

Status JNI memberi count struktural; bridge preview DFF terpisah tetap menyalin
vertex expanded bounded dari model terakhir dan, bila resolusi nama aman, satu
RGBA TXD bounded ke buffer Java. Font-4 TextDraw memakai lookup literal
`dictionary:texture` yang di-normalisasi ulang (tanpa path, titik, atau colon
lebih), membatasi salinan ke 512×512 pixel, dan boleh lazy-load paling banyak
dictionary child + satu parent `txdp`; hasilnya tetap hanya RGBA copy Canvas,
bukan resource Android atau file path. Font-5 dapat memakai batch DFF prelit
kecil lewat resolver model yang sama, tetapi tidak mengklaim kamera/material
frontend GTA. Selain itu bridge nearby-world menyeleksi
placement IPL dekat anchor pemain dengan ID interior authoritative yang persis
sama (default exterior 0 bila belum diketahui), lazy-load maksimal empat DFF
IMG per refresh, menerapkan transform IPL, lalu meng-copy batch prelit RGBA
bounded ke GLES. Bila mapping IDE dictionary dan material cocok, sampai empat overlay
TXD/material independen juga di-copy, termasuk material berbeda yang resolvable
dari satu DFF portable; placement/instance yang memakai texture immutable sama
digabung dalam satu slot. Lookup dapat jatuh dari `txdp` child ke
parent yang tervalidasi di primary atau auxiliary IMG yang memang dipasang.
Mesh tanpa UV yang tidak memakai material terpilih tidak lagi membatalkan
overlay dari sibling mesh yang aman, tetapi triangle yang diekspor tetap wajib
punya UV/prelit/index finite lengkap. Semua format/relasi yang tidak aman
dilewati. Transform server-object yang sudah direduksi dari RPC—termasuk
transform `MoveObject` sampled monotonic—serta vehicle/skin dinamis dapat
resolve melalui mapping IDE yang sama ke batch DFF prelit dan sampai empat
overlay TXD bounded tersendiri. Attachment/material override tidak ditebak dan
proxy tetap fallback untuk model/material yang tidak resolve. Batch belum memuat
binding material-override/arbitrary TXD penuh, LOD/occlusion/time-object, atau
streaming collision dinamis; companion IMG kini ikut full-COL sync bounded,
namun discovery `parent_streamN.ipl` tetap primary-only.

Pemain dapat memilih folder GTA yang sudah diekstrak lewat SAF, termasuk
removable SD card, memilih ZIP lokal miliknya, atau mengunduh ZIP HTTPS yang ia
masukkan sendiri ke folder pilihan itu. Untuk jalur ZIP, pembanding SHA-256 bersifat opsional tetapi
ditampilkan/disimpan setelah native mount sukses; tanpa pembanding, digest tetap
dihitung namun UI jujur menyatakannya belum dibandingkan. Ekstraksi memakai
staging tree provider yang sama—bukan cache internal—dan hanya root yang lolos
struktur + parser native yang dipersist. Resolver mencari `data/gta.dat`,
memasang `models/gta3.img` sebagai IMG utama lalu maksimal empat companion
`models/*.img` dari manifest sebagai source read-only DFF/TXD dan COL gabungan
bounded, memvalidasi `data/handling.cfg` bila file itu ada, memuat COL/IDE/IPL
tree dalam batch descriptor, lalu secara opsional mencoba dua table resolver
stock yang memang tidak tercantum di manifest (`data/vehicles.ide` dan
`data/peds.ide`). Setelah itu ia menemukan streaming
IPL relevan dan catalog COL IMG bounded untuk root stock. Grant folder dan
pilihan sinkronisasi map dipersist agar IMG/manifest/map assembly dapat dibuat
ulang setelah process death tanpa menyalin data ke internal storage. File manifest
yang tidak ada di tree tidak ditebak; dua table dinamis tersebut juga no-op bila
root custom tidak memilikinya. `handling.cfg` yang tidak ada juga bukan alasan
mount gagal—driver tetap memakai envelope generic aman—sedangkan file yang ada
tetapi malformed ditolak jujur sebelum diintegrasikan.

Fixture regresi sintetis kecil untuk semua parser/bridge di atas sudah disiapkan
untuk validasi final batch ini; tidak ada data GTA yang masuk ke repository.

## Urutan menuju gameplay nyata

1. **Data source Android** — picker/read-only file descriptor + manifest data
   source, bukan path hardcode atau storage permission lebar.
2. **Map binding** — manifest, model ID IDE (`objs`/`tobj`/`anim`, `cars`, dan
   `peds`), COL, transform IPL text/strict `bnry`, dan discovery
   `parent_streamN.ipl` IMG sudah tersedia. `cars`/`peds` memungkinkan model
   vehicle streamed, skin player remote/local (local wajib memiliki template
   `SetSpawnInfo`; `SetPlayerSkin` non-DL kemudian dapat mengganti modelnya),
   serta NPC `ShowActor` visual-only dipetakan ke DFF user; state interior server kini memilih placement IPL
   ber-ID sama, dan floor/sweep collision memakai partition sama bila COL-nya sudah
   termuat; streaming multi-IMG masih berikutnya.
3. **Streaming world** — spatial index/zone loading agar ARMv7 tidak memegang
   seluruh San Andreas di RAM; batch saat ini hanya memilih placement pada
   interior server yang sama dekat anchor dan lazy-load DFF terbatas.
4. **Renderer asset native** — batch DFF prelit placement serta object/vehicle
   streamed (`cars`), skin player remote/local, NPC `ShowActor` (`peds`), dan
   sampai 16 `CreatePickup` terdekat (IDE user-owned, radius 96 m) sudah masuk
   GLES dengan copy/refresh bounded. Pickup marker tetap fallback/interaksi;
   semua entitas tersebut tetap bukan collision, target, atau authority gameplay.
   TXD/material multi-batch, cache GPU, LOD, occlusion, animasi actor/vehicle,
   dan fallback perangkat rendah masih perlu.
5. **Local simulation** — floor grounding + side sweep COL dasar sudah ada;
   seat driver 0 yang diotorisasi juga punya pilot kinematic Packet-200 terbatas
   di atas collision yang sama. Bila tree user memuat `handling.cfg` valid dan
   `cars` IDE yang cocok, envelope speed/accel/brake/coast/steer pilot memakai
   source profile vehicle itu; tanpa join yang pasti ia tetap fallback generic.
   Capsule/mesh physics lengkap, camera, animation, weapons, suspension/wheel/
   drivetrain/damage kendaraan GTA, dan collision nyata penuh masih perlu.
   Input tidak akan menjadi sync posisi bebas.
6. **SA-MP bridge penuh** — static `Create/Set/DestroyObject` sudah punya jalur
   bounded sampai batch DFF bila mapping asset tersedia; global `CreatePickup`
   juga dapat memakai DFF user-owned terdekat dengan marker fallback + `USE` →
   RPC 131 user-confirmed tanpa reward lokal. Model/font TextDraw GTA penuh
   (font-4 sprite strict sudah
   bisa resolve satu TXD RGBA user-owned bounded dan font-5 punya preview mesh
   prelit bounded, tetapi bukan frontend GTA lengkap), attachment/material/MoveObject DFF dinamis, vehicle/passenger/trailer, serta
   continuous sync yang hanya aktif setelah
   state local dan collision tervalidasi masih perlu.

Sampai langkah 4–6 selesai, radar/proxy, batch DFF per-interior, dan preview
mesh satu-per-satu tetap diberi label visual/eksperimental; semuanya belum klaim
bahwa GTA world atau gameplay PC sudah terporting penuh.
