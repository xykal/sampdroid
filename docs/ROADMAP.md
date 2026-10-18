# Roadmap port native SampDroid

Tujuan roadmap ini jelas: mengganti kebutuhan menjalankan client SA-MP PC lewat
Winlator dengan APK Android native. Tidak ada milestone launcher modpack PC.

## Milestone 0.6 — protocol/state observability

- [x] Open connection, cookie, auth, accepted, ClientJoin.
- [x] ACK reliable, multi-packet datagram, split reassembly.
- [x] `PlayerInit`, PlayerJoin/Quit/Name/Color state reducer.
- [x] Host regression tests untuk wire vector dan lifecycle player state.
- [x] Client `PlayerRequestClass` / `PlayerRequestSpawn` builder + decoder state balasan.
- [x] Retain socket setelah ClientJoin dan kirim `PlayerRequestClass` lewat session native.
- [ ] Live interoperability capture untuk class/spawn di beberapa server nyata.
- [ ] Decode player stream, vehicle/object/pickup state.
- [ ] Sanitized capture replay test dari server nyata.

## Milestone 0.7 — persistent native session

- [x] Explicit connect/disconnect dan clean UDP teardown dari Android shell.
- [x] Socket retained setelah ClientJoin; peer IP/port filter dan bounded state.
- [x] `RELIABLE_ORDERED` channel-2 class request, ACK consumption, bounded retry queue.
- [x] Class selection + spawn response state reducer.
- [x] Lifecycle-aware automatic poll saat detail session terlihat.
- [x] Transport keepalive reliable 5 detik + reply internal ping.
- [ ] Full ACK-range behavior, reconnect, ping/timeout metrics, dan resend window.
- [x] Bounded `SendClientMessage` chat state.
- [x] Normal + race checkpoint native state.
- [ ] Dialog/textdraw native state.
- [ ] Player sync encoder dengan rate limiter dan test vector.

## Milestone 0.8 — native Android controls + HUD

- [ ] Runtime native input layout v1.
- [ ] Touch joystick, camera swipe, buttons, aim/fire, vehicle control.
- [ ] Gamepad / keyboard / gyro mapping.
- [ ] Editor layout di perangkat + export JSON internal.
- [ ] Accessibility: tombol besar, one-hand, high contrast.

## Milestone 0.9 — world/game bridge

- [ ] Android renderer/world bridge interface tanpa Windows runtime.
- [ ] User-owned game data selector/check tanpa copy atau download asset.
- [ ] World/entity bridge: player, vehicle, object, weather, HUD.
- [ ] ARMv7/ARM64 performance budget dan low-memory fallback.

## Milestone 1.0 — native client loop

- [ ] Join → class → spawn → sync → world/HUD/input berjalan sebagai APK native.
- [ ] Tidak membutuhkan Winlator, `samp.exe`, atau modpack PC.
- [ ] Reproducible tagged APK draft releases GitHub Actions untuk kedua ABI, lalu publish manual setelah review.

## Label issue

`good first issue`, `help wanted`, `protocol`, `android`, `native-port`,
`controls`, `accessibility`, `armv7`, dan `documentation`.
