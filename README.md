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

SampDroid tidak didesain sebagai launcher modpack PC. Kontrol touch/gamepad yang
akan tersedia adalah bagian dari client Android sendiri.

Detail kontrak port: [docs/NATIVE_PORT_TARGET.md](docs/NATIVE_PORT_TARGET.md).

## Status implementasi saat ini

| Modul client | Status | Bukti / catatan |
|---|---:|---|
| Browser + query server | ✅ | info, rules, player list, ping native |
| RakNet 2.x open/cookie/auth | ✅ | diverifikasi terhadap server live |
| `CONNECTION_REQUEST_ACCEPTED` + ClientJoin | ✅ | server memproses lalu stream balik |
| ACK reliable + multi-packet datagram | ✅ | retransmisi tidak menumpuk |
| Split reassembly | ✅ | fragment disusun sebelum RPC didecode |
| State pemain dasar | ✅ | `PlayerInit`, Join/Quit/Name/Color, StreamIn/Out, dan local spawn/posisi/vitals → model native bounded |
| Native chat state | ✅ | `SendClientMessage` bounded 100 line; HUD chat masih berikutnya |
| Checkpoint state | ✅ | normal + race checkpoint reducer; marker renderer masih berikutnya |
| Class/spawn packet core | ✅ | Class → RequestSpawn → PlayerSpawn confirmation + ACK gate native |
| Persistent native session | 🧪 | auto-poll, peer filter, ACK/retry bounded, keepalive transport reliable + internal pong; reconnect belum |
| OnFootSync core | 🧪 | encoder packet-207 client→server vector-tested, USeq channel-1, rate/spawn gate + native input→local preview; belum ada loop/gameplay packet otomatis |
| Remote on-foot bridge | 🧪 | decoder broadcast packet-207 server→client + lifecycle StreamIn/Out → state entity native bounded; belum ada model/radar/renderer atau movement send |
| Class/spawn native | 🧪 | explicit `Class 0` + `Spawn`, response/ACK terlihat di diagnostic; belum gameplay/render |
| Player/vehicle/object sync penuh | ⏳ | remote on-foot state baru fondasi; vehicle/object dan renderer belum |
| HUD + textdraw/dialog native | ⏳ | belum |
| Touch/gamepad/gyro native | 🧪 | touch pad runtime → semantic native action → OnFoot preview; gamepad/gyro dan world loop berikutnya |
| World/game bridge Android | ⏳ | belum di APK |

Saat ini tombol **Probe RakNet** di detail server membuktikan alur nyata:

```text
open → cookie → auth → accepted → ClientJoin → stream RPC → state pemain
(remote Packet 207 siap didecode saat server membroadcast sync)
```

## Kompatibilitas

SampDroid dibangun native untuk:

- **`armeabi-v7a`** — perangkat Android ARM 32-bit;
- **`arm64-v8a`** — perangkat Android ARM 64-bit.

Minimum Android: **API 24 / Android 7.0**. Dua ABI berarti APK tidak memerlukan
Winlator, tetapi performa akhir tetap akan bergantung pada RAM/GPU saat renderer
dunia selesai dibuat.

## Native Android input

Touch, gamepad, keyboard Bluetooth, gyro, dan HUD akan menjadi bagian internal
APK. File layout hanya mendeskripsikan posisi tombol native — tidak memuat script,
plugin PC, executable, atau asset eksternal.

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
Push tag `v*` membuat **draft release** beserta APK; maintainer harus mempublishnya
secara manual setelah review.

## Roadmap

Lihat [docs/ROADMAP.md](docs/ROADMAP.md). Jalur prioritas port sekarang:

1. persistent RakNet session, class selection, spawn, dan sync;
2. state world untuk player, vehicle, object, pickup, checkpoint, dialog, chat;
3. HUD + input Android native;
4. world/game bridge tanpa runtime Windows;
5. optimasi perangkat ARMv7 dan ARM64.

## Asset policy

APK tidak membundel GTA APK/OBB/data, binary Windows, atau asset pihak ketiga.
Backend dunia nantinya memakai data yang pengguna miliki sendiri; ini menjaga
SampDroid tetap APK Android native, kecil, dan bukan repack game.
