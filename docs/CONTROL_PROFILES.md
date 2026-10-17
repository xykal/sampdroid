# Control profile v1

Control profile adalah bagian paling penting untuk membuat klien native terasa
lebih nyaman daripada emulator/Winlator. Format ini sengaja berbasis koordinat
relatif agar satu layout bisa diskalakan untuk layar 16:9, 19.5:9, tablet, dan
perangkat ARMv7 lama.

> **Status:** schema + contoh sudah siap untuk kontribusi. Runtime touch overlay
> akan memakai format yang sama; belum aktif di APK v0.5.0.

## File dan schema

- Schema: [`schemas/control-profile.schema.json`](../schemas/control-profile.schema.json)
- Contoh: [`examples/control-profiles/classic-landscape.json`](../examples/control-profiles/classic-landscape.json)

## Bentuk dasar

```json
{
  "schema": 1,
  "id": "org.sampdroid.classic-landscape",
  "name": "Classic landscape",
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

`x` dan `y` berada pada rentang `0.0–1.0` relatif terhadap safe area, bukan pixel
absolut. `anchor` menentukan dari sisi mana posisi dihitung.

## Kinds yang didefinisikan

| `kind` | Contoh action | Fungsi |
|---|---|---|
| `stick` | `move`, `camera` | joystick analog dua sumbu |
| `button` | `jump`, `sprint`, `fire`, `enter-vehicle` | tombol tekan/tahan |
| `trigger` | `aim`, `brake` | tombol analog/hold |
| `radial` | `weapon-wheel`, `quick-chat` | menu radial |
| `swipe-zone` | `look` | area kamera tanpa ikon |

Actions adalah semantic binding, bukan keycode Android. Backend game/controller
menerjemahkannya ke input sesuai platform, sehingga profile bisa dipakai oleh
touch, gamepad, keyboard, maupun accessibility switch.

## Aksesibilitas wajib dipikirkan

Setiap profile baru sebaiknya:

- menjaga jarak antartombol supaya tidak mudah salah tekan;
- menyediakan `opacity` dan `size` yang masuk akal;
- tidak hanya bergantung pada gesture multitouch sulit;
- memberi alternatif satu tangan / tombol besar jika memungkinkan;
- menguji minimal satu layar kecil (sekitar 720p) dan satu layar tinggi/tablet.

## Kontributor profile

Profile bisa dikirim sebagai JSON kecil + screenshot legal. Tidak boleh memasukkan
texture/icon copyrighted tanpa izin. Kalau perlu ikon, gunakan vector sendiri,
Material symbols berlisensi kompatibel, atau pack asset yang punya attribution.
