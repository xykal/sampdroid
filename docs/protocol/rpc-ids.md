# Tabel ID RPC SA-MP / open.mp

Diekstrak langsung dari header `Shared/NetCode/*.hpp` di
[openmultiplayer/open.mp](https://github.com/openmultiplayer/open.mp)
(`NetworkPacketBase<ID, NetworkPacketType::RPC, OrderingChannel_SyncRPC>`).

Format wire RPC legacy:

```
[0x14 = ID_RPC][rpc id (u8)][payload bit-packed]
```

Semua RPC dikirim pada ordering channel 2 (`OrderingChannel_SyncRPC`),
reliability `RELIABLE_ORDERED` (9).

## Client -> Server

| ID | Nama | Keterangan |
|----|------|------------|
| 25 | PlayerConnect / ClientJoin | lihat `core.hpp` |
| 54 | NPCConnect | |
| 50 | PlayerCommandMessage | chat diawali `/` |
| 101 | PlayerChatMessage | chat biasa |
| 62 | OnPlayerDialogResponse | |
| 128 | PlayerRequestClass | `u16 ClassID` — builder + wire test ada |
| 129 | PlayerRequestSpawn | payload kosong — builder + wire test ada |
| 155 | OnPlayerRequestScoresAndPings | |
| 103 | ClientCheck | jawaban anti-cheat |

### Layout PlayerConnect (25)

```
u32   VersionNumber        // 0.3.7 = 4057
u8    Modded
u8    NameLen  + char[] Name
u32   ChallengeResponse    // token dari ACCEPTED ^ 4057
u8    KeyLen   + char[] Key        // 40 char hex (serial)
u8    VerLen   + char[] VersionString
u32   (opsional) IsUsingOfficialClient / OmpVersion
```

## Server -> Client (yang sudah terverifikasi muncul)

| ID | Nama | Layout |
|----|------|--------|
| 11 | SetPlayerName | |
| 72 | SetPlayerColor | u16 PlayerID, u32 Color |
| 93 | SendClientMessage | u32 color, u32 len, char[] — reducer caps 144 bytes / 100 lines |
| 99 | MoveObject | u16 ObjectID, VEC3 pos, VEC3 target, float speed, VEC3 rot |
| 128 | PlayerRequestClassResponse | selectable, team, model, spawn, angle, 3 weapon/ammo slots |
| 129 | PlayerRequestSpawnResponse | u32 Allow |
| 137 | PlayerJoin | |
| 138 | PlayerQuit | u16 PlayerID, u8 Reason |
| 139 | **PlayerInit** | ~212 byte (lihat bawah) |

### Class selection / spawn response

Untuk client non-DL seperti SampDroid saat ini, server membalas RPC 128 dengan:

```text
u8 Selectable | u8 TeamID | u32 ModelID | u8 Unknown1 |
float SpawnX | float SpawnY | float SpawnZ | float ZAngle |
u32 Weapon[3] | u32 Ammo[3]
```

RPC 129 dari server berisi `u32 Allow`; `0` berarti spawn ditolak.
Keduanya sudah memiliki decoder/state + regression test. `PlayerRequestClass`
sudah dikirim explicit lewat retained native session; spawn sengaja belum diekspos
sebagai UI gameplay sampai sync/world bridge siap.

### SendClientMessage (93)

```text
u32 RGBA | u32 TextLength | char[TextLength] Text
```

Reducer menyimpan maksimal 100 line, masing-masing maksimal 144 byte. Payload
chat tidak dimasukkan mentah ke summary diagnostic agar log tetap bounded; future
native HUD mengambilnya dari `WorldState`.

### Layout PlayerInit (139) — paket terbesar saat join

```
1 bit  EnableZoneNames
1 bit  UsePlayerPedAnims
1 bit  AllowInteriorWeapons
1 bit  UseLimitGlobalChatRadius
float  LimitGlobalChatRadius
1 bit  EnableStuntBonus
float  SetNameTagDrawDistance
1 bit  DisableInteriorEnterExits
1 bit  DisableNameTagLOS
1 bit  ManualVehicleEngineAndLights
u32    SetSpawnInfoCount
u16    PlayerID
1 bit  ShowNameTags
u32    ShowPlayerMarkers
u8     SetWorldTime
u8     SetWeather
float  SetGravity
1 bit  LanMode
u32    SetDeathDropAmount
1 bit  Instagib
u32    OnFootRate, InCarRate, WeaponRate, Multiplier, LagCompensation
u8     ServerNameLen + char[] ServerName
u8[212] VehicleModels
u32    EnableVehicleFriendlyFire
```

Catatan: `VEC3` / `float` di open.mp ditulis apa adanya (32 bit per komponen),
bukan float terkompresi seperti client 0.3.7 legacy.
