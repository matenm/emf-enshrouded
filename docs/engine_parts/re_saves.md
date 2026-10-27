# Saves: file names, storage, slots, autosave, mod sidecars

Build 1269579, static analysis plus a look at the save files on disk (read only). Signatures come from `tools/re/sig.py`; each one matches exactly one place in `.text`.

**Correction to `plumbing.md` §3.1:** the `.ksd` files are not just old files read by the migration step. They are the live save files of the current "savedata2" system. The migration step only lists them at startup.

---

## 0. Short answers

1. **File names and API.** The savedata2 library builds `<type><slot>.ksd` (`character0.ksd`, `world0.ksd`, `global.ksd`) and `backup/<file>.backup-<0..9>`. There are two storage backends:
   - **Steam:** `ISteamRemoteStorage::FileWrite` (vtable slot 0). Steam keeps these files in `userdata\<id>\1203620\remote\`. Backups are written with `FileWrite` and then `FileForget` (vtable `+0x28`), so they stay on this PC and are not synced to the cloud.
   - **Local:** plain files in `%USERPROFILE%\Saved Games\keen games\Enshrouded\default\`.

   Every slot belongs to one backend. A slot id is a u16: `lo = index+1` (0 means invalid) and `hi = storage` (0 Local, 1 Steam).
2. **Slots.** There are 16 character slots and 10 world slots per backend (32 and 20 in total). They come from a static container table and are checked in code (`idx > 0xF`, `idx > 9`).
   - **Creating a world** only works on a slot whose state is "empty" (`createWorld` returns error 0x14 otherwise). An existing world cannot be overwritten by "new world".
   - **Creating a character** with slot id 0 takes the first empty slot.
   - **Seen on disk:** a new test character and a new test world both went to **Local slot 0**: `%USERPROFILE%\Saved Games\keen games\Enshrouded\default\character0.ksd` and `world0.ksd`. The log says `[play] Load local save Slot: '0 ( Local )'`. The Steam `remote\character0.ksd` and `world0.ksd` on the same PC were not touched. The two "slot 0"s are different files in different folders.
3. **Autosave.**
   - **Host:** a world save every **30 s** of real time while the server ECS world runs (`now - inst+0x17B0 > 30e9 ns`). Confirmed in game: the Local `world0.ksd` was written twice, 30 s apart.
   - **Quitting:** server shutdown requests a final save, sends each client its final character save, and waits up to 10 s (`[server] Shutdown Timeout`). Quitting to the menu or to the desktop saves. A crash or a killed process loses up to 30 s.
   - **Client:** yes, a client saves its character file. The host streams each player's character save to that player (700-byte chunks, 5 s ack timeout). The client commits it to `character<slot>.ksd` and `global.ksd`. Seen on disk after playing as a client: `remote\character0.ksd` and its backups were written four times in 8 minutes.
4. **Sidecar hooks.**
   - **Save:** post-hook `save2WriteWorld` `0x140e030e0` (runs on the Save2Thread). It succeeded when `*(u8*)(save2+0xB728C) == 0`. The slot is `*(u16*)(save2+0xB7287)`. (Build 1273043: `+0xB729C` / `+0xB7297`.) Alternatively, snapshot in a post-hook of `0x140e4d9d0` when it returns 0.
   - **Load:** post-hook `0x140e3c540` when the result byte is 0. The slot is `*(u16*)(inst+0x1754)` and `save2 = *(void**)(inst+0x1C8)`.
   - **Key the sidecar** by (storage, slot index). Store the world GUID (`save2+0xB7294`, 16 B) and the revision (`save2+0xB72A4`, u32) inside it.
   - **Never write into the Steam `remote` root or the Local `default` root.** The migration scanner picks up unknown files there.

---

## 1. Storage, file names, containers

### 1.1 Container type table (static, `0x141d464b0`, 5 entries × 0x18 B)
Fields: `{char* name; u64 len; u8 kind @+0x10; u8 flags @+0x11 (bit0 = raw JSON, no ".ksd"); u16 slotCount @+0x12; u16 @+0x14; u16 dirIndex @+0x16}`.

| idx | name | kind | flags | slots | file |
|---|---|---|---|---|---|
| 0 | `user_settings.json` | 1 | 1 | 1 | `user_settings.json` |
| 1 | `local_settings.json` | 2 (machine, never Steam) | 1 | 1 | `Saved Games\keen games\Enshrouded\local_settings.json` |
| 2 | `global` | 1 | 0 | 1 | `global.ksd` (KnownPlayers, user data, adventure subscriptions) |
| 3 | `character` | 0 | 0 | **16** | `character0..15.ksd` |
| 4 | `world` | 0 | 0 | **10** | `world0..9.ksd` |

Blob names used inside containers are listed right after this table at `0x141d46530`: `user`, `local`, `adventure_subscriptions`, `known_players`, `user_data`, … Confidence High (table read directly; the counts match the checks in code).

### 1.2 Functions
| Name | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| buildContainerPath | `0x141022160` | `48 89 5C 24 08 55 56 57 41 56 41 57 48 8D AC 24 F0 F7 FF FF` | `(err*, outPath, fileRef*)`. Builds the path from the user directory (`"default"` when there is no user name), then `"%s%.2k"` (type name + slot index; only when `slotCount != 1`), then `".ksd"` unless flags bit0 is set. Can create the directory (`[savedata2] Could not create directory path`). The real files have no zero padding (`world0.ksd`). | High |
| savedata2 write simple file | `0x14102b2d0` | `48 89 5C 24 08 4C 89 4C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC` | `(err*, sd2User*, {u16 typeIdx,…}*, Slice data*)`. Used by Save2Thread for `user_settings.json` (type 0) and `local_settings.json` (type 1). | High |
| Steam file write | `0x14102afc0` | `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D AC 24 80 FC FF FF` | `(err*, ISteamRemoteStorage*, name, Slice*, bool forget)`. Calls `vtbl[0]` = `FileWrite(name, data, size)`; if `forget`, also `vtbl[5]` = `FileForget(name)`. | High |
| write backup | `0x14102ac40` | `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 44 89 44 24 18` | Writes `backup/%s.backup-%u`. Local backend: a file write. Steam backend: `FileWrite` + `FileForget`, so backups stay local only. | High |
| read backups | `0x1410240b0` | `48 8B C4 48 89 58 08 48 89 70 10 55 57 41 54 41 56 41 57 48 8D A8 68 F7` | Loops `n = 0..9` over `backup/<name>.backup-<n>` and reads each header (revision), to restore or rotate. | High |
| isSteamStorage | `0x1410258e0` | `48 83 B9 48 01 00 00 00 74 0A 48 83` | `sd2+0x148 != 0` (ISteamRemoteStorage*) and `sd2+0x58 == 0` (no folder suffix). Used by `createCharacter` when no slot is given. | High |
| storage name | `0x141025560` | `48 83 EC 38 45 0F B6 08 33 C0 48 89` | 0 → `"Local"`, 1 → `"Steam"` (log text `'0 ( Steam )'`). | High |
| local root setup | `0x141025920` | — | `sd2+0x148 = steamSys+0x38` (remote storage). Local root = KnownFolder #2 (Saved Games) + `"keen games"` + app name (+ `"-"` + suffix if `sd2+0x58`). | High |
| migration filter | `0x141025dc0` | `48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 41 B8` | Returns true for an **unknown** file name: does not end in `.ksd` or `-index`, has no `/`, is not a known name, and is ≤ 32 characters. Such files are treated as old-format saves to migrate (they get moved to `migrated/`). This is why a mod must not put files in the Steam remote root or the Local `default\` root. | High |
| file name for dialogs | `0x140e0daf0` | `48 8B C4 48 89 58 18 48 89 78 20 55 41 56 41 57` | Formats `world%k.ksd` / `character%k.ksd` / `global.ksd` / `user_settings.json` / `local_settings.json` for the corrupt-save user dialog (`user::startInteraction` `0x14101bbd0`). Does not write anything. | High |

- **Steam remote storage accessor:** the init callback `0x1404af7c0` uses `"STEAMREMOTESTORAGE_INTERFACE_VERSION016"`. Its context is at `0x141fbe308` and is filled in Steam init `0x1404af8b0`.
- **Steam imports:** only `SteamAPI_*` / `SteamInternal_*`. All interface calls go through vtables.
- **Local file I/O:** `CreateFileW` / `MoveFileExW` / `DeleteFileW`.

### 1.3 Disk layout seen
- Steam: `<Steam>\userdata\<Steam id>\1203620\remote\`: `character0.ksd`, `world0.ksd`, `global.ksd`, `user_settings.json`, `backup\*.backup-0..9`, `migrated\characters` (the pre-savedata2 file, moved by the migration).
- Local: `%USERPROFILE%\Saved Games\keen games\Enshrouded\`: `local_settings.json`, plus `default\character0.ksd`, `default\world0.ksd` and `default\backup\` (created with the first Local save).
- Backup index: a new file starts at `backup-9` and counts down (seen in game: world0 9 → 8 → 7). After that the rotation is not a simple ring; some older backups are kept. Rule not mapped; Medium.

---

## 2. Save2System slot tables and the slot API

`save2` = Save2System. You can get it from `*(void**)(gameApp+0x690)` (plumbing), `*(void**)(inst+0x1C8)` (GameServer instance), `*(void**)(serverSaveSys+8)`, or `*(void**)(pregame+0x140)`.

| Field | Meaning |
|---|---|
| `save2+0x28` | savedata2 system: `+0x148` ISteamRemoteStorage*, `+0x150` local root path, `+0x50/+0x58` folder suffix |
| `save2+0x980 + st*0xE60 + idx*0x170` | world slot record (st = storage 0/1, idx 0..9). `+0` state, `+0x48` u64 set at creation (time?), `+0x58` name string, `+0xC0` byte flag, `+0xD8` settings block |
| `save2+0x27E8 + st*0x57980 + idx*0x5798` | character slot record (idx 0..15). `+0` state, `+0x10` name, `+0x158` time, `+0x160` flag, `+0x5790` own slot id |
| `save2+0x970` / `+0x974` | world slot list job flag / list state (3 = slot list loaded) |
| `save2+0x27D8` | character slot list busy flag (1 = busy; API calls return error 3) |
| `save2+0x1192AB` u16, `+0x1192AD` u8 | **selected world slot id**, valid flag |
| `save2+0x1192AE` u16, `+0x1192B0` u8 | **selected character slot id**, valid flag |
| `save2+0xB7280` / `+0xB7284` | world job busy flag / job state (1 open, 2 write, 3 patch knowledge) |
| `save2+0xB7285` u16 | world slot requested by `beginWorldSession` |
| `save2+0xB7287` u16 | **world slot the running world is bound to** (`save2WriteWorld` uses it). Build 1273043: `+0xB7297` |
| `save2+0xB728C` 8 B | result of the last world job (byte 0: 0 = ok). Build 1273043: `+0xB729C` (EMF reads both from `save2WriteWorld`'s code, `src/common/code_offsets.cpp`) |
| `save2+0xB7294` 16 B, `+0xB72A4` u32 | **world GUID** (created on the first save of a new world) and **save revision** (+1 on each successful write) |
| `save2+0x10DF18/+0x10DF1C` | "commit server-sent character" job flag / character slot id |

**Slot state byte** (both tables): 0 not scanned, **1 empty**, 2 exists on disk, 3 character being created, 4 changed (metadata must be written), 5 seen only as a flag in the slot lists (meaning unknown), 6 delete pending. 2/4/6 are High; 5 is Low.

| Name | Address | Signature | Notes | Conf. |
|---|---|---|---|---|
| slotIndex(id) | `0x140e78c70` | — (3 bytes: `0F B6 01 FE C8 C3`) | `*(u8*)id - 1` | High |
| slotStorage(id) | `0x140e7c0e0` | — | `((u8*)id)[1]` | High |
| slotIdValid(id) | `0x140e8b6a0` | `80 39 00 76 09 80 79 01 02 73 03 B0` | `lo != 0 && hi < 2` | High |
| ResolveWorldSaveSlot | `0x140dfe280` | `66 89 54 24 10 53 48 83 EC 50 48 8B D9 48 8D 4C 24 68 E8 09` | `(save2, u16 id)` → record pointer, or 0 if `idx > 9` | High |
| ResolveCharacterSlot | `0x140dfe0e0` | `66 89 54 24 10 53 48 83 EC 50 48 8B D9 48 8D 4C 24 68 E8 A9` | `(save2, u16 id)` → record pointer, or 0 if `idx > 15` | High |
| **createWorld** | `0x140dde5c0` | `40 53 55 56 41 54 41 56 48 81 EC F0` | `(KeenResult* out, save2, Slice* name, u16 slotId)`. **Requires slot state == 1**, otherwise error 0x14. Sets state 4, stores the name, selects the slot. `out+8` = slot id. | High |
| **createCharacter** | `0x140e0b2e0` | `48 89 5C 24 08 48 89 74 24 10 66 44` | `(out, save2, Slice* name, u16 slotId)`. If slotId is invalid: picks the **first state-1 slot**, in the Steam table if `isSteamStorage`, otherwise in the Local table. | High |
| createCharacterInSlot | `0x140e0b490` | `66 44 89 4C 24 20 53 57 41 56 48 83` | **No empty check:** resets the record and sets state 3. Never call it with a used slot. | High |
| createCharacterFirstFree | `0x140e0b3f0` | `40 53 48 83 EC 30 8B 82 D8 27 00 00` | `(out, save2, name, u8 storage)`. Used by app setup (`0x1408454d0`, `0x140846f00`). | High |
| FinishNewCharacter | `0x140dcb800` | `48 89 5C 24 10 48 89 7C 24 20 55 48 8B EC 48 83` | Selected slot, state 3 → 4 | High |
| selectWorld | `0x140e0ae00` | `48 89 5C 24 08 57 48 83 EC 20 8B 81 70 09 00 00` | Sets `save2+0x1192AB` if the slot state is 2 or 4 | High |
| selectCharacter | `0x140e0abc0` | `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 82 D8 27 00` | Sets `save2+0x1192AE` | High |
| deleteWorld | `0x140dc53d0` | `48 89 5C 24 18 66 89 54 24 10 57 48 83 EC 20 8B` | State → 6. The file is deleted later by `0x140e164a0` and the Save2Thread. | High |
| deleteCharacter | `0x140dc4ba0` | `48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 40 8B` | **slotId 0 falls back to the selected character.** State → 6 | High |
| renameCharacter / renameWorld | `0x140e049f0` / `0x140dfcf80` | `48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 40` / `40 57 48 83 EC 30 8B 81 70 09 00 00` | | High |
| listWorldSlots | `0x140dca150` | `40 53 56 41 54 41 57 48 83 EC 28 45` | `(out, Array*, save2, u8 storage)`. Lists used slots (state 2..6) for the UI | High |
| listCharacterSlots | `0x140dc86c0` | `40 53 55 41 57 48 83 EC 30 41 8B 80` | Same for characters | High |
| world slot metadata tick | `0x140e164a0` | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00` | Writes changed or deleted world slots; `[save2] Failed to %k WorldSlot` | High |
| getSelectedWorldName | `0x140dcfbc0` | `48 89 5C 24 08 57 48 83 EC 30 8B 82` | Returns the `(ptr, len)` name of the selected world | High |

**Pregame command path (UI → Save2).**
- **Dispatcher:** `0x140867e50` (`4C 8B DC 55 56 57 49 8D AB 58 FC FF`) switches on the command byte: `0x0F` create character, `0x10` finish character, `0x11` select character, `0x12` delete character, `0x13` rename character, `0x15` create world (then `playWorld` `0x1408342f0` sets `gameApp+0x35058` = slot), `0x16` select world, `0x17` delete world, `0x18` rename world.
- **Command posters:**
  - create character `0x140d58980` (`48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B 81 D0 00 00 00 41 0F B7`). The command is posted when the character creation screen opens, with the name `"(unnamed)"` and the UI's slot field `+0x249` (default 0 = automatic).
  - create world `0x140d7a9f0` (`40 55 53 56 41 54 41 55 41 57 48 8D 6C 24 D1 48`). It takes the slot from the UI field `+0x41`; the default name is `"<loca> %u"` with `%u = index + 1`.
- **Not found:** how the UI picks that slot. A test suggests it scans the Local table first (a new character and world both went to Local slot 0). Medium.

---

## 3. When saves happen

| Name | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| **requestServerSave** | `0x140e438f0` | `40 53 48 83 EC 20 80 B9 AA 18 00 00` | `(GameServerInstance*)`. If `inst+0x18AA == 0`, the save state `saveSys+0x78 == 0`, and flag bit2 (world running) is set: sets bit0 (pending), state = 1, `inst+0x17B0 = now`. Call it on the server thread to force a save. | High |
| autosave timer | in `serverInstanceTick` `0x140e46070` | — | `if (ecsWorldRunning && now - inst+0x17B0 > 30'000'000'000 ns) requestServerSave(inst)`. Time comes from `0x140891840` (QPC → ns). Confirmed in game: 30 s between writes. | High |
| serverSaveTick | `0x140e4cd10` | plumbing | Runs the state machine while bit0 & bit1 are set and bit3 is clear. Result 0 → clears bit0. An error sets bit3; up to 4 retries | High |
| serverShutdown (inst state 6) | `0x140e435c0` | `48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 40 80 BA 20` | First call: `requestServerSave` (unless reason == 2) and `0x140de1970(save2, player, flag)` for each of the 16 player entries. That call probably starts the final client save; the logs show `Server sent final client save` at quit (Medium). Then waits until the save is done, or 10 s → `[server] Shutdown Timeout`. | High (the wait) |
| world session open | `0x140dd4330` | `40 53 48 83 EC 20 8B 81 80 72 0B 00` | `beginWorldSession(save2, u16 slotId)`. Called from inst state 2 (`0x140e3a350`) with `slotId = *(u16*)(inst+0x1754)`. Queues job 1 (`0x140dd3050`): read the header, or create a new GUID for a new world. Sets `save2+0xB7287`. | High |
| open world job | `0x140dd3050` | `48 89 5C 24 18 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC B0 00 00 00` | Save2Thread job state 1 (`[save2] Failed to create new guid for savegame`) | High |
| save2WriteWorld | `0x140e030e0` | `48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 F4 FF FF` | Save2Thread job state 2. Container ref = `{type 4, idx(save2+0xB7287), GUID+rev, storage 0x100 Local / 0x101 Steam}`, opened with `0x141028790` and committed with `0x14101fbb0`. Result → `save2+0xB728C`. | High |
| client save replication (server) | `0x140e13fe0` | `48 8B C4 55 53 41 56 48 8D 68 C8 48` | Per-player state machine: sends the character save in 700-byte chunks, waits 5 s for the ack, logs `[save2] Server sent final client save for %k.` | High |
| client commit | `0x140dc0a30` | plumbing | Copies the received character save into Save2 and queues job `+0x10DF18` → `0x140dd2a00` (`48 89 5C 24 18 4C 89 4C 24 20 55 56 57 41 54 41 55 48 8D 6C`), which writes `character<slot>.ksd` | High |
| local character save | `0x140dffb60` | `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00` | Save2Thread job `+0x1138D8` (`[save2] Failed to save character`) | High |

- **Build 1273043 (EMF API v19 `request_save`):** the same signature matches once in both exes:
  `0x140e463e0` in the game, `0x1407afea0` in the dedicated server. The body is unchanged: `inst+0x18AA`, the save
  system at `*(inst+0x20)` with its state `+0x78`, flags `+0x70` (bit 2 = world running, bit 0 = pending) and request
  time `+0x80`, then `inst+0x17B0 = now`. EMF takes `inst` from its `loadSavegameStep` post-hook (it runs on the server
  thread after the world's setup) and calls it from a server world callback, which runs on that same thread inside
  the server instance's tick.
- **What triggers it.** World saves are timer driven (30 s) and also happen at shutdown. `requestServerSave` is also called from the server message dispatcher `0x140e47c20` (probably player join/leave; not checked, Low). Character files follow the server's replication; on the client side they get written about once a minute (from backup times; Medium).
- **Who saves what.** The host writes the world file. Every player, the host included (its own GameClient), writes its own character file and `global.ksd` from what the server sends.

---

## 4. Mod sidecar: recommended hooks

**Where to write.**
- Use a folder the framework owns, for example `<game>\mods\<modid>\saves\` or `%LOCALAPPDATA%\<framework>\saves\`.
- Do **not** use the Steam `remote` root or `Saved Games\keen games\Enshrouded\default\`. The migration scan `0x141025dc0` (callers `0x141020fd0` Steam, `0x14101fc40` Local) treats unknown short file names there as old saves.
- Do not use `ISteamRemoteStorage` either: it has a quota, and the same scanner reads those files.

**Key and validation.**
- File name: `world_<storage>_<idx>.dat`, with storage 0 Local or 1 Steam, from slot id `u16` (`idx = lo-1`, `storage = hi`).
- Inside the file, store the GUID (`save2+0xB7294`, 16 B) and the revision (`save2+0xB72A4`).
- On load, ignore the sidecar if the GUID differs. That happens when a world was deleted and a new one was created in the same slot.
- If `sidecar.rev > world.rev`, the world write failed after the sidecar was written.

**Save.**
1. Server thread: post-hook `serverSaveStateMachine 0x140e4d9d0`. Signature `KeenResult* (KeenResult* out, ServerSaveSys* sys, SaveCtx* ctx)`. When `out->code == 0` (finished; the ECS snapshot was just taken), snapshot the mod state into memory. `save2 = *(void**)((char*)sys+8)`.
2. Save2Thread: post-hook `save2WriteWorld 0x140e030e0`. Signature `void (Save2System* save2, void* sd2User)`. If `*(u8*)(save2+0xB728C) == 0`, write the sidecar for slot `*(u16*)(save2+0xB7287)`, with revision `*(u32*)(save2+0xB72A4)` (already incremented). Write to a temp file and then `MoveFileExW(REPLACE_EXISTING)`.

A simpler version that is good enough: do both steps in the post-hook of `0x140e4d9d0`.

**Load.**
- Post-hook `loadSavegameStep 0x140e3c540`. Signature `KeenResult* (KeenResult* out, GameServerInstance* inst)`.
- It returns code 1 every tick while still waiting. Act once, on the first `code == 0`.
- Read: `slot = *(u16*)(inst+0x1754)` (only if `*(u8*)(inst+0x1756) != 0`, which means a local world slot rather than an adventure), `save2 = *(void**)(inst+0x1C8)`, GUID at `save2+0xB7294`.
- `inst+0x18C0 == 0` means a brand-new world with nothing to load: start with empty mod state.
- This runs before inst state 4 (ECS world start).
- If the framework needs the slot earlier, hook `beginWorldSession 0x140dd4330` (`void (Save2System*, u16 slotId)`).

**Other hooks.**
- **Force a save:** call `requestServerSave(inst)` on the GameServer thread.
- **Delete:** post-hook `deleteWorld 0x140dc53d0` (`void (save2, u16 slotId)`) to remove the sidecar. With the GUID check, a leftover sidecar is harmless anyway.
- **Character data:** hook `0x140dc0a30` (client commit) or `0x140dd2a00` (slot `*(u16*)(save2+0x10DF1C)`). Key it by the character slot and remember that a client has no world save.

---

## 5. Safety notes for tests
- A player's real saves are usually Steam saves, in `<Steam>\userdata\<Steam id>\1203620\remote\`. A test character and world made as LOCAL go to the Local slots (`Saved Games\...\default\`), a separate set of files. In the test above the Steam files stayed untouched.
- **World creation is safe:** `createWorld` refuses any slot that is not empty.
- **Avoid deleting:** delete with an invalid id targets the selected character (`0x140dc4ba0`), and a world delete takes effect on the next metadata tick.
- **Corrupt-save dialogs:** they offer "Erase" / "Overwrite" (`0x14101abb0`); do not choose these for the real slots.
- **Back up first:** copy the `remote` folder before experiments; reading or copying it is harmless.
- **Joining someone's world with your real character** rewrites its `remote\character<N>.ksd`, which is normal game behaviour. Use the test character for modded multiplayer tests.

## Open questions
- How the UI chooses the slot for a new world or character (the UI fields `+0x41` / `+0x249`; Local seems to be scanned first).
- The backup rotation and retention rule (`0x14101f830` / `0x14101fbb0`).
- Slot state 5. The triggers of `requestServerSave` in `0x140e47c20`.
- Exact meaning of the world record fields `+0x48`, `+0xC0`, `+0xD8`.

## The menu's slot records (build 1273043)

Static analysis for EMF's save backups (`src/core/save_slots.h` reads these at the main menu into
`%LOCALAPPDATA%\EMF\save-info.json`). Record bases as in `src/core/menu_start.h` (`menu_layout`): character record =
`save2+0x27F8 + st*0x57980 + idx*0x5798`, world record = `save2+0x990 + st*0xE60 + idx*0x170`.

**Character record.** Two blobs, filled from the character file by the slot scan `0x140dd4fc0`: part A
`character_data` at `+0x10` (0x2A0 bytes; copy `0x140dbb4d0`, writer `0x140e1c090`; the name `{char*; u64}` is its
start) and part B `player_character` at `+0x2B0` (0x54E0 bytes; copy `0x140dbb990`).

| Field | Type | What | How found | Conf. |
|---|---|---|---|---|
| `+0x2F0` (part B +0x40) | u32 | **level**, 1-based | the migration `0x140ddc4d0` copies `keen::ecs::Level` there (fnv1a 0xe550da37) and `Experience` to +0x44; the live save `0x140e03f90` stores the level attribute (0x724bf568) there, 1 when missing; the record constructors (`0x140da0220`, `0x140dc5f30`) write `qword [rec+0x2F0] = 1`; the menu's list builder `0x14083ded0` copies `[partB+0x40]` into its entry next to the name | High |
| `+0x2F4` (part B +0x44) | u32 | experience (cumulative or per level: not checked) | as above | Medium |
| `+0x158` | i64 | Unix seconds set by `createCharacterInSlot` `0x140e0dd00` (`time()`), and later saves update it: the menu's date under the level ("<date>, 13:37", then "14:10" after a session) is the last time the character was saved | the menu copies it next to the level; in game, see below | Medium-High |
| `+0x160` | u8 | flag, 1 at creation | | Low |

No per-character play time was found (part B +0x54C0 is a component timestamp, not play time).

**World record.** `world_metadata` starts at `+0x20` (reader `0x140dfd5d0`, writer `0x140e246e0`); the running world's
copy is at `save2+0xB72B8`.

| Field | Type | What | How found | Conf. |
|---|---|---|---|---|
| `+0x20` | 16 B | world GUID | | High |
| `+0x48` | i64 | **last played**, Unix seconds | `createWorld` `0x140de08f0` and every world save (`0x140e05670`, `save2+0xB72E0` in the session copy) set it to `time()` | High |
| `+0x50` | u32 | 1 at creation, refreshed from a server byte at each save; the old JSON key here was `progressLevel` (maybe the flame level) | | Low |
| `+0x58` | `{char*; u64}` | the world's name | | High |
| `+0xB8` | i64 | **play time**, nanoseconds | the server adds frame deltas to `inst+0x17B8` (`0x140e48b60`), copies it to the save system at a save request, then to `save2+0xB7350` (setter `0x140e0d120`): this field in the session copy | Medium-High |
| `+0xC0` | u8 | probably the game-settings preset | | Low |
| `+0xD8` | blob | `game_settings` | | High |

The records hold what the slot scan read from the files; whether they are refreshed when the player returns to the menu
after a session was not checked (EMF writes what it reads each time the main menu comes up, so a stale value is
replaced at the next game start at the latest).

**Confirmed in game (build 1273043, EMF's save-info recorder at the main menu):** a LOCAL test character read
level 1 at `+0x2F0`, as the menu showed ("Level 1"); a Steam character read level 11. The worlds' `+0x48` equalled
their files' modification times to the second (a Steam world and the LOCAL test world), and
`+0xB8` gave 32976 s for the LOCAL test world. After `world0.ksd` was restored from an older backup, the next game's
menu record had the restored file's `+0x48`, not the newer one the game had saved since: the slot scan reads the files
on disk (the game did not fall back to its `backup\` copies), and the restored world loaded and ran.

**Quest progress:** neither record has a field for it that could be identified. Quest state lives in the player's
knowledge and the ECS data inside the packed save; part A's u32 at +0x298 is unidentified (the old JSON had
`activeQuestId` on characters), a guess that was not checked. EMF shows no quest progress.

Functions (1273043): ResolveCharacterSlot `0x140e004e0`, ResolveWorldSaveSlot `0x140e00680`, listCharacterSlots
`0x140dca620` (returns `{u16 slotId, ..., part A ptr, part B ptr}`), the menu list builder `0x14083ded0`, character
metadata update `0x140de54c0`, slot write tick `0x140e14980`.
