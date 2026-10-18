# Kontribusi ke SampDroid

Makasih mau bantu port client SA-MP PC menjadi client Android **native**. Misi
proyek ini adalah supaya pemain tidak perlu Winlator, Wine/x86 emulation, atau
modpack PC hanya untuk masuk dan main dari Android.

Area kontribusi: C++/networking, state/RPC, Android/Kotlin, native HUD, touch,
gamepad, gyro, renderer/world bridge, ARMv7 performance, test, aksesibilitas,
dokumentasi, dan localization.

## Lisensi kontribusi — bebas tanpa CLA

Kode proyek memakai **MIT License**.

- **Tidak ada CLA** dan tidak ada pengalihan hak cipta ke maintainer.
- Kontributor tetap memegang copyright atas karyanya.
- PR dilisensikan ke publik di bawah MIT (*inbound = outbound*), sehingga siapa
  pun bebas memakai, memodifikasi, fork, dan mendistribusikannya.
- Setiap commit kode wajib memakai **DCO**: `git commit -s`.
  Teks lengkap ada di [`DCO.txt`](DCO.txt).

## Yang dibutuhkan

- Persistent RakNet session, ACK/resend, class selection, spawn, dan sync.
- Decoder/state RPC player, vehicle, object, pickup, chat, dialog, checkpoint.
- Native Android input/HUD: touch, gamepad, keyboard, gyro, accessibility.
- World/game bridge yang tidak bergantung pada Windows runtime.
- Profil **input native internal**, bukan loader modpack.
- Test vector protokol, test device ARMv7/ARM64, docs, dan localization.

## Yang tidak boleh masuk repo

- GTA APK/OBB/data, `libGTASA.so`, texture/model/audio Rockstar, crack, serial,
  atau link mirror asset game.
- `samp.exe`, client Windows, binary hasil dump, atau source yang disalin dari
  proyek dengan lisensi tidak kompatibel.
- Cheat, bypass anti-cheat/ban, credential server, token, atau data pemain.
- Runtime modpack PC, script loader, plugin Windows, Wine/Winlator wrapper, atau
  executable yang dijalankan dari layout/config Android.

SampDroid boleh membaca data yang pengguna miliki sendiri melalui backend dunia
nanti; repo dan release APK tidak membundel data tersebut.

## Alur kerja

1. Cari issue berlabel [`good first issue`](../../issues?q=is%3Aissue%20state%3Aopen%20label%3A%22good%20first%20issue%22)
   atau buka issue desain untuk perubahan besar.
2. Fork repo, buat branch jelas: `fix/ack-range`, `feat/native-session`,
   `feat/touch-hud`, atau `docs/port-target`.
3. Buat perubahan fokus; sertakan test bila menyentuh codec/protokol/state.
4. Commit dengan sign-off:

   ```bash
   git commit -s -m "fix(net): acknowledge split fragments"
   ```

5. Buka PR. GitHub Actions adalah **source of truth** untuk compile/build dan
   wajib lulus untuk dua ABI (`armeabi-v7a`, `arm64-v8a`).
6. Jangan commit APK/OBB/binary build. Artifact CI dan Releases dipakai untuk APK.

## Konvensi kode

- C++17, `-Wall`, batas alokasi ketat untuk input jaringan.
- Kotlin tidak boleh blok UI thread; socket/parser di `Dispatchers.IO`.
- Parser harus memvalidasi length/count sebelum alokasi dan punya test vector.
- Jangan mengasumsikan arm64 saja: ARMv7 adalah syarat proyek.
- Layout input hanya menghasilkan semantic action (`move`, `jump`, `aim`), tidak
  boleh menjalankan script/binary atau meng-hook client Windows.

## Review dan keputusan

Lihat [`GOVERNANCE.md`](GOVERNANCE.md). Keputusan teknis dibuat terbuka di issue/PR
supaya port ini dapat diteruskan/fork tanpa bergantung pada satu maintainer.

Untuk celah keamanan, ikuti [`SECURITY.md`](SECURITY.md), jangan post exploit
atau packet reproduksi berbahaya secara publik.
