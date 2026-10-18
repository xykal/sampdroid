# Arsitektur port native SampDroid

```text
Android shell (Kotlin)
 ├─ server browser / native HUD / Android native input
 └─ JNI boundary
       │
Native client core (C++17, ARMv7 + ARM64)
 ├─ SA-MP query protocol
 ├─ RakNet 2.x codec                         ← deterministic test vectors
 │   ├─ bitstream / ACK / reliability
 │   └─ split reassembly
 ├─ Persistent session + RPC/world state
 ├─ Player sync encoder + remote broadcast decoder
 └─ Android world/game bridge
       │
Native Android renderer backend (future)
 └─ user-owned game data only; no Windows/x86 runtime
```

## Prinsip port

- Client core tidak memanggil `samp.exe`, Wine, Winlator, atau plugin Windows.
- Network state mengubah packet server menjadi state aman (`player`, `spawn`,
  `vehicle`, `object`, `dialog`, dst.), bukan langsung menulis memory binary PC.
- Input native menghasilkan semantic action (`move`, `jump`, `aim`, `fire`) yang
  diterjemahkan oleh Android client sendiri.
- Renderer/world bridge menerima state native dan data milik pengguna; APK tidak
  membundel asset atau binary game.

## Session yang sudah ada di `main`

`raknet_session` mempertahankan satu socket UDP yang dipilih pengguna setelah
`ClientJoin`, membawa message/order counter, split reassembler, `WorldState`, ACK
inbound, ACK outbound, dan retry queue bounded. Saat idle lima detik tanpa
reliable outbound yang pending, session mengirim reliable transport keepalive
`ID_DETECT_LOST_CONNECTIONS`; inbound `ID_INTERNAL_PING` dibalas dengan
unreliable `ID_CONNECTED_PONG`. Android shell memanggil JNI ini dari Kotlin IO
coroutine; selama detail server terbuka, poll UDP berjalan otomatis dan
berhenti/disconnect saat dialog ditutup.

Fondasi encoder `OnFootSync` sudah ada: payload client→server yang diuji,
`UNRELIABLE_SEQUENCED` channel 1, rate gate `PlayerInit`, dan spawn/session
safety gate. Spawn gate sekarang mencakup `RequestSpawn` → respons → `PlayerSpawn`
(52) → transport ACK, sementara `WorldState` menahan template spawn/posisi/vitals
lokal terverifikasi. Kebalikannya, decoder read-only Packet 207 menerima broadcast
server→client hanya untuk entity yang sudah `PlayerStreamIn`; ia memvalidasi
bit-packed quaternion/percent/velocity serta field surfing/animation opsional lalu
mereduksinya ke state remote bounded. `PlayerStreamOut`, ID lokal, dan sync orphan
diabaikan. Gate `UNRELIABLE_SEQUENCED` per channel juga membuang index stale
(sekaligus menangani wrap 16-bit) sebelum state disentuh. Ini belum menghasilkan
model, collision, atau packet movement. Sebagai surface pertama yang tidak lagi
sekadar log, `WorldSnapshot` menyalin maksimal 64 actor + checkpoint ke JNI dan
Canvas Android menggambar radar/peta top-down. Canvas ini hanya memvisualkan
state authoritative yang sudah ada; drag/zoom mengubah kamera lokal saja.

Touch pad runtime sudah memetakan intent Android ke preview input/heading `OnFoot`
lokal, tetapi tidak mengirim gameplay packet atau mengarang posisi sebelum
world/collision bridge ada. Namun belum ada gameplay loop, reconnect/full
congestion handling, atau world bridge. Transport keepalive bukan player sync.
Karena itu session surface sekarang diberi label eksperimen dan tidak boleh
dipasarkan sebagai renderer/playable client.

## Batas layer

| Layer | Boleh | Tidak boleh |
|---|---|---|
| `raknet_codec` | parse/serialize bytes, bounds checks, test vector | UI, asset IO |
| `rpc_decoder` / `sync_decoder` / `rpc_state` | decode RPC/broadcast menjadi state | hook/execute binary Windows |
| session | lifecycle, ACK/retry, ordered RPC, sync | block UI thread |
| Android UI | HUD, browser, input event | socket blocking di main thread |
| world bridge | render state dengan backend Android | bundle/download proprietary game asset |
| native input layout | posisi/action tombol internal | script/plugin/executable loader |

## Kontribusi berdampak besar

1. Persistent session, class/spawn response, player sync, dan teardown.
2. Decoder/state player, vehicle, object, pickup, dialog, textdraw, HUD.
3. Native touch/gamepad/keyboard/gyro mapper.
4. Renderer/world bridge yang portable dan ramah ARMv7.
5. Capture test yang disanitasi, docs, performance profiling, dan accessibility.
