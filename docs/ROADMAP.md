# Roadmap port native SampDroid

Tujuan roadmap ini jelas: mengganti kebutuhan menjalankan client SA-MP PC lewat
Winlator dengan APK Android native. Tidak ada milestone launcher modpack PC.

## Milestone 0.6 — protocol/state observability

- [x] Open connection, cookie, auth, accepted, ClientJoin.
- [x] ACK reliable, multi-packet datagram, split reassembly.
- [x] `PlayerInit`, PlayerJoin/Quit/Name/Color state reducer.
- [x] Host regression tests untuk wire vector dan lifecycle player state.
- [x] Client `PlayerRequestClass` / `PlayerRequestSpawn` / `PlayerSpawn` builder,
  response decoder dual legacy/open.mp, dan transport-ACK gate.
- [x] Retain socket setelah ClientJoin dan kirim class/spawn flow lewat session native.
- [ ] Live interoperability capture untuk class/spawn di beberapa server nyata.
- [x] Decode player StreamIn/StreamOut dan broadcast remote `PlayerFootSync` (207)
  ke state entity bounded; belum renderer/model/radar.
- [ ] Decode vehicle/object/pickup state.
- [ ] Sanitized capture replay test dari server nyata.

## Milestone 0.7 — persistent native session

- [x] Explicit connect/disconnect dan clean UDP teardown dari Android shell.
- [x] Socket retained setelah ClientJoin; peer IP/port filter dan bounded state.
- [x] `RELIABLE_ORDERED` channel-2 class request, ACK consumption, bounded retry queue.
- [x] Class selection, SetSpawnInfo, local-player spawn/position/vitals, dan spawn response state reducer.
- [x] Lifecycle-aware automatic poll saat detail session terlihat.
- [x] Transport keepalive reliable 5 detik + reply internal ping.
- [ ] Full ACK-range behavior, reconnect, ping/timeout metrics, dan resend window.
- [x] Bounded `SendClientMessage` chat state.
- [x] Normal + race checkpoint native state.
- [ ] Dialog/textdraw native state.
- [x] Fondasi `OnFootSync` packet 207: vector regresi client→server,
  `UNRELIABLE_SEQUENCED` channel 1, ordering counter terpisah, rate gate dari
  `PlayerInit`, serta spawn/session gate.
- [x] Touch semantic mapper bounded + local-player → OnFoot preview, tanpa packet
  movement otomatis atau posisi/collision palsu.
- [x] Decoder Packet 207 server→client dengan quaternion/percent/velocity
  compressed, opsi surfing/animation, guards malformed, dan lifecycle remote entity.
- [ ] Hubungkan encoder OnFoot ke world bridge + input native dan validasi capture/live
  sebelum mengaktifkan loop periodik gameplay.

## Milestone 0.8 — native Android controls + HUD

- [x] Runtime touch pad v1: joystick, camera swipe, jump/run/fire/aim/use →
  bounded semantic state + OnFoot preview.
- [ ] Touch joystick, camera swipe, buttons, aim/fire, vehicle control dalam
  renderer/world loop yang tervalidasi.
- [ ] Gamepad / keyboard / gyro mapping.
- [ ] Editor layout di perangkat + export JSON internal.
- [ ] Accessibility: tombol besar, one-hand, high contrast.

## Milestone 0.9 — world/game bridge

- [ ] Android renderer/world bridge interface tanpa Windows runtime.
- [ ] User-owned game data selector/check tanpa copy atau download asset.
- [ ] World/entity bridge: remote player state ada; model/radar/renderer, vehicle,
  object, weather, dan HUD masih berikutnya.
- [ ] ARMv7/ARM64 performance budget dan low-memory fallback.

## Milestone 1.0 — native client loop

- [ ] Join → class → spawn → sync → world/HUD/input berjalan sebagai APK native.
- [ ] Tidak membutuhkan Winlator, `samp.exe`, atau modpack PC.
- [ ] Reproducible tagged APK draft releases GitHub Actions untuk kedua ABI, lalu publish manual setelah review.

## Label issue

`good first issue`, `help wanted`, `protocol`, `android`, `native-port`,
`controls`, `accessibility`, `armv7`, dan `documentation`.
