# Native Android input layout v1

Layout ini adalah konfigurasi **internal SampDroid** untuk menyajikan kontrol
SA-MP di layar Android. Ini bukan modpack PC, bukan plugin loader, dan tidak
memerlukan file dari client Windows.

Format berbasis koordinat relatif supaya controller native bisa nyaman dipakai
di layar kecil ARMv7, ponsel modern, atau tablet.

> **Status:** runtime touch sekarang hidup di scene GPU native **dan radar
> fallback**, lalu menerjemahkan joystick/look/action ke state semantic. Gerak hanya dapat menjadi Packet-207
> ketika gameplay tick native sudah memiliki collision, spawn/ACK, authority
> controllable, serta state lokal yang valid. AIM on-foot juga dapat membangun
> Packet-203 terbatas dari transform collision/heading + weapon/ammo server
> known; touch tidak pernah mengarang posisi, target, atau hit sendiri. Editor
> **LAYOUT** in-scene menyimpan posisi/ukuran per perangkat, preset satu tangan,
> kontras, dan opt-in gyro look lalu dapat menyalin JSON schema v1 ke clipboard.
> Gyro hanya menerjemahkan kecepatan sudut sensor menjadi delta yaw/pitch relatif
> bounded pada mapper yang sama; tidak pernah menjadi posisi, target, atau paket sendiri.

## File

- Schema: [`schemas/native-input-layout.schema.json`](../schemas/native-input-layout.schema.json)
- Contoh: [`examples/native-input-layouts/default-landscape.json`](../examples/native-input-layouts/default-landscape.json)

## Bentuk dasar

```json
{
  "schema": 1,
  "id": "org.sampdroid.default-landscape",
  "name": "Default native landscape",
  "orientation": "landscape",
  "safeArea": true,
  "highContrast": false,
  "gyroLook": false,
  "controls": [
    {
      "id": "move",
      "kind": "stick",
      "action": "move",
      "anchor": "bottom-start",
      "x": 0.12,
      "y": 0.83,
      "size": 0.18,
      "opacity": 0.72
    }
  ]
}
```

`x` dan `y` adalah koordinat `0.0–1.0` relatif terhadap safe area, bukan pixel
absolut. Tidak ada script, executable, asset, maupun binary external yang bisa
dimuat melalui file ini.

## Semantic actions

| `kind` | Action umum | Fungsi native |
|---|---|---|
| `stick` | `move`, `camera` | joystick analog dua sumbu |
| `button` | `jump`, `sprint`, `enter-vehicle` | tombol tekan/tahan |
| `trigger` | `aim`, `fire`, `brake` | hold/trigger |
| `radial` | `weapon-wheel`, `quick-chat` | menu radial native |
| `swipe-zone` | `look` | zona kamera tanpa ikon |

Action diterjemahkan oleh input mapper SampDroid menjadi state client Android,
bukan keycode atau hook client PC.

## Runtime touch pad dan editor

Tombol **Kontrol touch** di detail sesi tetap menyediakan pad multi-touch native
untuk inspeksi input. Overlay scene GPU/radar memakai default internal yang
setara layout contoh, lalu tombol **LAYOUT** membuka editor drag langsung di
perangkat.
Editor menyimpan koordinat relatif, skala, kontras, dan opt-in gyro di
`SharedPreferences` aplikasi; preset satu tangan kiri/kanan hanyalah titik awal
yang tetap dapat digeser. Ia tidak membutuhkan storage permission, tidak membaca
script/asset luar, dan hanya mengubah presentasi/input Android lokal. Bila sensor
gyroscope tidak ada, toggle ditampilkan tidak tersedia. **Salin JSON** menghasilkan
schema v1 data-only ke clipboard agar dapat dibagikan/diaudit manual; import
dinamis belum dibuka.

Pad menghasilkan `moveX/moveY`, delta look yaw/pitch relatif, serta action
tahan `USE`, `FIRE`, `RUN`, `JUMP`, dan `AIM`. State tersebut masuk ke C++
dengan batas finite/range lalu dipetakan ke field legacy berikut:

| Intent native | Field PlayerFootSync | Aturan |
|---|---|---|
| joystick X | `LeftRight` signed raw | `-128..128`, dead zone 0.08 |
| joystick Y | `UpDown` signed raw | forward menjadi `-128` sesuai `Key::UP` |
| action tahan | `Keys` | bit SA-MP/open.mp audited (`ACTION`, `FIRE`, `SPRINT`, `JUMP`, `AIM`, dll.) |
| swipe horizontal | quaternion W/X/Y/Z preview | heading diakumulasi dan dinormalisasi `0..360` |
| swipe vertikal | front vector + `AimZ` Packet-203 saja | pitch relatif dibatasi `-75..75°`; tidak mengubah transform atau gerak dunia |
| gyro opt-in | delta yaw/pitch relatif yang sama | angular velocity Android × waktu sample, remap orientasi layar, finite/range bounded; berhenti saat freeze/spectate/dead, app masuk background, atau scene dismiss dan tidak menyimpan orientasi absolut |

Local-player bridge hanya memakai posisi, vital, velocity, special action, dan
facing authoritative yang sudah direduce dari RPC. Ia tidak mengarang collision,
posisi langkah, senjata aktif, surfing, atau animation. Pad diagnostik sendiri
tetap preview-only; overlay scene/radar menyerahkan intent yang sama ke tick
native, yang baru boleh memanggil sender OnFoot setelah collision world dan gate
session terpenuhi. Saat AIM tetap ditahan on-foot, tick yang sama dapat mengirim
PlayerAimSync Packet-203 pada `WeaponRate` terpisah hanya bila armed weapon/ammo
server known dan server tidak mengendalikan endpoint kamera; yaw/pitch relatif
menjadi front vector + `AimZ` bounded, bukan target/raycast. Bila kandidat lolos,
scene dapat mempresentasikan view aim orang-pertama dan reticle dari yaw/pitch
identik; visual itu juga tidak menjadi target/hit claim. Jika **AIM+FIRE**
sama-sama held dan semua gate tadi lolos, native boleh mengirim Packet-206
`HitType=None` rate-capped dari origin kamera yang sama. Form tanpa `PlayerID`
ini diaudit terhadap reader + generator NPC open.mp commit `3d20f3dc`, dan sender
juga berhenti bila `PlayerInit.LagCompensation=0`; ini bukan interoperabilitas
live server legacy/anticheat. Ia membawa ID dan offset netral; `HitPos` hanya
nol atau contact absolut dari ray COL statik dekat (maks. 20 unit), tanpa
resolver target entity. Ia tidak dapat menentukan target/hit entity, damage,
reward, atau decrement ammo.
Tidak ada layout/UI yang dapat melewati gate tersebut.

## Aksesibilitas

Setiap layout native sebaiknya menyediakan tombol cukup besar, spacing aman,
opasitas yang bisa dibaca, dan alternatif untuk satu tangan atau perangkat layar
kecil. Preset satu tangan kiri/kanan hanya awal, bukan jaminan semua ukuran layar;
kontrol masih wajib ditata ulang bila bertumpuk. Gyro bersifat opt-in karena
sensitivitas/kenyamanan sensor berbeda per perangkat. Kontributor harus menguji
landscape 720p, satu perangkat layar tinggi, dan (bila tersedia) gyro nyata.
