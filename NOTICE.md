# Notices and provenance

## License of this repository

Kode SampDroid berada di bawah [MIT License](LICENSE). Kontributor memakai DCO
dan prinsip *inbound = outbound*; lihat `CONTRIBUTING.md`.

## Native port, not a Windows wrapper

SampDroid adalah implementasi client Android native. Repository/release tidak
mengandung atau menjalankan `samp.exe`, binary Windows, Winlator, Wine, atau
runtime x86. Tujuannya adalah kompatibilitas perilaku client dari APK ARM native.

## No game assets or proprietary binaries

Repository dan release tidak mengandung GTA: San Andreas APK/OBB/data,
`libGTASA.so`, client Windows, crack, maupun asset pihak ketiga. Backend world
nanti hanya boleh memakai data yang pengguna miliki sendiri; APK tidak menjadi
repack game.

## Protocol interoperability references

Transport/parser ditulis sendiri. Untuk verifikasi interoperabilitas, maintainer
merujuk dokumentasi/paket publik berikut:

- [openmultiplayer/open.mp](https://github.com/openmultiplayer/open.mp), MPL-2.0:
  layout packet/RPC open.mp dan perilaku server.
- [openmultiplayer/open.mp-network](https://github.com/openmultiplayer/open.mp-network),
  MIT: semantik `NetworkBitStream` untuk mengonfirmasi bit order.
- [openmultiplayer/RakNet](https://github.com/openmultiplayer/RakNet): perilaku
  RakNet 2.x/SA-MP compatibility layer.

Tidak ada source upstream yang di-vendor atau di-link sebagai library di APK.
Kontributor wajib mencatat sumber/lisensi tabel, capture, dan hasil observasi di
PR; jangan copy-paste source/binary dengan lisensi tidak kompatibel.

## Compatibility data

`samp_crypto_table.inc` dan `samp_auth_table.inc` adalah data wire-format untuk
kompatibilitas legacy, bukan asset game atau implementasi RakNet yang di-link.
Provenance dan batas penggunaan harus tetap dicatat di perubahan terkait.

## Trademarks

"Grand Theft Auto", "GTA", "Rockstar Games", "SA-MP", dan "open.mp" adalah
merek pihak masing-masing. Nama dipakai hanya untuk menjelaskan kompatibilitas;
SampDroid tidak berafiliasi atau disponsori oleh mereka.
