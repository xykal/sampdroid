# RakNet + SA-MP: catatan protocol dan status validasi

Dokumen ini membedakan bukti **live**, source-compatible, dan regression host.
Hanya baris yang secara eksplisit berstatus ✅ live yang sudah dibuktikan langsung
terhadap server SA-MP/open.mp publik; jalur SampDroid yang belum diuji live tetap
ditandai 🧪/pending dan tidak boleh dipromosikan sebagai koneksi berhasil.
Diperbarui: 2026-10-21.

---

## 1. SA-MP memakai RakNet 2.x, bukan 4.x

Ini jebakan pertama. Semua tutorial RakNet di internet (Minecraft Bedrock dkk) pakai
RakNet 4.x dengan `OpenConnectionRequest1/2` + magic 16 byte + MTU probing.
**SA-MP sama sekali tidak begitu.**

RakNet 2.52 (yang dipakai SA-MP) punya handshake 3 byte:

```
C -> S   [24] [cookie_lo] [cookie_hi]        ID_OPEN_CONNECTION_REQUEST
S -> C   [26] [cookie_lo] [cookie_hi]        ID_OPEN_CONNECTION_COOKIE   (challenge)
C -> S   [24] [x_lo] [x_hi]                  x = cookie ^ 0x6969 (SA-MP) / ^ 0x6D70 (open.mp)
S -> C   [25] [00]                           ID_OPEN_CONNECTION_REPLY
```

ID paket (RakNet 2.52 / `RAKNET_LEGACY`):

| ID | Nama |
|---|---|
| 6 | ID_INTERNAL_PING |
| 7 | ID_PING |
| 9 | ID_CONNECTED_PONG |
| 11 | ID_CONNECTION_REQUEST |
| 12 | ID_AUTH_KEY |
| 23 | ID_DETECT_LOST_CONNECTIONS |
| 24 | ID_OPEN_CONNECTION_REQUEST |
| 25 | ID_OPEN_CONNECTION_REPLY |
| 26 | ID_OPEN_CONNECTION_COOKIE |
| 28 | ID_RSA_PUBLIC_KEY_MISMATCH |
| 29 | ID_CONNECTION_ATTEMPT_FAILED |
| 30 | ID_NEW_INCOMING_CONNECTION |
| 31 | ID_NO_FREE_INCOMING_CONNECTIONS |
| 32 | ID_DISCONNECTION_NOTIFICATION |
| 33 | ID_CONNECTION_LOST |
| 34 | ID_CONNECTION_REQUEST_ACCEPTED |
| 36 | ID_CONNECTION_BANNED |
| 37 | ID_INVALID_PASSWORD |

Sumber: `openmultiplayer/RakNet` → `Include/raknet/PacketEnumerations.h`,
`Source/RakPeer.cpp`, `SAMPRakNet.cpp`.

Client menyatukan `28/29/31/32/33/36/37` dalam classifier terminal murni.
Classifier itu dipakai pada fase accepted, AUTH_KEY, dan setelah ClientJoin;
penolakan eksplisit tidak boleh berubah menjadi pesan timeout AUTH_KEY yang
menyesatkan.

---

## 2. Cookie challenge (anti-bot)

Server tidak langsung menerima request. Pada request pertama (cookie = 0), server
membalas ID 26 berisi cookie yang dihitung dari IP kita:

```cpp
// SAMPRakNet.cpp
uint16_t GetCookie(unsigned int address) {
    const uint8_t* a = (const uint8_t*)&address;
    return (cookies[0][a[0]] | cookies[1][a[3]] << 8) ^ ((a[1] << 8) | a[2]);
}
```

Client harus mengirim ulang request dengan:

```
xordCookie = cookie ^ 0x6969      // SAMP_PETARDED (legacy)
xordCookie = cookie ^ 0x6D70      // OMP_PETARDED  (open.mp)
```

`0x6969` = "ii", `0x6D70` = "mp". Nama konstanta di source: `SAMP_PETARDED`,
`OMP_PETARDED`, `MAGIC_OMP_IDENTIFICATION_NUMBER = 0x006F6D70` ("omp").

**Catatan penting:** cookie di-*reseed* berkala oleh server. Kalau echo datang
terlambat (beberapa detik), cookie sudah berubah dan server mengirim cookie baru.
Implementasi harus: probe → terima cookie → echo **segera** → kalau dapat cookie
baru, ulangi dengan nilai terbaru (maks 5x).

---

## 3. Obfuscation layer — ini yang bikin semua paket RakNet polos di-drop

Setiap paket SA-MP (termasuk paket RakNet offline) dibungkus:

```
[1 byte checksum] [N byte hasil transformasi]
```

Algoritma decrypt (server, `SAMPRakNet::Decrypt`):

```cpp
uint8_t cur, checksum = 0, port = GetPort() ^ 0xCC;
for (int i = 1; i != len; ++i) {
    cur = src[i];
    if (!(i & 1)) cur ^= port;      // index genap (1-based)
    cur = key[cur];                 // substitution table 256 byte
    checksum ^= cur & 0xAA;
    out[i-1] = cur;
}
if (src[0] != checksum) return nullptr;   // drop
```

Inverse (yang harus dikirim client):

```
checksum = XOR dari (plain[i] & 0xAA)
out[i+1] = inv_key[plain[i]] ^ (i+1 genap ? (port ^ 0xCC) : 0)
out[0] = checksum
```

`key[256]` ada di `SAMPRakNet.cpp` dan sudah diekstrak ke
`app/src/main/cpp/samp_crypto_table.inc`. Provenance tercatat di `NOTICE.md`.

**Konsekuensi:** siapa pun yang mencoba connect dengan RakNet library biasa
(termasuk `facebookarchive/RakNet`) akan gagal total — paketnya di-drop diam-diam
sebelum masuk ke RakNet. Ini explains kenapa tidak ada proyek client SA-MP publik
yang pakai library RakNet standar.

---

## 4. Reliability layer (datagram format)

Datagram RakNet 2.x:

```
bit 0            : hasAcks
[jika hasAcks]   : compressed(count), lalu per range:
                     bit isSingle, uint16 minIndex [, uint16 maxIndex]
per internal packet:
  [jika hasAcks] : bit 0 (penanda)
  uint16         : messageNumber
  uint4          : reliability   (RAKNET_LEGACY offset: UNRELIABLE=6, RELIABLE=8, ORDERED=9)
  [jika ordered/sequenced]: uint5 orderingChannel + uint16 orderingIndex
  bit            : isSplit
  [jika split]   : uint16 splitId + compressed uint32 splitIndex + compressed uint32 splitCount
  compressed u16 : dataBitLength
  align ke byte  : data
```

`WriteCompressed(unsigned short)`:

```
high byte == 0  → bit 1 ; lalu
  low nibble high == 0 → bit 1 + 4 bit (nilai)
  else                 → bit 0 + 8 bit
else             → bit 0 + 16 bit
```

Contoh nyata — `ID_CONNECTION_REQUEST` reliable, messageNumber=1, payload `0B`:

```
02 00 D0 08 0B
│  │  │  └── 0x0B = payload (ID_CONNECTION_REQUEST)
│  │  └───── header bits: reliability=8, split=0, compressed length 8 bit
│  └──────── messageNumber bits (lanjutan)
└─────────── ack bit + messageNumber low bits
```

### Keepalive transport retained session

Referensi RakNet 2.52 mengirim `ID_DETECT_LOST_CONNECTIONS` (23) sebagai
`RELIABLE` apabila tidak ada reliable send selama lima detik dan resend queue
kosong. SampDroid menerapkan policy yang sama di retained session; frame
keepalive ikut queue/ACK/retry bounded biasa. Bila server mengirim
`ID_INTERNAL_PING` (6) dengan timestamp 32-bit, client membalas
`ID_CONNECTED_PONG` (9) unreliable dengan timestamp peer yang diecho dan
monotonic timestamp lokal. Ini behavior transport yang dicakup regression test,
bukan pengganti `PlayerSync` atau validasi gameplay live.

### Channel outbound RPC dan sync

Kontrak source open.mp yang diaudit di `SDK/include/network.hpp` menetapkan
angka ini secara eksplisit:

```text
OrderingChannel_Internal   = 0
OrderingChannel_SyncPacket = 1
OrderingChannel_SyncRPC    = 2
```

`PlayerConnect`/`ClientJoin` (25), class/spawn, chat, dialog, menu, dan RPC
client lain memakai `RELIABLE_ORDERED` pada **SyncRPC channel 2**. Movement
client (`PlayerFootSync`, vehicle, aim, passenger, dan BulletSync no-target)
memakai `UNRELIABLE_SEQUENCED` pada **SyncPacket channel 1**. Stand-alone
RakNet reference probe yang diuji terhadap UIF juga mengirim ClientJoin pada
channel 2 dan menerima `PlayerInit` tanpa memilih class/spawn.

RakNet tetap mempunyai `waitingForOrderedPacketWriteIndex[channel]` dan
`waitingForSequencedPacketWriteIndex[channel]` yang terpisah. SampDroid menjaga
`nextRpc` dan `nextSync` terpisah **dan** memakai channel berbeda: ClientJoin
mengonsumsi SyncRPC channel 2/index 0, RequestClass berikutnya channel 2/index
1; sync USeq pertama memakai SyncPacket channel 1/index 0. Regression host
mengunci header ClientJoin channel 2 serta fixture USeq channel 1 dan
independensi kedua counter.

### Comparator admission-path open.mp (bukan identifikasi UIF)

Audit read-only pada `openmultiplayer/open.mp` commit
[`3d20f3dc0e26917264cdc8585c65913e080a143c`](https://github.com/openmultiplayer/open.mp/tree/3d20f3dc0e26917264cdc8585c65913e080a143c)
memperlihatkan `PlayerConnect`/RPC 25 membaca body berikut:

```text
u32 VersionNumber | u8 Modded | dynstr8 Name | u32 ChallengeResponse |
dynstr8 Key | dynstr8 VersionString | [opsional u32 open.mp marker/version]
```

SampDroid baseline membangun enam field pertama dengan `4057`, `Modded=1`, nama
ASCII bounded, token accepted XOR `4057`, serial hexadecimal valid kelipatan
`1001`, dan `0.3.7-R2`; profile DL R1 mengganti tiga identity field terkait
menjadi `4062`, token XOR `4062`, dan `0.3.DL-R1`. Stock writer open.mp sendiri
tidak menulis trailer `u32`, jadi SampDroid juga tidak mengaku sebagai client
open.mp atau official via marker tersebut. Regression host memakai reader kecil
yang independen dari builder untuk memeriksa kedua profile terhadap bentuk source
ini, serial `<50`, batas versi `<=24`, dan penolakan body mandatory yang
terpotong.

Pada jalur server open.mp yang sama, `OnPlayerConnect` terlebih dulu memeriksa
auth mode player, hasil parse, serial/version, dan `requestPlayer`. Hasil
penolakan request-player selain `Ignore` mengirim `ConnectionRejected`/RPC 130
lalu kick. Sesudah input handler tidak memveto koneksi, dispatcher network
memanggil jalur player/core;
`Core::onPlayerConnect` memanggil `playerInit`, yang mengirim RPC 139. Artinya
receipt RakNet untuk ClientJoin masih berada **sebelum** bukti admission ini.
Implementasi UIF tidak diketahui; mekanisme tersebut hanya comparator dan tidak
membenarkan spoof marker, mengubah serial/channel, atau menyimpulkan alasan UIF
tidak mengirim InitGame.

---

## 5. Status implementasi

| Tahap | Status | Bukti |
|---|---|---|
| Query protocol (i/r/c/d/p) | ✅ | 1.237 server dari open.mp index, live query |
| Open connection + cookie | ✅ | server 185.169.134.67 membalas `19 00` |
| Obfuscation layer | ✅ | semua fase di atas butuh ini |
| Reliability datagram | ✅ **byte-identical** dengan BitStream RakNet; inbound `RELIABLE_ORDERED` kini juga dirilis per-channel hanya setelah run index kontigu (buffer bounded/overflow → close session) |
| Connection request accepted | ✅ **diterima dari server live** |
| AUTH_KEY challenge/response | ✅ 256 entri tabel, lookup runtime |
| ClientJoin / PlayerConnect RPC 25 | 🧪 frame msg=5/SyncRPC channel 2 dan burst post-accept 2/3/4/5 lengkap 70-byte byte-identical dengan BitStream legacy FakeBots; reliable ACK/retry tetap per-frame, snapshot/probe memisahkan ACK transport msg 2/4/5, dan loopback memverifikasi burst keluar sebagai satu datagram. Reader comparator open.mp current juga mengunci enam field baseline/DL dan absent marker. Probe UIF SampDroid source-current 2026-10-21 (1.000 ms, tanpa traffic gameplay) mencapai accepted + ClientJoin dan menerima ACK RakNet untuk msg 2/4/5, lalu 9 packet/4 RPC (`SetPlayerColor` ×2, `SendClientMessage` ×2) tanpa PlayerInit/class/spawn. ACK hanya receipt RakNet—bukan receipt semantik ClientJoin, admission, atau gameplay |
| ACK reliable | ✅ setiap fragment reliable di-ACK; outbound sort/dedupe serta range contiguous non-wrap bounded |
| Multi-paket datagram + split reassembly | ✅ sebelum RPC diserahkan ke decoder |
| PlayerInit RPC 139 | ✅ decoder wire format + test vector; observability di UI |
| PlayerStreamIn/Out RPC 32/163 + SetPlayerSkin 153 | ✅ lifecycle actor remote bounded dan update model non-DL exact; renderer tetap bounded/fallback bila DFF user belum resolve |
| Server NPC `Show/HideActor` + transform RPC 171/172/175/176/178 | 🧪 layout non-DL source-verified, lifecycle separate 1.000 retained/64 snapshot, host regression + radar/GLES proxy/DFF path; belum capture server nyata/animasi 173/174 |
| Broadcast remote PlayerFootSync 207 | 🧪 decoder compressed + reducer state/regression; belum live capture/renderer |
| SendClientMessage RPC 93 | ✅ bounded chat state + regression coverage; HUD renderer masih pending |
| Checkpoint RPC 37/38/39/107 | ✅ normal + race marker state, finite/negative-size guards; renderer masih pending |
| Retained native session | 🧪 auto-poll, bounded ACK/retry, 5s reliable keepalive, internal-ping pong; live capture multi-server masih pending |
| Persistent game sync | 🧪 | Packet client outbound inti memakai USeq SyncPacket channel 1 dengan counter sequenced terpisah dari SyncRPC channel 2; vector/regression host lulus, validasi live SampDroid masih pending |

### Bug yang ditemukan (dan diperbaiki)

**Bit ordering.** RakNet menulis bit **MSB-first** di dalam setiap byte, dan
nilai multi-byte ditulis byte-per-byte mulai dari byte paling rendah
(little-endian), tiap byte MSB-first. Implementasi pertama memakai LSB-first,
sehingga server membaca `messageNumber=260, reliability=10, length=1284` dan
membuang paket sebagai sampah. Versi yang benar menghasilkan:

```
00 80 43 80 0B     # msg=1, rel=8, len=8, payload=0x0B
```

Diverifikasi dengan mengompilasi `BitStream` milik RakNet sendiri dan
membandingkan output byte-per-byte.

### Urutan wire yang diimplementasikan

Berikut urutan outbound setelah `CONNECTION_REQUEST_ACCEPTED`, yang dikunci
oleh source `RakPeer.cpp`, fixture BitStream FakeBots `RAKNET_LEGACY=1`, dan
regresi host. Ini **bukan** klaim bahwa SampDroid sudah lolos sesi gameplay
server tertentu.

```text
C -> S  [18 00 00]                     OPEN_CONNECTION_REQUEST (probe)
S -> C  [1A xx xx]                     OPEN_CONNECTION_COOKIE
C -> S  [18 lo hi]                     cookie ^ 0x6969
S -> C  [19 00]                        OPEN_CONNECTION_REPLY
C -> S  reliable msg=0 {0B}            CONNECTION_REQUEST
S -> C  ACK untuk msg=0
S -> C  reliable {0C 11 "challenge"}  AUTH_KEY challenge
C -> S  reliable msg=1 {0C 28 "reply"} AUTH_KEY response
S -> C  reliable {22 ...}              CONNECTION_REQUEST_ACCEPTED
C -> S  satu datagram normal, hasAcks=false:
        reliable msg=2 {1E ip4 port}   NEW_INCOMING_CONNECTION (7 byte)
        unreliable msg=3 {06 time32}   INTERNAL_PING
        reliable msg=4 {29}            empty static-data record
        reliable ordered msg=5,
        channel=2 index=0 {14 19 ...}  PlayerConnect / ClientJoin SyncRPC 25
S -> C  INTERNAL_PING/static-data       post-accept transport metadata
C -> S  jika ping: pong unreliable msg=6
S -> C  reliable RPC stream             PlayerJoin, time, PlayerInit, world RPC, dst.
```

`ClientJoin` tidak menunggu ping/static-data inbound. Menunggu dan membalas ping
lebih dahulu akan menggesernya ke msg=6, tidak sama dengan jalur RakPeer yang
diaudit. RakPeer mengirim empat frame source-ordered itu bersama dalam satu
normal datagram; SampDroid juga hanya menaruh satu `hasAcks=false` di depannya,
sambil mempertahankan record retry terpisah untuk reliable msg 2/4/5. Dengan
payload fixture `Native_01` dan serial `3E9`, frame msg=5 harus tepat dimulai
`02 80 48 80 00 01 00 10 14 19 D9 0F ...`; pembandingan langsung terhadap
`BitStream` FakeBots menghasilkan 42 byte yang sama. Burst lengkapnya berukuran
70 byte dan dimulai `01 00 42 38 1E 5E 17 91 89 60 1E 03 00 64 50 ...`; fixture
dan peer loopback mengunci baik byte maupun fakta bahwa session path mengirimnya
sebagai satu datagram. Fixture loss membuang datagram pertama lalu memverifikasi
retry individual reliable msg 2/4/5 dengan nomor/order asli; ping msg 3 memang
tidak diretry. Ping inbound tetap dibalas sesudah join oleh receive loop yang
retained.

### Profile ClientJoin legacy vs 0.3.DL-R1

SampDroid tidak menerima angka/string versi bebas dari UI atau server. Ia memiliki
tepat dua literal profile ClientJoin berikut:

| Profile | NetGameVersion field | Challenge response | client-version field |
|---|---:|---:|---|
| baseline | 4057 | `acceptedToken ^ 4057` | `0.3.7-R2` |
| DL R1 | 4062 | `acceptedToken ^ 4062` | `0.3.DL-R1` |

Pemilih profile hanya menerima hasil query yang dipotong whitespace ASCII dan
cocok case-insensitive **persis** `0.3.DL-R1`; semua input lain memakai baseline.
Ini mencegah `open.mp`, nilai parsial `0.3.DL`, atau banner versi campuran
mengganti identity wire secara spekulatif. Konstanta 4062 telah diaudit pada
source [FakeSAMP 0.3.DL](https://github.com/cizaquita/FakeSAMP)
(`NETGAME_VERSION`), sedangkan string normal player `0.3.DL-R1` didokumentasikan
oleh [open.mp GetPlayerVersion](https://open.mp/docs/scripting/functions/GetPlayerVersion).

Regresi host mengunci payload DL (field `DE 0F 00 00`, XOR token, panjang/string
9 byte) dan loopback retained yang membaca ClientJoin aktual sebelum mengirim
PlayerInit. Profile tersebut juga mengunci **layout** reducer core DL: field
`CustomSkin/CustomModelID:u32` setelah base model pada RPC 32/68/128/171, serta
perbedaan RPC 153 (`u16 PlayerID | u32 Skin | u32 CustomSkin` untuk DL versus
`u32 PlayerID | u32 Skin` legacy). Base model dan custom reference disimpan
terpisah; renderer saat ini memakai base stock fallback. Bentuk writer ini
diaudit pada open.mp [`class.hpp`](https://github.com/openmultiplayer/open.mp/blob/3d20f3dc0e26917264cdc8585c65913e080a143c/Shared/NetCode/class.hpp),
[`core.hpp`](https://github.com/openmultiplayer/open.mp/blob/3d20f3dc0e26917264cdc8585c65913e080a143c/Shared/NetCode/core.hpp), dan
[`actor.hpp`](https://github.com/openmultiplayer/open.mp/blob/3d20f3dc0e26917264cdc8585c65913e080a143c/Shared/NetCode/actor.hpp).
DL `ModelInfo`/`ModelRequest` (179) juga didecode sebagai catalog pasif fixed
39-byte yang dibatasi 4.096 mapping signed custom-object → base stock. Ini dapat
membuat `CreateObject`/attachment memakai model base lawful dan memperbaiki
proxy yang sudah retained, tetapi checksum/size/virtual-world/time dibuang
sesudah validasi layout. Scope masih tidak mencakup `ModelFile`, request/receipt
DFF/TXD, download/verifikasi asset, atau renderer custom model; client tidak
membuka URL server atau memalsukan completion download. Semua ini juga bukan
bukti admission/live gameplay server mana pun.

### Observasi UIF (bukan bukti admission)

Dua observasi native UIF pada 2026-10-20 perlu dipisahkan: sebelum koreksi urutan
2/3/4/5, sesi sembilan detik accepted/aktif menerima 36 packet/34 RPC broadcast
tanpa `PlayerInit`/class/spawn. Satu konfirmasi bounded sesudah koreksi urutan
itu—**sebelum** coalescing yang sekarang—menerima 38 packet/33 RPC, satu pong,
dan tidak melihat retransmit/error, namun tetap tanpa `PlayerInit`/class/spawn.

Probe SampDroid source-current paling baru pada 2026-10-21 memakai jendela
**1.000 ms** dan tidak mengirim class/spawn/sync/chat/command/input gameplay. Ia
mencapai open/cookie/auth/accepted lalu ClientJoin baseline, menerima ACK RakNet
untuk record reliable msg 2/4/5, serta menerima 9 packet/4 RPC
(`SetPlayerColor` ×2, `SendClientMessage` ×2) dengan satu player remote
retained. Ia tidak menerima `PlayerInit`, class, spawn, `joined`, ataupun
`ConnectionRejected`/RPC 130. Jadi fixture/coalescing kini memiliki exercise
endpoint UIF nyata dan receipt transport lengkap untuk bootstrap, tetapi itu
masih hanya bukti transport/broadcast—bukan parse ClientJoin, admission, atau
gameplay.

Comparator read-only open.mp commit `3d20f3dc` menunjukkan baseline enam field
SampDroid memenuhi bentuk `PlayerConnect`, sementara RPC 139 hanya keluar sesudah
jalur server request-player dan dispatcher koneksi. UIF tidak diidentifikasi
sebagai implementasi itu; hasil comparator tidak mengizinkan perubahan wire
spekulatif atau menyimpulkan penyebab diamnya UIF.

Setiap paket reliable yang diterima segera di-ACK, termasuk **setiap fragment**
dari split packet. Sesudah ACK, gate receipt transport yang dibatasi 4.096 nomor
message `u16` membuang retransmit/nomor stale sebelum split maupun reducer;
ini mengikuti urutan ReliabilityLayer legacy: ACK tetap dikirim, aplikasi tidak
menerima payload reliable yang sama dua kali. Fragment tidak boleh didecode sebagai RPC sendiri: codec
menyusun seluruh split sequence lebih dulu, baru `rpc_decoder` membaca payload.
Selain `splitId`/count, semua fragment dalam satu retained sequence wajib punya
reliability, ordering channel, dan ordering index yang sama seperti hasil
`SplitPacket` legacy; identitas berbeda mengganti state split-ID lama alih-alih
mencampur body dua packet. Ini penting karena `PlayerInit` dapat lebih besar
daripada satu datagram.

Tabel auth berisi 256 pasangan tetap `{"challenge", "response"}` di
`app/src/main/cpp/samp_auth_table.inc`. Challenge dipilih acak oleh server,
jadi response harus dicari saat runtime. Inbound `AUTH_KEY` divalidasi sebagai
`[12][len termasuk NUL][maksimal 16 byte challenge][satu NUL terminal]` dengan
panjang tepat sebelum lookup, sehingga length palsu, NUL tersisip, atau trailing
byte tidak pernah dibaca sebagai teks challenge.

Catatan: server **SA-MP 0.3.7 legacy** memakai tabel berbeda yang tidak
tersedia di source open.mp. Untuk server legacy, challenge tidak akan ketemu
di tabel — perlu tabel dari `samp.dll` (lihat `samp-re`).

### Yang berikutnya

1. **Session loop yang benar** — reconnect, ping/timeout
   metrics, resend window, lalu capture sanitasi terhadap beberapa server di
   atas auto-poll/keepalive transport yang sudah ada.
2. **PlayerSync (207) live validation** — decoder broadcast remote sudah ada;
   berikutnya replay capture sanitasi dan renderer state. Encoder posisi dari
   semantic input tetap harus lewat rate limiter agar tidak flood server/pemain lain.
3. **Renderer adapter** — interface asset-agnostic yang membaca data milik
   pengguna sendiri, bukan hook/bundle `libGTASA.so` ke APK.

---

## 6. Broadcast remote PlayerFootSync (Packet 207)

Packet 207 **server→client** berbeda dari payload OnFoot yang dikirim client ke
server. Setelah byte ID 207, server menulis `u16 PlayerID`, bit presence
`LeftRight` + `u16` bila ada, bit presence `UpDown` + `u16` bila ada, `u16 Keys`,
lalu `VEC3 Position`. Rotasi memakai `WriteNormQuat`: empat bit tanda W/X/Y/Z
dan tiga `u16` magnitudo X/Y/Z; W direkonstruksi dari akar `1-x²-y²-z²`.

Setelah itu susunannya adalah satu byte percent health/armour (nibble tinggi/
rendah; `F` berarti 100, lainnya langkah 7), `u8 WeaponAdditionalKey`,
`u8 SpecialAction`, dan compressed velocity (`float magnitude`, lalu tiga
compressed float arah hanya bila magnitude > `0.00001`). Dua bagian terakhir
juga conditional: bit surfing diikuti `u16 SurfingID + VEC3 offset`, lalu bit
animation diikuti `u16 AnimationID + u16 AnimationFlags`.

Decoder SampDroid membaca seluruh branch ini agar alignment tetap benar dan
menolak frame kurang, float non-finite, posisi/velocity/offset di luar envelope,
dan quaternion tidak normal. Frame hanya boleh mengubah actor yang sudah
`PlayerStreamIn`; `StreamOut`, ID pemain lokal, dan ID orphan tidak mengubah
state. Untuk frame `UNRELIABLE_SEQUENCED`, gate per ordering channel membuang
index stale/duplikat dan menangani wrap 16-bit sebelum reducer berjalan. State ini
renderer-neutral: tidak ada interpolasi visual, movement, collision, atau
transmit packet di milestone ini.

---

## 7. Catatan metodologi

Yang bikin fase ini bisa dikerjakan tanpa game dan tanpa HP:

- Semua paket handshake adalah **UDP polos** — bisa dikirim dari laptop/CI.
- Semua offset dan algoritma dipastikan dari **source server open-source**,
  bukan dari binary hasil decompile.
- Setiap klaim diuji dengan test vector yang bisa diulang
  (`tests/native/raknet_codec_test.cpp`, `tools/sampquery`).

Kalau nanti bagian engine mulai dikerjakan, urutan debugging yang sama berlaku:
transport dulu, baru memori game.
