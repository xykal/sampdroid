# Arsitektur terbuka SampDroid

```text
Android shell (Kotlin)
 ├─ Server browser / favorite / profile import
 ├─ HUD + accessibility + touch/gamepad mapper       ← komunitas UI
 └─ JNI boundary
       │
Native core (C++17, ARMv7 + ARM64)
 ├─ Query protocol
 ├─ RakNet 2.x codec                                 ← test vector deterministik
 │   ├─ bitstream / ACK / reliability
 │   └─ split reassembly
 ├─ Session state + RPC decoder                       ← contributor protocol
 ├─ Game abstraction                                  ← renderer/input adapter
 └─ Optional user-owned data resolver                 ← tidak ada game asset di APK
       │
Renderer backend (future, pluggable)
 ├─ legally supplied GTA SA data adapter
 └─ open renderer / community backends
```

## Kenapa dipisah seperti ini

- **Network core** tidak mengenal UI Android atau asset game, jadi bisa dites di
  GitHub runner dan dipakai di ARMv7/ARM64.
- **Session/RPC state** mengubah packet server menjadi state aman (`player joined`,
  `weather`, `spawn`, `object`, dsb.), bukan langsung tulis memory game.
- **Game abstraction** mencegah proyek terkunci pada satu binary/offset. Kontributor
  bisa membuat backend berbeda tanpa mengubah transport.
- **Control profiles/modpacks** hanya berbicara semantic actions (`move`, `jump`,
  `aim`) dan file user-provided — tidak menjalankan kode arbitrary.

## Batas layer

| Layer | Boleh | Tidak boleh |
|---|---|---|
| `raknet_codec` | parse/serialize bytes, bounds checks, test vector | UI, JNI, asset IO |
| `rpc_decoder` | decode state dari payload | mengubah game memory |
| Android UI | menampilkan state, ambil input | socket blocking di main thread |
| renderer adapter | render user-owned data | download/bundle asset proprietary |
| modpack importer | validate manifest + path sandbox | execute script/native binary |

## Kontribusi yang paling berdampak

1. Tambah test capture yang sudah disanitasi dan punya izin.
2. Lengkapi decoder RPC secara satu per satu, dengan bounds check dan unit test.
3. Bangun state reducer untuk spawn, player stream, object, vehicle, chat, HUD.
4. Implementasikan mapper touch/gamepad dari `control-profile` v1.
5. Buat backend renderer yang tidak memaksa perangkat 64-bit/Winlator.
