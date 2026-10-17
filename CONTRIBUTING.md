# Kontribusi ke SampDroid

Makasih mau bantu bikin klien multiplayer Android native yang lebih ringan daripada
menjalankan client Windows lewat layer kompatibilitas. Semua orang boleh ikut:
C++/networking, Kotlin/Android, UI touch control, aksesibilitas, dokumentasi,
profil kontrol, test, terjemahan, dan tooling modpack.

## Lisensi kontribusi — bebas tanpa CLA

Kode proyek ini memakai **MIT License**.

- **Tidak ada CLA** dan tidak ada pengalihan hak cipta ke maintainer.
- Kontributor tetap memegang copyright atas karyanya.
- Dengan membuka pull request, kontribusimu dilisensikan ke publik di bawah MIT
  (prinsip *inbound = outbound*), sehingga siapa pun boleh memakai, memodifikasi,
  fork, dan mendistribusikannya.
- Setiap commit kode wajib memakai **DCO**: `git commit -s`.
  Git akan menambahkan `Signed-off-by:` sebagai pernyataan bahwa kamu berhak
  mengirim kode itu. Teks lengkap ada di [`DCO.txt`](DCO.txt).

Jangan kirim kode/aset yang hak distribusinya tidak kamu punya.

## Yang aman dan dibutuhkan

- Perbaikan transport RakNet, reliability, parser RPC, dan test vector.
- Renderer adapter open-source, HUD, input touch, gyro, gamepad, dan aksesibilitas.
- Profile kontrol / tema HUD berlisensi bebas.
- Tool import dan validator manifest modpack.
- Dokumentasi protokol yang berasal dari observasi sendiri atau sumber yang
  izinnya jelas.
- Perbaikan kompatibilitas `armeabi-v7a` dan `arm64-v8a`.

## Yang tidak boleh masuk repo

- GTA: San Andreas APK/OBB/data, `libGTASA.so`, texture/model/audio Rockstar,
  crack, serial, atau link mirror aset game.
- Client Windows SA-MP/original executable, binary hasil dump, maupun source
  yang disalin dari proyek dengan lisensi tidak kompatibel.
- Cheat, bypass ban/anti-cheat, credential server, token, atau data pemain.
- Modpack yang mengemas ulang aset game atau aset kreator lain tanpa izin.

SampDroid boleh membaca **data yang pengguna miliki sendiri** melalui jalur import
nanti; repo dan release APK tidak akan membundel data tersebut.

## Alur kerja

1. Cari issue berlabel [`good first issue`](../../issues?q=is%3Aissue%20state%3Aopen%20label%3A%22good%20first%20issue%22)
   atau buka issue dulu untuk perubahan besar.
2. Fork repo, buat branch dengan nama jelas: `fix/ack-range`,
   `feat/touch-layout`, atau `docs/modpack-schema`.
3. Buat perubahan sekecil dan sefokus mungkin; sertakan test jika menyentuh
   codec/protokol.
4. Commit dengan sign-off:

   ```bash
   git commit -s -m "fix(net): acknowledge split fragments"
   ```

5. Buka PR menggunakan template. GitHub Actions adalah **source of truth** untuk
   compile/build; workflow wajib lulus untuk dua ABI (`armeabi-v7a`, `arm64-v8a`).
6. Jangan mengunggah APK/OBB/binary build ke git. Artifact CI dan Releases dipakai
   untuk distribusi APK.

## Konvensi kode

- C++17, `-Wall`, tidak ada allocation tak terbatas dari data jaringan.
- Kotlin: jangan blok UI thread; operasi socket/parser di `Dispatchers.IO`.
- Parser jaringan harus memvalidasi panjang, count, dan batas memori sebelum
  alokasi. Sertakan test vector deterministik untuk bug protokol.
- API/network parser tidak boleh mengasumsikan arm64 saja: dua ABI adalah syarat.
- Teks UI boleh Bahasa Indonesia; identifier dan komentar teknis boleh Inggris.

## Modpack dan control profile

Lihat [`docs/MODPACKS.md`](docs/MODPACKS.md) dan
[`docs/CONTROL_PROFILES.md`](docs/CONTROL_PROFILES.md). Kami menerima manifest,
layout, kode loader, dan dokumentasi yang bebas. Aset tetap harus dibuat sendiri,
berlisensi kompatibel, atau diimpor dari kepemilikan pengguna.

## Review dan keputusan

Aturannya ada di [`GOVERNANCE.md`](GOVERNANCE.md). Singkatnya: diskusi publik,
review yang bisa dilihat, dan keputusan teknis dicatat di issue/PR supaya proyek
nggak tergantung satu orang.

## Butuh bantuan?

Pakai issue untuk bug/fitur dan GitHub Discussions untuk obrolan desain. Untuk
celah keamanan, ikuti [`SECURITY.md`](SECURITY.md) — jangan post proof-of-concept
berbahaya di issue publik.
