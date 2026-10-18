# Native RakNet session lifecycle

Dokumen ini menjelaskan session client Android native SampDroid setelah `ClientJoin`.
Ini bukan wrapper `samp.exe`, bukan Wine/Winlator, dan bukan runtime modpack PC.

## Batas implementasi saat ini

```text
user pilih server
  → native UDP socket
  → open/cookie/auth/accepted
  → NEW_INCOMING_CONNECTION + ClientJoin
  → socket + message counters + split reassembler TETAP hidup
  → poll datagram / ACK reliable / reply internal ping / reduce RPC world state
  → reliable transport keepalive otomatis saat idle
  → user dapat minta class secara eksplisit
```

`PlayerRequestSpawn` sudah punya builder/guard di native core, tetapi belum
ditampilkan sebagai kontrol gameplay. Jangan meminta spawn otomatis sebelum player
sync, world bridge, dan renderer siap.

## Ownership dan keamanan

- Hanya **satu** session eksplisit disimpan pada satu waktu.
- Membuka session baru menutup socket lama secara aman.
- Menutup dialog/detail session juga mengirim disconnect best-effort dan menutup
  UDP socket; client tidak meninggalkan koneksi pemain tersembunyi.
- Datagram diterima hanya dari IP + port yang dipilih pengguna.
- Split reassembly, player state, ACK record, pending reliable queue, dan retry
  semuanya dibatasi agar server/packet malformed tidak dapat mengalokasikan tanpa
  batas.
- Network call harus dari `Dispatchers.IO`, bukan UI thread.

## Reliable ordered client RPC

Class/spawn request dibungkus seperti ini:

```text
RakNet RELIABLE_ORDERED (9)
ordering channel: 2 (SyncRPC)
RPC payload: [0x14][rpc id][bit-packed fields]
```

Session menyimpan nomor message dan ordering index setelah `ClientJoin`. Packet
reliable outbound di-track sampai server meng-ACK-nya. Retry setiap 750 ms,
maximum 5 retry, dan queue dibatasi 32 packet.

Saat tidak ada reliable outbound yang menunggu ACK selama 5 detik, session juga
mengirim `ID_DETECT_LOST_CONNECTIONS` (23) sebagai `RELIABLE` (8). Ini mengikuti
perilaku transport RakNet 2.52 kompatibel SA-MP. `ID_INTERNAL_PING` (6) inbound
dijawab dengan `ID_CONNECTED_PONG` (9) yang unreliable; count keduanya terlihat
di diagnostic. Ini transport keepalive, **bukan** player/gameplay sync atau
mekanisme reconnect. Full congestion control/resend window tetap pekerjaan
session/sync berikutnya.

## Android diagnostic surface sementara

Di detail server terdapat:

- **Sesi native** — membuka socket dan memulai poll lifecycle-aware otomatis
  selama dialog masih terlihat;
- **Class 0** — mengirim `PlayerRequestClass(0)` hanya bila socket aktif;
- **Putus sesi** — menghentikan poll lalu menutup socket native.

Loop memanggil native poll di `Dispatchers.IO` (timeout UDP 250 ms) dan memberi
main thread jeda kecil antartick. Saat dialog ditutup, coroutine dibatalkan dan
disconnect best-effort dikirim. Poll juga menjalankan keepalive transport reliable
dan reply internal ping, tetapi belum ada player sync/gameplay; server yang
membutuhkan sync gameplay tetap bisa timeout, dan itu tidak boleh disamarkan.

Surface ini sengaja menulis **eksperimen** dan **belum renderer/gameplay**. Ini
memvalidasi lifecycle secara jujur tanpa mengklaim APK sudah playable.

## Berikutnya

1. Full ACK range behavior, reconnect, ping/timeout metrics, dan resend window
   yang benar di atas keepalive transport yang sudah ada.
2. Class chooser native, spawn flow, lalu `OnFootSync` dengan rate limiter.
3. Player/vehicle/object state → native HUD + renderer/world bridge.
4. Input touch/gamepad/keyboard/gyro → semantic action → sync encoder.
