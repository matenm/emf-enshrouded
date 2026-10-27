# Enshrouded engine map (build 1269579, release_1.0; build 1273043 differences in "Game update" below)

This map covers the engine pieces a general modding framework needs (BepInEx + Jotunn style), not just NPC mods.

- Binary: `enshrouded.exe`, sha256 `82e5020a8736348cd0374f004b2f5e5ef456b5bd27aa5420df34c7dd241cfeeb`, image base `0x140000000`.
- Analysis: REA + Ghidra 12.1.4 (static). What was checked in the running game is in the "Confirmed in game" sections.
- Addresses are virtual addresses at the default image base. At runtime: `real = moduleBase + (addr - 0x140000000)`.
- Signatures are hex with `??` wildcards. Each one was checked for a unique match in `.text` unless marked otherwise.
- Type layouts come from EML `.cache/types.json` (version `1269579|^/game38/branches/release_1.0`).

Confidence levels:
- **High**: seen directly in code (string reference, decompiled logic, matching struct offsets).
- **Medium**: strong inference from several clues.
- **Low**: a guess that still needs checking.

---

## Deep dives
- `engine_parts/re_companions.md`: the guard system (StartGuarding interaction → `enemy01` guards `guardEntityId`), companion candidates, and the in-game result (a spawned companion follows the player).
- [`engine_parts/re_events.md`](engine_parts/re_events.md): how programs send (`emitEvent` `0x140955400`) and read (paged queues) engine events.
- [`engine_parts/re_time_spawn_weather.md`](engine_parts/re_time_spawn_weather.md): ECS time outside programs (`ecsNow = *(i64*)(world+0xE16280) * *(u32*)(world+0xE1629C)`, 60 Hz), how the `SpawnEntity` native spawns with a transform (queued, created in the sim-step flush), weather layer reset.
- [`engine_parts/re_custom_programs.md`](engine_parts/re_custom_programs.md): mods can very likely register their own ECS programs (hook `0x1404a5a30`, register before the first tick, names resolved by FNV-1a, `+0x184 = 1`, flag rules).
- [`engine_parts/re_saves.md`](engine_parts/re_saves.md): save file names, backends, slots, autosave, sidecar hook points; the menu's slot records (build 1273043: a character's level at record +0x2F0, a world's last-played time at +0x48 and play time at +0xB8; no quest progress).
- [`engine_parts/re_attributes_menu.md`](engine_parts/re_attributes_menu.md) (build 1273043): attributes by id through the registry (`attrFind`, current/max health), `attrAdd`, damage with kill credit through a pending hit (`HitCandidate`), starting a LOCAL save offline from the main menu through the UI command queue, the world's display name.
- [`engine_parts/re_teleport.md`](engine_parts/re_teleport.md) (build 1273043): fast travel, teleport zones and respawn all add a `Teleport` component through the running program's command buffer (`QueuePush` `0x140953670`, applied at the end of SimStep); program `teleport` #701 then moves the entity and bumps the replicated `TeleportCount`. A design for C API G5, and the in-game test that confirmed it (the player in a hosted game and on the dedicated server, villagers, an enemy).
- [`engine_parts/re_take_items.md`](engine_parts/re_take_items.md): removing items through the inventory transaction (`removeItems` `0x14040a400` on 1273043), moving a stack with its pide (`moveStack`), the player's level (`Level.dataStorage[0]`), an item's level/rarity (`ItemState`) and slot type (`ItemInfo+0x104`). Checked in game: a take from EMF's own program removes the pide; a put with level/rarity recreates the item (the game caps the level).
- [`engine_parts/re_villages.md`](engine_parts/re_villages.md): farming (crop roots, ripe = last Upgrade level, harvest with a bypass `LootInteractionEvent` into a chest, re-arming the root keeps the plant), farm animals (feeders take `put_items`, products collected with the same loot event), traders are NPC workshops, villages/beds/happiness; static map plus in-game test results.
- [`engine_parts/re_companion_orders.md`](engine_parts/re_companion_orders.md): companion orders through game events (attack: `AddThreatTargetEvent`; stay: guard a static entity; come back: guard the player) and its strength through `Level` (level 10: health 119 -> 417, hits 48 -> 156).
- [`engine_parts/re_villager_jobs.md`](engine_parts/re_villager_jobs.md): real crafting with a chest as the operator (`CraftRecipeEvent`), a station's recipes (`RecipeRegistry` + 0xF8), moving villagers (`NpcCommandEvent`), what the player looks at (`PlayerInput.fromClient.camera`).
- [`engine_parts/re_inventory_api.md`](engine_parts/re_inventory_api.md): the inventory transaction API (setup block, `addItems`, commit) and a simpler "spawn a pickup" route for giving items.

## Confirmed in game: the probe DLL (a local test world)
Seen with the probe DLL (`src/probe`):
- The `dinput8.dll` proxy loads; all 18 probe signatures resolve at runtime (exe relocated by ASLR to `0x7FF6D5610000`). Hooks on the sim step, `registerSingleton` and villager `SetState` install and the game runs normally (main menu, character creation, world load, walking).
- **Server world** (kind 0) ticks on its own thread at **60 Hz** (~121 ticks per 2 s), 69 singletons, ~22,500 entities in the starting area.
- **Client world** (kind 2) ticks on a different thread, 75 singletons; its tick stays 0 while loading, then follows the server tick.
- World objects and singleton objects live in Keen's `GF_App` block (`0x300'xxxx'xxxx`).
- **`LocalPlayerData+0x04` = local player EntityId** (1 in a single-player session), and `getComponent(clientWorld, CurrentTransform, id)` returns the live position: 32.32 fixed point → meters, changes as the player walks.
- Singletons such as `g38_daytime::Daytime`, `WeatherSystem` exist separately in the server and the client world.
- Synthetic keyboard input via `SendInput` scan codes is accepted by the game (useful for automated tests).
- Villager states and split components: see the next section.

## Confirmed in game: EMF and the sample mod (a local test world)
Seen with `emf_core.dll` + `sample_mod.dll`:
- Loader → core → mod chain works; all framework signatures (22, and 23 with MarkWritten) resolve at runtime.
- **Program hooks:** hooking a program's update function (captured from `registerProgram` by name) works. `client_player_state` ran ~135 times per second.
- **Custom programs:** a system program (flag `0x4`, no entities) registered from a post-hook on `registerAllPrograms` (`0x1404a5a30`) is scheduled by the game and runs once per server tick (60 Hz).
- **Time of day:** calling `setTimeOfDay` with `ecsNow = tickPeriod (world+0xE16280) × tick (world+0xE1629C)` moved the clock 23.36 h → 2.36 h, and the change persisted.
- **Spawn / destroy:** the spawn wrapper `0x1409788d0` with a template GUID plus a `CurrentTransform` spawn component created a barrel (`Prop_Deco_Steppeprops_Barrel_01`, entity 6808) 2 m from the player, visible in game. `destroyEntity` removed it. The wrapper returns createEntity's result: the new EntityId (6811 in another test, found exactly at the spawn point).
- **Spawned entities are saved.** After an autosave and a reload in the same game session, both spawned villagers came back, with new ids (loaded entities are numbered from 1025). `loadSavegameStep` fires again for the second load.
- **GUIDs** are stored in Windows layout (first three groups little-endian): EML text `a56d2777-daf1-4164-90d4-b7b12749c723` = bytes `77 27 6d a5 f1 da 64 41 …` at `0x14150ccb0`.
- **Entity ids are shared** between the server and client worlds on the host (the local player id 1 is valid in both).
- **Save events:** `save2WriteWorld` (`0x140e030e0`) runs on the save thread on every 30 s autosave, result byte 0, slot `0x0001` (local slot 0). `loadSavegameStep` (`0x140e3c540`) returns result 0 once when the world finishes loading, slot at `inst+0x1754`.
- **Split components (answers open question 1):** `getComponent` with the `ActiveNpcState` hash (`0x1a8b5909`) returns nothing. The per-entity half is stored under its **own** hash: `DynamicActiveNpcState` `0x9e65507a`, 136 bytes. Read it as `get<DynamicActiveNpcState>(id)`.
- **Villager state live:** entity 1025 reported `state` 7 → 3 → 7, `previousState` 5/7, `decision` 3 → 2 over a minute.
- **Villager command works.** Replaying the engine's GoToPosition handler (`0x14044f040`, type 1) from a hook on the per-villager worker `0x1404552c0` made the nearest villager walk to the player three times out of three: it arrived within 5-11 s (8-14 m), stopped about 2 m away in WaitingCloseToPlayer (13), then walked back toward its home area within the next 10 s (normal roaming around its spawn point; the log shows walking, not a teleport). Targets beyond the leash radius max(targetRadius, bedRadius)+1 should be teleported home instead (Medium, not observed). The current entity id inside the worker comes from `0x1409b2e40(*(void**)ctx, &out)`.
- **Villagers are unique per template.** Spawning `test_Townsfolk_Male_01` while that villager already existed (entity 1025, spawned in an earlier game session) gave an idle duplicate with **no** `DynamicActiveNpcState`. Spawning a townsfolk that did not exist yet (`test_Townsfolk_Male_02`, entity 6815) gave a working villager at once: WaitingCloseToPlayer (13) → Idle (3) → Roam (7) within 20 s. The programs `unique_npc_init` / `unique_npc_spawns` key villagers by an FNV-1a hash of their GUID (see `engine_parts/ai.md`).

## Confirmed in game: the in-game console (main menu and a local test world)
Seen with the drop-down console:
- **Keyboard input:** the game window (class `localWindowClass`) gets the keyboard as **raw input** (`WM_INPUT`) **and** as legacy `WM_KEYDOWN`/`WM_CHAR`. `SendInput` Unicode characters arrive only as `WM_CHAR` (no usable raw-input character), so the console takes characters from `WM_CHAR` when the window gets key messages. A subclassed WndProc that swallows messages while the console is open keeps keys and clicks from the game; a key release always passes, so nothing stays held after closing.
- **Mods' hotkeys** (`GetAsyncKeyState` from a mod DLL, e.g. the companion's F6) are blocked while the console is open by hooking `GetAsyncKeyState`/`GetKeyState` and checking the caller's address against the mods' image ranges; the game's own calls are untouched.
- **Vulkan:** the game loads `vulkan-1.dll` itself after start (system loader 1.4.341), creates one instance and one device with `VK_KHR_swapchain`, and presents from queue family 0 (graphics). At the default window size the swapchain is 2858x1481 with 3 images, created first as format 37 (`R8G8B8A8_UNORM`), then recreated as format 64 (`A2B10G10R10_UNORM_PACK32`). Maximizing gives 3840x2054; minimize/restore recreates it. `vkDestroyDevice` runs on exit.
- **Other overlays in the process:** RTSS (`RTSSHooks64.dll`), the Steam overlay (layer + "Shift+Tab" notice), the Discord overlay and the fossilize layer. **RTSS restores `vulkan-1.dll`'s code from disk and re-hooks its exports after start**: MinHook detours EMF had put on `vkCreateDevice`/`vkGetDeviceProcAddr` were wiped, and its relays on the present/swapchain functions were rewritten (sometimes RTSS still called EMF, sometimes not). Inline hooks on `vulkan-1.dll` are therefore unreliable here.
- **A Vulkan layer works where the hooks did not:** with `VK_ADD_LAYER_PATH=<game>\emf\vk` and `VK_INSTANCE_LAYERS=VK_LAYER_EMF_console` set by the `dinput8.dll` proxy in `DllMain`, the Vulkan loader loaded `emf_core.dll` as a layer; the game's device, both swapchains and every present came through it from the first run. The console draws on top of the RTSS OSD (RTSS draws before it calls present); the Discord overlay's popups draw on top of the console.
- **Commands from the console:** in the local world `pos`, `time 12` (0.05 h → 12.00 h), `give Material_T1_Wood_Logs 2` ran and replied in the console; on the main menu `help` answered at once and `time 12` answered `not run: load a world you host first`. Holding W with the console open left the position unchanged; F6 did nothing while open and summoned/dismissed the companion once closed.
- **Frame time** with the console closed: the main menu stays at the 240 fps cap (3.6 ms, RTSS OSD).
- Not used: loading `vulkan-1.dll` from EMF before the game did was followed by game crashes in `vulkan-1.dll` (+0xdce55, `0xc0000409`) in three tries; the cause was not found.
- **Keys and the mouse with the console open (build 1273043):** closing the console with Esc in the world does not open the pause menu; ten repeated make codes of the console key (auto-repeat) toggle it once and type nothing; Shift+Tab opens the Steam overlay while the console is open and closes it again; moving the mouse with the console open leaves the camera where it was (it turns with the console closed).
- **Text chat is off in an offline session:** the game binds "Open Text Chat" to Enter, but Enter opens nothing in the offline local world (matches the session's `isTextChatEnabled` gate in `docs/engine_parts/re_chat.md`). Chat commands therefore need an online session to test; they were checked on the dedicated server (`docs/engine_parts/re_chat.md`).

## Game update: build 1273043 (a known build)
Build 1273043 came as a Steam update (`enshrouded.exe` sha256 `d64966e23b1a458ee6f147bcbceac0b577fa7e39a212f371287d8cc119817882`, menu "Build version: 1273043"). Run as an unknown build ("UNKNOWN build: signatures only"), EMF found **all 26 signatures exactly once**; every function moved (e.g. SimStep `0xa1e2e0` → `0xa1fe10`, QueueChatMessage `0xd5ce70` → `0xd5e8f0`). The addresses in this map are still those of 1269579; `src/common/signatures.cpp` has each signature's RVA on both builds.

Making it a known build:
- **Type registry unchanged.** EML's export run (`emm.exe run --export`, read-only; `enshrouded.exe`, `.kfc` and `.kfc_resources` hashes checked before and after) regenerated `types.json` as version `1273043|^/game38/branches/release_1.0|2026-10-20T14:55:29Z`: the same 12,646 types with identical fields, sizes and hashes, so the generated headers changed only their source-version line.
- **Engine offsets (`src/common/offsets.h`).** The world offsets still occur in the new code (disp32 counts in `.text`: kStorage 62, kKind 65, kTick 31, kEntityMap 13, kSingletonArray 5, kEventTypes 5, …) and every feature using them worked in game (below).
- **The save state moved.** `save2WriteWorld`'s job result and world slot are now at `save2+0xB729C` / `+0xB7297` (were `+0xB728C` / `+0xB7287`, +0x10; the world GUID `lea [rsi+0xb72a4]` moved the same way). With the old offsets `on_save` handed mods slot id 0 (index −1) on this build. EMF now reads both offsets from `save2WriteWorld`'s own instructions at startup (`src/common/code_offsets.cpp`: the first `mov [rcx+disp32], rax` at function +0x5a, and `movzx eax, word [rsi+disp32]; mov word [rbp+..], ax` at +0x2c7); on a known build they must equal the recorded values. The load hook's slot (`inst+0x1754`) stayed correct (slot 0x0001 on both builds); `loadSavegameStep` itself tests the flag right after it (`cmp byte [rbx+0x1756], 0` at function +0x7c; the instance's state-2 handler reads the slot), so EMF reads that flag offset from code too and takes the u16 before it.
- **In-game regression on 1273043 with all four mods:** "known build 1273043", 26 of 26 signatures; items (4207), big_stacks (2253 raised), villager state reads (2 villagers), `on_load` (slot 0x0001), mod programs in the server and client worlds; console `help`, `mods`, `pos`, `time 12`, `give`, `entities` (22,511), `inspect 1` (196 components), `tpl 1026 ActiveNpcState`, `sample.villagers`; F6 companion (spawn, StartGuardingEvent, guard distances from template data, follows), F12 give, F8 time, F9 spawn / F10 destroy, F7 villager walks to the player; a hostile `Enemy_Vukah_Melee` from `spawn` produced `HitEvent`s and `EntityDiedEvent`s (the player and the companion died; the companion mod saw its own companion's death). With the offsets read from code: "save state offsets from code: result +0xb729c, world slot +0xb7297", autosaves every 30 s and on quit report slot 0x0001 (index 0, Local); `destroy <entity>` removed a prop and refused the player and unknown ids.
- A hostile spawned with `spawn` was not in the world after a reload (the player's dropped bag was).

## Confirmed in game: save backups (build 1273043)

- The menu's slot records (`docs/engine_parts/re_saves.md`, "The menu's slot records"): a character's level at record
  `+0x2F0` (a LOCAL test character: 1, as the menu shows), a world's last-played time at `+0x48` (equal to the file's time) and play time
  at `+0xB8`. A restored `world0.ksd` is what the next game's slot scan and load use.
- EMF's backup at game start ran 47 ms after the core started (about 10 s before the main menu, long before any world
  could save) and copied 7 save files (427 KB) with every hash checked.

## Confirmed in game: chat lines and saves (build 1273043)
- A player's `BasicPlayerInfos` entry +0x04 is their connection handle (0x40) on the dedicated server and in a hosted
  game; the server's connection record of that handle holds their name at +0x20 (text) / +0x28 (length): the platform
  name the chat shows as the speaker. `BasicPlayerInfos` itself holds no name.
- Lines added to the server's chat system spoken by a player and narrowed to their reader reach that player's game
  outside any command (dedicated server, joined client). The host of a game it hosts has a connection record and a
  reader too; an offline session has no text chat to show the lines.
- `requestServerSave(inst)` called from a server world callback (same thread as `loadSavegameStep`) writes the world
  within a few ms. Details: `docs/engine_parts/re_chat.md` (lines from mods) and `re_saves.md`.

## Confirmed in game: crafting for a player (build 1273043)
- `CraftRecipeEvent` with operator = a player crafts `recipeAmount` times in the next step from the player's own
  inventory into it (one `PickupEvent` and one `CraftRecipeSuccessEvent` per craft); with materials for fewer, it makes
  as many as they pay for. No timer (`craftingDuration` is not applied), no knowledge check (a level-1 character made a
  T7 recipe; neither `player_crafting` nor `handle_crafting_events` reads `G38Knowledge`), no distance check (a station
  115 m away worked), no "station in use" check. The station must have `Crafting` with the recipe's workshop.
  `PlayerPinnedRecipes` is client-only. Details: `docs/engine_parts/inventory_crafting.md` (crafting for a player, in game).

## Confirmed in game: tick fields, full chests, recipe knowledge (build 1273043, a local test world)
- SimStep's code holds the world's tick fields as `mov rax, [rsi+0x4C0]; test rdx, rdx` (+0x71d) and `lea rbx, [rsi+0x4DC]; mov r15, [rsp+..]` (+0x6f5), rsi = entity storage (world + 0xE15DC0): unique in the first 0x1000 bytes; EMF code offsets 4 and 5.
- `CraftRecipeEvent` into a full chest: with 2 stones and 15 swords in a 16-slot chest (no free slot for the product) the game does nothing (no `CraftRecipeSuccessEvent`, no product); with 1 stone and 15 swords it crafts (the stone's slot is freed for the wall) and the wall lands in the chest.
- `RecipeInfo +0x50` (knowledgeRequirement): its first u32 is non-zero for the WaterDrain Stone Wall/Floor recipes, which villagers craft without anything unlocked: not a "needs knowledge" flag.
- `put` 40 non-stacking swords into a 16-slot spawned chest: 16 added; 25 arrows put into the player were still there after a save and reload.

## Confirmed in game: villager jobs (build 1273043, a local test world)
- `CraftRecipeEvent` with operator = a spawned chest and station = villager 1026, sent every 10 s from a mod program: each one took 1 stone from the chest and put 1 `WaterDrain Stone Wall` into it (`PickupEvent` pickedUpBy = the chest, `CraftRecipeSuccessEvent` workshop 0). With the stone gone nothing came (no event at all), and the first refill was used on the next send.
- `NpcCommandEvent` GoToPosition re-sent every 10 s: villager 1025 walked from 18 m to the chest in about 10 s, stopped near it and stayed while the job ran (before the job it roamed 15-20 m around).
- The camera ray from `PlayerInput.fromClient.camera` (forward = orientation applied to +z) picked villagers and chests under the crosshair.
- Entity ids of the test world's villagers (1025, 1026) stayed the same across reloads; a spawned chest is gone after a reload (its id may go to another entity, so the mod re-checks what a saved id names).
- The game draws into an `A2B10G10R10_UNORM` swapchain (format 64) when HDR is on (format 37 on the main menu first): the overlay's colours are then PQ-encoded values, so channel values above ~150 look very bright.

## Confirmed in game: items straight into an inventory (build 1273043, a local test world)
Seen with API v8:
- **The inventory transaction works from a mod program.** EMF's internal server program `emf_inventory` (sys-read `BalancingRegistry`, `g38_items::ItemRegistry`, `impact::AttributeRegistry`; sys-write `KnowledgeTriggerQueue`; other-write `Fuel, Inventory, InventoryEmptyState, InventorySetup, ItemState, OwnerRelationship, SlotSelection`; events-out `InventoryFull, PickupEvent`) registered into the server world, and `iter_begin` filled its iterator as predicted by re_custom_programs.md 6.1 for a program without an entity query: `[ctx][singletons: sys-read in descriptor order, sys-write][other-entity accessors in engine component order][events-out = the job pointer]`. Every accessor checked (InventorySetup and SlotSelection on the receiver, Inventory and InventoryEmptyState on its first linked inventory) read the same pointer as `getComponent`; the item registry slot matched the world's singleton; the events-out slots held the job pointer.
- `put Material_T1_Wood_Logs 5` → `InventoryTx_ctor/init → addItems(owner 1, slot 0) → commit → freeArrays`: 5 added, the game's own "Wood Logs ×5" popup and a `PickupEvent` (pickedUpBy 1), no loose pickup entity.
- `put Weapon_T1_1H_SwordEpic_01` (HasPIDE gear): 1 added, popup "Bronze Sword", the sword appeared in hotbar slot 1 (the game's own slot choice for a new weapon).
- `put Material_T1_Wood_Logs 2 1025`: a **villager** received 2 logs (`PickupEvent` pickedUpBy 1025). Townsfolk have an initialized `InventorySetup`.
- The `companion` mod's body (`FakePlayer_Vukah_Melee`, 110 components) has **no inventory and no equipment components** (`inspect`): `put` reported "entity 6826 has no inventory (InventorySetup)" and 0 added. Gear for a companion needs another body (e.g. `Player_Vukah_Melee`, which has Inventory) or another route.
- `on_load` with the slot offset read from code: "load slot offset from code: world slot +0x1754", "world loaded (slot 0x0001)".

## Confirmed in game: template and item data (a local test world)
Seen with API v7:
- **getTemplateComponent (`0x1409a6f00`) works:** `inspect 1025` (a villager) found 75 components, 15 of them template data (ActiveNpcState, Locomotion, NpcState, PathWalker, Shelter, ...); `tpl` dumped `ActiveNpcState`. The companion's `Enemy01BehaviorState.guardSetup` reads start 10 m, target 3 m, arrive 2 m, `canGuardPlayer` 0 (FakePlayer_Vukah_Melee).
- **Template and item data are read-only memory:** both live in one ~400 MB `MEM_PRIVATE` region allocated `PAGE_READWRITE` and switched to `PAGE_READONLY` after loading (template at `0x20005F77A70`: base `0x20005F77000`, protect 0x2). A plain write is refused; EMF's `write_memory` makes the touched pages writable for the copy and restores them.
- **The server and the client world share the same template data** (same pointer for the same entity) and the change survives a new companion: a second FakePlayer_Vukah_Melee spawned with the already changed distances.
- **Template writes change behaviour:** with the defaults the companion followed a 20 m walk and stood 1-3 m away; after `tplset ... 0x588 f32 60` / `0x58c f32 58` (start/target distance) it stayed 12-21 m away; a new companion after restoring 10/3 followed again. With start 8 / target 6 it still came to 1-3 m when the player stopped (other settings also decide the final distance).
- **Item writes change the inventory:** `big_stacks` raised all 2253 stackable items from 250 to 1000 (a first version raised 1661, then 592 five seconds later: `VirtualQuery` costs ~0.6 ms per call on the 400 MB read-only region, so the pass blocked the server thread for ~2 s, after which `server_world()` looked stale and the remaining writes failed. EMF now caches the region, treats a world as live inside its own callback, and big_stacks spreads its work over ticks); giving 600 wood logs topped an existing 250 stack up to 850 (with stack 250 the same pickup of 600 had added only 250).
- **With those changes:** big_stacks raised all 2253 items in one pass without failures; a `stack Material_T1_Wood_Logs 5000` survived 90 s of big_stacks passes; `get_template_component` reports the game's own size from the reflection descriptor (registry `+0x08` entries, `+0x10` count, entry `+0x30` descriptor): all 15 template components of a villager matched EMF's headers (ActiveNpcState 456 B, Locomotion 632 B, NpcState 128 B, ...), and the companion's `get_template<Enemy01BehaviorState>` passed its size check (2504 B).
- On the host, the client world's tick could see the server world as gone (its clock read before the server stamped its tick) and answer commands with "needs the host". EMF's tracker treats a newer stamp as fresh, and the router waits 10 s.

## Confirmed in game: developer commands (a local test world)
Seen with API v6:
- **Commands from `emf/commands.txt` run on the server world after its step**, all in the same tick they were read: `give Material_T1_Wood_Logs 3` (backpack 5 → 8 wood logs: the 5 from the items test were saved with the local character), `time 12` (15.35 h → 12.00 h through `setTimeOfDay`), `pos`, `items Wood_Log` (6 of 4207), `spawn <barrel guid> 2` (entity count 22519 → 22521), `entities`, and bad input (unknown command / item, `time 25`, a malformed GUID) answered with a usage line.
- **A mod command works:** `sample.villagers` listed the 2 villagers with `DynamicActiveNpcState` state/decision and position.
- **Lines read from `commands.txt`:** a line typed on the main menu was answered `not run: typed 321 s ago` when the world started; files written by Windows PowerShell 5.1 (`Out-File -Encoding utf8` = BOM, `>` = UTF-16LE) ran; console output goes through its own writer thread.
- **A console window in the game process works:** `AllocConsole` from `emf_core` opened a classic console host window (`ConsoleWindowClass`, owned by the game's process) with the close button disabled; a line typed into it ran through the same queue (`time 6`). The game quit normally with it open.

## Confirmed in game: items (a local test world)
Seen with API v5 and the sample mod:
- **Item registry reading works.** `g38_items::ItemRegistry` (server world) held 4207 items once the world loaded; the map at `+0x38` (keys `+0x60`, `ItemInfo*` values `+0x78`) and `ItemInfo` `+0` id, `+4` GUID, `+0x14` maxStackSize read as documented. **Debug names ship with the game:** `ItemInfo.debugName` (`+0x820`) is a BlobString `{u32 offset from the field itself, u32 length}` (e.g. `Material_T1_Wood_Logs`, stack 250).
- **Giving an item as a pickup works.** Spawning `Base_LootPickup_Generic_Gravity` with only `CurrentTransform` + `PickupItem{itemId, item GUID, level 0, rarity 0, amount 5}` (no Velocity or ModelDynamic) 1 m above the player: the game's loot code collected it within ~80 ms (`PickupEvent` 5 × `0x2b0cf1d5` by player 1, through `on_event`), and the backpack showed "Wood Logs ×5, level 1, common".

## Confirmed in game: on_event (a local test world)
Seen with API v4 and the `event_log` mod:
- The core's internal relay program `emf_event_relay_server` (events_in = the 8 events `event_log` subscribed to) registered into the server world and copied that tick's events; the subscribers got them after the step on the server thread.
- A hostile `Enemy_Vukah_Melee` spawned next to the player hit it three times (`HitEvent`: target 1, source = the Vukah, `healthChange` -48 each), the player died (`EntityDiedEvent` 1), then the Vukah killed the companion (three hits, `EntityDiedEvent` 6814; the companion mod saw the same death through its own program). `PickupEvent` was not triggered in this test.

## Confirmed in game: events and the companion (a local test world)
Seen with API v3 and the `companion` mod:
- **Sending events works.** A mod system program declaring `StartGuardingEvent` in its events-out list sent one through `emitEvent(*(J+0x1B8), hash, size, *(u16*)(J+0xB8))`; the next tick the event was in the game's own `StartGuardingEvent` queue (read back by the same program), and `enemy01` applied it (`guardEntityId` became the player).
- **Reading events works**, for the mod's own events and for the game's: the companion program saw the game's `EntityDiedEvent` when a summoned skeleton companion died.
- **Mod programs with event lists** are accepted by `registerProgram` and resolved at finalize like game programs (EMF logged no problem, and the declared events were delivered; the game's own log was not checked).
- **`IsReadyForGameplay` (actor state flag 50) gates enemy AI** (answers open question 15 for the villager worker too): `enemy01`'s per-entity update returns at once while its state is not Invalid and the actor is not ready. Program `actor_is_ready_for_gameplay` (`0x1402663e0`, re-checks each entity every 16 ticks) computes it: for actors with `PlayerInput` it requires `PlayerInput.fromClientIsReadyForGameplay` (+0x5b8) and clears the flag every tick while that is 0; for others a terrain-streaming check at the actor's position (`0x140a8b740`, voxel level 6) and an active-cell check (`0x1409c45b0`).
- **Spawned "automated player" actors stay dormant.** `Player_Vukah_Melee` (faction AutomatedPlayer) has `PlayerInput`; spawned without a client it never becomes ready (stuck in enemy state `Spawning`). Setting `fromClientIsReadyForGameplay = 1` makes it ready, but it still does not move (its locomotion seems to follow player input). Plain enemy actors are ready at once.
- **Companion follows.** `FakePlayer_Vukah_Melee` (faction Player, no `PlayerInput`) spawned 2 m from the player and sent one `StartGuardingEvent` guards the player: when the player walked 17 m away it entered enemy state 20 and caught up to 1-3 m, also on the way back; standing still it idled or roamed 5-8 m away. `StopGuardingEvent` + `destroyEntity` dismissed it. `Player_Companion_Skeleton_Melee_GemOfNecromancy` also followed but died after ~55 s (summon lifetime).
- Of 14 game-spawned enemy actors near the test world's start, all were ready and roaming.

---

## 0. Type facts (from types.json, no code needed)

These hold regardless of where the code lives.

### Reflection data
- **The reflected field offsets match memory (confirmed).** Example: program `fog_of_war` (`0x140307040`) tests `*(u32*)(PlayerState + 0x34) & 0x40000`, and types.json puts `PlayerState.playerStateMask` at `+0x34`.
- The reflection data is also in the exe at runtime. Descriptor layout: `+0x00` name, `+0x10` impact name, `+0x20` qualified name (each ptr,len), `+0x30` namespace, `+0x38` base type, `+0x40` u32 size + u16 align + u16 elemAlign, `+0x48` u32 fieldCount, `+0x50` u32 qualifiedHash + u32 internalHash, `+0x58` fields, `+0x70` default value (ptr,size), `+0x80` attributes (ptr,count).
  Master list of 12,646 descriptor pointers at `0x1416fd950` (holder struct `0x14171a230` = {list, 0x3166}); group "ecs.component" (687) at `0x141716cb0`, "ecs.event" (131) at `0x141718430` (group table `0x141718aa0`). A framework can build its type database from this at runtime instead of shipping types.json.
- 12,646 reflected types. 8,652 are runtime types; the `keen::ds::*` copies are the serialized (save/network) form and can have different sizes.
  Example: `keen::ecs::ActiveNpcState` is 456 bytes, `keen::ds::ecs::ActiveNpcState` is 552. **Use the non-`ds` sizes for memory.**
- 553 types derive from `keen::ecs::Component` (index 136, `innerType == 136`). Full list: see `docs/components.txt`.
- Each type has `qualifiedHash` (looks like a hash of the qualified name) and `internalHash` (looks like a layout hash: every empty component shares `0x6c36572e`).
- Component attributes we can use: `server_only`, `client_only`, `is_dynamic`, `do_not_save`, `savegame_relevant`, `persistent`, `hidden`.

### Core value types
| Type | Size | Layout |
|---|---|---|
| `keen::ecs::EntityId` | 4 | `uint32 id` |
| `keen::Time` | 8 | `sint64 value` |
| `keen::WorldPosition` | 24 | `sint64 x, y, z`, **32.32 fixed point**: meters = value / 2^32 (code multiplies by `2.3283064e-10`, seen in `fog_of_war` `0x140307040`) |
| `keen::WorldTransform` | 56 | `+0 WorldPosition position`, `+0x18 Quaternion orientation`, `+0x28 float3 scale` |
| `keen::Guid` | 16 | resource/object references are 16-byte GUIDs (`*ResourceReference`, `ObjectReference<T>`) |
| `keen::ItemStack` | 12 | `+0 ItemId id (4)`, `+4 ItemData data (8)` |
| `keen::HashKey32` | 4 | `uint32 value` |

### Components a framework will touch first
| Component | Size | Notes |
|---|---|---|
| `ecs::CurrentTransform` | 56 | `+0 WorldTransform transform`. Saved. |
| `ecs::Faction` | 1 | `+0 FactionType` (Environment=0, HitAll, Fogger, Vukah, Scavenger, Wildlife, WildBeast, Player=7, AutomatedPlayer, ...) |
| `ecs::OwnerRelationship` | 4 | `+0 EntityId ownerEntityId`. Server only. |
| `ecs::Inventory` | 96 | `+0 StaticArray<ItemStack,8> slots` |
| `ecs::Equipment` | 1380 | server only |
| `ecs::PlayerState` | 96 | client+server |
| `ecs::PlayerInput` / `ClientPlayerInput` | 1472 / 1536 | server / client |
| `ecs::BuffContainer` | 472 | saved |
| `ecs::Enemy01BehaviorState` | 2504 | enemy AI config |
| `ecs::NpcCommon` | 1688 | server only, villager common runtime data |

### Villager AI (ActiveNpc) layout
`keen::ecs::ActiveNpcState` (456 B) is the **config** component (sequences and setups from the template). The live state is in a separate struct:

`keen::ecs::DynamicActiveNpcState` (136 B, not a component itself; probably the per-entity "dynamic" half of ActiveNpcState):
| Offset | Field | Type |
|---|---|---|
| 0x00 | state | `active_npc::State` (u8) |
| 0x01 | previousState | `active_npc::State` |
| 0x02 | decision | `active_npc::Decision` (u8) |
| 0x08 | socializingData | `SocializingData` (40) |
| 0x30 | npcsInRangeCount | u8 |
| 0x34 | npcRangeQueryId | u32 |
| 0x38 | npcsInRange | `NpcInRange[8]` (entityId u32, squareDistance f32) |
| 0x78 | bedInitialized | bool |
| 0x79 | wasPaused | bool |
| 0x7A | debugStateHistory | `State[8]` |
| 0x82 | debugStateHistoryIndex | u8 |

`active_npc::State`: Invalid=0, Start, FindBed, Idle, DecideTask, FindTarget, GoToTarget, Roam, Sleep, Sitting, Interacting, NpcSocializing, Eating, WaitingCloseToPlayer, AvoidBuildingCursor, AvoidWater, InsideWater, FinishTask=17.
`active_npc::Decision`: None=0, WantsToRoam, WantsToSitDown, WantsToInteract, WantsToSocialize, WantsToSleep, WantsToEat, WantsToIdle=7.

Other villager components: `ActiveNpcStateSaved` (48, saved: spawnPosition, roamingRange, happiness), `ClientActiveNpcState` (24), `ActiveNpcTarget` (68) + `DynamicActiveNpcTarget` (32: isAvailable, usedByEntityId), `NpcState` (128) + `DynamicNpcState` (96).

The same `X` / `DynamicX` split shows up elsewhere (NpcState/DynamicNpcState, ActiveNpcTarget/DynamicActiveNpcTarget). Working theory: a component's template data is the static half, and the engine allocates a `DynamicX` per entity for mutable state.

### Templates and spawning
- `ecs::TemplateResource` (24): `+0 string name`, `+8 TemplateFamily family`, `+0x10 BlobArray<BlobVariant<Component>> components`.
- `ecs::EntitySpawnData` (168): `+0 float3 position`, `+0xC Quaternion orientation`, `+0x1C EntityId parentEntityId`, `+0x20 Time spawnTime`, `+0x28 u64 randomSeed`, `+0x30 float3 velocity`, `+0x4C HashKey32 itemId`, `+0x50 EnemySettings`, `+0x9C sint32 level`, `+0xA0 EntitySpawnValueMask mask`.

---

## 1. Core (Tier 1)

### How the engine is put together (summary)
- The game is an **ECS**. One C++ "world" object (~30 MB) holds everything for one simulation: program (system) table, singletons, entity storage, tick clock.
- There are up to three worlds per game: **server** (kind 0, log tag `ecss`), **prediction** (kind 1, `ecsp`) and **client** (kind 2, `ecsc`). A client that joins someone else's game still creates a GameServer and a GameClient (log: `GameServerCreate`, `GameClientCreate`, `ServerThread`), but in that mode (`Client_Online`) the server create/init steps do nothing; the log only prints the step names (see 3.2). The user's own log agrees: `[ecss] ComponentTypeSize 0` and no server simulation. **Gameplay logic only runs on the host** (High).
- Gameplay code is ~816 named **programs** (systems), each with a descriptor that lists the components and singletons it reads and writes. Full list: `docs/ecs_programs.md`.
- Components are identified by the **`qualifiedHash` from types.json** (FNV-style 32-bit). Every lookup goes hash → u16 type index → chunk offset.
- Shared state lives in ~100 named **singletons** per world (`LocalPlayerData`, `g38_daytime::Daytime`, `g38_items::ItemRegistry`, `ServerPlayers`, ...).

### World object layout (offsets from the world base)
| Offset | What | Evidence |
|---|---|---|
| `+0x000` | u8 state (1 = loading templates, 2 = running) | world tick `0x1409c86a0` |
| `+0x928` | start of the "registry" part; `*(world+0xE15DC0)` points here | sim step math |
| `+0x7462D0` | singleton array (ptr), stride 0xE8; `+0x7462D8` count | `registerSingleton` |
| `+0x7462F8` | singleton name map (FNV-1a hash of name → u16 index) | `registerSingleton` |
| `+0x746350` | program table, stride 0x1F20 (entry `+0x00` desc ptr, `+0x08` u16 index, `+0x10` name, `+0x20` flags) | `registerProgram` |
| `+0xE15350` | u16 program count (max 0x380 = 896) | `registerProgram` |
| `+0xE15A60` | program name map (FNV-1a → index) | `registerProgram` |
| `+0xE15DC0` | **entity storage / sim** struct (passed to most ECS functions) | everywhere |
| `+0xE15DC8` | u8 world kind: 0 server, 1 prediction, 2 client | log code (`'s'`,`'p'`,`'c'`) |
| `+0xE15F08` | entity map (EntityId → entity record), = storage`+0x148` | `getEntity` |
| `+0xE1629C` | u32 current tick number | world tick |
| `+0x1CFB6C0` | i64 tick length in ns | world tick |
| `+0x1CF3AC0` | pointer to a second storage (prediction?) | `destroyAllEntities` |

Inside the registry (`world+0x928`): `+0x08` → component type info array (stride 0x100), `+0x88` component map (qualifiedHash → u16 type index), `+0xE15440` (= world+0xE15D68) resource system pointer used to lock templates.

Keen hash map (used for entities, components, singletons, programs): `+0x08` occupied bitset (u64 words), `+0x10` capacity (power of 2), `+0x28` u32 keys, `+0x40` values, `+0x54` u32 count. Linear probing. Entity-ID keys are mixed first: `x = ((x>>16)^x)*0x45d9f3b; x = ((x>>16)^x)*0x45d9f3b; x = (x>>16)^x`. Component/singleton keys are used as-is (`hash & (cap-1)`).

### 1.1 Per-frame game update

| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| Server thread main | `0x140e38fe0` | `40 55 53 57 41 55 41 57 48 8D 6C 24 C9 48 81 EC E0 00 00 00 48 8B 59 78` | Thread proc of the `ServerThread` (strings `Main_Server`, `ServerStack`, `Error while creating the event loop for the game server`). Registered in `0x140e34060` (string `GameServer`), which the app creation step `ServerThread` calls. Loops calling the server system update. | High |
| Server system update | `0x140e47710` | `48 8B C4 55 41 54 48 8D 68 E8 ?? ?? ?? ?? 01 00 00 48 89 70 E0 4C 8D A1` | Server thread, per frame: loops over game-server instances (array `+0x440`, count `+0x448`, stride 0x90; entry `+0x00` GameServer*, `+0x18` u8 state), creates them (state 1/2 → `0x140e3a350`) and calls GameServer update. | High |
| GameServer update | `0x140e46070` | `48 89 4C 24 08 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 F8 FF FF` | `(out, ?, GameServer*, nowNs)`. Ticks the **server world** (`GameServer+0x1D0`) via `0x1409c86a0`, spawns players (`0x140e48dc0`: `[server] Could not spawn Player Entity`). | High |
| Client game update | `0x14086e7b0` | `44 88 44 24 18 48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D` | Client "play" state per frame (main thread side): handles server messages (`[Client] Input Message error`), stats `ecsc_entities`/`ecsp_entities`, ticks the **client world** via `0x1409c80d0`. Called from `0x1408672f0`. | High |
| Server world tick (clock) | `0x1409c86a0` | `40 53 56 41 54 41 56 48 83 EC 28 48 8B DA C6 41 01 00 48 8B F1 E8 F6 F2` | `bool(world, nowNs)`: fixed-step accumulator; on first call finishes template loading (`Initialized %u template infos`); returns true when a tick is due. | High |
| Client world tick (clock) | `0x1409c80d0` | `40 53 41 54 41 56 48 83 EC 50 4C 8B E1 C6 42 01 00 48 8B CA 49 8B D8 4C` | `TickInfo*(out, world, nowNs)`: same idea, plus time scaling (`world+0x1CF3B78`) to follow the server's tick. | High |
| World update | `0x140a11580` | `40 53 55 56 57 41 56 48 83 EC 50 8B A9 9C 62 E1 00 48 8D 99 C0 5D E1 00` | `(world)`: runs the sim step, then logs `[ecs%c] Stats:` (`0x140a01c70`). No direct callers found (called indirectly). | Medium |
| **ECS sim step** | `0x140a1e2e0` | `40 55 53 56 41 55 48 8D AC 24 F8 FF FD FF B8 08 01 02 00 E8 ?? ?? ?? ??` | `(storage = world+0xE15DC0, bool ticked)`: schedules all programs for this world on the task system, runs them, then processes queued creates/destroys (`0x140a15960`), replication. **Best "OnTick(world)" hook.** | High |
| Program scheduler | `0x1409a0e00` | `48 89 4C 24 08 55 53 56 48 8D AC 24 70 E2 FF FF B8 90 1E 00 00 E8 E6 A4` | Picks programs by flags and world kind, builds job slices (0x38 B each), queues `0x1409e1390` jobs or runs `0x1409a2020` inline. | High |

Program flag bits (from the scheduler): `0x1` client program (skipped on server), `0x2` disabled, `0x8` init-only (runs on entity creation, not every tick), `0x10` not run in prediction, `0x20`/`0x40` skipped, `0x100` runs with a per-program sub-world, `0x200` split into per-chunk jobs, `0x400` main-thread/serial.

Threads: the **server world runs on the server thread** (`0x140e38fe0`); the client world runs from the client game update (`0x14086e7b0`, main/game thread). Programs themselves run as jobs on the task system's worker threads (`Task-Default*` threads in the log), so never touch ECS data from a hook on a worker while the sim step is running.

Recommendation: hook `0x140a1e2e0`. The world is `storage - 0xE15DC0`; read the kind byte at `storage+0x8`. Do mod work just before or after calling the original, on the owning thread. **Before** the call no jobs of that world are running. **After** the call that is true for the client and prediction worlds, but **not for the server world**: its SimStep returns with the EntityGrid update and the range queries still running (see "Jobs still running after SimStep" below). Wait for them first with the game's own `waitForWorldJobs` (EMF does, `src/core/world_jobs.h`). For per-frame client work, hook `0x14086e7b0`. For rendering overlays, hook Vulkan `vkQueuePresentKHR` (Vulkan is loaded dynamically, not imported by the exe).

#### Jobs still running after SimStep (server world; build 1273043, found from a crash)
Addresses are 1273043's. The dedicated server of 1273043 has the same code (waitForWorldJobs RVA `0x6c2120`, one call at `+0x7b39c7`).
- **SimStep `0x140a1fe10` leaves two jobs running on the server world (kind 0):**
  - It submits job `0x1409cb590` **asynchronously** (at `+0x2019d`).
    - That job batches the chunks whose entities moved, then runs task A `0x140989830` (once per batch).
    - Then it runs task B `0x140989820`, which is `mov rcx,[rcx]; jmp 0x1409a5760`: the **EntityGrid update**. For each moved or new entity it removes it from its old cell (`0x140989bd0`) and inserts it into the new one (`0x140955c80`).
  - Right after (at `+0x201b8`) the range-query scheduler `0x140a1c7e0` submits task `0x140a1ca50` **asynchronously, with no dependency** on that job.
    - That task runs each registered range query: `0x140a1d990` (radius > 0), `0x140a1d290` (the client world, or a query's `+0x51` flag) or `0x140a1cb20`.
    - The queries read the same grid.
  - The scheduler waits only on the client world (kind 2), at `0x140a1ca24`.
  - The prediction and client worlds run job `0x1409cb590` and its tasks inline.
- **The game waits for both later in the frame:** GameServer update `0x140e48b60` calls `if (world+1 /*ticked*/) waitForWorldJobs(world)` at `0x140e49b52`, after its other post-tick work.
  - `waitForWorldJobs` `0x140a20f70` is `mov rcx,[world+0xE15D60]; jmp 0x1408e5e20`. `world+0xE15D60` is the world's task group: registry `+0xE15438`, the same group SimStep submits to.
  - The task-group wait `0x1408e5e20` takes the group's shared lock at `+0x148`.
    - While a task is running (`+0x1C0`) or queued (`+0x1A0`), it helps run them (`0x1408e1190`) and waits on the group's event at `+0x138` (100 ms timeout; on a timeout with work pending it logs "[task] taskListFinishedEvent was not triggered!").
    - With nothing pending it only empties the finished-task list (`+0x1A8`, count `+0x1B8`) and returns.
  - So **calling it early, and twice, is safe**: the game's own call then finds nothing to wait for.
  - Signature `48 8B 89 60 5D E1 00 E9 ?? ?? ?? ??` (`WaitWorldJobs`), unique in `.text`.
- **The race:** the grid insert frees a full 4-entry bucket before it points the cell at the new 128-entry bucket.
  - The pool free `0x140891ad0` writes the free-list link into the block's first slot and leaves its count byte at 4.
  - A range query reading that cell at that moment walks a freed bucket. When the free list was empty its first slot is NULL, and the query crashes on `mov rax,[rdi+0x18]` at `0x140a1dcb1`, on a `Task-Default` worker.
  - This happened once in about 140 runs, 0.5 s after a world started (the first ticks fill the grid). It is a game race; EMF was not involved.
- **What it means for mods:** anything that creates or destroys entities between SimStep's return and the game's wait races both jobs.
  - `DestroyEntity(storage, id, 1, 0)` removes from the grid at once (`0x140996eb0` → `0x1409af9c0` → `0x140989b20` → `0x140989bd0`).
  - SimStep itself makes that call only before it kicks the jobs (`+0x2006f`).
  - EMF therefore calls waitForWorldJobs before its tick work on the server world. Without the signature it logs "world jobs: off" and runs as before.
- **Confirmed in game** (a LOCAL test world):
  - At start-up EMF logged "world jobs: on", and at the first server tick "waited for the server world's jobs … (first wait)".
  - Spawn and destroy ran from mods' tick work while the world ran:
    - Night Raids: 2 raids of 2 waves × 6 = 24 raiders; 18 killed by `raids.kill`, 6 removed by `raids.stop`;
    - Wandering Champion: `champion.summon 40` and `champion.dismiss` 4 times.
  - Nothing crashed or faulted, and there was no "taskListFinishedEvent" warning.
  - The server step stayed at 16.6 ms (60 Hz).
- **EntityGrid** (`storage+0x1F8`):
  - `+0x14` f32 1/cell size (1/64 m); `+0x18` u32 width, `+0x1C` u32 height (160x160); `+0x68` u32 entries.
  - Pools: `+0x70` small buckets (base, `+0x10` free head, `+0x18` in use, `+0x28` capacity, `+0x2C` stride 0x30) and `+0xB0` big buckets (stride 0x410).
  - `+0xF8` cells (12 bytes each): u16 dirty link, u16 bucket reference (bit 15 = small pool, low 15 bits = index+1), u32 count with 0x80000000 = dirty.
  - Small bucket: 4 entity-record pointers, u8 count `+0x20`, u16 next `+0x28`. Big bucket: 128 pointers, count `+0x400`, next `+0x408`.
  - Range-query records are 0x470 bytes each, at `storage+0x3E0`:
    - `+0x4` the querying entity, `+0xA` u16 component filter (type index + 1);
    - `+0x10` the centre (float3), `+0x30` the radius;
    - `+0x34` generation, `+0x38` dirty;
    - `+0x58` results ({EntityId, squared distance, u64} x 64), `+0x458` result count.

### 1.2 ECS world, entity iteration, component lookup

**Programs (systems)**
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| registerProgram | `0x1409d5770` | `48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC A0 00` | `bool(world, ProgramDesc*, bool disabled)`. Logs `entity program '%s' already registered`. | High |
| register all programs | `0x1404a5a30` | `40 53 48 83 EC 50 48 8B D9 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 40 41 B8 24` | 757 `registerProgram` calls (`call getDesc; mov rdx,rax; xor r8d,r8d; mov rcx,rbx; call registerProgram`). | High |
| register more programs | `0x14122c1c0` | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B DA 48 8B F9 E8 36 36` | 59 more. | High |

ProgramDesc (0x188 B, in `.rdata`): `+0x00` name (ptr,len), `+0x10` update fn `void(ProgramContext*)`, then (ptr,count) lists of name strings, `+0x180` u32 flags. See `docs/ecs_programs.md` for the slot meanings and all 816 programs.

**Iterating entities inside a program**
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| iter_begin | `0x1409bab60` | `48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B 09 48` | `(ctx, iter, iterSize)`. Fills `iter` with per-chunk array pointers in the order of the descriptor lists, then singleton pointers. | High |
| iter_next | `0x1409b5a50` | `40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 30 08 00` | `bool(ctx, iter, iterSize)`: advance to next entity/chunk. | High |
| current entity id | `0x1409b2e40` | `8B 41 08 4C 8B C2 85 C0 75 06 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 28 08 00 00 8B 04 D1 41` | `EntityId*(ctx, out)`: reads `chunk+0x828` entity list (16-byte entries, id at +0). | High |

**Looking up an entity or component from anywhere**
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| **getComponent** | `0x1409a6190` | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D BA C0 5D E1 00 44 89` | `{void* ptr, u64 size}* (out, world, u32 qualifiedHash, EntityId)`. Returns {0,0} if missing. Verified: 31 constant globals passed here hold types.json qualifiedHashes (e.g. `CurrentTransform 0x027ac564`). | High |
| getComponent by type index | `0x1409a5ec0` | `48 89 5C 24 08 48 89 74 24 10 44 89 4C 24 20 57 48 83 EC 20 48 8D BA 48` | `(out, storage, u16* typeIndex, EntityId)`. | High |
| getEntityTransform | `0x1409a7160` | `48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 5D E1 00 89 54 24 38 48 8D 8F` | `WorldTransform*(world, EntityId)`, null if the entity does not exist. Good existence check. | High |

Entity record (value in the entity map): `+0x18` archetype layout ptr, `+0x20` chunk data base, `+0x30` u32 index in chunk. Layout: `+0x00` component bitset, `+0x84` u16 offset per type index, `+0xA84` u16 stride per type index.
Component address = `chunkBase + layout.offset[t] + index * layout.stride[t]` (if bit `t` is set).

How a mod loops over **all** entities without being a program: walk the entity map at `world+0xE15F08` (bitset `+0x08`, capacity `+0x10`, keys `+0x28`, values `+0x40`); each occupied slot gives an EntityId and record. Then call `getComponent(world, hash, id)` or compute the address as above. Only safe on the thread that owns the world, between ticks (e.g. inside a hook on `0x140a1e2e0`).

**Singletons**
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| registerSingleton | `0x1409d50d0` | `48 89 5C 24 20 55 56 57 41 54 41 55 48 83 EC 20 4C 8B 52 08 33 F6 4C 8B` | `bool(world, {char* name, u64 len}*, void* object, Options*)`. 137 call sites. Entry (0xE8 B): `+0x00` inline name, `+0xA0` u32 FNV-1a(name), `+0xA8` object ptr. | High |
| client singleton setup | `0x14039fbb0` | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FD FF FF` | Registers `LocalPlayerData` (object at owner`+0x648`), `LocalPlayerCamera`, `g38_daytime::Daytime`, ... | High |

**World creation (where programs and singletons get registered)**
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| client world setup | `0x14039fbb0` | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FD FF FF` | Registers client singletons (`LocalPlayerData`, `AmbientSystem`, `FogQuery`, ...) then calls both program-registration functions. Called from `0x140841c40`. | High |
| client world setup caller | `0x140841c40` | `48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 50 FD FF FF` | Creates the client side. | Medium |
| server world setup | `0x1404949b0` | `48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 E3 FF` | Registers server singletons (`ImpactSystem`, `AnimationGraph_Server`, `VoxelWorld`, `g38_daytime::Daytime`, `ServerVoxelDamageSystem`, ...), script function names (`spawnEntity`, `destroyEntity`, `applyBuff`, ...) and the programs. | High |
| GameServer init | `0x140e3a350` | `48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 40 ED FF FF` | Creates the server world (stored at owner`+0x1D0`), registers `ServerPlayers` (owner`+0x1E0`), calls server world setup. | Medium |

Framework idea: hook the two world-setup functions, call the original, then call `registerSingleton` / `registerProgram` with the mod's own descriptors. Mods could then add real ECS systems that run in the game's scheduler with the same iterator API. The `registerProgram` half is confirmed in game (open question 7 below).

GetSingleton(world, name) can be done without calling game code: FNV-1a 32 of the name, look it up in the map at `world+0x7462F8`, entry = `*(world+0x7462D0) + idx*0xE8`, object = `*(entry+0xA8)`. 102 distinct singleton names are used by programs (list in `docs/ecs_programs.md`, slots `sys-read`/`sys-write`).

### 1.3 Template / resource lookup by name or GUID
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| lock template resource | `0x14094c030` | `48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 48 8B EC 48 81 EC 80 00 00 00 33 F6 48 C7 45 C0 75 87 76 39` | `Handle*(out, resourceSystem, Guid*)`. Handle type tag `0x39768775` = qualifiedHash of `keen::ecs::TemplateResource`. | High |
| resource find by GUID | `0x141063570` | `40 53 48 83 EC 20 49 8B 40 08 48 8B D9 49 0B 00 75 0B 89 01 48 8B C1 48 83 C4 20 5B C3 48 8B 82` | `(out, resourceSystem, Guid*)` → resource id. Generic, 217 callers. | High (code) / Medium (name) |
| **getTemplateComponent** | `0x1409a6f00` | `48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 5D E1 00 89 54 24 38 48 8B 0F 48 8D 54 24 38 48 83 C1 30 41 8B D8 E8` | `void*(world, qualifiedHash, entityId)`: the entity's template data for a component (template map `world+0x958`). EMF signature 25. Details: [`engine_parts/re_templates.md`](engine_parts/re_templates.md). | High (runtime-confirmed) |
| resource get data | `0x1410630d0` | `40 53 48 83 EC 40 48 8B D9 45 85 C0 75 15 33 C0 0F 57 C0 0F 11 01 48 89` | `(out, resourceSystem, id)` → data pointer/size. 331 callers. | Medium |

- Templates are referenced by **16-byte GUID** at runtime. Names only exist inside `TemplateResource.name`.
- Recommendation: build a name → GUID table offline with EML (12,267 templates) and ship it with the framework. Runtime lookup by name is possible by scanning resources, but not needed.
- The resource system pointer is at registry`+0xE15440` (= world`+0xE15D68`).

### 1.4 Spawn and destroy an entity
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| **createEntity** | `0x14097b790` | `4C 89 4C 24 20 4C 89 44 24 18 89 54 24 10 48 89 4C 24 08 55 56 41 54 41` | `(storage, u32 entityId (0 = allocate), SpawnRequest*, ?)`. Locks the template, allocates the entity, copies spawn components. Logs `Could not lock template resource`, `Entity id %k already exists`, `EntityMap out of space`. | High |
| spawn wrapper | `0x1409788d0` | `48 83 EC 58 0F 10 02 8B 15 ?? ?? ?? ?? 33 C0 41 0F 10 08 48 81 C1 C0 5D` | `(world, Guid* template, Slice<SpawnComponent>*)` → createEntity(id 0 = new). 19 callers. Recipe and safety notes: [`engine_parts/re_time_spawn_weather.md`](engine_parts/re_time_spawn_weather.md). | High |
| spawn player-owned | `0x1409807f0` | `48 83 EC 58 41 0F 10 00 4C 8B D1 8B CA 41 0F 10 09 B8 01 00 00 00 C7 44` | `(world, playerIndex, Guid*, Slice*)`: sets player mask `1<<idx`, kind 2. Used for the player entity. | High |
| process queued creates | `0x140a15960` | `40 53 41 54 48 83 EC 38 45 33 E4 48 8B D9 41 8B D4 44 38 A1 18 DC ED 00` | Called at the end of the sim step. | Medium |
| **destroyEntity** | `0x140995ad0` | `48 89 5C 24 18 48 89 74 24 20 89 54 24 10 55 57 41 55 41 56 41 57 48 8D` | `(storage, EntityId, bool, ?)`: queues destruction (`Entity Update Queue is full for destroy`). | High |
| destroy all entities | `0x140994000` | `48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 55 41 56 48 8D AC 24 B0 C0` | `bool(world)`: destroys every entity in the world and the secondary storage. | High |

SpawnRequest (0x30 B): `+0x00` u32 parent EntityId (0 = none), `+0x04` Guid template (16 B), `+0x18` SpawnComponent* list, `+0x20` u64 count (max 128), `+0x2A` u16 player mask, `+0x2C` u8 kind.
SpawnComponent (0x18 B): `+0x00` u32 component qualifiedHash, `+0x08` void* data, `+0x10` u64 size (must equal the component size, else `does not match to spawn size`). The data is copied over the template default.
To spawn at a position, pass a `CurrentTransform` (hash `0x027ac564`, 56 bytes, `WorldTransform` with fixed-point int64 position).

Open: entity creation should happen on the **server** world, on its thread, outside program execution (e.g. right before or after `0x140a1e2e0` runs). Confirmed in game (open question 3 below). Gameplay code usually spawns through the Impact native `SpawnEntity` (`0x1403e3960`, see 3.3), which takes a position; its body shows how the game fills a SpawnRequest with a transform.

### 1.5 Local player
| Function | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| isLocalPlayer | `0x1403b4e40` | `39 4A 04 0F 94 C0 C3 CC CC CC CC CC CC CC CC CC 48 89 5C 24 08 48 89 7C` (fragile: includes padding) | `bool(EntityId, LocalPlayerData*)` = `*(u32*)(lpd+4) == id`. | High |

- **`LocalPlayerData` (client-world singleton) `+0x04` = local player EntityId.** 60 client programs read it. `LocalPlayerData+0x2600` points to settings (e.g. arachnophobia flag at `+0x1FF`).
- Get it with GetSingleton(clientWorld, "LocalPlayerData"), or by hooking `registerSingleton` at startup.
- The server keeps a `ServerPlayers` singleton (registered in `0x140e3a350`, object at owner`+0x1E0`).
- Fallback that needs no singleton: the only entity in the client world with a `ClientPlayerInput` component (`0x9621e4c3`, client-only) is the local player.
- Entity IDs are shared between the server and client worlds on the host (replication messages use the same IDs), so the same ID works with `getComponent` on the server world for server-only components like `Equipment`. Confirmed in game (open question 2 below).

## 2. Gameplay systems (Tier 2)

### 2.1 Inventory, items, equipment
Full details and signatures: [`engine_parts/inventory_crafting.md`](engine_parts/inventory_crafting.md).

- **ItemStack** (12 B): item id, count, and an optional per-item entity id ("pide") for unique items such as gear. Every `Inventory` component is exactly 8 slots. A player or chest is a tree: `InventorySetup` links up to 16 child 8-slot inventories, each with a category.
- **Item definitions**: singleton `g38_items::ItemRegistry`. Lookup `0x140e7a870(itemId, ItemRegistry*)` → `ItemInfo*` (hash map at registry `+0x38`, built by `0x140ea8d40`). Item ids look like FNV-1a over the item's 16-byte GUID (Medium).
- **Inventory changes are transactions**: init `0x140411df0` → operations → commit `0x140408b50` (47 callers) or rollback `0x140410e60`.

| Operation | Address | Conf. |
|---|---|---|
| add item | `0x14040b170` | High |
| remove item | `0x14040a080` | High |
| destroy stack | `0x14040b920` | High |
| move / merge / swap | `0x14040d6d0` | High |
| equip / unequip | `0x14040dff0` / `0x1404123f0` | High (dispatch), bodies not decompiled |
| read / write equipment slot | `0x140e990a0` / `0x140e76040` | Medium |
| player inventory input program `inventory_actions` | `0x1403fd610` | High |
| admin "create item" handler (admin-only, check `0x1403b41f0`) | `0x1403f5940` | High |

Caveat: add/remove are not free functions. They need component storage handles from a 0xF0-byte setup block each program builds from its iterator (only ~10 of ~30 fields known). Practical route for "give item": hook `inventory_actions` and call add + commit inside it, on the host.

### 2.2 Crafting
- **Recipes**: singleton `g38_items::RecipeRegistry`. Lookup `0x140e7fbd0(recipeId, RecipeRegistry*)` → `RecipeInfo*` (map at `+0x48`); per-workshop lists at `+0xF8` (max 512 each). Built by `0x140eac060`.
- **Player crafting**: program `player_crafting` (`0x1402e0e50`) turns input into a `CraftRecipeEvent` (event hash `0xa4766fb7`, size 0x38, sent via `0x140955400`). Program `handle_crafting_events` (`0x140274a40`) calls the **craft core `0x140470f10`**: consume inputs `0x1403f1760` (inventory + base storage), add outputs, commit, emit `CraftRecipeSuccessEvent`.
- **Automated stations**: program `factory_station` (`0x140330660`) state machine; timed craft completes in `0x1403282a0`.
- **Add a recipe at runtime** (Medium): after `0x140eac060` runs, insert a `RecipeInfo*` into the `+0x48` map and the workshop list at `+0xF8`, on both server and client. Unknown: whether it shows in the UI and what unlock knowledge it needs.
- **NPC crafting** (Low-Medium): emit a `CraftRecipeEvent` with the NPC as operator and a station with a `Crafting` component, or call the craft core directly. Not checked that an NPC operator is accepted.
- Do not patch `0x140e99af0` (`xor al,al; ret`, a stub shared by unrelated code). Hook `0x1403f1760` instead.
### 2.3 AI: enemy behavior and villager ActiveNpcState
Full details: [`engine_parts/ai.md`](engine_parts/ai.md) (all 17 villager state handlers, iterator layout, enemy FSM).

**Villager AI = ECS program `npc_active_behavior`** (server world, flags 0).
| Function | Address | What it does | Conf. |
|---|---|---|---|
| program update (per batch) | `0x140456790` | Builds a per-entity context and calls the worker for each entity. | High |
| per-entity worker | `0x1404552c0` | Runs only if a player is within 150 m (else pauses 1–10 s). Calls interrupts check `0x14044b3f0`, then the state machine. **Best hook to read villager state.** | High |
| RunStateMachine | `0x14045e650` | phase 0 enter, 1 update, 2 exit, 3 stop, 4 resume. | High |
| state dispatch switch | `0x14044adf0` | Switch on `DynamicActiveNpcState.state` → handler per state. | High |
| SetState | `0x140453cf0` | exit old, set previousState/state, enter new. Hook for transitions. | High |
| FinishState | `0x1404546a0` | Next state from table `0x141fd2780` (u8[18][2]: finished/failed; .bss, filled on first run, writable). | High |
| SetDecision | `0x1404532a0` | Sets `decision`. | High |
| DecideTask | `0x14045bab0` | Debug stubs (force sit/interact/roam `0x140447090/70/80`) → sleep time → socialize → weighted pick over `decisionSetup`. | High |
| FindTarget | `0x14044d980` | Async range query around spawn, filter by `ActiveNpcTarget.setup` flag for the decision, distance weighting, claim via `usedByEntityId`. `isFarmland` is never used. | High |
| NpcCommandEvent handler | `0x14044f040` | Game's own villager command: Reset or GoToPosition. | Medium |

Per-entity context (passed to the worker): `+0x1B0` → DynamicActiveNpcState* (state `+0`, previousState `+1`, decision `+2`), `+0x1E0` ActiveNpcStateSaved*, `+0x300` NpcCommon*, `+0x168` CurrentTransform*, `+0x450` ActiveNpcState* (template), `+0x148` Daytime singleton.

Findings that matter for mods:
- **State 12 `Eating` has no handler and nothing picks `WantsToEat`**, but target search and GoToTarget already support it. It is the cheapest slot for a new task such as "harvest farmland": hook DecideTask and the dispatch, switch the target filter to farmland, set table bytes `0x141fd2796/97` to `0x11` (FinishTask).
- Force a state the safe way: SetDecision then SetState from inside the worker hook. Writing the byte directly skips exit/enter logic.
- Writes to components should call the change-tracking function `0x1409c5df0(tracker, ...)` (1,090 callers) so replication and derived state notice them.
- Template data (`ActiveNpcState`, 456 B) reaches programs through per-entity template pointers. The chunk column holds the dynamic half (`DynamicActiveNpcState`, 136 B). The `Dynamic*` structs are **not** in the registered component list, yet `getComponent` answers for them: the per-entity half is keyed by `DynamicActiveNpcState`'s own hash `0x9e65507a` (136 B); the `ActiveNpcState` hash returns nothing (runtime-confirmed).

**Enemy AI** (program `enemy01`, medium-high confidence): update `0x1403688f0` → per-entity `0x140367bd0` → behavior `0x140370170`. Condition-driven FSM: `0x140367260` picks the next state from table `0x141fd1e20` (state × 33 conditions), SetState `0x140365cb0`, per-state dispatch `0x140375490`. Aggro/target choice in `enemy_combat_targets` `0x140379210` (6 aggro slots, decay, winner in `SelectedTargets[0]`).
### 2.4 Factions and combat damage
Full details and signatures: [`engine_parts/combat.md`](engine_parts/combat.md). All of this runs on the **server world only**.

**Damage pipeline**
| Step | Address | What it does | Conf. |
|---|---|---|---|
| program `impact_simulation` | `0x1403ef7a0` | Runs impact node graphs; nodes call C++ "impact natives". | High |
| native `applyDamage` | `0x1403d4d50` | Computes the health/stamina change (damage types, armor, susceptibility, crit, block, faction modifiers). Queues a 0x150-byte pending hit via `0x141216920`. Does not touch health. | High |
| program `impact_hit_conclusion` | `0x1403ed8f0` | Keeps the strongest hit per target, charges attacker mana, calls applyHitToEntity. | High |
| **applyHitToEntity** | `0x1403e8b40` | Changes Health/Stamina attributes, sets WasKillingBlow, sends HitEvent. **Hook here to change or log damage**: edit `hit+0xBC` (healthChange) / `hit+0xC0` (staminaChange) before the original; skip the original to cancel. | High |
| program `death` | `0x140273300` | Sends `EntityDiedEvent` when health < 1. | High |
| program `destructible` | `0x14032e5e0` | Removes destructibles on a killing blow. | High |
| `fall_damage_infliction` | `0x140274070` | Writes health directly (bypasses the pipeline): needs its own hook. | High |

**Health / attributes**: health is an attribute on the server: an 8-byte header (rootId, storageOffset, flags) + up to 8 int32 slots; slot meaning comes from `impact::AttributeRegistry`. Read with `attrFind` `0x14121bea0`; write with `attrAdd` `0x1403e83a0` (adds a delta and recomputes formulas). Attribute ids: current health `0x8EB84995`, max health `0xDC8358BE`, stamina `0x04B6AA8B` (High), mana `0x6AA2574E` (Medium). Clients only see `NetworkHealth` {health, healthMax}.

**Factions**: no data table; rules are hard-coded in small leaf functions. `isEnemyFaction` `0x140e892f0` (by value) / `0x140e89230` (by pointer, used by AI targeting). Friend checks `0x140e89880`, `0x140e897e0`, `0x140e89820` (pacified NPCs count as friends of players). `0x140e8b200` = "player side" {Player, AutomatedPlayer, PlayerAlly}. Full rule list (including friendly monster pairs such as Vukah–WildBeast) in combat.md. A faction-rules mod hooks these leaf functions.

**Buffs**: native `applyBuff` `0x1403d4920` → `getBuffType` `0x140e729d0` → **`applyBuffCore` `0x1403acd70`** (faction exclude mask, slots, reapply rules; spawns the buff as its own entity; sends `AppliedBuffEvent` via `0x1403b7430`).

**Impact natives**: 108 C++ functions callable from impact graphs, registered in server world setup `0x1404949b0` through `0x141220f30`, keyed by FNV-1a of the node name. Table: [`impact_natives.txt`](impact_natives.txt). Robust scan pattern per native: `4C 8D 05 ?? ?? ?? ?? BA <hash32>` (needed because some natives, e.g. isEnemy/isFriend, have identical bodies).
### 2.5 Building and terrain
Full details and 52 verified signatures: [`engine_parts/world_building_daytime.md`](engine_parts/world_building_daytime.md).

- Building is driven by **action-sequence events** (`PlaceVoxelObjectEvent`, `ChangeVoxel`, `Dismantle`) handled by four server programs, not by raw input.

| Function | Address | Conf. |
|---|---|---|
| placeBlueprint | `0x140486ef0` | High |
| removeBlueprint | `0x140487770` | High |
| placeProp | `0x1404866b0` | High |
| finish (commit/rollback inventory transaction, emit `UiBuildingEvent`) | `0x14048baa0` | High |
| VoxelWorld_placeBlueprint / VoxelWorld_removeBlueprint | `0x140a837b0` / `0x140a8c220` | High |
| impact native `terraforming` (branches on `TerraformingType`) | `0x1403e5b70` | High |

- `PlayerBasesSystemServer` (0x2070 B): 16 base records at `+0xC0`, stride 0x130, each with an AABB; lookups `0x140468ce0`, `0x140469f70`. Bed, comfort and crafting-prop counts are rebuilt every tick (Medium). Relevant for villagers.
- `BuildZoneSystem` is rebuilt every tick; `addZone` `0x140e557e0` (list 0 build zones, 1 no-build, 2 creature no-build) (Medium).
- Not found: the "may I build here" check, the exact arguments for calling `VoxelWorld_placeBlueprint` without a building context, where the flame-altar level is stored.

### 2.6 Day/night and weather
**Day/night: singleton `g38_daytime::Daytime` (0x70 B, High)**
| Offset | Field |
|---|---|
| `+0x10` / `+0x18` / `+0x20` | anchor: ecsTime, gameTime, timeScale (same as `DaytimeSync`) |
| `+0x24` | change counter |
| `+0x28` / `+0x30` | day start / night start (6 h / 18 h) |
| `+0x38` / `+0x40` | real length of day / night (30 / 12 min) |
| `+0x48` | **current time of day, ns** |
| `+0x58` | isDay |

| Function | Address | What it does | Conf. |
|---|---|---|---|
| Daytime_update | `0x140ea7c00` | Advances the clock each tick (called by programs `server_update_daytime` / `client_update_daytime`, which have identical bytes: find them through their descriptors). | High |
| **setTimeOfDay** | `0x140e98630` | `(daytime, todNs, ecsNow)`. Used by admin, save load and new worlds (07:00). Resets game time to day 0. | High |
| setTimeScale | `0x140e99ab0` | Only used by night skip. | High |
| isDaytime | `0x140e89080` | | High |
| Daytime_applySync (client) | `0x140e86280` | Applies `ServerInfoMessage.daytimeSync`. | High |

**Weather is local, not global.** Each weather layer runs one moving rain/snow/fog "phenomenon".
- Server decides in `WeatherSystem_updateLayers` `0x140a9d730`: weighted random pick when a layer's `nextPhenomenonTime` is reached (layers at `ws+0xC8`, stride 0x18), then `spawnPhenomenon` `0x140a9c700`. Called from the GameServer tick (`0x140e46070` → `0x140a9dd90`), not from an ECS program.
- The weather clock is the Daytime game time. `weatherFrequency` multiplies only Rain/Blizzard chances (0, 0.5, 1, 2).
- Force weather: set `nextPhenomenonTime = 0`, or hook `spawnPhenomenon` and swap the index.
- Caveat: `setTimeOfDay` resets game time to day 0, which can stall weather until `nextPhenomenonTime` is reset.

## 3. Plumbing (Tier 3)

Full details and signatures: [`engine_parts/plumbing.md`](engine_parts/plumbing.md).

### 3.1 Save/load
| Function | Address | What it does | Conf. |
|---|---|---|---|
| Save2Thread loop | `0x140dfebb0` | Background save thread. | High |
| save tick | `0x140e4cd10` | Host only. | High |
| **save state machine** | `0x140e4d9d0` | Post-hook here to save mod data (sidecar file). | High |
| ECS world snapshot | `0x140985470` | Serializes `savegame_relevant` components. | High |
| blob writer (save thread) | `0x140e030e0` | | High |
| **load** | `0x140e4a4f0`, called from `0x140e3c540` | Post-hook `0x140e3c540` to load mod data. | High |
| client character save | `0x140dc0a30` | Written from the server's final save. | High |
| ECS savegame JSON loader | `0x14098c7a0` | Parses `templates`/`entities`/`guid`/`ctype`/`dtype`/`data` keys. No direct callers (debug/JSON path?). | Medium |

**Correction ([`engine_parts/re_saves.md`](engine_parts/re_saves.md)):** the `.ksd` files are the **live** save format. Two backends:
- Steam: `ISteamRemoteStorage::FileWrite` → `<Steam>\userdata\<id>\1203620\remote\character<N>.ksd`, `world<N>.ksd`, `global.ksd`; rolling `backup\<file>.backup-0..9` stay local.
- Local: `%USERPROFILE%\Saved Games\keen games\Enshrouded\default\`.
- 16 character slots and 10 world slots per backend; slot id u16 = (storage << 8) | (index + 1). createWorld refuses used slots.
- Host autosaves the world every 30 s; quitting saves (waits up to 10 s). A client also writes its character file from the host's stream.
- Sidecar hooks for mods: post-hook `0x140e030e0` (save written) / `0x140e3c540` (load), keyed by (storage, slot) plus the world GUID at `save2+0xB7294`. Never put files in the save roots (the migration scanner picks them up).

### 3.2 Multiplayer: host vs client
- **Session mode** at `gameApp+0x34FA8`: 0 Lobby, 1 Host_Offline, 2 Host_Online, 3 **Client_Online**. Play-state steps in `0x140849090`.
- The GameServer host thread (`0x140e34060` → `gameApp+0x630`) is created at every startup, but **in Client_Online the `GameServerCreate`/`GameServerInit` steps do nothing** (the log prints the step name first). So a joined client runs no server world, and all gameplay mods must run on the host. (Corrects the guess in the summary above.)
- GameClient object is 0x6C3760 bytes at `playState+0x1B8`. The client applies replication in `0x1409cf8e0` (`processMessage` `0x1409ce210`).
- No global pointer to the game app was found; capture it from hooks. No mod-to-mod network channel was found.

Cheat sheet: server-only components (Health/attributes, Equipment, BuffComponent, villager AI, ...) exist only on the host. Clients see replicated copies (`NetworkHealth`, `ClientActiveNpcState`, ...).

### 3.3 Impact VM interpreter
- Interpreter **`0x14121afb0`**: u32 stack VM, 30 opcodes. `0x13` calls a native by name hash, `0x1C` yields to the next tick, `0x1E` ends.
- A program starts when an `ecs::Impact` component is added (`impact_initialize` `0x1403eed40`); it runs from program `impact_simulation` `0x1403ef7a0` via `runImpact` `0x1412261e0`.
- 108 natives, signature `fn(out, Slice<u32>* args, ctx)`, registered in `0x1404949b0` through `0x141220f30` (hook point for **adding custom natives**). Table: [`impact_natives.txt`](impact_natives.txt).
- Interesting natives: SpawnEntity `0x1403e3960` (the positioned spawn path used by gameplay), DestroyEntity `0x1403d98f0`, ApplyDamage `0x1403d4d50`, SetTransform `0x1403e3860`, applyBuff `0x1403d4920`, terraforming `0x1403e5b70`.
- Unknown: operand layout of opcodes `0x10` and `0x16`–`0x18`.

### 3.4 Logging / console
- **`log(u8 level, const char* fmt, FormatArg* args, size_t n)` = `0x1408a98f0`**, thread safe; a mod can call it directly. FormatArg is 24 B: `{u32 type; pad; formatFn @+8; value @+0x10}`. Types: 2 char, 3–6 u8..u64, 7–10 s8..s64, `0x13`/`0x14` plain `const char*`, `0x27` custom formatter (used for `%s`/`%k`). `%,u` / `%'u` = digit grouping.
- Levels: 0 fatal, 1 error, 2 warning, 3 info, 4 verbose, 5 debug. Per-thread default 3; `setThreadLogLevel` `0x1408a6fe0`.
- Path: log → `0x140898fa0` → up to 4 listeners → stream sink `0x1408a9e00`. **`addLogListener` `0x14087a300` is never used by the game, so all 4 slots are free**: a framework can mirror the whole game log into its own console. File is `<exe>.log` (`-log <path>` overrides; old log moves to `backup/`), set up in `0x1408fbfe0`.
- **No console, command registry, cvars or ini system in this build** (`DebugTextConsole`, `DebugUi`, `DebugMenu`, `IniVariables` init steps take 3–4 µs: empty stubs). The framework needs its own console and config.

---

## Signature check
All 247 signatures in this file and `engine_parts/*.md` were re-verified with `tools/re/sig.py` against build 1269579: each matches exactly one place in `.text`, at its documented address. The exceptions are flagged where they appear (`setThreadLogLevel` matches two functions; the hash-map helpers and some twin programs have no unique signature).

## Open questions
Things static analysis could not settle. Answered ones are struck through; the in-game results are in the "Confirmed in game" sections above.

**Runtime checks**
1. ~~Split components~~ answered: the dynamic half is keyed by its own hash (see "Confirmed in game: EMF and the sample mod").
2. ~~Shared entity ids~~ answered: yes.
3. ~~Spawn/destroy after the sim step~~ answered: works (barrel spawn + destroy), and spawned entities are written into the world save.
4. ~~`ecsNow` outside a program~~ answered: tickPeriod × tick.
5. Does SteamStub or anything else react to inline hooks in `.text`? (No integrity check was seen.)

**Needs more static work**
6. Program flag bits `0x4`, `0x100`, `0x200`, `0x400`: exact meaning.
7. ~~Own ECS program~~ answered for system programs (no entities). Programs that iterate components still need component lists (see `engine_parts/re_custom_programs.md`).
8. No global pointer to the game app or worlds was found. Capture them from hooks (world setup, sim step), as EMF does with the sim step.
9. Inventory add/remove need a 0xF0-byte setup block (only ~10 of ~30 fields known).
10. Injected recipes: do they appear in the crafting UI, and what knowledge unlock do they need?
11. Attribute id names (hashes are known for health, max health and stamina; mana is Medium).
12. Impact VM opcodes `0x10` and `0x16`–`0x18` operand layout.
13. Building: the "may I build here" check, and the arguments for calling `VoxelWorld_placeBlueprint` directly.
14. ~~Save location, slot, autosave~~ answered: `.ksd` files under Steam `remote\` or `Saved Games\keen games\Enshrouded\default\`; slot id `(storage<<8)|(index+1)`; the host autosaves every 30 s (see `engine_parts/re_saves.md`).
15. ~~Villager worker gate: meaning of actor flag `0x32`~~ answered: actor state flag 50 = `IsReadyForGameplay` (see "Confirmed in game: events and the companion").
16. ~~How a townsfolk gets its `DynamicActiveNpcState`~~ answered: spawning its template is enough, as long as that unique villager does not exist yet. Still open: what exactly happens to the duplicate (command type 1 from `unique_npc_spawns`).
