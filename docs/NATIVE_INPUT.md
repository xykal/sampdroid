# Native Android input layout v1

Layout ini adalah konfigurasi **internal SampDroid** untuk menyajikan kontrol
SA-MP di layar Android. Ini bukan modpack PC, bukan plugin loader, dan tidak
memerlukan file dari client Windows.

Format berbasis koordinat relatif supaya controller native bisa nyaman dipakai
di layar kecil ARMv7, ponsel modern, atau tablet.

> **Status:** v0.14 memasang touch pad runtime pertama di detail sesi native.
> Joystick, swipe look, dan tombol action sekarang diterjemahkan menjadi state
> semantic native dan preview `OnFootSync` lokal. Ia **belum** mengirim movement
> packet atau menggantikan renderer/world collision; itu sengaja ditahan sampai
> posisi gameplay benar-benar dimiliki bridge dunia.

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

## Runtime touch pad v0.14

Tombol **Kontrol touch** di detail sesi membuka pad multi-touch native. Pad
v0.14 memakai default internal yang setara dengan layout landscape contoh; schema
JSON tetap kontrak untuk editor/import berikutnya dan belum diload dinamis. Pad
menghasilkan `moveX/moveY`, delta look relatif, serta action tahan `USE`,
`DUCK`, `FIRE`, `RUN`, `JUMP`, dan `AIM`. State tersebut masuk ke C++ dengan
batas finite/range lalu dipetakan ke field legacy berikut:

| Intent native | Field PlayerFootSync | Aturan |
|---|---|---|
| joystick X | `LeftRight` signed raw | `-128..128`, dead zone 0.08 |
| joystick Y | `UpDown` signed raw | forward menjadi `-128` sesuai `Key::UP` |
| action tahan | `Keys` | bit SA-MP/open.mp audited (`ACTION`, `FIRE`, `SPRINT`, `JUMP`, `AIM`, dll.) |
| swipe horizontal | quaternion W/X/Y/Z preview | heading diakumulasi dan dinormalisasi `0..360` |

Local-player bridge hanya memakai posisi, vital, velocity, special action, dan
facing authoritative yang sudah direduce dari RPC. Ia tidak mengarang collision,
posisi langkah, senjata aktif, surfing, atau animation. Karena itu preview ini
tidak secara otomatis memanggil `raknet_session_send_onfoot()` dan tidak bisa
menjadi loop gameplay sebelum renderer/world bridge selesai.

## Aksesibilitas

Setiap layout native sebaiknya menyediakan tombol cukup besar, spacing aman,
opasitas yang bisa dibaca, dan alternatif untuk satu tangan atau perangkat layar
kecil. Kontributor harus menguji landscape 720p dan satu perangkat layar tinggi.
