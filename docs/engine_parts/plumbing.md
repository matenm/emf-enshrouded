# Plumbing: logging, debug console, save/load, multiplayer, Impact VM

Build 1269579, image base 0x140000000. Static analysis only (Ghidra via REA + local byte scans). Signatures come from `sig.py`: the number before the pattern is its length in bytes, and `??` is a wildcard. Every signature was unique in `.text` when it was generated, unless a note says otherwise.

Short version:
- **Logging** is easy to use from a mod. `log(level, fmt, FormatArg* args, count)` at 0x1408a98f0 is thread safe. There is also a 4-slot log listener table (hook output without patching code), and a per-thread level filter.
- **There is no console command registry or ini/cvar system in this release build.** The debug init steps are empty stubs.
- **World saves are made on the host only.** The host's server thread builds the save, and the `Save2Thread` writes it. A client only stores the character data that the server sends it. The cleanest mod persistence hooks are the server save state machine (0x140e4d9d0) and the server load state machine (0x140e4a4f0).
- **Host vs client:** the session mode is at `gameApp+0x34FA8` (0 Lobby, 1 Host_Offline, 2 Host_Online, 3 Client_Online). On a client (mode 3) the GameServerCreate and GameServerInit steps do nothing. The ECS world kind byte (`world+0xE15DC8`) tells you which world a system is running in.
- **Impact VM:** this is a small stack VM. The interpreter is 0x14121afb0 (a switch over 30 opcodes). Native "function nodes" are looked up by name hash in a registry. There are 108 registrations at startup through 0x141220f30 (105 with code), and every native has the same signature, so a mod can hook them or add new ones.

---

## 1. Logging

### 1.1 log (main entry)
- **Address** 0x1408a98f0. **Signature** `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 64 24 20 55 41 56 41 57 48 8D AC 24 40 E0 FF`
- **Prototype**: `bool log(uint8 level, const char* fmt, const FormatArg* args, size_t argCount)`.
- **What it does**: Drops the line if `level > min(TLS level, global cap)`. Otherwise it formats into an 8 KB stack buffer, adds a `\n` and calls the dispatcher (0x140898fa0). The return value is `level <= TLS "break level"` (an assert/break hint), which callers can ignore.
- **Levels** (the prefix letter comes from the sink): 0 `X` fatal, 1 `E` error, 2 `W` warning, 3 `I` info, 4 `V` verbose, 5 `D` debug. The real `enshrouded.log` has only I/W/E lines because the default thread level is 3. (`[save2] Save Character committed` is logged at level 4 and never appears.)
- **Confidence**: High.

### 1.2 FormatArg (24 bytes, an array of `argCount` entries)
```
struct FormatArg { uint32 type; uint32 pad; void (*formatFn)(Stream*, FormatSpec*, const void*); const void* value; };
```
Type tags, from the formatter switch at 0x14088cf60:

| type | meaning | `value` points to |
|---|---|---|
| 1 | bool | bool |
| 2 | char (printed by `%c`) | char |
| 3 / 4 / 5 / 6 | uint8 / uint16 / uint32 / uint64 | the integer |
| 7 / 8 / 9 / 10 | sint8 / sint16 / sint32 / sint64 | the integer |
| 0x10 / 0x11 | float / double | the value |
| 0x12, 0x15, 0x16 | pointer-like (printed as a number) | **the value itself** (`%n` uses 0x12 as int*) |
| 0x13, 0x14 | C string, run through strlen | **the `const char*` itself** |
| 0x17-0x1F | uint2/3/4, sint2/3/4, float2... printed as "(%d, %d...)" | the vector |
| 0x20-0x22 | bool2/3/4 | the vector |
| 0x23-0x26 | pairs printed as "(%d:%d)" | the pair |
| 0x27 | **custom**: calls `formatFn(stream, spec, value)` | anything |

- Engine code passes keen strings, error codes and GUIDs as type 0x27 with a type-specific `formatFn`. For example, `%s` with a keen string uses formatFn 0x1400dde20, and `%k` with an error code uses 0x140030160.
- Format specifiers (engine 0x14088e070): `%c %s %d %i %u %x %X %o %b %p %f %g %y %k %n %hs %hc %hm`, the flags `' ' # ' + , - 0 q`, width and precision (`*` is allowed), and `%[N]` for a positional argument. `%k` is the generic "keen" conversion, mostly used with type 0x27 arguments. The `,` flag turns on digit grouping with the default separator `,`. The `'` flag uses `'` as the separator, which is why the log shows numbers like `18'874'368`. The default config separators are `0x2e2c20` (' ', ',', '.').
- **What a mod needs**: to log `"[mymod] %s = %u"`, pass `{0x13, 0, nullptr, "name"}` and `{5, 0, nullptr, &u32value}` with count 2. Use level 3 or lower, or the line is filtered out.
- **Confidence**: High (seen in the switch and in many call sites).

### 1.3 Dispatcher and sink
- **logDispatch** 0x140898fa0 `40 55 41 56 41 57 48 83 EC 30 49 83`. It guards against re-entry (TLS 0x141fc4f08). It then calls every registered listener under a spinlock (0x141fe2fa4) as `cb(level, threadLevel, const StringView* line, void* user)`, and finally calls the thread's trace stream sink. The sink is the TLS stream (0x141fc4f00), or else the global slot array at 0x141fc4300 (128 slots of 0x18 bytes). High.
- **defaultTraceSink** 0x1408a9e00 `48 85 C9 0F 84 ?? ?? ?? ?? 48 8B C4 4C 89 40 18`. It writes `"[L hh:mm:ss,mmm] "` plus the text to the stream, and flushes for levels below 4. High.
- **addLogListener(fn, user)** 0x14087a300 `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B FA 48 8B F1 33 DB`. Table at 0x141fe2f50 (pairs of {fn, user}), count at 0x141fe2f90, **at most 4 entries**. Nothing in the shipped code calls it, so all 4 slots are free. High.
- **removeLogListener(fn, user)** 0x1408a1880 `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 20 48 8B FA 48 8B F1 33 DB`. High.
- **setThreadLogLevel(uint8) -> old** 0x1408a6fe0. It writes TLS index `*(DWORD*)0x141fc4f10`. The global cap is the byte at 0x141fc4f20 (0x05 at start). The trace init (0x140895aa0) sets the init thread to level 3. **Signature note:** `48 89 5C 24 08 57 48 83 EC 20 0F B6 F9 8B 0D` matches two functions: 0x1408a3880 (the break-level setter for TLS 0x141fc4f18) and 0x1408a6fe0. Pick the hit whose `mov ecx,[rip+X]` resolves to 0x141fc4f10. High.
- **setThreadTraceStream** 0x1408a7020 `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 48 8B F2 48` (wrapper 0x1408a7160 `40 53 48 83 EC 30 48 89 54 24 20 48`). High.

### 1.4 Where the log file comes from
- **GameFramework "Debug" init step** 0x1408fbfe0 `40 55 53 41 55 41 57 48 8D AC 24 E8 ?? ?? ?? ?? 81 EC 18 0C`.
  - The log path is the exe path with `.log` instead of `.exe`, which gives `enshrouded.log`.
  - The command line option `-log <path>` overrides the path.
  - The previous log is renamed to `backup/<name>_<timestamp>.log` (`[holistic] Backed up log to '%s'.`).
  - The file stream lives at `framework+0x1EE8` and becomes the global trace stream through 0x1408a7160 (`[GameFramework] Starting to write traces into file '%s'`).
  - High.
- Fatal/assert: `FUN_140033cf0(0, fmt, args)` and then abort `FUN_140895310` (already known).

### 1.5 Mod usage
- **Write lines**: call 0x1408a98f0 directly, as described in 1.2. It is thread safe (spinlocks plus a per-call stack buffer). On a thread the mod creates itself, call `setThreadLogLevel(3)` first. A new thread's TLS level starts at 0, which shows only fatal lines. That is an inference: the engine's own thread wrapper probably sets the level.
- **Capture all engine output** (for a mod console or overlay): `addLogListener(myCb, ctx)`. No hook is needed.
- **More engine detail**: call `setThreadLogLevel(5)` on the main or server thread, or hook it, to get the verbose and debug lines (`[save2] Save Character committed` and others).

## 2. Debug console, DebugUi, DebugMenu, ini variables

- The GameFramework step name table is the leaf function 0x1408faf60 (`83 FA 44 0F 87 ?? ?? ?? ?? 48 63 C2`). It holds 69 step names, among them `IniVariables` (4), `DebugUi` (0x18), `ReadIniVariables` (0x1A), `DebugRenderer` (0x25), `DebugMenu` (0x26) and `DebugTextConsole` (0x27). In the real log each of these steps finishes in 3-4 µs, so they are effectively empty in this release build.
- **No command registry was found.** There is no "help" or "unknown command" text, no cvar names and no `.ini` file name. The only ini-like data is an **unreferenced** embedded text block at 0x141d42db0. It contains `game/DebugUi/Equipment=false`, `game/DebugUi/Teleport=false`, `game/DebugUi/Spawn=false`, `game/waitForPreload=true`, `game/preloadContent=true`, `g38_ecs/GodMode/Health/Player=true`, `game_framework/sleepGameInBackground=false` and `g38_types/DayTime/SetDaytimeTo = 21_hour`. This shows the developer variable format (`module/Group/Name=value`), but nothing reads it in this build. The variable system itself is compiled out.
- Command line switches do exist and are parsed by string compare. Examples: `-log`, `--savegame`, `--play`, `--use-save-beta`, `--disable-vfx`, `--run-unittests`, `--wait-for-debugger`, `--vulkan-device-index`, `--large-pages` (strings at 0x141d46bf8 and 0x141d56c30 onward).
- **For a mod framework**: it has to bring its own console (an ImGui overlay fed by `addLogListener`), its own config files and its own command registry. Engine admin features run through ECS programs instead (`admin_player` 0x1402cae30 reads `PlayerInput` and writes `GameSettings`/`Daytime`/`Health`; see ecs_programs.md).
- **Confidence**: Medium. Absence was shown from strings and step timings, not from reading every step body.

## 3. Save / load

### 3.1 Big picture
- **Save2System** (app object at `gameApp+0x690`) runs its own thread, `Save2Thread`. Its main loop is **0x140dfebb0** (`48 89 4C 24 08 55 53 56 41 54 41 56 41 57 48 8D AC 24 28 FF`). Other threads post a request by setting an atomic flag plus a state byte in the Save2System object. The thread polls these requests:
  - `+0x298`: global user data load and save (KnownPlayers and similar).
  - `+0x10DF18`/`+0x10DF20`: commit a character save received from the server (writer FUN_140dd2a00).
  - `+0x1138D8`: save the local character (FUN_140dffb60, `[save2] Failed to save character`).
  - `+0xB726C` / `+0x27D0`: character and world slot metadata (`Failed to save to slot`, 0x140de3880).
  - **`+0xB7280` with state `+0xB7284`**: world save job. State 1 creates a new savegame GUID (0x140dd3050). **State 2 writes the world** (0x140e030e0). State 3 patches knowledge.
  - `+0x8A4` / `+0x938`: user_settings and machine settings JSON.
  - High (control flow is clear; the meaning of each flag comes from its strings).
- **Container format** ("savedata2"): a file has a header magic, a version, a header size, a container id, a blob count, named blobs and a checksum, and it gets backup copies. The `.ksd` names (`world%k.ksd`, `character%k.ksd`, `global.ksd`, strings at 0x141d88e60) are the **old Steam Cloud file names**. They are only found during `savedata2-migration`, which the real log shows: `Found steam file 'world0.ksd'`. The blob list parse is 0x1410242a0 (`48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 70 45 33`). Medium.
- **Save2System init** (app creation step 0xC "Save2System") is 0x140dd1ae0 (`48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC 90 00 00 00 48 8B 42 10 4C 8D`). The default world name is "My World". Medium.

### 3.2 World save (host only; runs on the GameServer thread)
1. **serverSaveTick** 0x140e4cd10 `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 70 48 8B F9 49 8B F0 49 8B 48`
   - It is called every server tick from 0x140e46070 as `(result, serverSaveSys = instance+0x20, ctx)`.
   - It runs a save when the flag bits at `saveSys+0x70` allow it: bit0 means pending changes, bit1 means enabled, bit3 means error. On failure it logs `[server] Failed to save %k` and retries up to 4 times (`[server] Failed to Save. Retry %k`).
   - High.
2. **serverSaveStateMachine** 0x140e4d9d0 `4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 57 41 54 41 56 48 8D`
   - The state byte is at `saveSys+0x78`.
   - It waits for the previous Save2 world job (0x140e0ba20 `48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 20 8B`, which also sets `save2+0xB7284 = 2`).
   - It then serializes logic data, snapshots the **ECS world** (0x140985470), and adds voxels, water, base zones (`[server] Failed to save 'ServerSaveGame', while getting base zones`), custom strings and per-player knowledge.
   - It hands the result to Save2System (`saveSys+8` = Save2System*).
   - Returns a keen error byte (0 = ok).
   - High.
3. **ecsCreateSavegameSnapshot(alloc, world)** 0x140985470 `48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 A8 48 81 EC 58 01 00 00 4C 8B F1`
   - It serializes the type table (`world+0xE16240`) with FUN_1409e4d30.
   - It then walks the entity chunk/sector table at `world+0x1CD1DE8` (0xB0-byte entries) and serializes each dirty chunk with FUN_1409f3240, or reuses that chunk's cached blob (`+0x98/+0xA0`).
   - Returns {alloc, world+0x928, typesBlob, chunksBlob}.
   - Medium-High.
4. **save2WriteWorld** 0x140e030e0 `48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 F4 FF FF` runs on Save2Thread. It writes the world blobs into the container: entities (`[save2] Failed to save entities`), bases (0x90-byte entries), and metadata blobs with ids from FUN_140dcbf20(kind). High (from the strings); the details are Medium.
- Which ECS data is saved: 37 components have the `savegame_relevant` attribute (for example CurrentTransform, Inventory, InventorySetup, Faction, BuffContainer, Growth, LifeTime, ActiveNpcStateSaved, AnimalSaved). 292 components are `do_not_save`. Per-entity checks give errors like `[save2] Failed to save entity %,k ( template '%k', family '%k' ): Could not find required component 'ecs::CurrentTransform'.` (about 20 small validator functions at 0x140dab9d0-0x140e02b60).

### 3.3 World load (host)
- **serverLoadStateMachine** 0x140e4a4f0 `40 55 56 41 56 41 57 48 8D AC 24 48` ("AdventureDeserializeState", state byte at `param_2+0x90`). It loads in this order: knowledge, bases, terraforming, water, misc data, chunk persistent data (`[server_save] Create Chunk Persistent Data`), weather, snapping boxes, logic blob, entity container (tombstone inventories). The final state 0xE logs `[server] Savegame deserialization took %k`. High.
- It is called from 0x140e3c540 (`48 89 5C 24 18 48 89 7C 24 20 55 48 8B EC 48 83 EC 70 48 8B`). That function is the GameServer instance state 3 ("load savegame") in 0x140e47710. Afterwards state 4 starts the ECS world. High.
- `[ecs%c] Could not deserialize savegame` lives in 0x14098c7a0 (`4C 89 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 57 48`). It has no direct call sites; Ghidra shows only table (RVA) references. It parses a document (it calls a parser at 0x140f7a900) into a world. It may be the JSON/debug savegame path (`--savegame`). Low.
- `serializeToJson` errors: 0x1409e23c0 (called from 0x14049f660). This is a JSON dump of components and is probably debug or tooling only. Low.

### 3.4 Characters
- The client's character lives on the server while playing. At exit the server sends the "final savegame" (the log shows `[save2] Final Savegame received`). The client then commits it with **0x140dc0a30** (`48 89 5C 24 10 57 48 83 EC 40 0F 10`, logs `[save2] Save Character committed` at level 4). That function copies the data into Save2System `+0x10DF48...` and sets the request state to 3. Save2Thread writes the file. The function is called from the client message dispatcher 0x1408695a0. High.

### 3.5 Hooks a mod would use to persist its own data
- **World data (host)**: Post-hook 0x140e4d9d0 and act when it returns 0 and `saveSys+0x78` went back to 0 (finished). Write a sidecar file next to the world save. For load, post-hook 0x140e3c540: a result byte of 0 means the world loaded. Read the sidecar there, before the ECS world starts (instance state 4).
- **Character data (client)**: hook 0x140dc0a30 (the commit of the server-sent character).
- **Per-entity data in the save itself** would mean adding a new `savegame_relevant` component type. That needs reflection and template registration, which are not mapped. Use sidecar files instead.
- **Open questions**: the exact on-disk file names and folder for new-format saves; how to get the world GUID or slot from Save2System (see `ResolveWorldSaveSlot` 0x140dfe280); who sets the "pending changes" bit (autosave timer?).

## 4. Multiplayer: host vs client

### 4.1 Objects and startup
- **App creation steps** 0x140846f00 (`48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C0 F5 FF FF`), log tag `[app]`.
  - Step "ServerThread" calls **createServerHost(alloc, cfg)** 0x140e34060 (`48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 50 48 8B 01`) and stores the result in **`gameApp+0x630`**.
  - The host object is 0x5E0 bytes. It starts the OS thread "GameServer" (1 MB stack, entry 0x140e38fe0).
  - This happens at every startup, even before you pick a game, so the thread always exists.
  - High.
- **Server thread main** 0x140e38fe0 (`40 55 53 57 41 55 41 57 48 8D 6C 24`). It creates the "Main_Server" allocator (128 MB) and the event loop, then loops: FUN_140e44dc0 (network/messages), then 0x140e47710. High.
- **Instance state machine** 0x140e47710 (`48 8B C4 55 41 54 48 8D 68 E8 48 81`).
  - It runs per game instance. The array is at `host+0x440`, the count at `host+0x448`, and each entry is 0x90 bytes with its state byte at `+0x18`.
  - States: 1 create (0x140e43a10) → 2 apply settings (0x140e3a350, `[server] Game Settings '%k'`) → 3 load savegame (0x140e3c540) → 4 start the ECS world → 5 running (0x140e46070 every tick) → 6 shutdown (0x140e435c0) → 7 destroy.
  - High.
- **GameServer instance** fields seen: `+0x20` server save system, `+0x1C8` Save2 world context, **`+0x1D0` the server ECS world** (kind 0), `+0x17B0` start time. Medium-High.
- **serverInstanceTick** 0x140e46070 (`48 89 4C 24 08 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24`). In order it handles player login and spawn (0x140e48dc0), receives messages (0x140e47c20), updates the ECS world (FUN_1409c86a0(world, dt)), runs the save tick (0x140e4cd10) and replication. High.
- **Play creation steps** 0x140849090 (`48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 F7 FF FF`). It switches over the steps 0 Init, 1 LoadingScreenStart, 2 LoadSessionResources, 3 Session, 4 BaseSharingCreate, **5 GameServerCreate**, **6 GameServerInit**, **7 GameClientCreate**, **8 GameClientInit**, 9 LoadingScreenDone, 10 GameClientSpawn. `param_1+0x28` = gameApp.
  - Step 5: if `*(int*)(gameApp+0x34FA8) == 3` it does nothing. Otherwise it calls **startServerGame(host, cfg)** 0x140e43940 (`4C 8B DC 53 48 81 EC 30 01 00 00 0F B7 42 1C 49`), which loads a local slot (`[play] Load local save Slot`) or an adventure.
  - Step 6: also does nothing in mode 3. Otherwise it calls 0x140e3b410 (`48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8D 99 D8 05`).
  - Step 7: allocates the **GameClient (0x6C3760 bytes)**, constructs it with 0x14082cdc0 (`48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 33 C0 48 89 29 48 8B F1 48 89`), stores it at `playState+0x1B8`, and creates it with 0x140838f50 (`48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81`). The create flags are byte0 = (mode==2 Host_Online) and byte1 = (mode==1 Host_Offline).
  - Step 8: loops on 0x140840bf0 (`48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D AC 24 F0 EA FF FF`) until `client+0x466B0C == 12`. The log shows these sub-steps as `[Client] Initialize 'X' done`.
  - Step 10: spawns the player with 0x140862010 (`40 55 57 41 56 48 8D AC 24 D0 FE FF`).
  - High.
- **Correction to the briefing**: a client that joins someone else's game runs no game instance on its own GameServer. The thread exists but steps 5 and 6 are skipped. The log still prints `[play] start creation step GameServerCreate` because the step is logged before the switch. The `[ecss] ComponentTypeSize 0` line on the client probably comes from shared ECS setup, not from a running server world. That part is a guess. Medium-High for the skip; Low for the `[ecss]` explanation.

### 4.2 How a mod can tell if it is host or client
1. **Session mode** `*(int*)(gameApp+0x34FA8)`: 0 Lobby, 1 Host_Offline, 2 Host_Online, 3 Client_Online. The enum order comes from the string table at 0x141d63d50 ("Lobby", "Host_Offline", "Host_Online", "Client_Online"), and it matches the code tests (`==3` skips the server, `==2`/`==1` set the client flags). To get gameApp, capture `*(playState+0x28)` in a hook on 0x140849090, or the `this` of 0x140846f00. No global pointer has been found yet. Medium-High.
2. **Inside ECS code**: `*(uint8*)(world+0xE15DC8)` is 0 for the server (authoritative) world, 1 for prediction and 2 for client. Programs with descriptor flag bit0 run in client worlds; programs without it are server only. High (from the briefing and the `[ecs%c]` code at 0x14098c7a0).
3. **Server host**: `*(void**)(gameApp+0x630)`. When `host+0x448 > 0` and an instance is in state 5, this process hosts a running world. Medium.

### 4.3 Replication path (high level)
- **Server side**: `*_to_network` and `network_*` ECS programs (server world, flag 0) copy state into replicated `Network*` components, for example `actor_to_network` 0x140270be0 and `network_health_attribute` 0x140275bd0. Replication then sends `CreateEntity`/`UpdateEntity`/destroy messages per tick. The server-side message builders were not pinned down.
- **Server message dispatcher** 0x140e47c20 (`40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 98 F9 FF`) handles login (`[server] Invalid login message`), machine and player channels, and client savegame chunks (`[save2] Server received chunk ...`, 0x140de73d0). High.
- **Client side**:
  - **GameClient update** 0x14086e7b0 (`44 88 44 24 18 48 89 54 24 10 55 53 56 57 41 54`).
  - **Player message dispatcher** 0x1408695a0 (`48 8B C4 55 41 57 48 8D A8 E8 F9 FF`) handles channels, the save game and chat.
  - **Store ECS server messages per tick** 0x1409b9990 (`48 85 C9 0F 84 ?? ?? ?? ?? 44 88 44`). It decodes and allocates `CreateEntity` messages (0x140955b20 `44 89 4C 24 20 55 53 56 57 41 54 41 55 41 56 41`) and `UpdateEntity` messages (0x1409561f0 `44 89 4C 24 20 55 53 56 41 55 41 56`). Message storage per tick: `[ecs%c] ServerMessageStoarge is out of memory`.
  - **applyServerMessages(world)** 0x1409cf8e0 (`40 55 41 54 48 83 EC 48 44 8B A1 9C`). It walks the per-tick buckets (`world+0x1CF3BD8`, count `+0x1CF3BE0`) up to the current tick `world+0xE1629C`. It calls **processMessage** 0x1409ce210 (`4C 8B DC 49 89 4B 08 55 56 57 41 54 41 55 41 57`, CreateEntity/UpdateEntity into the client world) and 0x1409cf0f0 (`4C 8B DC 55 53 56 57 41 55 41 56 49`, UpdateEntity into the prediction world at `world+0x1CF3AC0`). Log strings: `processMessage UpdateEntity: Entity %,u not found`, `Entity state clash`.
  - Statistics at shutdown: `[ecsc] Create/Destroy Replication Message Order Statistics` (0x140996940).
  - High.
- **Client world programs**: 196 of the 816 programs have descriptor flag bit0 set (`client_*`, `*_network_to_client`, and others). See ecs_programs.md.
- **For mods**: gameplay changes must run on the host, in the server world or in server-only programs. A client-only mod can change presentation (client world programs) and must treat replicated `Network*` components as read-only. A custom mod-to-mod network channel was not found. A mod would probably have to piggyback on chat or custom player strings (`CustomPlayerStringServerMessage`, `ChatMessageServerMessage`). Open question.

## 5. Impact VM (gameplay scripting)

### 5.1 Data
- `keen::ImpactProgram` (64 B, blob resource): `+0 ProgramId id`, `+4 Guid programGuid`, `+0x14 u16 stackSize`, `+0x16 EventStreamMask usedStreams`, `+0x18 BlobArray<ImpactCommand> code` (an ImpactCommand is a **u32** word), `+0x20 codeShutdown`, `+0x28 BlobArray<ImpactVariable> dataLayout` (24 B each: name hash, configId, type hash, u16 size, **u16 offsetInBytes**, dbgName), `+0x30 BlobArray<u8> data` (initial variable values), `+0x38 BlobArray<u32> queryCallIds`. Each BlobArray is a self-relative s32 offset plus a u32 count.
- The 149 editor node types (`keen::impact_nodes::*`) are compiled into this bytecode. Control and value nodes become plain opcodes. "Function" nodes become `CALL_NATIVE nameHash`, where the hash is the reflected `nameHash` of the node type (for example SpawnEntity is 0x61b11b03).
- The ECS component `ecs::Impact` (24 B, server_only, do_not_save) holds `+0 ImpactProgramReference program`, `+0x10 ProgramId programId`, `+0x14 ImpactId impactId` (instance handle). Related components: `ImpactInitialized` (qh 0xadf19f25), `ImpactHalted` (qh 0xe3b96393), `ImpactHierarchy`, `ImpactConfiguration`.

### 5.2 How a program starts and runs (server world only)
- **impact_initialize** program update 0x1403eed40 (`40 55 41 54 48 8D AC 24 B8 FE FF FF 48 81 EC 48 02 00 00 41 B8 F8 00 00`), flag 0x200. For each new `Impact` component:
  1. Resolve the program id (FUN_14021f6f0).
  2. Look up the program with **getProgram(registry, id)** 0x14121d5a0 (`48 89 5C 24 08 48 89 7C 24 10 83 B9 BC 00 00 00`).
  3. Create an instance with **createImpactInstance(impactSystem, programId, ownerEntity)** 0x14121e4a0 (`40 53 57 41 54 41 55 41 56 41 57 48`) and store the result in `Impact.impactId`.
  4. Add `ImpactInitialized` and apply the config values (item, buff, skill, sequence configs).
  - **To start a program on an entity, add an `ecs::Impact` component with a program reference.** The engine does the rest on the next tick.
  - High.
- **impact_simulation** program update 0x1403ef7a0 (`48 89 7C 24 20 55 48 8D AC 24 30 FC`), flag 0x1200. For each entity it checks the instance state (FUN_14121f080). If the instance is running it calls **runImpact(impactSystem, impactId, iterCtx)** 0x1412261e0 (`48 89 5C 24 08 57 48 83 EC 40 49 8B F8 4C 8B D1`). When the instance is finished it runs shutdown (0x141222750), destroys the instance (0x141223080) and clears the id. High.
- **runImpact** looks up the instance (0x14121d8e0 `4C 8B C9 85 D2 75 03 33 C0 C3 48 8B`) and the program, then calls the interpreter on `program.code`. If the instance flags say "done", it adds `ImpactHalted`. High.

### 5.3 Interpreter
- **impactInterpret(instance, Slice<u32>* code, ImpactProgram*, EcsIterCtx*)** 0x14121afb0 `48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 55 41 54 41 56`.
- **Instance layout** (~0x1F4 B): `+0 id`, `+4 programId`, `+8 owner entity`, `+0x18` data (copy of program.data), `+0x30` u32 stack (0x80 slots, the last slot is the canary 0xBAADF00D, `Impact stack overflow in program %k`), `+0x1EC u16 pc`, `+0x1EE u16 sp`, `+0x1F0 u16 frame`, `+0x1F2 u8 flags` (0x08 finished, 0x10 error, 0x40 halted, 0x02 ran this tick), `+0x1F3` usedStreams.
- **Opcodes** (`switch(code[pc])`):

| op | meaning |
|---|---|
| 1-4 | add, sub, mul, div (int, pop 2 / push 1) |
| 5 / 6 / 7 | less-than, equal, less-or-equal |
| 8 | jmp imm |
| 9 / 0xA | pop and jump if non-zero / zero |
| 0xB | push imm |
| 0xC / 0xD | push 0 / push 1 |
| 0xE / 0xF | increment / decrement top |
| 0x10 | query or stream related (looks up the instance query table) |
| 0x11 | dup |
| 0x12 | call subroutine (push frame) |
| 0x13 | **call native** (operands: ?, nameHash) |
| 0x14 | return |
| 0x15 | load frame-local |
| 0x16 | load typed variable onto the stack ("Can not laod Type") |
| 0x17 / 0x18 | store, typed global store ("Can not gstore Type") |
| 0x19 | push current time (ms) |
| 0x1A | float seconds to ms |
| 0x1B | pop |
| 0x1C | **yield** (resume next tick) |
| 0x1D | halt (flag 0x40) |
| 0x1E | end (flag 0x08) |
| other | error (flag 0x10) |

- **CALL_NATIVE**: looks up the nameHash in the native registry (`ecsCtx+0x110`, a hash map with 0x50-byte entries: `+0 fn`, `+8` argument-name slice, `+0x48 u8 argCount`), pops `argCount` u32 argument handles (at most 31, `'%s' has to many Arguments`) and calls:
  ```
  ImpactResult* native(ImpactResult* out, Slice<u32>* args, ImpactNativeCtx* ctx);
  // out: +0 u8 error (0 ok, 1-3 = yield/suspend, other = error), +8 value pushed on success
  // args[i] & 0xFFFF = index into ctx->layout; the value address is ctx->data + layout[i].offsetInBytes
  // ctx: +0 ImpactVariable* layout, +8 layoutCount, +0x10 u8* data, +0x18 dataSize, +0x30 EcsIterCtx* (the ECS program context)
  ```
  If `fn == null` it pushes 1. The three debugDraw natives are registered with null in release.
- **Confidence**: High (taken straight from the decompiled loop).

### 5.4 Native registry and the interesting nodes
- **registerImpactNative(registry, nameHash, fn, const ArgDesc* {const char* name, u32 ?, u32 ?}, u8 argCount)** 0x141220f30 `48 89 5C 24 18 48 89 74 24 20 89 54 24 10 55 57 41 56 48 8B`. All 108 registrations happen in **0x1404949b0**. 105 have code, 3 debug natives are null, and changeAttributeValue is registered twice. The function (`48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 E3 FF`), which is the big "create game systems" function. It also calls the 59-program ECS registration 0x14122c1c0 and is called from 0x140e3a350, the server instance setup. The registry is at `systems+0x3E8`. High.
- **A mod can add natives** by calling 0x141220f30 after 0x1404949b0 runs. They only fire if some program's bytecode calls that hash, which needs edited content. **Hooking existing natives** is the practical lever.

| Node | nameHash | Native | Notes |
|---|---|---|---|
| SpawnEntity | 0x61b11b03 | **0x1403e3960** `4C 89 44 24 18 55 53 56 57 41 54 41 55 48 8D 6C` | Reads the node inputs (template, transform/position, velocity, parent, level and faction overrides), then calls the spawn helper **0x1403e3e40** (`48 89 5C 24 08 55 56 57 41 56 41 57 48 8D AC 24 20 F3 FF FF`), which returns the new EntityId into the `entityId` output. High (signature), Medium (helper role). |
| DestroyEntity | 0xc691c4da | **0x1403d98f0** `48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8B 02 48 8B` | arg0 = EntityId. `self = FUN_1409b2e40(*ctx->ecs)`, then `FUN_140954250(FUN_1409b0860(*ctx->ecs), self, target)` queues the destroy. Returns bool. High. |
| ApplyDamage | 0xb04d0e7a | **0x1403d4d50** `48 8B C4 4C 89 40 18 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 08 F9` | Large. It computes the hit and queues a pending hit; see combat.md. High. |
| SetTransform | 0x236b6555 | **0x1403e3860** `48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 02 48 8B F9 49 8B` | Validates the WorldTransform (error 0x16), gets the self entity's transform component through FUN_1402569e0(world, ctx->ecs+0x230, entity) (error 0x1F if missing) and copies 56 bytes. High. |
| GetTransform | 0xa18f5959 | 0x1403dfc40 `40 56 41 54 41 55 41 56 41 57 48 83 EC 30 4C 8B` | High (by registration). |
| ApplyBuff | 0x91f30b26 | 0x1403d4920 `4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC` | High (by registration). |

The full native table (name, native function, nameHash), taken from the registration calls in 0x1404949b0:
startQuery 0x1403e58d0 (0x59edd145); stopQuery 0x1403e5ae0; getTransform 0x1403dfc40; getBuffTarget 0x1403de410; getSelf 0x1403df5f0; getParent 0x1403df450; getEntityParent 0x1403de9c0; getRoot 0x1403df580; getHitTarget 0x1403ded00; getLockedTarget 0x1403df280; setHookTarget 0x1403e3630; getQueryResultCount 0x1403df4c0; fetchQueryResult 0x1403da5e0; getOnCollisionEventCount 0x1403df3e0; fetchOnCollisionEvent 0x1403da010; isValidEntity 0x1403e0e30; isSameEntity 0x1403e0bc0; isEnemy 0x1403e0930; isFriend 0x1403e0a00; isDaytime 0x1403e0900; isSprinting 0x1403e0d20; overrideTemplateBuildingFlyMode 0x1403e1f70; getWeaponBaseDamage 0x1403dfe30; createDamageSetup 0x1403d9710; applyDamage 0x1403d4d50; setAttribute 0x1403e2bc0; applyAttributeValue 0x1403d45a0; changeAttributeValue 0x1403d8c80; revertApplyAttributeValue 0x1403e2580; fetchAttribute 0x1403d9b70; fetchOptionalAttribute 0x1403da4f0; add/sub/mul/div 0x1403e8190; clamp 0x1403d8f30; equal/less 0x1403d9240; isInState 0x1403e0ad0; hasFaction 0x1403e05d0; hasAttackCategory 0x1403e01f0; hasWeaponCategory 0x1403e0710; hasWeaponCategoryType 0x1403e07d0; hasDamageType 0x1403e0410; consumeItem 0x1403d9530; applyOffset 0x1403d7f00; spawnEntity 0x1403e3960; applyBuff 0x1403d4920; hasBuff 0x1403e02e0; removeBuff 0x1403e23d0; randomBranchNode 0x1403e21f0; randomValueNode 0x1403e2340; randomIndex 0x1403e22a0; triggerNoise 0x1403e7480; pacifyFaction 0x1403e20e0; activateCallFactionForHelp 0x1403d8b70; forEachHitEvent 0x1403dc6b0; SendBaseHitEvent 0x1403e2900; SendRevivedPlayerEvent 0x1403e2ad0; itemBasedMaterialReaction 0x1403e0e80; forEachParryEvent 0x1403dd930; terraforming 0x1403e5b70; addWater 0x1403d3b10; getCursorTransform 0x1403de710; clearConsecutiveHits 0x1403d91d0; isConsecutiveHit 0x1403e0890; GetConsecutiveHitCount 0x1403de460; GetEnemyCountInRange 0x1403de830; IsSkillUnlocked 0x1403e0c30; SetTransform 0x1403e3860; Normalize 0x1403e1e30; SetLength 0x1403e3720; GetLength 0x1403dee70; GetSlotTransform 0x1403df650; SetEnemyVariable 0x1403e3300; onEvent 0x1403e1ee0; entityInRangeWithFaction 0x1403d9a70; forEachEntityInStream 0x1403dc2f0; forEachHitEventInStream 0x1403dd020; resetEntityStream 0x1403e24d0; isStreamEmpty 0x1403e0dd0; forEachCollisionInStream 0x1403dbd20; filterRandom 0x1403db250; filterFriends 0x1403dae10; filterSameRoot 0x1403db630; filterRootEntity 0x1403db440; filterFaction 0x1403dac50; filterIsInState 0x1403dafd0; filterConsecutiveHits 0x1403da8d0; filterEntityTag 0x1403daa30; getStreamEventCount 0x1403df960; forEachEntityWithFactionInRange 0x1403dc4b0; destroyFactionQuery 0x1403d9990; forEachPlayerInRange 0x1403ddbb0; destroyPlayerQuery 0x1403d9a00; getWeaponVisualEntityId 0x1403dff70; hasEntityTag 0x1403e0500; triggerKnowledge 0x1403e7370; getCurseModifier 0x1403de6b0; destroyEntity 0x1403d98f0; getWaterChargeSize 0x1403dfdd0; fromTransform 0x1403ddd90; addSkillLevel 0x1403d3320; revertaddSkillLevel 0x1403e2830; debugDraw / debugDrawCross / debugDrawArrow = null.
(The appendix lists every entry with its hash.)

### 5.5 Related ECS programs (all run in the server world)
`impact_initialize` 0x1403eed40, `impact_simulation` 0x1403ef7a0, `impact_prepare` 0x1403ef6e0, `impact_system_prepare` 0x1403efa30 (sends HitEvent), `impact_hit_conclusion` 0x1403ed8f0, `impact_cleanup` 0x1403ed7d0, `impact_hierarchy_init` 0x1403ed800, `server_update_impact_system` 0x1403f0340, `clear_hit_candidates` 0x1403ed770, `clear_resource_candidates` 0x1403ed7a0, `resource_conclusion` 0x1403efac0, `equipment_impacts` 0x140381ee0, `handle_skill_impacts` 0x1402d1a00, `load_buff` 0x14023cbd0.

### 5.6 Open questions (Impact)
- The exact operand layout of opcodes 0x10, 0x16, 0x17 and 0x18 (typed loads and stores by variable type hash) and of the first CALL_NATIVE operand.
- Where `ImpactSystem` and the native registry pointer live globally. They are reached through the ECS iteration context (`+0x110`) and the systems object (`+0x3E8`).
- The `impact::ProgramId` to resource mapping (FUN_14021f6f0) and how to build a new ImpactProgram blob at runtime.

## 6. Hook and call cheat sheet for the framework
| Purpose | Address | Kind |
|---|---|---|
| Write a log line | 0x1408a98f0 | call |
| Capture all log output | 0x14087a300 (add listener) | call |
| Raise or lower the thread log level | 0x1408a6fe0 | call |
| App creation steps (catch gameApp = `this`) | 0x140846f00 | hook |
| Play steps (gameApp = `*(p+0x28)`, GameClient = `*(p+0x1B8)`) | 0x140849090 | hook |
| Server thread created (host object) | 0x140e34060 (return value) | hook |
| Server instance tick (server world = `inst+0x1D0`) | 0x140e46070 | hook |
| World save produced (host) | 0x140e4d9d0 | post-hook |
| World loaded (host) | 0x140e3c540 | post-hook |
| Save2 thread jobs | 0x140dfebb0 | reference |
| Client receives final character save | 0x140dc0a30 | hook |
| Client applies replication | 0x1409cf8e0 | hook |
| Start Impact program | add `ecs::Impact` component | data |
| Impact interpreter | 0x14121afb0 | hook (tracing) |
| Impact natives (SpawnEntity, DestroyEntity, ApplyDamage, SetTransform) | 0x1403e3960 / 0x1403d98f0 / 0x1403d4d50 / 0x1403e3860 | hook |
| Register a new Impact native | 0x141220f30 | call |


## Appendix: Impact native registry (nameHash, native, registered name)
```
0x59edd145  0x1403e58d0  startQuery
0xe27c52c3  0x1403e5ae0  stopQuery
0xa18f5959  0x1403dfc40  getTransform
0xc24cb9bb  0x1403de410  getBuffTarget
0x01d09759  0x1403df5f0  getSelf
0xb8329a37  0x1403df450  getParent
0xaf655170  0x1403de9c0  getEntityParent
0xbf5ad13b  0x1403df580  getRoot
0x0d3c48b1  0x1403ded00  getHitTarget
0x1f5adf94  0x1403df280  getLockedTarget
0x265d26d5  0x1403e3630  setHookTarget
0xf39c6af3  0x1403df4c0  getQueryResultCount
0x1b467fe0  0x1403da5e0  fetchQueryResult
0x8469973d  0x1403df3e0  getOnCollisionEventCount
0xda5e432a  0x1403da010  fetchOnCollisionEvent
0xcf3810a6  0x1403e0e30  isValidEntity
0x530b5054  0x1403e0bc0  isSameEntity
0x79b5a97b  0x1403e0930  isEnemy
0xac31b17d  0x1403e0a00  isFriend
0x52de54cc  0x1403e0900  isDaytime
0xdb41ca5b  0x1403e0d20  isSprinting
0x1c9ae6cf  0x1403e1f70  overrideTemplateBuildingFlyMode
0x194ae077  0x1403dfe30  getWeaponBaseDamage
0x947a6bd5  0x1403d9710  createDamageSetup
0xb04d0e7a  0x1403d4d50  applyDamage
0x93853d29  0x1403e2bc0  setAttribute
0x798388df  0x1403d45a0  applyAttributeValue
0x044e4187  0x1403d8c80  changeAttributeValue
0xe41652a7  0x1403e2580  revertApplyAttributeValue
0x044e4187  0x1403d8c80  changeAttributeValue
0xdb35d7c9  0x1403d9b70  fetchAttribute
0xa385dcb9  0x1403da4f0  fetchOptionalAttribute
0x53f7fdba  0x1403e8190  add
0xd367cdeb  0x1403e8190  sub
0xc19163a7  0x1403e8190  mul
0x67eed8fe  0x1403e8190  div
0x7f11f072  0x1403d8f30  clamp
0x0294c6df  0x1403d9240  equal
0x69190348  0x1403d9240  less
0x44396293  0x1403e0ad0  isInState
0x695e7565  0x1403e05d0  hasFaction
0x2d3e3e5f  0x1403e01f0  hasAttackCategory
0x42c15437  0x1403e0710  hasWeaponCategory
0xb67b054f  0x1403e07d0  hasWeaponCategoryType
0x84891ee2  0x1403e0410  hasDamageType
0xa0209dda  0x1403d9530  consumeItem
0x90cf9818  0x1403d7f00  applyOffset
0x61b11b03  0x1403e3960  spawnEntity
0x91f30b26  0x1403d4920  applyBuff
0x9866d55a  0x1403e02e0  hasBuff
0xca687904  0x1403e23d0  removeBuff
0x9dcb0a66  0x1403e21f0  randomBranchNode
0x92bf9f5b  0x1403e2340  randomValueNode
0xf976c136  0x1403e22a0  randomIndex
0xd471249f  0x1403e7480  triggerNoise
0x39ee6f5d  0x1403e20e0  pacifyFaction
0x283aba15  0x1403d8b70  activateCallFactionForHelp
0x9f34f4ea  0x1403dc6b0  forEachHitEvent
0x6b4da871  0x1403e2900  SendBaseHitEvent
0xbe0e869d  0x1403e2ad0  SendRevivedPlayerEvent
0xad415283  0x1403e0e80  itemBasedMaterialReaction
0xc5453cfb  0x1403dd930  forEachParryEvent
0x9aabd181  0x1403e5b70  terraforming
0xbb6e8623  0x1403d3b10  addWater
0x79fb429b  0x1403de710  getCursorTransform
0x5fdbd8ee  0x1403d91d0  clearConsecutiveHits
0x7fca1b56  0x1403e0890  isConsecutiveHit
0xe56d6397  0x1403de460  GetConsecutiveHitCount
0xe8912af0  0x1403de830  GetEnemyCountInRange
0x4b131213  0x1403e0c30  IsSkillUnlocked
0x236b6555  0x1403e3860  SetTransform
0x4adde30c  0x1403e1e30  Normalize
0xa5ff238b  0x1403e3720  SetLength
0xd8e52c57  0x1403dee70  GetLength
0x3e1c0b5d  0x1403df650  GetSlotTransform
0x31fb9991  0x1403e3300  SetEnemyVariable
0x45dbf538  0x1403e1ee0  onEvent
0x61946152  0x1403d9a70  entityInRangeWithFaction
0xbe4ece89  0x1403dc2f0  forEachEntityInStream
0x72fdd0f5  0x1403dd020  forEachHitEventInStream
0x640f8053  0x1403e24d0  resetEntityStream
0xec2558de  0x1403e0dd0  isStreamEmpty
0xd0a532b0  0x1403dbd20  forEachCollisionInStream
0xa5e145f8  0x1403db250  filterRandom
0xd6bb2b78  0x1403dae10  filterFriends
0xf52a5619  0x1403db630  filterSameRoot
0xdb4bf324  0x1403db440  filterRootEntity
0xf56c44f1  0x1403dac50  filterFaction
0x55d4c71d  0x1403dafd0  filterIsInState
0x6e453d91  0x1403da8d0  filterConsecutiveHits
0xbd6aa1d0  0x1403daa30  filterEntityTag
0x9870a3b4  0x1403df960  getStreamEventCount
0x19143416  0x1403dc4b0  forEachEntityWithFactionInRange
0x93e40aa9  0x1403d9990  destroyFactionQuery
0xa8787e70  0x1403ddbb0  forEachPlayerInRange
0x22a62ea4  0x1403d9a00  destroyPlayerQuery
0x78244a69  0x1403dff70  getWeaponVisualEntityId
0x0afc5954  0x1403e0500  hasEntityTag
0x0f150d49  0x1403e7370  triggerKnowledge
0xd2d5826e  0x1403de6b0  getCurseModifier
0xc691c4da  0x1403d98f0  destroyEntity
0x63b0d4c7  0x1403dfdd0  getWaterChargeSize
0x866dfbeb  0x1403ddd90  fromTransform
0xee3b6ad1  0x1403d3320  addSkillLevel
0x1bed77df  0x1403e2830  revertaddSkillLevel
0x9127757a  null         debugDraw
0xe7397efa  null         debugDrawCross
0xe8573fe7  null         debugDrawArrow
```
