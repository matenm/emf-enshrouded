# emf_content.dll

`emf_content.dll` builds the game content of EML mods (mods made for EML, the Enshrouded Mod Loader) into a game
folder, so those mods work without a separate EML install. It is a small Rust shim (`src/lib.rs`) around EML's own
library crates, which are copied in `third_party/eml/`. One call does what `emm.exe run --patch -g <folder>` does: it
loads the folder's `mods\` (`ModEnvironment::load`) and runs them (`mod_loader::lua::run`), with patching on, export
off and runtime DLLs off.

A second call, `emf_content_portraits`, draws portraits: frames of a game model turning, from the player's own game
files, into a cache folder (below, [Portraits](#portraits)). Nothing of the game's art is shipped; the frames are made
on the player's PC.

Nothing in EMF loads it yet. `emf_content_run.exe` (`tools/content_run/`, built next to `emf_tests.exe`, never
packaged) runs builds from the command line:

```
emf_content_run <game_dir> [--force] [--dll <path>] [--repeat <n>]
```

It prints the log lines to stderr as `[level] line` and each reply JSON to stdout (one line per build), checks after
each build that no thread is left running in the DLL's code, unloads the DLL, and exits with the last build's code.
`--repeat <n>` loads, builds and unloads n times in one process. Other exit codes: 10 wrong arguments, 11 the DLL could
not be loaded or has another ABI, 12 a thread was still running in the DLL after a build (or the DLL stayed loaded).

## What it writes, and the shadow folder phase 2 must keep

EML works in the folder it is given, so that folder must be a copy EMF made (a shadow folder), never the game's own
folder. EML writes there:

- `mods\` (made when it is missing) and `.cache\` (the game's types, a record of the files it built from);
- `<name>.kfc.bak` (the vanilla `.kfc`), then the patched `<name>.kfc` and `<name>.kfc_resources`, all in place;
- new `<name>_NNN.dat` files from the base `.kfc`'s container count N up. It only reads `_000` to `_(N-1)`.

So the shadow keeps these rules:

- `<name>.kfc`, `<name>.kfc_resources` and `<name>.kfc.bak` are private copies, never links. The DLL refuses to build
  (code 2) when one of them has more than one name.
- Only `_000.dat` to `_(N-1).dat` are hard links to the game's files, with N taken from the base `.kfc`.
- Before each build, delete the shadow's `.dat` files at index N and above. (EML's writer also removes a file before
  it makes it again, patch P4, so even a stray link there is broken instead of written through.)
- `mods\` holds real folders (or copies), not junctions: EML skips a junction, because Windows reports it as neither a
  folder nor a file.
- On any code other than 0, or REPLY_TOO_SMALL, do not redirect the game to the shadow.

## The ABI

From `emf_content.h`:

```c
#define EMF_CONTENT_ABI 1
enum { EMF_CONTENT_OK = 0, EMF_CONTENT_MODS_FAILED = 1, EMF_CONTENT_RUN_FAILED = 2, EMF_CONTENT_BAD_REQUEST = 3,
       EMF_CONTENT_REPLY_TOO_SMALL = 4, EMF_CONTENT_PANIC = 5 };
/* emf_content_portraits: some items failed, the others are drawn or current (the same number as MODS_FAILED) */
enum { EMF_CONTENT_ITEMS_FAILED = 1 };
/* level: 0 error, 1 warning, 2 info, 3 debug; line: one UTF-8 log line without a newline */
typedef void (*emf_content_log_fn)(void* ctx, int level, const char* line);
uint32_t emf_content_abi(void);
int32_t emf_content_build(const char* request_json, emf_content_log_fn log, void* log_ctx,
                          char* reply, size_t reply_cap, size_t* reply_len);
int32_t emf_content_portraits(const char* request_json, emf_content_log_fn log, void* log_ctx,
                              char* reply, size_t reply_cap, size_t* reply_len);
/* The exports as GetProcAddress gives them. emf_content_portraits came after ABI 1 without changing it: a DLL
   without it is an older one (GetProcAddress gives NULL). */
typedef uint32_t (*emf_content_abi_fn)(void);
typedef int32_t (*emf_content_build_fn)(const char* request_json, emf_content_log_fn log, void* log_ctx,
                                        char* reply, size_t reply_cap, size_t* reply_len);
typedef int32_t (*emf_content_portraits_fn)(const char* request_json, emf_content_log_fn log, void* log_ctx,
                                            char* reply, size_t reply_cap, size_t* reply_len);
```

The build's request and reply (the portraits call's are [below](#portraits)):

- Request (UTF-8 JSON): `{"game_dir": "<folder>", "file_name": "enshrouded", "force": false, "server": null}`. Only
  `game_dir` is needed. `file_name` defaults to `enshrouded` or `enshrouded_server`, whichever has a `.kfc` or `.exe`
  there. `force` builds even when EML's cache says nothing changed. `server: null` lets EML decide from the file name.
  For tests only, `"test_panic": "<message>"` makes the call panic with that message.
- Reply (NUL-terminated UTF-8 JSON in the caller's buffer): `{"code": n, "message": "...", "mods": [{"id": "...",
  "version": "..."}], "mod_errors": [{"id": "..." or null, "path": "...", "error": "..."}], "dlls": ["..."], "ms": n}`.
  `mods` is sorted by id. `dlls` stays empty, because runtime DLLs are off. `mod_errors` is filled when a mod's
  folder or `mod.json` cannot be read (code 1).
- `mod_warnings: [{"id": "...", "warning": "..."}]` is in the reply only when a mod's Lua gave warnings or errors while
  the build ran: EML's `warn`, or an error logged inside the mod's `lua{mod_id}` span.
  - At most 30, and 3 of one mod.
  - Each is cut at 300 characters, before a stack traceback, with control characters dropped, without the mod's own
    `<id>:` prefix, and with the event's other fields after it.
  - Empty and `@word` messages are skipped.
  - The code stays what it was: a build that warned is still good, and the player is told.
- `*reply_len` is always set to the full reply's length with the NUL. Pass a big buffer (1 MB). When the reply does
  not fit, it is cut: it keeps the real code and `ms`, gets `"truncated": true`, has empty lists and as much of the
  message as fits, and the call still returns the real code. Only when not even that fits (about 100 bytes), or
  `reply` is NULL, does the call return `EMF_CONTENT_REPLY_TOO_SMALL` and write nothing. The build has run then and
  its result is unknown: treat it as a failure (no redirect).
- A missing folder, or one without `<file_name>.kfc` or `.exe`, gives code 2 and a message that names the folder.
- `log` may be NULL. It is called only on the calling thread, during the call. It must return normally: no C++
  exception and no longjmp may leave it.
- One call at a time, over both calls. A call made while another runs (from another thread, or from inside the log
  callback) gives `EMF_CONTENT_BAD_REQUEST` at once.

## Rules for the caller

- Load it only at the held start of the game, before any chain-loaded DLL, emf_core or mod starts, and only when the
  content needs building (or portraits drawing). Unload it (`FreeLibrary`) before anything else starts.
- Call `emf_content_build`, `emf_content_portraits` and `FreeLibrary` on the same thread.
- After the build and before `FreeLibrary`, check that no thread has its start address inside the DLL
  (`emf::loader::threads_started_in` in `src/loader/module_threads.h`). If one has, do not unload the DLL: unloading
  would crash the game the next time that thread runs.
- Never load it in EMF.exe, and never while mods run.
- Never load it while `lua.dll` (EMF's Lua host) is loaded: it has its own Lua inside.

What the DLL promises in return, and the tests in `tests/test_content_dll.cpp` and `src/lib.rs` check:

- It exports only `emf_content_abi`, `emf_content_build` and `emf_content_portraits` (no `lua_*`), and imports only
  Windows' own DLLs.
- No panic leaves it: a panic becomes code 5 with its message, and the message and its place go to the log (not to
  stderr). The panic hook is set for the length of a call only; the caller's hook is back afterwards.
- It starts no thread. The shim starts none, and EML's image crate is built without rayon and EXR (patch P3), the
  one part that started worker threads (EXR decoding, AVIF encoding). The tests check after every build that no
  thread runs in the DLL's code, and 50 load-build-unload cycles leave the thread count where it was. (Those builds
  stop at EML's type reader; `emf_content_run --repeat` repeats real builds on a real shadow.)
- It keeps no log subscriber after a call (logging is set for the length of one call), and drops control characters
  (all but tab) from log lines.
- It uses its own static C runtime and never frees the caller's memory.

## Portraits

`emf_content_portraits` draws frames of game models turning (`src/portraits/`), for mod windows to show a beast as
the game's own model (`emf.image("portrait:<key>")` on the window side). The frames are drawn on the player's PC from
the player's own game files, read-only; no game art is in EMF's repository or its zips. Which models a start draws,
and calling it after the content build, is the loader's part (not built yet).

Request:

```json
{"game_dir": "<folder holding enshrouded.kfc and enshrouded.exe>", "file_name": "enshrouded",
 "cache_dir": "<game>\\emf\\cache\\portraits",
 "items": [{"key": "wolf", "template": "478381c5-585f-4264-b373-dbb1ea596eca", "frames": 24, "size": 256},
           {"key": "boar", "model": "f58abca2-8332-48eb-99a3-79c9f8e94a1f"}]}
```

- `game_dir` and `cache_dir` (a full path) are needed; `file_name` as for the build. `game_dir` can be the shadow
  folder, so a content mod's own models can be drawn too.
- An item: `key` (1 to 64 of `A-Z a-z 0-9 _ . -`, the overlay's rule, and not ending in a dot or being a Windows
  device name such as `CON` or `COM1`; unique, ignoring case); `model` (a `keen::RenderModel`) or `template` (a
  `keen::ecs::TemplateResource`, drawn with the model of its `keen::ecs::ModelResource`), a GUID as text, exactly one;
  `frames` 1 to 64 (24); `size` 8 to 512 (256). A bad item fails alone; the others are drawn. So does an item whose
  drawing panics (the panic and its place go to the log, the item gets `"drawing it panicked: ..."`).

What it writes, per item, in `<cache_dir>\<key>\`:

- `00.png`, `01.png`, ...: `size` x `size` RGBA8 PNGs, sRGB, straight alpha, transparent background. Frame i shows
  the model turned by `360 * i / frames` degrees; frame 0 is a three-quarter view with its front to the viewer's left.
- `index.json`: the frame count, the size, the renderer's version, the template, and the inputs (the render model,
  its debugName, the content hashes of its render data and of its albedo textures, an FNV-64 of its records).

An item whose `index.json` matches and whose frames are all there is left alone (`"current"`; only its records are
read). Anything else is drawn again, into a staging folder beside it, `<key>~new-<pid>-<time>` (`~` is never in a
key), which then replaces `<cache_dir>\<key>` whole: the old folder is moved aside to `<key>~old-<pid>-<time>` and
removed. So the folder never mixes two drawings or keeps frames past the new count, and two processes on one cache
never write into each other's files. A failed item's folder is as it was before the call, but for one case: when the
new folder could not be moved in and the old one could not be moved back, the old one is left as
`<key>~old-<pid>-<time>`, which the item's error names. Windows does not move a folder while a file in it is open
(a virus scanner reading a frame, say): that item fails and keeps its folder, and a later call draws it. A
`<cache_dir>\<key>` that is not a folder (a file, a link) is never moved: that item fails. Staging and old folders
left by earlier calls are removed when a call has a good item: this process's, and other processes' after an hour.
No game file is written. A game update that changes a model (its records or the content they name) draws it again;
so does a new renderer version.

Reply: `{"code": n, "message": "...", "items": [{"key": "...", "status": "drawn" | "current" | "failed", "model":
"<render model GUID>", "name": "<debugName>", "frames": n, "size": n, "error": "...", "notes": ["..."]}], "ms": n}`,
items in the request's order. Codes: 0 every item drawn or current (also for no items: the archive is then not
opened), 1 some items failed, 2 the game's archive or types could not be read (every good item fails with that), 3
not JSON, no `game_dir`, or no full `cache_dir`. Cut, too small and panic replies as for the build (a cut reply has
`"items": []`).

How a model is drawn:

- The game's types come from `<file_name>.exe` (EML's reader, about 0.2 s; EML's `.cache\types.json` when the exe
  cannot be read and the cache's version matches the archive). The records are read with them as EML's Lua sees them.
- LOD 0's meshes, decoded from the render data the way `tools/models/rendermodel.py` does (PNTCT vertex streams,
  21-bit positions); other vertex formats are left out with a note. The rest pose: the vertices as stored.
- Per material, the albedo (the image whose name holds "albedo"), the largest mip of at most 256 px, read alone
  from its content and decoded with EML's `kfc_content::image::decode`. Each of a texture's `mipChainContentRanges`
  starts at its own level; in a texture with one range, a level is found after the sizes of the larger levels. No
  level over 512 px is decoded: an albedo without a level of at most 512 px is drawn plain, with a note.
  `AlphaTested` materials are cut out at alpha 0.5, `DoubleSided` ones drawn from both sides. The albedo is
  multiplied by the vertex colours, which hold the fur cards' colour (all black colours are no tint).
- A CPU rasteriser (`src/portraits/raster.rs`, no threads): orthographic, the camera 15 degrees up, one scale for
  all frames, a light fixed to the camera, 2 to 4 samples per pixel side averaged in linear light. The game's space
  is left-handed (a creature's right side at +X), so seen from the front its right side is on the viewer's left.
- The game's wolf: 24 frames of 256 px in about 2.5 s in a debug build of the tests.

Tests that read game data are skipped unless an environment variable names it (read-only, never copied into the
repository): `EMF_PORTRAIT_GAME` (the game folder: the wolf by template and by model, its meshes and textures, 24
frames drawn into `EMF_PORTRAIT_OUT` or `%TEMP%\emf_portraits_wolf`) and `EMF_PORTRAIT_DUMP` (a folder with
`char_enemy_wolf_01.json`, `.render.bin`, `.material1.json` and `.material2.json` from the `model_survey` dump).

## Building

CMake builds it with cargo when `EMF_CONTENT` is on (the default): `cargo build --release --locked` in this folder,
with the target folder in `build\<preset>\cargo`, copied to `build\<preset>\content\emf_content.dll`. ctest runs the
Rust tests (`emf_content_cargo_tests`). You need rustup ([rustup.rs](https://rustup.rs)); `rust-toolchain.toml`
pins Rust 1.99.1. Without Rust, configure with `-DEMF_CONTENT=OFF`: then neither the DLL nor `emf_content_run` is
built, and its tests compile to nothing.

By hand, from a Visual Studio developer prompt (`scripts\dev.cmd`; EML's Lua is built as C):

```
cd src\content
cargo test --release --locked
```

`Cargo.lock` started from EML's: the crates EMF shares with EML keep EML's versions, except tracing-subscriber
(0.3.20, a security fix). `tests/tools/test_content_crate.py` checks that the lock has no rayon and that the release
profile unwinds.

## EML's copy and its patches

`third_party/eml/` holds EML v0.2.0's library crates. Every change EMF makes to them is its own commit and a line in
`third_party/eml/EMF_PATCHES.md`, which also says how to update the copy.

## Licence

The DLL contains EML, so it is GPL-3.0-or-later as a whole: EML is GPL-3.0-or-later, see
[third_party/eml/LICENSE](../../third_party/eml/LICENSE).
