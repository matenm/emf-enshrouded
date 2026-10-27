#!/usr/bin/env python3
"""Generates docs/modding/llms.txt: a compact summary of EMF for an AI agent's context window (the C API from
sdk/include/emf/emf_api.h, the Lua emf.* functions from the Lua host's registration tables and docs/modding/lua.md,
mod.json fields and setting kinds from docs/modding/reference.md, console commands from their help texts, the debug
bridge's commands from docs/modding/debug_bridge.md, and the tools for agents).

    python tools/gen_llms_txt.py           writes docs/modding/llms.txt
    python tools/gen_llms_txt.py --check   exit 1 when docs/modding/llms.txt is not what this script makes
                                           (scripts/test_tools.ps1 runs it; verify.sh runs that)

Standard library only. The output depends only on the repository's files (no dates), so --check is exact.

Size: tests/tools/test_gen_llms_txt.py allows up to 46 KB (about 11k tokens). Trimming further would cut facts an
agent needs (function rules, setting kinds, the bridge's wait and log options).
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join("docs", "modding", "llms.txt")
sys.path.insert(0, os.path.join(REPO, "tools", "bridge"))
sys.path.insert(0, os.path.join(REPO, "tools", "mcp"))


def read(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as f:
        return f.read()


def squash(text):
    return re.sub(r"\s+", " ", text).strip()


def first_sentence(text, limit=230):
    """The text up to the first '. ' that ends a sentence, cut at about `limit` characters."""
    text = squash(text)
    m = re.search(r"(?<!e\.g)(?<!i\.e)(?<!etc)\.(\s|$)", text)
    s = text[:m.start() + 1] if m else text
    if len(s) > limit:
        cut = s[:limit].rsplit(" ", 1)[0]
        s = cut.rstrip(",;:") + " ..."
    return s


# ---------------------------------------------------------------------------------------------------- the C API
C_LIMIT = 140  # characters of a function's description


def parse_c_api(header):
    """[(version, name, signature, first sentence of its comment or '')] for every EmfApi field, in order."""
    body = header[header.index("typedef struct EmfApi {") + len("typedef struct EmfApi {"):header.index("} EmfApi;")]
    out = []
    version = 1
    pending = ""
    decl = ""
    i = 0
    tokens = re.split(r"(/\*.*?\*/)", body, flags=re.S)
    for tok in tokens:
        if tok.startswith("/*"):
            text = tok[2:-2]
            text = "\n".join(l.strip().lstrip("*").strip() for l in text.splitlines())
            m = re.match(r"\s*-+\s*v(\d+)", text)
            if m:
                version = int(m.group(1))
                pending = ""
                continue
            if decl.strip():  # a comment inside a declaration (between its lines): ignore
                continue
            if out and out[-1][4] == i and not pending:  # on the same line right after a field: its own comment
                v, n, s, d, _ = out[-1]
                if not d:
                    out[-1] = (v, n, s, first_sentence(text, C_LIMIT), i)
                continue
            pending = (pending + " " + text).strip()
            continue
        for ch in tok:
            if ch == "\n":
                i += 1
                if not decl.strip():
                    decl = ""
                    continue
            decl += ch
            if ch == ";":
                d = squash(decl)
                decl = ""
                m = re.match(r"^(.+?)\(\*\s*(\w+)\)\s*\((.*)\);$", d)
                if m:
                    ret, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
                    sig = "%s %s(%s)" % (ret, name, args)
                else:
                    m = re.match(r"^(.+?)\s+(\w+);$", d)
                    if not m:
                        continue
                    name = m.group(2)
                    sig = "%s %s" % (m.group(1), name)
                out.append((version, name, sig.replace(" *", "*"), first_sentence(pending, C_LIMIT), i))
                pending = ""
    return [(v, n, s, d) for v, n, s, d, _ in out]


def parse_defines(header):
    """[(name, value)] of #define EMF_* constants (not the include guard)."""
    return [(m.group(1), m.group(2)) for m in re.finditer(r"^#define (EMF_\w+) +(\S+)", header, re.M)]


def parse_structs(header):
    """[(name, 'field, field, ...')] of the typedef structs other than EmfApi, and callback typedefs."""
    structs = []
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} \1;", header, re.S):
        name = m.group(1)
        if name == "EmfApi":
            continue
        body = re.sub(r"/\*.*?\*/", "", m.group(2), flags=re.S)
        fields = [squash(f) for f in body.split(";") if squash(f)]
        structs.append((name, "; ".join(fields)))
    callbacks = [squash(m.group(0))[len("typedef "):].rstrip(";")
                 for m in re.finditer(r"typedef \w+\** ?\(\*\w+\)\([^)]*\);", header)]
    return structs, callbacks


# ---------------------------------------------------------------------------------------------------- Lua
def parse_lua_registrations(host_cpp, bindings_cpp):
    """(emf.* function names, world:* method names) in registration order."""
    def names(block):
        return re.findall(r'\{"(\w+)",\s*\w+\}', block)
    host_fns = re.search(r"static const luaL_Reg fns\[\] = \{(.*?)\{nullptr, nullptr\}\};\s*luaL_newlib", host_cpp,
                         re.S)
    bind_fns = re.search(r"static const luaL_Reg fns\[\] = \{(.*?)\{nullptr, nullptr\}\};", bindings_cpp, re.S)
    methods = re.search(r"static const luaL_Reg methods\[\] = \{(.*?)\{nullptr, nullptr\}\};", bindings_cpp, re.S)
    return names(host_fns.group(1)) + names(bind_fns.group(1)), names(methods.group(1))


def md_table_rows(md, after):
    """Rows (lists of cells) of the first Markdown table after the line that contains `after`."""
    start = md.index(after)
    lines = md[start:].splitlines()[1:]
    rows = []
    in_table = False
    for line in lines:
        if line.startswith("|"):
            in_table = True
            cells = [c.strip() for c in re.split(r"(?<!\\)\|", line.strip()[1:-1])]
            if all(re.fullmatch(r":?-+:?", c) for c in cells if c):
                continue
            rows.append([c.replace("\\|", "|") for c in cells])
        elif in_table:
            break
    return rows[1:]  # without the header


def lua_docs(lua_md):
    """[(first cell, description, [names it covers])] from lua.md's emf table and world table."""
    out = []
    for heading, prefix in (("## The `emf` table", "emf."), ("## Worlds and components", "world:")):
        for cells in md_table_rows(lua_md, heading):
            # the names the row describes: in its first cell, or mentioned with their arguments in the text
            # ("answer with `emf.reply(text)`")
            names = re.findall(re.escape(prefix) + r"(\w+)", cells[0])
            names += [n for n in re.findall(re.escape(prefix) + r"(\w+)\(", " ".join(cells[1:])) if n not in names]
            out.append((cells[0], cells[1] if len(cells) > 1 else "", prefix, names))
    return out


# ---------------------------------------------------------------------------------------------------- commands
def _cpp_strings(text):
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text))


def parse_builtin_commands(cpp):
    block = cpp[cpp.index("const Def defs[] = {"):]
    block = block[:block.index("};")]
    return [(m.group(1), _cpp_strings(m.group(2))) for m in
            re.finditer(r'\{"([\w.]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+),\s*cmd_\w+\s*\}', block)]


def parse_core_commands(repo):
    """[(name, help)] of the core's other console commands: center.add("name", "help"...) in src/core (not the
    built-in table)."""
    out = []
    folder = os.path.join(repo, "src", "core")
    for fn in sorted(os.listdir(folder)):
        if not fn.endswith(".cpp") or fn == "builtin_commands.cpp":
            continue
        with open(os.path.join(folder, fn), encoding="utf-8") as f:
            src = f.read()
        for m in re.finditer(r'center(?:\.|->)add\(\s*"([\w.]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+),', src):
            out.append((m.group(1), _cpp_strings(m.group(2))))
    return out


def parse_menu_commands(repo):
    """The core's commands that also run at the main menu (no world): the built-in table's `anywhere` names, and
    center.add calls whose `anywhere` argument is true."""
    cpp = read(os.path.join("src", "core", "builtin_commands.cpp"))
    anywhere = re.search(r"const bool anywhere = (.*?);", cpp, re.S).group(1)
    names = re.findall(r'std::strcmp\(d\.name, "([\w.]+)"\) == 0', anywhere)
    for fn in sorted(os.listdir(os.path.join(repo, "src", "core"))):
        if fn.endswith(".cpp") and fn != "builtin_commands.cpp":
            src = read(os.path.join("src", "core", fn))
            for m in re.finditer(r'center(?:\.|->)add\(\s*"([\w.]+)",\s*(?:"(?:[^"\\]|\\.)*"\s*)+,([^;]*?)\);', src):
                args = [a.strip() for a in m.group(2).split(",")]
                if len(args) >= 4 and args[3] == "true":  # (fn, ctx, user, anywhere, ...)
                    names.append(m.group(1))
    return names


def _mod_ids(pkg, var):
    m = re.search(r"\$%s = @\(([^)]*)\)" % var, pkg)
    return re.findall(r'"(\w+)"', m.group(1)) if m else []


def parse_mod_commands(repo):
    """[(mod, name, help)] for the mods the release zip installs (scripts/package.ps1's $playerMods and $scriptMods):
    C++ commands as any call ("name", "help"...) to add_command or a helper named like it (quick_stack's command(...)), and
    Lua commands from emf.command("name", "help", ...), named <id>.<name> as the Lua host names them."""
    pkg = read(os.path.join("scripts", "package.ps1"))
    out = []
    for mod in _mod_ids(pkg, "playerMods"):
        folder = os.path.join(repo, "mods", mod)
        for fn in sorted(os.listdir(folder)):
            if not fn.endswith(".cpp"):
                continue
            with open(os.path.join(folder, fn), encoding="utf-8") as f:
                src = f.read()
            for m in re.finditer(r'\b\w*command\(\s*"([\w.]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+),', src):
                out.append((mod, m.group(1), _cpp_strings(m.group(2))))
    for mod in _mod_ids(pkg, "scriptMods"):
        folder = os.path.join(repo, "mods", mod)
        try:
            with open(os.path.join(folder, "mod.json"), encoding="utf-8") as f:
                mod_id = json.load(f).get("id", mod)
        except (OSError, ValueError):
            mod_id = mod
        for fn in sorted(os.listdir(folder)):
            if fn.endswith(".lua"):
                with open(os.path.join(folder, fn), encoding="utf-8") as f:
                    src = f.read()
                for m in re.finditer(r'emf\.command\(\s*"([\w.]+)",\s*"((?:[^"\\]|\\.)*)"', src):
                    name = m.group(1)  # Lua 1.6: a command named exactly like the mod stays unprefixed
                    full = name if name == mod_id or name.startswith(mod_id + ".") else mod_id + "." + name
                    out.append((mod, full, m.group(2)))
    return out


# ---------------------------------------------------------------------------------------------------- the bridge
def parse_bridge_doc(md):
    """[(command, [request examples], description)] from debug_bridge.md's protocol sections."""
    proto = md[md.index("## The protocol"):]
    nxt = proto.find("\n## ", 5)
    proto = proto[:nxt] if nxt > 0 else proto
    out = []
    for m in re.finditer(r"^### (\w+)\n(.*?)(?=^### |\Z)", proto, re.S | re.M):
        name, body = m.group(1), m.group(2)
        examples = []
        for ex in re.findall(r'\{"id":\s*\d+,\s*"cmd":\s*"%s"[^\n`]*?\}' % name, body):
            if ex not in examples:
                examples.append(ex)
        blocks = re.findall(r"```\w*\n(.*?)```", body, re.S)
        if body.lstrip().startswith("```") and blocks:  # the section opens with an example: its reply says it
            desc = "Replies " + squash(" ".join(blocks[0].splitlines()[1:]))
        else:
            prose = re.sub(r"```.*?```", "", body, flags=re.S)
            paras = [p.strip() for p in re.split(r"\n\s*\n", prose)
                     if p.strip() and not p.strip().startswith(("|", "-"))]
            text = re.sub(r"^`\{[^`]*\}`\s*", "", paras[0]).replace("`", "") if paras else ""
            desc = first_sentence(text, 300)
            if desc.endswith(":") and blocks:
                desc += " " + squash(blocks[0])
            desc = desc[:1].upper() + desc[1:]
        if len(desc) > 240:
            desc = desc[:236].rstrip() + " ..."
        if name in DETAIL_COMMANDS:  # their options matter for tests: each bullet of the section, in short
            for bullet in re.findall(r"^- (.+(?:\n  .+)*)", body, re.M):
                b = squash(bullet).replace("`", "")
                desc += " " + (b if len(b) <= 220 else b[:216].rstrip() + " ...")
        out.append((name, examples[:3], desc))
    return out


DETAIL_COMMANDS = ("log", "wait")  # bridge commands whose bullets (since, max, follow, match...) llms.txt keeps


def bridge_commands_in_core(service_cpp):
    """The command names the core's bridge answers (command_names() in src/core/bridge/service.cpp)."""
    m = re.search(r"command_names\(\) \{.*?= \{(.*?)\};", service_cpp, re.S)
    return re.findall(r'"(\w+)"', m.group(1))


# ---------------------------------------------------------------------------------------------------- the file
def generate():
    header = read(os.path.join("sdk", "include", "emf", "emf_api.h"))
    api_version = re.search(r"#define EMF_API_VERSION (\d+)", header).group(1)
    L = []
    w = L.append
    w("# EMF (Enshrouded Modding Framework): a summary for AI agents")
    w("")
    w("Generated by tools/gen_llms_txt.py from the repository's headers, sources and docs. Do not edit by hand.")
    w("")
    w("EMF is a native mod framework for the game Enshrouded (Windows, Vulkan). A dinput8.dll proxy loads")
    w("<game>\\emf\\emf_core.dll, which loads mods from <game>\\emf\\mods\\<id>\\ (a folder with mod.json and a DLL, or")
    w("main.lua for a Lua mod). Mods get a C API (EmfApi, version %s); C++ wraps it in sdk/include/emf/emf.hpp." % api_version)
    w("Key facts:")
    w("- Gameplay changes happen on the host's SERVER world (your own game or the one you host); a client of someone")
    w("  else's game only sees results. Touch ECS data only inside world callbacks (on_tick, run_on_world, on_event,")
    w("  commands), on the world passed in. Component pointers are valid only during that callback.")
    w("- Settings: <game>\\emf\\config\\<id>.ini, section [<id>]; the in-game console (key under Esc) has a Mods tab.")
    w("- Log: <game>\\emf\\emf_core.log (truncated at every game start). Lines from a mod start with [<id>].")
    w("- Lua mods reload within a second of saving a .lua file or mod.json; a script error pauses the mod, never")
    w("  the game.")
    w("- Docs: docs/modding/README.md (start), first_mod.md (Lua in 10 minutes), lua.md, reference.md (every API")
    w("  function with C++ names and rules), cookbook.md, debug_bridge.md, ai_agents.md. Rules for agents: AGENTS.md.")
    w("")

    # C API
    w("## C API (sdk/include/emf/emf_api.h, EmfApi version %s)" % api_version)
    w("")
    w("A mod DLL exports: extern \"C\" __declspec(dllexport) bool emf_mod_load(const EmfApi* api). Fields are only")
    w("appended (check api->size before using a later one). Refusals return NULL/0/false and log a line.")
    last_v = None
    for v, name, sig, desc in parse_c_api(header):
        if v != last_v:
            w("")
            w("### v%d" % v)
            last_v = v
        w("- %s%s" % (sig, ": " + desc if desc else ""))
    w("")
    w("### Constants")
    defines = [(n, v) for n, v in parse_defines(header) if n != "EMF_API_H"]
    w(", ".join("%s=%s" % (n, v) for n, v in defines))
    structs, callbacks = parse_structs(header)
    w("")
    w("### Callback types")
    for c in callbacks:
        w("- " + c)
    w("")
    w("### Structs (set size = sizeof(struct) where it has one)")
    for name, fields in structs:
        w("- %s { %s }" % (name, fields))
    w("")

    # Lua
    host_cpp = read(os.path.join("src", "lua_host", "host.cpp"))
    bindings_cpp = read(os.path.join("src", "lua_host", "bindings.cpp"))
    fns, methods = parse_lua_registrations(host_cpp, bindings_cpp)
    docs = lua_docs(read(os.path.join("docs", "modding", "lua.md")))
    w("## Lua (the lua mod; docs/modding/lua.md)")
    w("")
    lua_version = json.loads(read(os.path.join("mods", "lua", "mod.json")))["version"]
    w("mod.json has \"main\": \"main.lua\" and \"depends\": {\"lua\": \">=%s\"} (the Lua host is %s). Callbacks get a"
      % (".".join(lua_version.split(".")[:2]), lua_version))
    w("`world` valid only during the callback. world:get(component, id) returns a component as a table with its real")
    w("field names (sdk/include/emf/generated/components.hpp); assigning a field writes the game's data.")
    w("")
    covered = {"emf.": set(), "world:": set()}
    for cell, desc, prefix, names in docs:
        w("- %s: %s" % (cell, squash(desc)))
        covered[prefix].update(names)
    for prefix, registered in (("emf.", fns), ("world:", methods)):
        for n in registered:
            if n not in covered[prefix]:
                w("- `%s%s`: (registered by the Lua host, not described in lua.md)" % (prefix, n))
    w("")

    # mod.json
    ref = read(os.path.join("docs", "modding", "reference.md"))
    w("## mod.json (docs/modding/reference.md)")
    w("")
    for cells in md_table_rows(ref, "**mod.json:**"):
        w("- %s: %s" % (cells[0], squash(cells[1])))
    w("")
    w("Version ranges in depends (SemVer, like npm/Cargo):")
    w("; ".join("%s = %s" % (c[0], squash(c[1])) for c in md_table_rows(ref, "**Version ranges in `depends`**")))
    w("")
    w("Settings: each entry of \"settings\" takes key (1-48 of A-Z a-z 0-9 _ . -; \"enabled\" is reserved), type,")
    w("default, label, help, live (true: applied at once), group (a heading), enabledWhen ({\"setting\": k, \"equals\": v}).")
    w("Kinds:")
    for cells in md_table_rows(ref, "The kinds:"):
        w("- %s: shown as %s%s; stored as %s" % (cells[0], squash(cells[1]),
                                                ("; fields " + squash(cells[2])) if cells[2] else "", squash(cells[3])))
    w("")

    # console commands
    w("## Console commands")
    w("")
    w("Typed into the in-game console (key under Esc), the bridge's console command, or <game>\\emf\\commands.txt")
    w("(read about once a second while a world runs). (host) = needs a world you host.")
    w(", ".join(parse_menu_commands(REPO)) + " also work at the main menu.")
    w("")
    for name, help_text in (parse_builtin_commands(read(os.path.join("src", "core", "builtin_commands.cpp"))) +
                            parse_core_commands(REPO)):
        w("- %s%s" % (name, help_text if help_text.startswith(":") else " " + help_text))
    w("")
    w("From the mods in the EMF zip (only while that mod runs):")
    for mod, name, help_text in parse_mod_commands(REPO):
        w("- %s%s [%s]" % (name, help_text if help_text.startswith(":") else " " + help_text, mod))
    w("Lua mods add <id>.<name> commands with emf.command.")
    w("")

    # bridge
    w("## Debug bridge (docs/modding/debug_bridge.md)")
    w("")
    w("Off unless <game>\\emf\\emf_core.ini has [debug] bridge=1 (scripts\\install_emf.ps1 -Bridge). A named pipe")
    w("\\\\.\\pipe\\emf-bridge-<pid> for this Windows user only; <game>\\emf\\bridge.txt names it. One JSON object per line:")
    w("request {\"id\": n, \"cmd\": \"...\", ...}; reply {\"id\": n, \"ok\": true, ...} or {\"id\": n, \"ok\": false, \"code\": c,")
    w("\"error\": \"...\"} with code timeout | busy | unavailable | bad_request | unknown_command | refused. Pushed events")
    w("(no id): {\"event\": \"log\" | \"game\" | \"dropped\", ...}. Clients: scripts\\emf_bridge.ps1, tools/bridge/emf_bridge.py.")
    w("")
    for name, examples, desc in parse_bridge_doc(read(os.path.join("docs", "modding", "debug_bridge.md"))):
        w("- %s: %s%s" % (name, desc, (" e.g. " + " ".join(examples)) if examples else ""))
    w("")

    # tools
    import emf_scenario
    import emf_mcp
    w("## Tools for agents (docs/modding/ai_agents.md)")
    w("")
    w("- scripts\\game_session.ps1 start | stop | status -LockOwner <you>: installs the dev build with the bridge, starts")
    w("  the game, stops at the menu for a character check (exit 2; then start -Character local), loads the LOCAL test")
    w("  world through the bridge's load_world (no clicks; menu clicks for a 3840x2160 screen only on a build without")
    w("  it), and afterwards quits, uninstalls and checks the saves. It acts only on the game it recorded.")
    w("- tools/bridge/emf_scenario.py <scenario.json>: runs a JSON scenario (tests/game/*.json) through the bridge;")
    w("  exit 0 pass, 1 fail, 2 no game, 3 invalid file. {\"name\": ..., \"load_world\": {slot, character, expect_name}")
    w("  (optional: the load_world steps' defaults), \"steps\": [{\"do\": <kind>, ...}]}. Kinds:")
    for kind, (what, fields) in emf_scenario.STEPS.items():
        w("  - %s (%s): %s" % (kind, ", ".join(k + ("*" if req else "") for k, (_, req, _) in fields.items()) or "-",
                               what))
    w("  (* = required; state paths: player.position.x, overlay.hud[id=clock.time].text, mods[0].id; ops: %s)" %
      " ".join(emf_scenario.OPS))
    w("- tools/mcp/emf_mcp.py: an MCP server (stdio) with the tools %s; the repository's .mcp.json registers it." %
      ", ".join(t["name"] for t in emf_mcp.TOOLS))
    w("  The tools that change the game (console, mod_key, run_scenario, load_world) act only on the game game_session")
    w("  recorded (--any-game for your own) and only at the main menu or in a LOCAL save played with a LOCAL character")
    w("  (state.character.storage \"local\"; a Steam or unknown character refuses them; --allow-cloud-world). The scenario")
    w("  runner's console, mod_key and edit_file steps follow the same rule.")
    w("")
    return "\n".join(L)


def check_lua_documented():
    """Lua functions the host registers that lua.md does not describe (a doc to fix)."""
    fns, methods = parse_lua_registrations(read(os.path.join("src", "lua_host", "host.cpp")),
                                           read(os.path.join("src", "lua_host", "bindings.cpp")))
    covered = {"emf.": set(), "world:": set()}
    for _, _, prefix, names in lua_docs(read(os.path.join("docs", "modding", "lua.md"))):
        covered[prefix].update(names)
    return ["emf." + n for n in fns if n not in covered["emf."]] + \
           ["world:" + n for n in methods if n not in covered["world:"]]


def main(argv=None):
    p = argparse.ArgumentParser(description="Generates docs/modding/llms.txt")
    p.add_argument("--check", action="store_true", help="exit 1 when the file is out of date")
    a = p.parse_args(argv)
    text = generate() + "\n"
    path = os.path.join(REPO, OUT)
    if a.check:
        try:
            with open(path, encoding="utf-8") as f:
                current = f.read()
        except OSError:
            current = None
        if current != text:
            print("%s is out of date: run python tools/gen_llms_txt.py and commit the result" % OUT)
            return 1
        print("%s is up to date (%d KB)" % (OUT, len(text.encode("utf-8")) // 1024))
        return 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d KB)" % (OUT, len(text.encode("utf-8")) // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
