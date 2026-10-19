# Signing APK rilis

APK yang dipublikasikan dari tag `v*` harus memakai keystore resmi milik
maintainer. Keystore, alias, dan password **tidak boleh** masuk repository,
`.env`, artifact, atau source code.

## Secret GitHub yang wajib ada

Tambahkan di **Repository → Settings → Secrets and variables → Actions**:

| Secret | Isi |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | Isi file keystore (`.jks`/`.keystore`) yang di-base64 satu baris |
| `ANDROID_KEYSTORE_PASSWORD` | Password keystore |
| `ANDROID_KEY_ALIAS` | Alias key release |
| `ANDROID_KEY_PASSWORD` | Password key release |

Contoh persiapan lokal oleh pemilik key (jangan commit hasilnya):

```bash
base64 -w 0 sampdroid-release.jks
```

Pemilik key harus menyimpan file asli dan recovery material di tempat aman.
Kehilangan key berarti update APK yang memakai application ID yang sama tidak
bisa lagi ditandatangani sebagai aplikasi yang sama.

## Perilaku workflow

- Pull request dan push branch memakai APK debug hanya untuk validasi internal.
- Push tag `v*` gagal tertutup bila salah satu secret signing tidak ada.
- Bila secret lengkap, CI mendecode key secara sementara di runner, menjalankan
  `assembleRelease`, memverifikasi tanda tangan dengan `apksigner`, memastikan
  `armeabi-v7a` dan `arm64-v8a` ada, lalu mengunggah APK sebagai **draft GitHub
  Release**.
- APK tag diberi nama `SampDroid-<versi>-release.apk`, bukan debug APK.

Keystore sementara hidup hanya di runner CI dan tidak diunggah sebagai artifact.
