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
  → poll datagram / ACK reliable / reduce RPC world state
  → user dapat minta class secara eksplisit
```

`PlayerRequestSpawn` sudah punya builder/guard di native core, tetapi belum
ditampilkan sebagai kontrol gameplay. Jangan meminta spawn otomatis sebelum player
sync, heartbeat, world bridge, dan renderer siap.

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
maximum 5 retry, dan queue dibatasi 32 packet. Ini belum menggantikan full
RakNet congestion control/resend window; itu pekerjaan session/sync berikutnya.

## Android diagnostic surface sementara

Di detail server terdapat:

- **Sesi native** — connect pertama, lalu poll datagram pada tap berikutnya;
- **Class 0** — mengirim `PlayerRequestClass(0)` hanya bila socket aktif;
- **Putus sesi** — menutup socket native.

Surface ini sengaja menulis **eksperimen** dan **belum renderer/gameplay**. Ini
memvalidasi lifecycle secara jujur tanpa mengklaim APK sudah playable.

## Berikutnya

1. Poll loop/heartbeat yang lifecycle-aware, bukan tombol diagnostic.
2. Full ACK range handling + resend window, timeout, reconnect, dan teardown.
3. Class chooser native, spawn flow, lalu `OnFootSync` dengan rate limiter.
4. Player/vehicle/object state → native HUD + renderer/world bridge.
5. Input touch/gamepad/keyboard/gyro → semantic action → sync encoder.
