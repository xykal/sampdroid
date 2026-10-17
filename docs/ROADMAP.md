# Roadmap publik

Roadmap ini sengaja berbasis modul agar banyak kontributor bisa kerja paralel.
Status bukan janji tanggal; CI dan test vector lebih penting daripada buru-buru
menyebut playable.

## Milestone 0.4 — protocol observability

- [x] Open connection, cookie, auth, accepted, ClientJoin.
- [x] ACK reliable dan decoder multi-packet datagram.
- [x] Split reassembly + decode `PlayerInit` (RPC 139).
- [x] Host regression tests untuk codec wire vector.
- [ ] Capture parser untuk stream nyata yang sudah disanitasi.
- [x] Decode + state reducer `PlayerJoin`, `PlayerQuit`, `SetPlayerName`, `SetPlayerColor`.
- [ ] Decode class/spawn response dan entity/world state berikutnya.

## Milestone 0.5 — persistent session state

- [ ] Session lifecycle: connect, disconnect, retry, timeout, clean teardown.
- [ ] Ordered RPC queue dan reliability resend window.
- [ ] Player/class/spawn state reducer.
- [ ] Chat/dialog/checkpoint state tanpa renderer.
- [ ] Player sync encoder dengan rate limiter dan test vector.

## Milestone 0.6 — native controls & HUD

- [ ] Runtime untuk `control-profile` v1.
- [ ] Touch joystick, camera swipe, buttons, aim/fire, vehicle controls.
- [ ] Gamepad / keyboard / gyro mapping.
- [ ] Editor layout di perangkat + export/import JSON.
- [ ] Accessibility presets (large buttons, one-hand, high contrast).

## Milestone 0.7 — renderer adapter contract

- [ ] Interface renderer/input yang tidak terikat binary/offset.
- [ ] User-owned game data picker + checks yang tidak menyalin asset.
- [ ] World/entity bridge untuk player, vehicle, object, weather, HUD.
- [ ] Performance budget ARMv7 dan fallback low-memory.

## Milestone 1.0 — community ready

- [ ] Manifest modpack/profile loader dengan sandbox path.
- [ ] Katalog komunitas berbasis link + attribution, bukan asset mirror.
- [ ] Reproducible tagged APK releases via GitHub Actions.
- [ ] Contributor docs, security handling, dan compatibility matrix matang.

## Label issue yang dicari

`good first issue`, `help wanted`, `protocol`, `android`, `controls`, `modpack`,
`accessibility`, `armv7`, dan `documentation`.
