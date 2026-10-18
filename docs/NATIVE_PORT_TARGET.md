# Target port native SampDroid

## Tujuan produk

SampDroid ditargetkan menjadi **client SA-MP Android native** yang menggantikan
kebutuhan menjalankan client PC lewat Winlator, Wine, x86 emulation, atau launcher
modpack PC.

Pengalaman target untuk pemain:

```text
Install APK SampDroid → pilih server SA-MP/open.mp → login/spawn/main
→ main dengan touch/gamepad Android native
```

Tidak ada keharusan memasang Windows, menjalankan `samp.exe`, atau mengandalkan
modpack PC agar client bisa bekerja.

## Arti “port” di proyek ini

`samp.exe` adalah binary Windows/x86, bukan source Android yang bisa langsung
compile menjadi ARM APK. Maka implementasi port dilakukan sebagai **client native
Android yang mereplikasi kompatibilitas client PC**: protocol, session state,
RPC, sync, HUD, input, dan bridge dunia game. Hasil yang dikejar tetap port
fungsional untuk pengguna — APK Android langsung, bukan emulator Windows.

## Kontrak kompatibilitas

| Area client PC | Target Android native |
|---|---|
| Browser + query server | native Kotlin/C++ |
| Handshake/RakNet/auth/join | native C++ ARMv7 + ARM64 |
| Class selection / spawn / player sync | state + encoder native |
| Player, vehicle, object, pickup, textdraw, dialog | world/HUD state native |
| Keyboard/mouse PC | touch, gamepad, keyboard Bluetooth, gyro Android |
| Runtime Windows / Winlator | **tidak digunakan** |
| Modpack PC sebagai syarat | **tidak digunakan** |

## Batas distribusi

APK SampDroid berisi kode client sendiri, bukan GTA APK/OBB, binary Windows,
atau asset pihak ketiga. Backend dunia game nantinya memakai data yang pengguna
punya sendiri; ini menjaga APK tetap kecil, native, dan tidak jadi repack game.

## Urutan implementasi

1. Persistent RakNet session, class selection, spawn, dan sync aman.
2. Decode/reduce semua state yang dibutuhkan dunia SA-MP.
3. Native Android input/HUD — touch, gamepad, keyboard, gyro.
4. Game/world bridge yang menerima state native, tanpa Windows runtime.
5. Optimasi ARMv7, ARM64, device RAM rendah, serta fallback GPU.

Input layout hanyalah konfigurasi internal SampDroid untuk kontrol nyaman. Itu
bukan loader modpack, bukan runtime script, dan bukan dependensi client PC.
