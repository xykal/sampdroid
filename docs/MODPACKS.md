# Modpack: kontrak terbuka SampDroid

Tujuan SampDroid bukan memindahkan Windows + Winlator ke Android. Targetnya adalah
klien native ringan yang nantinya bisa memakai **profile UI, kontrol, dan metadata
modpack** ala komunitas PC — tanpa APK mengemas ulang game/asset orang lain.

> **Status sekarang:** manifest dan schema v1 adalah kontrak kontribusi. Loader,
> renderer adapter, dan import UI belum dirilis. Jangan menjanjikan sebuah pack
> "langsung playable" sebelum backend renderer dan asset importer tersedia.

## Prinsip

1. **Kode/config open, asset terpisah.** Manifest, layout touch, shader buatan
   sendiri, dan source plugin boleh dipublikasikan. GTA data atau asset mod harus
   datang dari kepemilikan pengguna atau sumber yang izinnya jelas.
2. **Tidak ada download game tersembunyi.** Pack tidak boleh mengunduh/menyisipkan
   APK, OBB, `libGTASA.so`, crack, atau archive asset tanpa izin.
3. **Portabel.** Sebuah pack menyatakan ABI/minimum client/back-end yang dibutuhkan;
   tidak boleh mengunci semua pengguna ke arm64 atau Winlator.
4. **Aman untuk importer.** Semua path relatif, tidak boleh `..`, absolute path,
   executable shell, native `.so`, atau script yang auto-run.
5. **Attribution wajib.** Setiap pack punya `license`, author, dan sumber asset.

## Manifest v1

Simpan sebagai `manifest.json`; validasikan dengan
[`schemas/modpack-manifest.schema.json`](../schemas/modpack-manifest.schema.json).

```json
{
  "schema": 1,
  "id": "org.community.city-rp-touch",
  "name": "City RP Touch Layout",
  "version": "1.0.0",
  "license": "CC-BY-4.0",
  "authors": [{ "name": "Nama Kontributor", "url": "https://example.org" }],
  "minSampDroid": "0.4.0",
  "targets": ["armeabi-v7a", "arm64-v8a"],
  "controlProfile": "controls/city-rp.json",
  "hudProfile": "hud/city-rp.json",
  "assets": {
    "mode": "user-provided",
    "notice": "Pengguna menyediakan GTA SA dan mod yang mereka miliki sendiri."
  }
}
```

| Field | Wajib | Fungsi |
|---|---:|---|
| `schema` | ya | Versi format manifest, sekarang `1`. |
| `id` | ya | Reverse-domain lowercase: `org.nama.pack`. |
| `name`, `version`, `license` | ya | Identitas dan attribution pack. |
| `authors` | ya | Nama kreator; URL opsional. |
| `minSampDroid` | ya | Versi minimum untuk menghindari pack crash. |
| `targets` | ya | `armeabi-v7a`, `arm64-v8a`, atau keduanya. |
| `controlProfile` | tidak | Path relatif ke JSON layout kontrol. |
| `hudProfile` | tidak | Path relatif ke JSON layout HUD. |
| `assets` | ya | Saat ini hanya `mode: "user-provided"`. |

## Yang cocok untuk "style PC"

- Layout tombol/joystick berbasis game mode (RP, race, deathmatch, trucking).
- Tema HUD, font open-license, palette, icon buatan sendiri, dan localization.
- Binding gamepad/keyboard/gyro yang lebih nyaman dari overlay emulator.
- Preset sensitivitas/perangkat low-end dan high-end.
- Metadata server, rules UI, dan opsi visual yang tidak menyertakan asset game.

## Yang harus dipisahkan

Jika sebuah modpack PC membutuhkan texture/model/audio/game data tertentu,
SampDroid hanya boleh menyediakan **checker + path selector** untuk file yang
sudah pengguna miliki. Pack harus memberi petunjuk legal untuk mendapatkannya,
bukan menyalin file ke release atau mengotomatisasi download tanpa izin.

## Cara berkontribusi pack

1. Buat repositori pack sendiri berlisensi jelas (MIT/CC0/CC-BY atau kompatibel).
2. Validasi manifest/control profile terhadap schema v1.
3. Sertakan screenshot yang seluruh asset-nya boleh dipublikasikan.
4. Buka issue/PR untuk menambahkan link ke katalog komunitas kelak — jangan commit
   archive besar atau asset binary ke repo inti.
5. Sebutkan ABI yang diuji. Semua pack wajib fallback untuk layar kecil dan
   tidak mengasumsikan perangkat 64-bit.
