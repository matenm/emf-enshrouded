# When the game updates

EMF finds the engine functions it needs (the "signatures") and a few data offsets inside `enshrouded.exe` (or
`enshrouded_server.exe`). A game update moves code around, so EMF checks which build it is running in at every start:

- **A known build** (its SHA-256 is in EMF's table: 1269579 and 1273043, and the dedicated server of 1273043):
  every signature has a recorded address, and the data offsets are the recorded ones. The log says
  `game sha256 ... (known build 1273043)` and `signatures: 38 of 38 usable` (on the server
  `... usable (N not in this exe)`: client-only functions, such as the client world's setup, are not in the server exe).
- **An unknown build** (the log says `UNKNOWN build: signatures only`): EMF searches each signature by its byte
  pattern and uses those that match exactly once. Features whose
  signatures or offsets cannot be confirmed are turned off, each with a log line naming what is off (for example
  `world_tick` returns 0 and the `time` command refuses when the world's tick fields are not confirmed;
  `put_items` / `take_items` refuse when the inventory transaction is not usable). Everything else keeps working.

## Check a build without starting the game

```
tools\emf_sigcheck.exe "C:\SteamLibrary\steamapps\common\Enshrouded\enshrouded.exe"
```

(`tools\emf_sigcheck.exe` in the release zip; `build\msvc\emf_sigcheck.exe` when you build EMF from source.) A
dedicated server's `enshrouded_server.exe` is checked the same way.

It prints every signature and code offset with OK / FAIL (`--` for a function the exe has no copy of, as recorded) and
exits with:

| Exit | Meaning |
|---|---|
| 0 | a known build, and every signature and code offset matches |
| 1 | something does not match |
| 2 | everything found matches once, but the build is not in EMF's table (it prints the addresses it found) |
| 3 | the check itself failed: no path, or the exe could not be read |

In the EMF source, `scripts\verify.sh` runs the tests and this check, and fails on anything but 0.

`emf_sigcheck --patterns <file> <exe>` checks a mod's pattern file instead (next section): exit 0 when every pattern
is found exactly once, 1 when one is not (or a line is bad: `BAD` lines), 3 when the file or the exe cannot be read.

## What to do as a mod author

- Check API results: a null component, a `false` from `put_items`, a 0 from `spawn` are how a turned-off feature shows.
  `emf::World::get<T>` already refuses components whose size changed.
- Component and event layouts come from the game's own type registry (`sdk/include/emf/generated/components.hpp`).
  After an update they are regenerated from the new registry (`tools/` scripts); a struct whose size changed is
  refused by `get<T>` and `emit<E>` instead of corrupting memory. Rebuild your mod against the new headers.
- Template GUIDs and item names rarely change; item ids are derived from GUIDs.
- If your mod finds engine code by pattern (`find_pattern`, `hook_pattern`), keep the patterns in `<id>.patterns`
  next to your mod, one per line: `name = bytes [; offset=N] [; rel32] [; tail=N] [; section=.rdata]`, `#` starts a
  comment (the format in [reference.md](reference.md); `mods\sim_rate\sim_rate.patterns` and
  `mods\loot_scale\loot_scale.patterns` are examples). Run `emf_sigcheck --patterns <id>.patterns
  "<game>\enshrouded.exe"` after each update: a pattern that no longer matches once is the place to look (the game's
  code around it changed). The same works against `enshrouded_server.exe`. Prefer patterns from a function's first
  bytes, long enough to match once; wildcard (`??`) addresses and offsets that the compiler may change.

## What to report

If a feature you need is off after an update, the useful information is the first 40 lines of
`<game>\emf\emf_core.log` (build hash, signature counts, what was turned off) and `emf_sigcheck`'s output. Adding a
new build means recording its addresses in `src/common/signatures.cpp` (and the code offsets in
`src/common/code_offsets.cpp`); `README_EMF.md` in the EMF source has the steps.

## Lua 1.9 (Necromancer): what is new

Nothing to change: every Lua mod for 1.8 runs as before. New:

- **Wheel groups:** `emf.wheel_section(group, title, labels, fn)` publishes your part of a wheel that several mods share;
  `emf.wheel_group(group, title, key)` opens one wheel with every mod's section (a pick runs only its owner's
  `fn(world, index, label)`, with the label shown, and never a section of a paused or reloaded mod). Beastmaster and Necromancer share the group `"summons"`. Depend on
  `"lua": ">=1.9"`.

## Lua 1.8 (Beastmaster): what is new

Nothing to change: every Lua mod for 1.7 runs as before. New:

- **Lua host 1.8.0:** `world:template(component, id)` reads the template half of a component (the data every entity of
  that template shares; for example `world:template("ModelResource", id).model`, the model GUID as text) through the C
  API's `get_template_component`. Read-only: writing a field, or a field of a nested struct or array, is an error.
  Depend on `"lua": ">=1.8"` for it.
- **New mod:** `beastmaster` (Beastmaster, Lua: tame and call wild beasts; `/beastmaster` for every player), installed
  switched off. Its bear is an EML pack built with `tools/models` (`tools/models/packs/beastmaster_bear.json`).
- **Tools:** `tools/models/make_mod.py` packs take `template_add` (whole components, e.g. `ScaleComponent`) and
  `template_set` (fields, a value or `{"mul": n}`); `scripts/package.ps1 -ExtraContent <folder>` ships prebuilt EML
  packs in the zips' `content\`.

## API v20 and Lua 1.7: what is new

Nothing to change:
every mod built for an older API still loads (the table only grew). New:

- **C API v20: teleport.** `teleport(world, &EmfTeleport)` moves a player or NPC the way the game's own fast travel
  does, with the game's ground search (`EMF_TELEPORT_FIND_GROUND`), a spawn point's landing pads (`spawn_point`), a yaw,
  a replication checkpoint, and a callback with where the entity ended up (`EmfTeleportResult`: MOVED / FAILED /
  READY, `EMF_TELEPORT_OFF_TARGET`, `EMF_TELEPORT_RAW_TARGET`, `EMF_TELEPORT_LAST`, `EMF_TELEPORT_SESSION_ENDED`). Every
  request is answered exactly once; one whose session ended first is answered at once with SESSION_ENDED (touch no
  world, give nothing back). C++: `emf/teleport.hpp`
  (`emf::teleport`, `emf::teleport_to_spawn_point`, `emf::player_spawn_point`, `emf::spawn_points_near`,
  `emf::has_v20()`). It runs only on game builds whose command-buffer layout EMF checked (1273043, game and dedicated
  server): on a game build EMF does not know it is off (false, logged once). A mod built against the
  v20 header needs a v20 core; its mod.json `"emf"` should say 20.
- **Signature:** one new, `QueuePush` (the game's "push a record into a program's command buffer"), checked on both
  exes of 1273043; its 1269579 RVA is not recorded (`kRvaUnchecked`), and teleport stays off on that build.
- **Internal program:** EMF's own server program `emf_teleport` is registered into every server world (it declares no
  components: it only pushes records). EMF's internal programs do not count against the mods' 16.
- **Lua host 1.7.0:** `emf.teleport(world, id, x, y, z [, opts] [, fn])`. The `lua` mod needs API v20. Depend on
  `"lua": ">=1.7"` for it.
- **Console:** `goto [#k] <x> <y> <z> [ground]` and `goto [#k] <entity> [ground]` (host).
- **New mod:** `teleports` (Waypoint Teleports: `/unstuck`, `/home`, admin `tp`, `bring`, `tp.pos`, `tp.back`,
  `tp.list`), installed switched off.

## API v19 and Lua 1.6: what is new

Nothing to change: every mod built for an older API still loads (the table only grew). New:

- **C API v19: chat lines and saves on a server.** `chat_tell` (a line only one player sees, under their own name),
  `chat_broadcast` (the line for every connected player), `player_name`, `on_chat` (players' chat lines that are no EMF
  command, read-only) and `request_save` (the game's own world save now). C++: `emf::tell`, `emf::broadcast`,
  `emf::player_name`, `emf::on_chat`, `emf::request_save`, `emf::has_v19()`. They need players' chat: a dedicated
  server, or `[chat] players=1` in a game you host. A mod may send 30 chat lines in 10 s. A mod built against the v19
  header needs a v19 core; its mod.json `"emf"` should say 19.
- **Lua host 1.6.0:** `emf.tell`, `emf.broadcast`, `world:name`, `emf.on_chat`, `emf.request_save`; `emf.move`
  (take, put, give back, drop: nothing lost); `emf.take` takes `{slot =, exact =}`; a command named exactly like the
  mod (`emf.command(emf.id, ...)`) is the bare command `/<id>`. Items are safer in plain scripts too: what a put could
  not place while its script was paused or reloaded is dropped at the put's target, and a take whose callback raises
  puts back what it did not hand on. The `lua` mod needs API v19. Depend on
  `"lua": ">=1.6"` for them.
- **Signatures:** one new one, `RequestServerSave` (the game's save request; checked on both exes of 1273043).
  `request_save` is off without it (logged); the rest of EMF is not affected.
- **Console:** `chat.players` lists the players with their account hashes (what `[chat] admins` takes).
- **New mods:** Server Herald (`mods/herald`, C++, server side) and Hand-over (`mods/handover`, Lua), both off by
  default.

## API v18 and Lua 1.5: what is new

Nothing to change: every mod built for an older API still loads (the table only grew). New:

- **C API v18: attributes and damage.** `attribute_get`, `attribute_add` and `damage` (C++ `emf::attribute`,
  `emf::health`, `emf::attribute_add`, `emf::damage`, `emf::has_v18()`; ids `EMF_ATTR_HEALTH`, `EMF_ATTR_MAX_HEALTH`,
  `EMF_ATTR_STAMINA`, `EMF_ATTR_MAX_STAMINA`, `EMF_ATTR_MANA`, `EMF_ATTR_MAX_MANA`). They run only on game builds whose
  layouts EMF checked (1273043): on a game build EMF does not know they are off. They read and change health the way the game does, through its attribute
  registry, so the maximum is known (`Health` slot 0 had no maximum), and `damage` is a real hit with kill credit. A mod
  built against the v18 header needs a v18 core; its mod.json `"emf"` should say 18.
- **`emf::kill`** goes through `attribute_add` on a v18 core (any slot layout); the slot-0 write stays only as the
  fallback for an older core or a game build without the attribute signatures. A mod that called it keeps working.
- **Lua host 1.5.0:** `emf.health`, `emf.attribute`, `emf.attribute_add`, `emf.damage`; `emf.kill` through the
  attributes. The `lua` mod needs API v18. Depend on `"lua": ">=1.5"` for them.
- **`hook_program` on `impact_hit_conclusion` or `clear_hit_candidates`** is now refused (the log says why): EMF's
  core hooks both for `damage`.
- **Signatures:** ten new ones (attrFind, attrAdd, allocPendingHit and the main menu's for the debug bridge's
  `load_world`); their 1269579 RVAs are not recorded (`kRvaUnchecked`: a single match is accepted on that build, but
  the attribute functions stay off there because their layouts are unchecked). attrAdd's call target must also be the
  function its own pattern names.
- **Debug bridge:** `load_world` starts a LOCAL save offline from the main menu; `state` reports `world.name` and the
  player's health with its maximum (`source: "attributes"`) ([debug_bridge.md](debug_bridge.md)).

## API v17 and Lua 1.4: what is new

Nothing to change: every mod built for an older API still loads (the table only grew). New:

- **C API v17: commands any player may run.** `register_command_ex(&EmfCommandDef)` with `EMF_COMMAND_PLAYERS`, C++
  `emf::add_command(name, help, f, EMF_COMMAND_PLAYERS)` and `emf::has_v17()`. Before, a player who was not in
  `[chat] admins` could run no command at all, and `[chat] everyone=1` opened every one (`give`, `spawn`...). Now the
  admins run every command and other players the ones marked for them. `help` is marked: for a player who is no
  admin it lists only those. A mod built against the v17 header needs a v17 core (`emf::init` says so); its
  mod.json `"emf"` should say 17.
- **`EmfCommandSender.admin`** (v17, in the old padding: same size): whether the sender may run every command. Lua:
  `emf.sender().admin`. **`[chat] deny`** lets a server owner take commands for players away from players.
- **SDK helpers (header only, any core):** `emf::time_of_day(w)`, `emf::kill(w, id)` in `emf.hpp`; `emf::players(w)`,
  `emf::player_account(w, id)` in `emf/query.hpp`.
- **Lua host 1.4.0:** `emf.time_of_day()`, `emf.kill(world, id)`, `emf.put(..., {level = n, rarity = r})`,
  `emf.command(name, help, fn, {players = true})`, `emf.session_token()` and `emf.sender().admin`. The `lua` mod
  needs API v17. Depend on `"lua": ">=1.4"` for them. An older host ignores the
  `{players = true}` table (the command is then for admins only), but has no `emf.time_of_day` or `emf.kill`.
- **New mods:** `raids` (Night Raids), `loot_magnet` (Loot Magnet), `champion` (Wandering Champion, Lua).

## mod.json in EMF 0.3.0: what changed for existing mods

Nothing to change: every mod.json that loaded before still loads. A field EMF used to skip and now reads, written
wrong, can only cost that one setting (see "Fields EMF used to skip").

- **Dependencies** keep their meaning: `">=1.1"` is still 1.1.0 or newer, `"=1.2.3"` still exactly that version
  (`"=1.2"` exactly 1.2.0), `"*"` any. New: SemVer ranges (`"^1.3"`, `"~1.2"`, `">=1.0, <2.0"`, a bare `"1.2.3"`
  meaning `^1.2.3`, `"1.x"`, `"1.0 - 2.0"`, `||`), the object form `{"version": ..., "optional": true}` and a list
  of `{"id", "version", "optional"}` (ShroudForge's form, also under its name `"dependencies"`). An optional
  dependency's version counts only when it runs. See [guide.md](guide.md), "Dependencies and version ranges". A version
  that does not fit now says whether it is too old or too new.
- **Settings:** `bool`, `int`, `float`, `choice`, `key` and `text` work as before (same storage, same API values). New:
  `slider`, `select`, `keybind`, `color`, `step`, `maxLength`, `group` and `enabledWhen` ([reference.md](reference.md)).
  A `select` stores the option's value (text) where a `choice` stores its index, so keep `choice` where your code
  reads the index. A `slider` holds whole numbers only with a whole `step` (no step: decimals). A setting added to or
  changed in mod.json while the game runs now shows in the Mods tab within a second (while a world is loaded); a live
  change that narrows a range, drops an option or changes a type fits the value in memory only, and the saved value
  comes back once mod.json allows it again (a half-saved edit never overwrites what the player chose). A `key`
  setting whose default is no key is still refused on its own, with a log line, and the mod loads, as before.
- **Lua:** the `lua` host is 1.3.0 (`emf.on_key`, `emf.key_down`, corner names and `#rrggbb` colours in `emf.hud`;
  `"0x..."` text and numbers still work). Depend on `"lua": ">=1.3"` for them: an older EMF reads that form and says
  which `lua` it needs.
  A mod.json saved with a mistake while the game runs pauses the Lua mod with that mistake, instead of letting the
  script fail on missing settings.
- **Fields EMF used to skip** are read now: `step`, `options`, `maxLength`, `group`, `enabledWhen` and `control`. On
  the old kinds (`bool`, `int`, `float`, `choice`, `key`, `text`) one written wrong (for example `step` on a switch,
  `options` on a `choice`) refuses only that setting, with a log line naming it and the field; the mod loads. On the
  new kinds a mistake refuses the mod, with the reason. `null` counts as left out everywhere. Text that is not UTF-8
  is refused (labels show a bad byte as U+FFFD).
- **Older EMF:** an EMF older than 0.3.0 refuses a mod that uses a new setting `type` (`slider`, `select`,
  `keybind`, `color`) or a new `depends` form (a range other than `*`, `>=x`, `=x`; the object or list form), with a
  reason naming the field (for example `"depends": "lua" needs a valid id and "*", ">=x.y" or "=x.y.z"`). It skips
  `step`, `maxLength`, `group` and `enabledWhen` on the old types without a word. Tell your players which EMF version
  your mod needs.

## mod.json since EMF 0.3.0 (there was no 0.2.1 release)

`"emf"` must be a whole number (the API version your mod needs, e.g. `16`). A string (`"16"`), `null` or a fraction
used to be read as 0 and the mod loaded anyway; now the mod is refused with a message saying why. mod.json must be
UTF-8 and at most 256 KB.
