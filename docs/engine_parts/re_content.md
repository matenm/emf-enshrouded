# New content for Enshrouded: items, recipes, loot

Scope: how a mod can add a new item (for example a sword with its own name and stats that reuses an existing model and icon), new crafting recipes and changed loot tables, and which route works. The modders' page is `docs/modding/content.md`.
Game build: **1273043** (the current exe; EML `types.json` version `1273043|^/game38/branches/release_1.0|2026-10-20`).
Addresses in this file are for 1273043 unless marked "(1269579)". The repo docs use 1269579 addresses; in the inventory/loot code region the offset between the two builds is about +0x380.

Labels used below:
- **[obs]** = observed: read from game data, from the exe, or from EML source, with the evidence named.
- **[inf]** = inferred: not verified.
- **[test]** = can only be settled in game.

How the facts were found: the game data and the exe were read with the `tools/re` scripts (caches rebuilt for 1273043; the repository's `tools/re/*.pkl` are for 1269579) and the capstone disassembler, and every EML run used a sandbox copy of the game files. The in-game results are in the next section.

---

## In game: what the static analysis got right and wrong

The Emberbrand example (`content/emberbrand/`, applied with `scripts/content_test.ps1`) was tested in the local world:

- **Works:** the new ItemInfo, its ItemRegistryResource entry and the keenglish loca tag. EMF's `items` lists the
  item (4208 items), `put` places it, and the item panel shows "Emberbrand, One-Handed Sword, Damage 71 (Cutting 71,
  Fire 35.5)": the new name, `dmgMod` and the added fire damage all apply.
- **Wrong in the static analysis: icons.** Every item needs a `keen::UiTextureResource` (type hash `0xf9e86997`) stored
  under the item's own GUID, part 0. Without it the game stops with the fatal error
  `corrupted game ui texture: <item guid>:f9e86997:0` (an access violation in the deliberate crash routine) as soon
  as the item is shown. Copying the source item's UiTextureResource under the new GUID fixes it.
- **Does not work: the recipe.** A copied recipe (the campfire's, workshop 0, the same unlock requirement) is in
  the RecipeRegistryResource but the own crafting menu does not list it, and the game logs nothing. The menu's list
  probably comes from knowledge unlocks (`GameKnowledgeResource` / per-player knowledge), not from every recipe whose
  requirement passes. Where the crafting UI gets its recipe list is not known.
- **Loot:** hooking the getter `0x140e83190` (`loot_scale`, pattern `48 8B 81 90 00 00 00 F3 0F 10 40 38 C3`, unique
  in the client and the dedicated server) works: with factor 2 the first loot logged 1.00 → 2.00 and EMF's
  `give 10` wood logs dropped 20.
- **EML and the game folder:** EML rewrites `enshrouded.kfc`, appends to `.kfc_resources` and writes
  `enshrouded_055..063.dat`; `emm.exe restore` only restores the `.kfc`. `content_test.ps1` keeps its own copies and
  restores byte for byte.

## TL;DR

1. **Data patches can add new content, not just edit it.** [obs] EML 0.2.0 has `game.assets.create_resource(value, type, guid, part)` and `create_content(buffer)`. Its writer adds the new resource to the `.kfc` resource map, appends the data to `.kfc_resources` and rebuilds the per-type bundles; new content goes into a new `enshrouded_0NN.dat`.
   A full "new sword" patch was run against a **sandbox copy** of the game files, and the written files were checked with a separate reader: the new `ItemInfo` (fixed GUID, `itemId = FNV-1a(GUID)`, dmgMod 1.75, fire 0.5), the `ItemRegistryResource` entry, a new `RecipeInfo` and the item's name in the loca table are all present and correct.
2. **What the game needs for a new item** [obs]:
   - an `ItemInfo` resource;
   - its GUID appended to `ItemRegistryResource.itemRefs`, **plus** a matching entry in the parallel `dbgNames` array (`preloadItemInfos` reads `dbgNames[i]` for `itemRefs[i]`);
   - a loca tag for the name (`LocaTagId = FNV-1a(name GUID)`).
   The game's loca lookup falls back to **keenglish** for every language (`0x1411fa7c0`), so one new tag in the keenglish blob names the item in all languages.
   ~~Icons need nothing~~ **Wrong (see "In game" above):** every item also needs a UiTextureResource under its own
   GUID; copy the source item's.
3. **Saves store items by ItemId (u32 = FNV-1a of the GUID), never by GUID** [obs, `keen::ds::ItemStack` / `ds::ecs::ItemState`]. A mod item therefore needs a **fixed GUID forever**. EML's `create_resource(value, type)` without a GUID picks a random v4 UUID on every run, which would orphan saved items.
   If the mod is removed, the inventory code deletes unknown stacks: `validateItemStack` `0x140410de0` logs "Removed invalid Item from Stack." and zeroes the stack. The UI draws "[Invalid Item Stack]".
4. **Multiplayer:** each process builds its own registries from its own files. The network carries only ItemIds and RecipeIds, and no data-hash handshake was found (only Steam's "Version Mismatch"). The host and every client must have identical content, and a mismatch is not detected.
5. **Loot:** a "Loot Amount" multiplier already exists in vanilla: `GameSettings.resourceDropStackAmountFactor` (+0x38; presets 0.25–2.0; tooltip "Scales the amount of materials per loot stack").
   Its only reader is the getter `0x140e83190` (`BalancingRegistry+0x90 → GameSettings+0x38`). That getter is called by default-inventory application, random container loot and both world-pickup paths.
   A loot-multiplier mod is a one-value write, or a hook on that 13-byte getter.
6. **The route EMF uses:** an **EML data patch shipped next to EMF**: a Lua mod with fixed GUIDs (the Emberbrand example in `content/emberbrand/`).
   EMF's `find_item`/`give`/`put` find the new item by its debug name. A runtime route without game-file changes is described in §2.4; EMF does not have one. Loot scaling uses the getter (§4.2; the `loot_scale` mod).

---

## 1. How the game stores content

### 1.1 Files and EML [obs]
- **`enshrouded.kfc`** (9.4 MB) is a KFC4 directory: version string, container list, a resource static map (91,812 resources), a content static map (101,459 blobs) and 146 resource bundles (one per type).
- **`enshrouded.kfc_resources`** (214 MB) holds the **resources**: reflected structs, zstd-compressed in 8 MiB chunks. A resource is addressed by (GUID, type qualifiedHash, part).
- **`enshrouded_000..054.dat`** (55 × ~1 GB) hold the **contents**: raw blobs addressed by a ContentHash `{size, hash0..2}`, 4 KiB aligned. Loca tables, textures and meshes live here.
- Evidence: kfc-parser source (`crates/kfc-base/src/container/{header,file,reader,writer}.rs`, fetched with `gh api`) and a read-only reader of the real header and `.dat` loca blobs.

**What EML 0.2.0 does** [obs, EML source + docs at brabb3l.github.io/kfc-parser]:
- Mods are Lua with `capabilities` `patch` and/or `export`; `runtime` is a stub. They run once before the game reads the data: through the `dbghelp.dll` proxy at process start, or offline with `emm.exe run -g <dir> --patch`.
- On patch, EML:
  - copies `enshrouded.kfc` to `.kfc.bak` (reads always come from the `.bak`, so patches are re-applied from vanilla each time);
  - appends changed and new resources to `.kfc_resources`;
  - writes new content into new `.dat` files and pads the container count to a power of two (one run created `055.dat` plus empty `056..063.dat`);
  - rewrites the `.kfc` header and rebuilds the bundles.
  A file-state cache skips patching when nothing changed. `emm.exe restore` copies `.kfc.bak` back; the appended data stays in place, unreferenced.
- **New resources: yes.** `create_resource(value, type)` takes a random v4 GUID (`app_state.rs`: `uuid::Uuid::new_v4()`). `create_resource(value, type, guid, part)` takes a **fixed** GUID. `table.insert/remove` work on mapped arrays (`env/table.rs`).
- **New content: yes.** `create_content(buf)` returns a ContentHash that can be assigned to `ResourceContentReference.hash`.
- Writable types: all 146 resource types, including `ItemInfo`, `RecipeRegistryResource`, `LootableItemsResource`, `DefaultInventoryResource`, `TemplateResource` (component data), `BalancingTable`, `GameSettingsPresetsResource` and loca (as content).
- Limits: it cannot change code. The `create_content` hash must match Keen's content hash. The game has a "[kfc] Verify Content … Wrong content hash" pass (`0x141247cd0`), apparently only a verify mode [inf]. EML texture mods are used in the community, so EML's hash is accepted [inf].
- Content hashes seen in the game data: keenglish loca `{1361145, 724576563, 2753418770, 1243646054}`, stored in `enshrouded_053.dat` at 0x35ad0000.

### 1.2 Resource types involved (counts from an EML export of the sandbox copy) [obs]
| Purpose | Type (types.json) | Count | Notes |
|---|---|---|---|
| Item definition | `keen::ItemInfo` (2088 B struct; resource blob 2.1–13.3 KB, Bronze Sword 5409 B) | 4262 | `objectId == resource GUID` and `itemId == FNV-1a(GUID bytes, MS GUID byte order)` for all 4262 |
| Item list | `keen::ItemRegistryResource` | 1 | `itemRefs` 4207, `dbgNames` 4207 (parallel), `itemTags` 210, `weaponCategories` 16, `blueprints` 52. The 55 ItemInfos **not** in `itemRefs` (DEPRECATED/TEST items) are never loaded |
| Recipes | `keen::RecipeRegistryResource` | 1 | `recipes` 2536 (`RecipeInfo` 168 B, inline array), `inputCategories` 22; `recipeId == FNV-1a(recipeGuid)` for all; 46 workshops, at most 338 recipes per workshop (the engine limit is 512) |
| Loca | `keen::LocaTagCollectionResource` (GUID 163bb0a2-dc73-428b-b01a-a11e2eff947c, hard-coded in app init `0x140848130`) | 1 | `keenglishData` + 15 `languages` (En_Us … Uk_Ua), each a `ResourceContentReference` to a content blob holding `LocaTagCollectionResourceData { BlobArray<LocaTagResource{id u32, text, args, genericArgs}> }`, **sorted by id** (keenglish 17,228 tags, En_Us 16,715) |
| Item knowledge | `keen::ItemKnowledgeResource` | 1 | StaticHashMap: 8192 buckets `{start,count}`, 4480 keys (ItemId) and values (`ItemKnowledge`: name/description/lore/recipe locks, mostly just "Inspected"). Bucket = `key & 8191` (checked for all 4480) |
| Knowledge queries | `GameKnowledgeQueryResourceDb`, `GameKnowledgeResource`, `GameKnowledgeQueryTriggerResource` | 1 each | recipes point at queries through `RecipeInfo.knowledgeRequirement` (Extern 2191, SimpleFlag 238, SimpleBool 107) |
| Random loot | `keen::LootableItemsResource` | 1 | 4207 `LootableItemInfo` (one per registered item): rarities, `stackSizeMin/Max/MaxScaled`, `stackSizeScalable`, shufflebag type, `labelGroups` (tag weights; 913 items have labels); `globalRarityDropRates` = [40.59, 27.35, 17.06, 9.71, 5.29] |
| Scene loot | `keen::SceneRandomLootResource` | 5 | only the main scene (509feadb…) is filled: 700 `lootSettings` (modifiers + tags), 661 `specialItems`, 61,918 `lootContainer` |
| Label sets | `keen::DefaultLootLabelCollectionResource` | 1 | 99 `lootSettings`, referenced by `ecs::RandomLoot.defaultLabels` on templates |
| Fixed / enemy drops | `keen::ecs::DefaultInventoryResource` | 420 | trees of groups/selectors with stacks `{itemRef GUID, item id, countMin, countMax, spawnProbability, weight}`; used by the `ecs::DefaultInventory` component |
| Other loot | `fishing::FishSpawnTableResource` 30, `fishing::TrashLootTableResource` 7; `ecs::ResourceNodePickupDrops` (template component: mining/harvest drops) | | |
| 2D icons | `keen::UiTextureResource` | 5699 | only 273 items set `ItemInfo.iconImage`; the others (all weapons) render the icon from `iconModel`/`iconScene`/`iconRenderOffset` |
| World settings | `keen::GameSettingsPresetsResource` | 1 | `minValues`/`maxValues`/presets; `resourceDropStackAmountFactor` 0.25…2.0, presets 1.5/1/1/0.75 |

### 1.3 What an item references (Bronze Sword `Weapon_T1_1H_SwordEpic_01`, id 0x43108313, GUID 1d622bd9-8170-41b1-822e-d3984b386d95) [obs, EML export of the item]
- **Name / description:** `name` = GUID 1702b218…, `description` = 45bdde79…. Both are `ObjectReference<LocaTag>`. The text is `loca[FNV-1a(GUID)]`. EML's own example (`examples/basic/export_item_list.lua`) does the same with `game.guid.hash(data.name)`, and joining the export with the loca table resolves 4202 of 4202 non-null names against keenglish.
  Note: keenglish holds Keen's source strings ("Bronzen Sword"); En_Us holds the shipped text ("Bronze Sword").
- **Visuals:**
  - `equipment.visualEntity` (template fc562683…) and `equipment.equipmentEntityModelSetup.model` 9c38c7f0…;
  - `iconModel` 9c38c7f0… (the same model) and `iconScene` 1f663079…;
  - `iconImage` is null.
- **Behaviour and stats:**
  - `equipment.impactEntity` 7100f7c2…, `sequences` (attack/special sequences, some carrying knowledge triggers);
  - `pidEntity` dfc38033… (the per-item-data template **shared by all one-handed swords**);
  - `damageSetup {speedMod 1, dmgMod 1, distribution {physicalCut 1, wood 4, stone 4, metal 2, …}}`;
  - `impactValues.simple` (6 variants);
  - `perkReferences`/`perkIds` (5);
  - `itemLevelRange 1..55`, `weaponCategoryReference` d52db28f…;
  - `flags` [HasPIDE, IsSalvageable], `tags`, `collectionInfo`.
- **A new sword can reuse all of these GUIDs.** Only `itemId`, `objectId`, `name` (and optionally `description`), `debugName` and the stat fields change.

---

## 2. How the registries are built at runtime and what references items

### 2.1 ItemRegistry (one per world: server and client) [obs]
- `createItemRegistry` `0x140e67540` (1269579: `0x140e64980`), called from the world setups.
- `ItemRegistry_update` `0x140eab900` runs every frame:
  - **Phase 0:** once `ItemRegistryResource` is loaded, it reserves the maps with `0x140e64c00`: capacity = **2 × next_pow2(max(16, n))**, so 16,384 slots for 4207 items, which leaves plenty of free slots. It then calls `preloadItemInfos` `0x140e8a9d0`.
  - **`preloadItemInfos`** loops over `itemRefs`. For each GUID it inserts a load entry into the GUID map `+0xe8` (key = {type 0xb5ce8765 = ItemInfo, GUID}, 0x30-byte entries), requests the resource (`0x1410661d0`), and inserts `FNV-1a(GUID) → dbgNames[i]` into `+0x90`.
  - **Phase 1:** for each loaded ItemInfo it inserts `itemId → ItemInfo*` into **`+0x38`**. The insert uses slot finder `0x140e917f0`, the bitset at `+0x40`, keys at `+0x60`, values at `+0x78` and `++count` at `+0x8c`.
    It also adds the item to two side maps, only for voxel/building items (`ItemInfo+0x444/+0x454` → `+0x140`) and overgrowth items (`+0x4e0` → `+0x198`). Neither applies to weapons.
  - **Flags:** `+0x320` = "loading", `+0x321` = "started".
- **Lookups:**
  - `ItemRegistry_find(id, reg)` `0x140e7d430` (341 callers) = `HashMap.find(reg+0x38)` → `ItemInfo*` or NULL.
  - `ItemRegistry_contains` `0x140e8e280` (true even while the value is still NULL during loading).
- **Known from EMF:** ItemInfo memory is read-only after load, both worlds share the same ItemInfo memory (`emf_api.h` item_data), and the registries are rebuilt for every world session.

### 2.2 RecipeRegistry (one per world) [obs: docs + code]
- `createRecipeRegistry` `0x140e68570` and `RecipeRegistry_finishLoading` `0x140eaec20` build three maps from `RecipeRegistryResource`:
  - `+0x48` RecipeId → `RecipeInfo*`;
  - `+0xA0` input categories;
  - `+0xF8` workshopId → SmallDynamicArray<RecipeInfo*, 512>.
- `RecipeRegistry_find` is `0x140e82790`; the craft core is `0x140471290` and `applyRecipe` `0x1403f2030` (signatures from `docs/engine_parts/inventory_crafting.md` still match).
- A recipe carries its `workshopId` and two knowledge queries:
  - `knowledgeRequirement`, mostly `Extern` queries: the real unlock;
  - `completionRequirementQuery`, a `SimpleFlag` on the recipe's own id: the "crafted once" tracking.
  Copying the knowledge requirement of an existing recipe gives a new recipe the same unlock condition [inf] **(not enough in game: the copied recipe is not listed, see "In game")**.
  Workshop 0 is the player's own crafting menu (57 recipes, for example `Tool_basic_FishingRod_T0_PK`); 0x41bf48ce is the Blacksmith (Scrappy Sword) and 0x841ad755 the Hunter.

### 2.3 Localization at runtime [obs, disassembly with capstone]
- One application-level **LocaPool** (0x128 B) is created by `0x1411f9c40` during app init (`0x140848130`). It loads `LocaTagCollectionResource` (type hash 0x3f13ff35) and holds three language slots at `+0x40`, `+0x90` and `+0xe0`; the current language id is at `+0xd8`.
- **Lookup** `LocaPool_lookup(Result*, LocaPool*, u32 id)` = **`0x1411fa7c0`** (sig `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 33 FF 48 8B F2`). It:
  1. returns error 0x12 "Invalid Tag Id" for id 0;
  2. returns a special constant for id `pool+0x18` (0x69691905);
  3. otherwise searches slot `+0xe0` (if loaded), then `+0x40` if the language is 0, then `+0x90` (the current language), then **`+0x40` (keenglish) as the final fallback**;
  4. returns error 3 "Loading" if keenglish is not loaded yet, or 0x1f "Unknown Tag Id" if no slot has the id.
- Per slot, `0x1411fa0d0` runs a lower_bound binary search over the sorted 24-byte `LocaTagResource` entries in the content blob at `slot+0x20`.
- `0x1411fa6e0` (58 call sites) wraps the lookup and turns errors into the strings "Unknown Tag Id", "Invalid Tag Id" and "Loading".
- Consequences:
  - **Data route:** insert the new tag (in sorted position) into the keenglish blob only, and it shows in every language.
  - **Runtime route:** detour `0x1411fa7c0` and answer the mod's own ids first. No LocaPool layout knowledge is needed.

### 2.4 Could a runtime mod clone an item and a recipe? Yes, with these steps [obs for the mechanics, inf for completeness]
1. **Clone the ItemInfo:** copy the **whole resource blob** (Bronze Sword: 5409 B). Every BlobArray/BlobString offset is self-relative inside that blob, so a straight memcpy stays valid; copying only the 2088-byte struct would break every blob field. Then set:
   - `+0x00` itemId = FNV-1a(new GUID);
   - `+0x04` objectId = new GUID;
   - `+0x48` name = new name GUID;
   - `+0x67c` damageSetup (speedMod, dmgMod, distribution);
   - `+0x608`/`+0x658` perks.
   A new debug name needs an appended string and a re-pointed `+0x820` BlobString.
   The resource system's "get data" (`0x1410630d0` (1269579), `(out, resourceSystem, id)` → pointer/size) or `item_data` plus a known size gives the source.
2. **Insert into `ItemRegistry+0x38` of every live world** (server and client registry objects), with the same probing as phase 1: `slot = id & (cap-1)`, linear probing, set the bitset bit, key, value and `++count`. The capacity has room.
   Do it right when phase 1 finishes (hook `0x140eab900` and watch `+0x320` go 1 → 0) and again for every new session.
   **Do not** insert into the `+0xe8` GUID map: its entries own resource handles that the registry releases [inf].
3. **Name:** detour `LocaPool_lookup` `0x1411fa7c0` and return a StringView for FNV-1a(new name GUID).
4. **Recipe:** build `RecipeInfo` (168 B) with its input/output arrays placed directly after it (self-relative offsets). Insert it into `RecipeRegistry+0x48` and the workshop list at `+0xF8` (u16 count at `+0x1000`) in **both** worlds. This is already sketched in `inventory_crafting.md` §5b.
5. **What a runtime clone misses compared with a data patch** (tables built from resources at load that the clone is absent from):
   - `ItemKnowledgeDatabase` (client): the item has no entry, which probably means "no locks" [inf/test];
   - `LootGenerationSystem` item pools: the item never drops as random loot (fine for a crafted item);
   - collection/journal sorting (`[sortItemCollections] Could not find item info…`);
   - GUID → ItemInfo lookups through the resource system or the `+0xe8` map: any code that resolves `ItemReference` GUIDs instead of ids will not find the clone [test].
   - **Timing risk:** if a saved inventory holding the item goes through an inventory transaction before the insert, `validateItemStack` deletes the stack [obs: code; inf: ordering].
6. This route needs core support that EMF does not have: signatures for `0x1411fa7c0` and `0x140eab900` (or a registry-ready event), an item-map insert helper (`src/common/keen_map.*` already reads Keen maps) and a recipe insert helper.

### 2.5 Other references to items
- **Save format** [obs, types.json `ds` types]:
  - `ds::ecs::Inventory` = 8 × `ds::ItemStack {ItemId u32, count u32, pide EntityId}`;
  - the pide entity's `ds::ecs::ItemState.itemId` is a u32;
  - no GUIDs are stored, so the ItemId (= FNV-1a of the GUID) is the permanent identity.
- **Mod removed** [obs: code]: `validateItemStack` `0x140410de0` (called from commit `0x140408ed0`, `0x14040bae0` and `0x140410d90`) checks `ItemRegistry_find(id) == NULL && !contains(id)`. In that case it logs "Removed invalid Item from Stack." and zeroes id, count and pide; the pide entity is not destroyed there, so it stays as an orphan [inf]. The UI has a "[Invalid Item Stack]" placeholder (`0x140d06530`).
  Whether every other path (equipped slots, `item_state` programs, client render) null-checks is unknown, so removal with the item **equipped** is the case to test [test].
- **Recipes in saves:** only the knowledge flags (`completionRequirementQuery` uses the RecipeId) [inf].
- **Loot tables** reference items by ItemId plus GUID (`LootableItemInfo.itemId`, `ItemStackInfoResource{itemRef, item}`, `SpecialItemInfo.itemId`).
- **`GameKnowledgeQueryResourceDb.queriedItems`:** items whose possession feeds knowledge queries. A new item only needs to be there if it unlocks something.

---

## 3. Multiplayer

- [obs] Every process loads its own `.kfc` and builds its own ItemRegistry, RecipeRegistry and LocaPool. Servers and clients exchange ItemIds (Inventory/ItemState components), RecipeIds (`PlayerCraftingAction.recipeId` → server `handle_crafting_events`) and templates by id.
- [obs] No game-data version or hash check was found in the exe strings; the only "Version Mismatch" is in the Steam init.
  [inf] An unmodded client can therefore join a modded host without any warning.
- **Host has the item, the client does not:**
  - the client UI shows "[Invalid Item Stack]";
  - client-side prediction (`predict_player_inventory`) may run `validateItemStack` locally [inf];
  - the equipment model of a player holding the item may be missing on that client [test].
- **Recipe only on one side:**
  - host only: clients never see it in their crafting UI (the UI uses the client registry);
  - client only: the server answers "[crafting] Invalid recipe id '%k'".
- **Conclusion:** host and all clients must install the **same** content mod with the **same fixed GUIDs**.
  - EML patches are deterministic per install if the GUIDs are fixed: everyone runs the same Lua on vanilla data.
  - An EMF runtime mod runs on clients too, since the dinput8 proxy loads in every client.
  - Dedicated servers use `enshrouded_server.exe`/`.kfc`: EML supports them (`file_name enshrouded_server`).

---

## 4. Loot

### 4.1 How loot is rolled [obs: programs from `docs/ecs_programs.md` and call graph; [inf] for the details inside the generators]
- **Containers and scene loot:** `random_loot_on_interaction` (`0x140403370`) fills a container when it is first opened.
  It uses the `g38_loot::LootGenerationSystem` singleton (built by `update_loot_generation_system`), `DefaultLootLabelRegistry` and `g38_loot::LootShufflebag`.
  Containers carry `RandomLoot{defaultLabels}`, `RandomLootContainer`, `RandomLootSlotCount` and `RandomLootProbability`. Items are picked by label/tag weights (`LootableItemInfo.labelGroups` against the container's lootSettings tags) and rarity rates.
- **Enemy drops and fixed chests:** the `DefaultInventory{DefaultInventoryResource GUID}` component is applied by `default_inventory` → `applyDefaultInventory` **`0x1403f3610`** (1269579 `0x1403f3290`). The admin "apply inventory" in `inventory_actions` uses the same function.
- **Other sources:**
  - world pickups: `random_pickup_loot_spawner`, `loot_pickup_in_range`, `loot_pickup_item`;
  - destructibles: `spawn_random_loot_on_destroy`;
  - mining/harvest: `mining_node_events` + `ResourceNodePickupDrops` template data;
  - multiplayer scaling: `random_loot_multiplayer_scale` → `RandomLootPlayerScale`;
  - fishing tables.

### 4.2 Simplest loot multiplier: the built-in "Loot Amount" setting [obs]
- `ecs::GameSettings.resourceDropStackAmountFactor` sits at **+0x38**. Its En_Us tooltip is "Loot Amount: Scales the amount of materials per loot stack". The presets range is 0.25–2.0 (`GameSettingsPresetsResource`).
- The only code that reads it is the 13-byte getter **`0x140e83190`** (`mov rax,[rcx+0x90]; movss xmm0,[rax+0x38]; ret`, sig `48 8B 81 90 00 00 00 F3 0F 10 40 38`). It runs on `BalancingRegistry`, whose `+0x90` points at the live GameSettings (as in `world_building_daytime.md`).
- The getter has **5 call sites**, all loot:
  - `applyDefaultInventory` `0x1403f3610`;
  - random container loot `0x1403f3a70` (called from `random_loot_on_interaction`);
  - `0x1403f4990` (from `loot_pickup_in_range`) and `0x1403f4e00` (from `loot_pickup_item`);
  - `random_loot_on_interaction` itself (`0x140403370`).
  Mining nodes do **not** use it.
- Two EMF ways to use it:
  1. Write the float: `singleton(server,"BalancingRegistry")` → `*(+0x90)` → `+0x38` = e.g. 4.0. This needs no new EMF core. Caveats [inf/test]: the value is part of the world settings, so it is probably saved; it may be clamped or reset when the host opens the world settings; it is replicated to clients for display.
  2. Detour the getter and return `original × N`. Nothing reaches the save, but EMF needs a new signature.
- Whether the factor scales every stack or only scalable materials (`LootableItemInfo.stackSizeScalable`) is decided inside the generator (`0x1403f99d0`) and was not traced; the tooltip says "materials".

### 4.3 Bigger loot changes
- **Data (EML):** multiply `stackSizeMin/Max/MaxScaled` in `LootableItemsResource`, `countMin/countMax` in the 420 `DefaultInventoryResource`s, `ResourceNodePickupDrops.drops[].stackAmount` in templates, or `RandomLootSlotCount`. Raise `globalRarityDropRates` for better rarities.
- **New item as loot:** append a `LootableItemInfo`, for example a copy of the Bronze Sword's with the new id; keep its labelGroups to drop like a Bronze Sword, or empty them to never drop. Alternatively add a stack to a `DefaultInventoryResource` group (an enemy drop table).
- **Runtime alternative for full coverage, mining included:** detour `addItems` (`0x14040b4f0`) and scale `count` when the return address is inside the generation callers (applyDefaultInventory, random loot fill, `mining_node_events`, the pickup spawners). Do **not** scale the container → player transfer (`immediate_loot_action`), or items get duplicated on every transfer.

---

## 5. Routes

### 5.1 EML data patch shipped next to EMF (the route EMF uses)
Why this route:
- The engine builds every table from the data, so nothing in §2.4.5 is missing: ItemRegistry incl. the GUID and debug-name maps, knowledge, loot pools, client and server, saves.
- It needs **no new reverse engineering and no EMF core change**. The Emberbrand example (`content/emberbrand/src/mod.lua`) is such a patch. It does these things:
  1. `create_resource(bronzeSword.data, "keen::ItemInfo", FIXED_GUID, 0)`, then sets `itemId`, `objectId`, `name`, `debugName`, `damageSetup.dmgMod = 1.75` and `distribution.magicalFireDamage = 0.5`;
  2. copies the source item's `UiTextureResource` under the new GUID (the icon, see "In game");
  3. `table.insert` into `ItemRegistryResource.itemRefs` **and** `dbgNames`;
  4. copies a recipe, sets new `recipeGuid/recipeId`, output = the new item and `completionRequirementQuery` = the new id;
  5. reads the keenglish loca blob, inserts `{id = hash(nameGuid), text}` at its sorted position, writes a new content blob and assigns it to `lc.keenglishData.hash`.
- Checked in the sandbox copy with a read-only reader of the patched files:
  - the new ItemInfo resource (4263 ItemInfos), size 5410, with the right `itemId`, `objectId` and name GUID, dmgMod 1.75 and fire 0.50;
  - `ItemRegistryResource` with 4208 refs and 4208 dbgNames, the last entries the new item's;
  - `RecipeRegistryResource` with 2537 recipes, the last one outputting 0x01b9f0cd;
  - the keenglish content moved to `enshrouded_055.dat`, still sorted, with tag 0xfeb9ec14 → "Emberbrand (EMF test)".
- A recipe for the player's own crafting menu needs a **workshop-0** source recipe such as `Tool_basic_FishingRod_T0_PK` or the campfire's (the example copies the campfire's). `Weapon_ScrappySword_T1_Grassland` is a Blacksmith recipe, which a level-1 character may not have. EMF's console can also hand the item over: `give Weapon_T1_1H_Sword_Emberbrand` (find_item works by debug name).

### 5.2 Risks
- **Game files are modified:** EML rewrites `enshrouded.kfc`, appends to `.kfc_resources` and adds `055–063.dat`.
  - `emm.exe restore` undoes the header; Steam "verify files" fixes the rest (and re-downloads `.kfc_resources`).
  - Applying it to the real game folder needs the user's OK. Use only a LOCAL test character and world, and run `scripts/save_guard.ps1` first.
- **Saves:** items are keyed by FNV-1a(GUID), so the GUIDs must never change; never use the random-GUID `create_resource` form.
  Removing the mod deletes the stacks on the next transaction. The equipped and pide paths are untested.
- **Multiplayer:** every player and the host need the identical patch, and nothing warns about a mismatch.
- **Updates:** Steam replaces `enshrouded.kfc`; the EML proxy or CLI re-applies the patch on the next run (the Lua works on the new data). The patch breaks only if the reflected layouts change (1269579 → 1273043 had identical types) or a source GUID such as the Bronze Sword disappears.
- **EML maturity:** 0.2.0 is young, and no public mod is documented to create new items with it. `runtime` is a stub.
- **Tooling coexistence:** EML's proxy is `dbghelp.dll`, EMF's is `dinput8.dll`; they can live side by side. Never install EML's dinput8 fallback next to EMF.

### 5.3 Loot multiplier
The `loot_scale` mod detours the getter `0x140e83190` (§4.2) and returns the value times its factor; nothing reaches the save. The other way, writing `GameSettings.resourceDropStackAmountFactor` on each `server_session` load (and restoring it on quit), is not tested: whether a value above 2.0 is clamped and how it shows in the world settings UI are unknown.

### 5.4 A runtime content API (no game-file changes)
EMF does not have one. It would be `emf_register_item(clone_of, guid, name_guid, edits…)`, `emf_register_recipe(…)` and a loca override table, built from §2.4 and §2.3. Another option is for EMF to run the EML patch step itself.

---

## 6. Open questions

1. **[test] The new item in game.** Confirmed: the name, the stats, the icon (with the copied UiTextureResource), `items`/`put`. Not tested: crafting it (the recipe is not listed, see "In game"), equipping it and hitting with fire damage, a save and load, and removing the patch (`emm.exe restore`, then watch `enshrouded.log` for "Removed invalid Item from Stack." and for a crash), also with the sword **equipped**.
2. **[test] Loot Amount by a write:** GameSettings+0x38 = 4.0 from a mod, compared with 1.0 on a fresh chest and an enemy drop; whether it is clamped and whether the value is saved.
3. **[RE] For a runtime API:**
   - when saved inventories are first validated relative to `ItemRegistry_update` phase 1;
   - how the client crafting UI enumerates recipes (`unlocked_recipes_ui`, workshop map vs. knowledge DB);
   - the `ItemKnowledgeDatabase` lookup (what a missing entry means);
   - who resolves `ItemReference` GUIDs through the `+0xe8` map (its insert is `0x140e93320` in `preloadItemInfos`; its lookup and that lookup's callers are not known) or through the resource system;
   - whether `render_equipment_models_ag2` and the `item_state` programs null-check `ItemRegistry_find`.
4. **[data] Other entries a patch can add:**
   - an `ItemKnowledge` entry: StaticHashMap insert = put the key and value at `start+count` of bucket `id & 8191`, `count++`, `start++` for all later buckets;
   - a `LootableItemInfo` entry (empty labels = never drops);
   - a translated name: the same tag in the En_Us blob gives a proper English string; the keenglish fallback already covers it.
