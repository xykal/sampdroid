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
 ├─ Player sync encoder
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
inbound, ACK outbound, dan retry queue bounded. Android shell memanggil JNI ini
dari Kotlin IO coroutine; selama detail server terbuka, poll UDP berjalan
otomatis dan berhenti/disconnect saat dialog ditutup.

Belum ada gameplay loop, protocol heartbeat/full congestion handling, atau sync
encoder. Karena itu session surface sekarang diberi label eksperimen dan tidak
boleh dipasarkan sebagai renderer/playable client.

## Batas layer

| Layer | Boleh | Tidak boleh |
|---|---|---|
| `raknet_codec` | parse/serialize bytes, bounds checks, test vector | UI, asset IO |
| `rpc_decoder` / `rpc_state` | decode packet menjadi state | hook/execute binary Windows |
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
