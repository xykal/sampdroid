# SampDroid

Native SA-MP (San Andreas Multiplayer) client untuk Android — dibangun dari nol.
Bukan porting engine: engine-nya (GTA: San Andreas Android) sudah ada, yang dibangun adalah
**transport network + server browser + HUD + input**.

[![Build APK](https://github.com/xykal/sampdroid/actions/workflows/build-apk.yml/badge.svg)](https://github.com/xykal/sampdroid/actions/workflows/build-apk.yml)

**Download APK:** [Releases → v0.1.0](https://github.com/xykal/sampdroid/releases) ·
`armeabi-v7a` + `arm64-v8a`

---

## Status build

| Modul | Status | Bukti |
|---|---|---|
| SA-MP query protocol (i/r/c/d/p) | **DONE** | `tools/sampquery`, live ke server publik |
| Server browser (open.mp index) | **DONE** | 1.200+ server, live query per server |
| Obfuscation layer (checksum + key table) | **DONE** | diverifikasi vs `SAMPRakNet.cpp` |
| RakNet 2.x open connection + cookie | **DONE** | server live balas `19 00` |
| RakNet reliability datagram | format OK | belum diterima server |
| CONNECTION_REQUEST_ACCEPTED | **BELUM** | lihat `docs/protocol/raknet-samp.md` §5 |
| Hook ke `libGTASA.so` | belum | butuh binary dari device sendiri |

APK v0.1.0 = **server browser + diagnostics**, belum gameplay.
Semua tombol (Info / Rules / Players / Ping / Probe RakNet) memanggil implementasi native beneran.

---

## Build

### Otomatis (GitHub Actions)

Push ke `main` → workflow `.github/workflows/build-apk.yml` build debug APK untuk
**armeabi-v7a** dan **arm64-v8a**, lalu verifikasi kedua `.so` benar-benar ada di dalam APK.

### Lokal

```bash
# butuh: JDK 17, Android SDK (platform 34, build-tools 34.0.0),
#        NDK 26.1.10909125, CMake 3.22.1
gradle :app:assembleDebug
# output: app/build/outputs/apk/debug/app-debug.apk
```

NDK r26b, `minSdk 24`, `targetSdk 34`, C++17.

---

## Arsitektur

```
app/                        Kotlin (UI)
  ui/MainActivity           server browser
  ui/ServerDetailDialog     query info/rules/players/ping + RakNet probe
  net/ServerListRepository  open.mp server index
  NativeBridge              JNI

app/src/main/cpp/           C++ native (dipakai dua ABI)
  samp_query.cpp            protokol query SA-MP
  raknet_probe.cpp          RakNet 2.x handshake + obfuscation + datagram encoder
  samp_crypto_table.inc     substitution table hasil ekstraksi

tools/sampquery/            query client versi CLI (desktop)
tools/serverlist/           server browser versi Python
native/net/raknet_handshake/ prototype handshake (desktop)
docs/protocol/raknet-samp.md  protokol hasil reverse
```

---

## Kenapa bagian network dibangun duluan

Ini satu-satunya bagian yang **tidak butuh game, tidak butuh device, tidak butuh offset**.
Semua bisa diverifikasi dari CI. Kalau transport belum benar, hook engine cuma bikin bug
yang mustahil dilacak.

Temuan utama yang bikin proyek sejenis gagal:

1. **SA-MP pakai RakNet 2.52, bukan RakNet 4.x.** Handshake-nya 3 byte, tanpa magic, tanpa MTU probing.
2. **Semua paket dibungkus obfuscation** — `[checksum][byte ter-substitusi via table 256, di-XOR port]`.
   Paket RakNet polos di-drop diam-diam.
3. **Cookie challenge anti-bot** — server balas `[26][cookie]`, client harus echo `cookie ^ 0x6969`
   (SA-MP) atau `cookie ^ 0x6D70` (open.mp).

Detail lengkap + hex dump: [`docs/protocol/raknet-samp.md`](docs/protocol/raknet-samp.md)

---

## Legal

- Project ini **tidak** mengandung aset Rockstar. User wajib punya GTA: San Andreas sendiri.
- **Tidak** mendistribusikan `libGTASA.so`, OBB, atau APK game.
- Sideload only. Tidak untuk Play Store.
- Nama "SampDroid" tidak berafiliasi dengan Rockstar Games, Take-Two, atau tim SA-MP.

---

## Lisensi

MIT untuk kode di repo ini. Lihat `LICENSE`.
