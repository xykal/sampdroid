# Roadmap port native SampDroid

Tujuan roadmap ini jelas: mengganti kebutuhan menjalankan client SA-MP PC lewat
Winlator dengan APK Android native. Tidak ada milestone launcher modpack PC.

## Milestone 0.6 — protocol/state observability

- [x] Open connection, cookie, auth, accepted, ClientJoin.
- [x] ACK reliable, multi-packet datagram, split reassembly.
- [x] `PlayerInit`, PlayerJoin/Quit/Name/Color state reducer.
- [x] Host regression tests untuk wire vector dan lifecycle player state.
- [x] Client `PlayerRequestClass` / `PlayerRequestSpawn` builder + decoder state balasan.
- [ ] Send class/spawn melalui persistent session ke server nyata.
- [ ] Decode player stream, vehicle/object/pickup state.
- [ ] Sanitized capture replay test dari server nyata.

## Milestone 0.7 — persistent native session

- [ ] Connect/disconnect/retry/timeout/clean teardown.
- [ ] Ordered RPC queue dan resend window.
- [ ] Class selection + spawn state.
- [ ] Chat/dialog/checkpoint/textdraw native state.
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
- [ ] Reproducible tagged APK releases GitHub Actions untuk kedua ABI.

## Label issue

`good first issue`, `help wanted`, `protocol`, `android`, `native-port`,
`controls`, `accessibility`, `armv7`, dan `documentation`.
