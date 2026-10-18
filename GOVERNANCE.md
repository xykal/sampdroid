# Governance SampDroid

Tujuannya: proyek port SA-MP Android tetap **public, forkable, dan native-first**,
bukan wrapper Winlator, launcher modpack PC, atau binary tertutup milik satu orang.

## Prinsip

1. **Kode dan keputusan publik.** Desain dibahas di issue/PR/Discussion.
2. **MIT + DCO.** Tidak ada CLA atau transfer copyright.
3. **Native-first.** Prioritasnya Android ARMv7/ARM64 native; Windows/x86 runtime,
   Wine, Winlator, dan ketergantungan modpack PC bukan bagian dari produk.
4. **Asset-separated.** APK tidak mendistribusikan game data; world bridge hanya
   menerima data yang pengguna punya sendiri.
5. **Kompatibilitas nyata.** Kedua ABI diuji CI pada setiap PR.
6. **Keamanan pemain/server.** Tidak menerima bypass anti-cheat/ban, credential
   theft, atau fitur yang merusak server/pemain lain.

## Peran

### Kontributor

Siapa pun yang membuka issue, mengirim PR, membuat test vector, native input
layout, dokumentasi, atau review. Semua kontribusi mengikuti `CONTRIBUTING.md` dan
DCO.

### Reviewer

Kontributor yang konsisten dapat mereview networking, Android, native input/HUD,
world bridge, atau documentation. Review tidak memindahkan hak karya penulis.

### Maintainer

Menjaga CI, merge/release, label, dan batas asset. Untuk perubahan protocol,
security, native port interface, atau API publik, maintainer meminta minimal satu
review substantif bila memungkinkan.

## Cara keputusan dibuat

- **Bug kecil/docs/test:** satu PR jelas dan CI hijau cukup.
- **Perubahan protocol/session/world bridge/input API:** issue desain dulu; trade-
  off dicatat di PR agar fork dapat mengikuti.
- **Kontroversi:** pilih desain paling portable, native, teruji, berlisensi jelas,
  dan tidak memaksa Winlator, arm64-only, maupun asset bundle tertutup.
- **Maintainer tidak aktif:** repo bisa difork karena kode tetap MIT.

## Release

APK resmi dibuat GitHub Actions pada repo publik `xykal/sampdroid`. Artifact harus
berisi:

```text
lib/armeabi-v7a/libsampdroid.so
lib/arm64-v8a/libsampdroid.so
```

Tidak ada build laptop yang dianggap release resmi.
