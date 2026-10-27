# Reverse-engineering helper scripts

Small Python scripts used to build `docs/ENGINE_MAP.md`. No dependencies beyond Python 3.
They read the game's `enshrouded.exe` and EML's `.cache\types.json` (`gamefiles.py`): pass `--exe <path>` or `--types <path>`
with any script, or set `EMF_GAME_DIR` to the game folder; without either they stop and say so. `EMF_EXE` picks another
exe for `pe.py` (for example the dedicated server); use a separate working folder per exe so the caches do not mix.
Run them from this folder (they import each other). Some write caches (`calls.pkl`, `rdata_strs.pkl`) in the current folder.

| Script | Use |
|---|---|
| `pe.py` | PE sections, VA/file-offset mapping, `ptr_refs(va)`, `rip_refs(va)` (RIP-relative code refs). |
| `funcs.py` | `.pdata` function table: `containing(va)`, `callers(func)` (direct calls). |
| `allstrs.py` | Builds `rdata_strs.pkl` (all .rdata strings). Run once per game build. |
| `anchor.py "text"` | String → code references → containing functions. The main entry point. |
| `func_strings.py 0x...` | All strings a function references. |
| `sig.py 0x...` | Unique byte signature for a function start; `sig.verify(pattern)`. |
| `dump.py 0xVA 0x100` | Annotated qword dump. |
| `show2.py <type>` / `ktypes.py grep <regex>` | Struct layouts / type search in types.json. |
| `tables.py` | Finds the runtime reflection type lists. |
| `imports.py` | Import table → IAT slot addresses. |
| `programs.py`, `programs2.py` | Rebuild the ECS program catalog (`programs2.json`) from the 817 `registerProgram` calls (`programs.py` has 1269579's registerProgram address built in). |
| `program_access.py <name>` / `--uses <Component>` / `--addremove` | Program descriptors with the real field layout (add/remove lists, other-entity lists, events). Finds registerProgram from its log string, so it works on any build and on the server exe. |
| `disasm.py 0x... [len]` | Annotated disassembly of a function (all its `.pdata` parts) without Ghidra: program names on calls, strings on RIP operands. Needs capstone (`pip install capstone`, for example in a venv): `python disasm.py ...`. |
| `rea_pseudocode.py <saved rea json>` | Compact pseudocode from a saved REA/Ghidra result file. |

When the game updates: rerun `allstrs.py`, then `anchor.py` on the anchor strings listed in ENGINE_MAP, regenerate signatures with `sig.py`, and diff.
