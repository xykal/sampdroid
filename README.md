# SampDroid

[![Build APK](https://github.com/xykal/sampdroid/actions/workflows/build-apk.yml/badge.svg)](https://github.com/xykal/sampdroid/actions/workflows/build-apk.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-0b8f5a.svg)](LICENSE)
[![ABIs](https://img.shields.io/badge/ABI-armeabi--v7a%20%7C%20arm64--v8a-2563eb.svg)](#kompatibilitas)

**SampDroid** adalah proyek klien multiplayer Android native untuk ekosistem
SA-MP/open.mp. Tujuannya: pengalaman Android yang lebih ringan dan fleksibel
daripada menjalankan client Windows lewat Winlator/layer kompatibilitas,
termasuk perangkat **32-bit ARMv7**.

> **Status jujur:** APK saat ini adalah server browser + diagnostik transport
> RakNet/ClientJoin. Gameplay/render dunia belum ada. Network core sengaja
dibangun dan diuji dulu agar backend renderer serta kontrol komunitas nanti
> punya fondasi yang stabil.

**APK terbaru:** [GitHub Releases](https://github.com/xykal/sampdroid/releases)

---

## Apa yang sudah jalan

| Modul | Status | Bukti / catatan |
|---|---:|---|
| Server browser open.mp | ✅ | daftar server dari index publik |
| Query SA-MP (`i/r/c/d/p`) | ✅ | info, rules, players, ping native |
| RakNet 2.x open/cookie/auth | ✅ | diverifikasi terhadap server live |
| `CONNECTION_REQUEST_ACCEPTED` | ✅ | token server diterima |
| `ClientJoin` RPC 25 | ✅ | server memproses lalu mengirim stream balik |
| ACK reliable + multi-packet datagram | ✅ | retransmisi tidak menumpuk |
| Split reassembly | ✅ | fragment disusun sebelum RPC didecode |
| RPC observability | ✅ | nama RPC + decoder `PlayerInit` (139) |
| Persistent game session / sync | ⏳ | milestone berikutnya |
| Touch/gamepad HUD runtime | ⏳ | schema profile v1 sudah terbuka |
| Renderer/game bridge | ⏳ | asset-agnostic, belum di APK |

Tekan **Probe RakNet** di detail server untuk melihat alur nyata:

```text
open → cookie → auth → accepted → ClientJoin → stream RPC
```

## Kenapa bukan Winlator

Winlator berguna, tapi menjalankan client Windows berarti overhead x86 emulation,
Wine/translation layer, dan sering hanya nyaman di perangkat 64-bit yang cukup
kuat. SampDroid ditulis native untuk Android dan dibangun untuk dua ABI:

- `armeabi-v7a` — agar perangkat ARM 32-bit tidak langsung ditinggal;
- `arm64-v8a` — perangkat Android modern.

Target minimum Android adalah **API 24 / Android 7.0**. Kompatibilitas gameplay
penuh tetap tergantung backend renderer serta RAM/GPU perangkat; jangan anggap
"dua ABI" sebagai janji bahwa semua device pasti kuat menjalankan map/mod berat.

---

## Open source yang benar-benar bebas

- Kode SampDroid: **[MIT License](LICENSE)**.
- **Tidak ada CLA** dan tidak ada pengalihan copyright ke maintainer.
- Kontributor memakai [DCO](DCO.txt): kontribusi masuk dan keluar di bawah MIT.
- CI, issue, roadmap, dan APK release ada di repo publik `xykal/sampdroid`.
- Aturan kontribusi: [CONTRIBUTING.md](CONTRIBUTING.md).
- Aturan komunitas: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
- Security disclosure: [SECURITY.md](SECURITY.md).
- Governance/review: [GOVERNANCE.md](GOVERNANCE.md).
- Attribution/provenance: [NOTICE.md](NOTICE.md).

Kontributor boleh membuat fork, client UI alternatif, control profile, tooling,
dan modpack manifest berlisensi bebas. Yang tidak boleh masuk repo: game asset,
APK/OBB, `libGTASA.so`, crack, credential, cheat/bypass, atau mod yang tidak
punya izin distribusi.

---

## Modpack style PC, tapi aman dan portable

SampDroid akan memisahkan **kode/config** dari **asset game**:

- profile tombol, HUD, gamepad, gyro, accessibility, dan metadata pack dapat
  dipublikasikan/difork komunitas;
- asset GTA/mod milik pengguna tetap di perangkat pengguna, bukan dibundel APK;
- importer nanti hanya menerima manifest/path sandbox, bukan script atau native
  binary arbitrary;
- pack wajib menyatakan target `armeabi-v7a`/`arm64-v8a` dan lisensi author.

Mulai kontribusi dari:

- [Kontrak modpack v1](docs/MODPACKS.md)
- [Control profile v1](docs/CONTROL_PROFILES.md)
- [Contoh classic landscape](examples/control-profiles/classic-landscape.json)
- [Schema manifest](schemas/modpack-manifest.schema.json)
- [Schema control profile](schemas/control-profile.schema.json)

---

## Arsitektur

```text
Kotlin Android shell
  ├─ server browser / import profile / HUD
  └─ JNI
       └─ C++17 native core
            ├─ query protocol
            ├─ RakNet bitstream + ACK + split reassembly
            ├─ RPC decoder + session state
            ├─ semantic input mapper
            └─ renderer adapter (future, user-owned assets)
```

Detail batas antarlayer: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Dokumentasi protocol:

- [RakNet + SA-MP](docs/protocol/raknet-samp.md)
- [Tabel RPC open.mp](docs/protocol/rpc-ids.md)

## Build resmi — GitHub Actions

Sesuai kebijakan proyek, **compile/build APK resmi dilakukan di GitHub Actions**
pada repo publik, bukan dari laptop maintainer. Setiap push/PR menjalankan:

1. regression test C++ untuk wire vector RakNet, ACK, split reassembly, dan
   `PlayerInit`;
2. build APK debug untuk `armeabi-v7a` + `arm64-v8a`;
3. verifikasi dua `.so` benar-benar ada di dalam APK;
4. upload artifact; push tag `v*` juga menerbitkan GitHub Release.

Kontributor cukup fork → branch → PR. Lihat [CONTRIBUTING.md](CONTRIBUTING.md).

## Roadmap

Lihat [docs/ROADMAP.md](docs/ROADMAP.md). Area kontribusi paralel paling berguna:

1. decoder/state untuk RPC join/class/spawn/chat/object/vehicle;
2. persistent RakNet session dan player sync yang dibatasi rate;
3. touch/gamepad/gyro + editor layout;
4. renderer adapter yang legal dan ramah ARMv7;
5. test vector/sanitized capture, docs, localization, dan accessibility.

## Legal & asset policy

- Project ini tidak berafiliasi dengan Rockstar Games, Take-Two, SA-MP, atau
  open.mp.
- SampDroid **tidak** mendistribusikan game asset atau proprietary binary.
- Pengguna wajib memiliki game/data/mod yang mereka impor sendiri dan mematuhi
  lisensi/server rules masing-masing.
- Detail attribution dan referensi interoperabilitas: [NOTICE.md](NOTICE.md).
