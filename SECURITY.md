# Security policy

## Scope

Laporkan secara privat jika menemukan masalah yang dapat membahayakan pemain,
server, atau perangkat, misalnya:

- overflow/crash dari UDP atau RPC malformed;
- allocation, battery, atau bandwidth exhaustion;
- kebocoran token, IP, atau data pribadi;
- path/import data milik pengguna yang tidak aman;
- bypass autentikasi, anti-cheat, ban, atau akses server;
- execution/hook path yang menjalankan binary/script tidak tepercaya.

SampDroid adalah client Android native; Wine/Winlator/x86 runtime dan modpack
script loader bukan bagian dari scope produk.

## Cara melapor

Jangan taruh exploit atau packet reproduksi yang bisa disalahgunakan di issue
publik. Gunakan [GitHub private security advisory](../../security/advisories/new)
untuk `xykal/sampdroid`, sertakan versi APK/commit, perangkat/ABI, dampak, dan
langkah reproduksi aman.

Kami akan mengonfirmasi laporan, mengerjakan patch, lalu mengoordinasikan
disclosure setelah pengguna punya waktu update.

## Bukan tempatnya

- minta cheat, bypass, credential, atau cara menyerang server;
- kirim game asset/binary proprietary;
- post server target dan proof-of-concept yang berdampak pada pemain lain.
