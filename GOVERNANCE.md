# Governance SampDroid

Tujuan governance ini sederhana: proyek tetap **public, forkable, dan enak
dikontribusi**, bukan bergantung pada satu binary tertutup atau satu maintainer.

## Prinsip

1. **Kode publik, keputusan publik.** Perubahan desain dibahas di issue/PR atau
   Discussion yang dapat dibaca siapa saja.
2. **MIT untuk kode SampDroid.** Tidak ada CLA dan tidak ada transfer copyright.
   DCO dipakai agar asal kontribusi jelas.
3. **Asset-agnostic.** Project tidak mendistribusikan game data. Backend renderer
   dan import modpack wajib memisahkan kode open-source dari asset pengguna.
4. **Kompatibilitas nyata.** `armeabi-v7a` dan `arm64-v8a` diuji CI pada setiap PR.
5. **Keamanan dan pemain lebih dulu.** Tidak menerima bypass anti-cheat/ban,
   pencurian credential, atau fitur yang merusak server/pemain lain.

## Peran

### Kontributor

Siapa pun yang membuka issue, mengirim PR, membuat test vector, profil kontrol,
dokumentasi, atau review. Semua kontribusi teknis mengikuti `CONTRIBUTING.md` dan
DCO.

### Reviewer

Kontributor yang konsisten dan memahami area tertentu dapat diminta mereview
networking, Android, UI/input, atau dokumentasi. Review tidak memberi hak atas
kode kontributor; hak tetap di tangan penulis di bawah MIT.

### Maintainer

Maintainer menjaga CI, melakukan merge/release, mengatur label, dan memastikan
repo tidak memuat aset/proprietary binary. Untuk perubahan protokol, keamanan,
format modpack, atau API publik, maintainer meminta minimal satu review yang
substantif bila memungkinkan.

## Cara keputusan dibuat

- **Bug kecil/docs/test:** satu PR yang jelas dan CI hijau biasanya cukup.
- **Perubahan API/protokol/modpack:** issue desain dulu; keputusan dan trade-off
  dicatat pada PR supaya fork lain bisa mengerti.
- **Kontroversi:** pilih desain yang paling portable, teruji, berlisensi jelas,
  dan tidak mengunci pengguna ke Winlator, perangkat 64-bit, atau asset bundle
  tertutup.
- **Jika maintainer tidak aktif:** repo dapat difork karena seluruh kode proyek
  tetap MIT. Catat asal fork dan pertahankan attribution.

## Release

APK resmi dibuat oleh GitHub Actions pada repo publik `xykal/sampdroid`. Artifact
harus menunjukkan dua native library berikut sebelum release:

```
lib/armeabi-v7a/libsampdroid.so
lib/arm64-v8a/libsampdroid.so
```

Tidak ada APK yang di-build dari laptop maintainer lalu dianggap rilis resmi.
