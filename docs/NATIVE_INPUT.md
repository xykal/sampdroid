# Native Android input layout v1

Layout ini adalah konfigurasi **internal SampDroid** untuk menyajikan kontrol
SA-MP di layar Android. Ini bukan modpack PC, bukan plugin loader, dan tidak
memerlukan file dari client Windows.

Format berbasis koordinat relatif supaya controller native bisa nyaman dipakai
di layar kecil ARMv7, ponsel modern, atau tablet.

> **Status:** schema dan contoh siap dipakai pengembang. Runtime touch overlay
> akan memakai format sama; belum aktif di APK karena HUD/input gameplay masih
> milestone berikutnya.

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

## Aksesibilitas

Setiap layout native sebaiknya menyediakan tombol cukup besar, spacing aman,
opasitas yang bisa dibaca, dan alternatif untuk satu tangan atau perangkat layar
kecil. Kontributor harus menguji landscape 720p dan satu perangkat layar tinggi.
