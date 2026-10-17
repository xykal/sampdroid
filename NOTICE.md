# Notices and provenance

## License of this repository

Kode yang ditulis untuk SampDroid berada di bawah [MIT License](LICENSE). Kontributor
memakai DCO dan prinsip *inbound = outbound*; lihat `CONTRIBUTING.md`.

## No game assets or proprietary binaries

Repository dan release SampDroid tidak mengandung GTA: San Andreas APK/OBB/data,
`libGTASA.so`, client Windows, crack, maupun asset modpack pihak ketiga. Pengguna
harus menyediakan data yang mereka miliki sendiri, bila backend renderer kelak
membutuhkannya.

## Protocol interoperability references

Implementasi transport dan parser SampDroid ditulis sendiri. Untuk memverifikasi
interoperabilitas, maintainer merujuk dokumentasi/paket publik dan proyek berikut:

- [openmultiplayer/open.mp](https://github.com/openmultiplayer/open.mp), MPL-2.0:
  layout packet/RPC open.mp dan perilaku server.
- [openmultiplayer/open.mp-network](https://github.com/openmultiplayer/open.mp-network),
  MIT: semantik `NetworkBitStream` untuk mengonfirmasi bit order.
- [openmultiplayer/RakNet](https://github.com/openmultiplayer/RakNet): perilaku
  RakNet 2.x/SA-MP compatibility layer.

Tidak ada source file upstream yang di-vendor atau di-link sebagai library di APK.
Saat menambah tabel, sample capture, atau hasil reverse engineering, kontributor
wajib mencatat sumber dan lisensinya di PR. Jangan copy-paste source/binary dari
proyek dengan lisensi yang tidak kompatibel.

## Compatibility data

File `samp_crypto_table.inc` dan `samp_auth_table.inc` berisi data wire-format
untuk kompatibilitas protokol legacy, bukan game asset dan bukan implementasi
RakNet yang di-link. Asal teknis dan batas penggunaannya harus tetap didokumentasi
di perubahan terkait. Jika ada pemilik hak/lisensi yang meminta koreksi provenance,
maintainer akan mengisolasi atau menggantinya sebelum release berikutnya.

## Trademarks

"Grand Theft Auto", "GTA", "Rockstar Games", "SA-MP", dan "open.mp" adalah
merek/identitas pihak masing-masing. Penggunaannya di sini hanya untuk menjelaskan
interoperabilitas; SampDroid tidak berafiliasi atau disponsori oleh mereka.
