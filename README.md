# samp-android

Native SA-MP client untuk Android — dibangun dari nol, modul per modul.
Bukan porting engine: game engine-nya (GTA: San Andreas Android) sudah ada, yang dibangun adalah
**network client + HUD + input**.

Blueprint lengkap: [`../blueprint-samp-android.md`](../blueprint-samp-android.md)

---

## Status

| Modul | Status | Bukti |
|---|---|---|
| M1 — SA-MP query protocol (i/r/c/d/p) | **DONE** | `tools/sampquery` — UDP client portable, jalan di Linux & NDK |
| M1 — Server browser data layer | **DONE** | `tools/serverlist` — 1.237 server dari open.mp index, live query |
| M2 — Open connection + cookie challenge | **DONE** | `native/net/raknet_handshake` — server SA-MP live membalas `19 00` |
| M2 — Obfuscation layer (checksum + key table) | **DONE** | `samp_crypto_table.inc`, diverifikasi vs `SAMPRakNet.cpp` |
| M2 — Reliability datagram encoder | format OK, handshake belum | lihat `docs/protocol/raknet-samp.md` §5 |
| M2 — Connection request accepted | **blocked** — sedang di-debug | butuh capture client asli untuk diff |
| M3 — Hook / engine API | blocked | butuh `libGTASA.so` dari device kamu |

Detail lengkap protokol yang sudah di-reverse: [`docs/protocol/raknet-samp.md`](docs/protocol/raknet-samp.md)

---

## tools/sampquery

Klien query SA-MP/open.mp, zero dependency, C++17, ~300 baris.
Protocol: `"SAMP"` + IP octets + port LE (2 byte) + opcode.

```bash
cd tools/sampquery
g++ -O2 -std=c++17 -o sampquery samp_query.cpp
./sampquery 185.194.177.29:7777 all
```

Contoh output nyata:

```
-> 185.194.177.29:7777
  [i] 83 bytes
  hostname  NEW YORK TIMES 2010 | ...
  gamemode  Build 2.7B
  language  Russian
  players   0 / 500
  password  yes
  [r] 163 bytes
  rules(8):
    allowed_clients        = 0.3.7, 0.3.DL
    version                = omp 1.5.8.3183
    lagcomp                = On
    weburl                 = newyork-gta.online
    ...
  ping: 153.894 ms (echo ok)
```

Catatan: opcode `d` (detailed players) sering tidak dijawab open.mp — server bisa mematikannya.
`i`, `r`, `c`, `p` cukup untuk server browser penuh.

## tools/serverlist

Data layer server browser: ambil open.mp index → urutkan by player count → query live.

```bash
cd tools/serverlist
python3 fetch_servers.py --top 10 --query
```

```
index: 1237 servers | showing top 6

 1. 188.212.101.20:7777       849/1000 omp   B-Zone RPG | Hunter Job, VIP Account
    -> hostname:  B-Zone RPG | Hunter Job, VIP Account
    -> gamemode:  v10.0 (13:25 / 12.10.2026)
    -> players:   935 / 1000
```

Perhatikan: satu server melaporkan `players 568 / 500` — slot overflow, normal di server open.mp
yang limitnya sudah dilepas. Ini alasan client harus membaca `maxplayers` dari response, bukan hardcode.

---

## Kenapa modul ini dibangun duluan

Ini satu-satunya bagian yang **tidak butuh game, tidak butuh device, tidak butuh offset**.
Semua bisa diverifikasi dari laptop. Kalau network layer belum benar, hook engine cuma bikin bug
yang mustahil dilacak. Urutan debugging yang benar: protokol dulu → engine belakangan.

---

## Struktur yang akan diisi

```
samp-android/
├── app/                    # Kotlin launcher (deteksi GTA, OBB, patch, settings)
├── native/
│   ├── core/               # hook manager, pattern scan, offset table per versi
│   ├── engine/             # CPlayerPed, CVehicle, CStreaming, CCamera wrappers
│   ├── net/                # RakNet + SA-MP bitstream + session
│   ├── sync/               # player/vehicle/bullet/aim sync + interpolasi
│   ├── render/             # ImGui + GLES3: chat, scoreboard, dialog, HUD
│   └── input/              # touch layout, gamepad, gyro
├── tools/
│   ├── sampquery/          # ✅ query protocol
│   ├── serverlist/         # ✅ server browser data layer
│   ├── packet-dump/        # dump & replay paket dari server lokal
│   └── offset-scanner/     # pattern scan libGTASA.so
└── docs/
    └── offsets/            # tabel offset per versi game (2.00 / 2.10 / 2.11)
```

---

## Yang dibutuhkan dari sisi kamu

1. **GTA SA Android legal** dari Play Store (versi 2.10 atau 2.11).
2. **Device/emulator** buat testing — ARM64.
3. **`libGTASA.so`** hasil ekstrak dari APK kamu sendiri (jangan redistribute).
4. Server open.mp lokal di PC buat test harness.

Tanpa #3, bagian hook tidak bisa dimulai — bukan karena susah, tapi karena offset hanya
bisa dihitung dari binary yang kamu punya.
