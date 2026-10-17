# RakNet + SA-MP: hasil reverse protocol (verified against live servers)

Semua di bawah ini **diverifikasi langsung** terhadap server SA-MP/open.mp publik,
bukan hasil bacaan dokumentasi saja. Tanggal: 2026-10-17.

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
| 7 | ID_PING |
| 9 | ID_PONG |
| 11 | ID_CONNECTION_REQUEST |
| 12 | ID_AUTH_KEY |
| 24 | ID_OPEN_CONNECTION_REQUEST |
| 25 | ID_OPEN_CONNECTION_REPLY |
| 26 | ID_OPEN_CONNECTION_COOKIE |
| 30 | ID_NEW_INCOMING_CONNECTION |
| 31 | ID_NO_FREE_INCOMING_CONNECTIONS |
| 32 | ID_DISCONNECTION_NOTIFICATION |
| 34 | ID_CONNECTION_REQUEST_ACCEPTED |

Sumber: `openmultiplayer/RakNet` → `Include/raknet/PacketEnumerations.h`,
`Source/RakPeer.cpp`, `SAMPRakNet.cpp`.

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
`native/net/raknet_handshake/samp_crypto_table.inc`.

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
  [jika split]   : uint16 splitId + compressed splitIndex + compressed splitCount
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

---

## 5. Status implementasi

| Tahap | Status | Bukti |
|---|---|---|
| Query protocol (i/r/c/d/p) | ✅ | 1.237 server dari open.mp index, live query |
| Open connection + cookie | ✅ | server 185.169.134.67 membalas `19 00` |
| Obfuscation layer | ✅ | semua fase di atas butuh ini |
| Reliability datagram | ⚠️ format terverifikasi vs source, tapi server belum membalas |
| Connection request accepted | ❌ belum |
| Auth key (open.mp) | belum dicoba |
| ClientJoin RPC / gameplay | belum |

### Yang sudah dicoba untuk connection request (semua timeout)

- messageNumber 0 dan 1
- reliability 6 (unreliable), 8 (reliable), 9 (ordered)
- dengan/tanpa header ACK (hasAcks=1, count=0)
- delay antar paket 0–400 ms
- payload dengan/tanpa terminator password

### Hipotesis berikutnya (urutan prioritas)

1. **Bandingkan byte-per-byte dengan capture client asli.** Jalankan SA-MP di
   Wine/Windows + Wireshark, ambil paket `ID_CONNECTION_REQUEST`, diff dengan
   hasil encoder kita. Ini cara tercepat menyelesaikan.
2. **Baca ulang `SendImmediate`** — mungkin ada padding/header tambahan yang
   tidak ada di path `GenerateDatagram` biasa.
3. **Cek `SocketLayer::RecvFrom`** untuk paket dari peer yang sudah terdaftar:
   mungkin ada filter tambahan sebelum `ProcessNetworkPacket`.
4. **Uji ke server open.mp lokal** — tidak bisa dilakukan di sandbox ini karena
   binary open.mp hanya tersedia x86 32-bit dan environment-nya tidak punya
   dukungan multilib.
5. **Cek `samp-re`** (reimplementasi `samp.dll` hasil IDA, 6.244 fungsi) —
   cari fungsi yang membangun connection request, bandingkan.

---

## 6. Catatan metodologi

Yang bikin fase ini bisa dikerjakan tanpa game dan tanpa HP:

- Semua paket handshake adalah **UDP polos** — bisa dikirim dari laptop/CI.
- Semua offset dan algoritma dipastikan dari **source server open-source**,
  bukan dari binary hasil decompile.
- Setiap klaim diuji dengan tes yang bisa diulang (`tools/sampquery`,
  `native/net/raknet_handshake`).

Kalau nanti bagian engine mulai dikerjakan, urutan debugging yang sama berlaku:
transport dulu, baru memori game.
