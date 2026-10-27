"""Generate sdk/include/emf/generated/components.hpp from EML's types.json.

Emits every ECS component (types deriving from keen::ecs::Component), every keen::ecs::Dynamic* struct,
every ECS event (types deriving from keen::ecs::GameEvent or keen::ecs::Event, with kName = short name),
and the value types they use, as byte-exact packed structs with static_asserts on size and field offsets.

Usage: python tools/gen_headers.py [types.json] [out.hpp]
Without types.json: EML's .cache/types.json in the game folder EMF_GAME_DIR names, else a plain error.
"""
import json
import os
import re
import sys
from pathlib import Path

DEFAULT_OUT = Path(__file__).resolve().parent.parent / "sdk" / "include" / "emf" / "generated" / "components.hpp"
COMPONENT_BASE = 136  # index of keen::ecs::Component
EVENT_BASE_NAMES = ("keen::ecs::GameEvent", "keen::ecs::Event")

PRIMS = {
    "BOOL": "bool", "UINT8": "uint8_t", "SINT8": "int8_t", "UINT16": "uint16_t", "SINT16": "int16_t",
    "UINT32": "uint32_t", "SINT32": "int32_t", "UINT64": "uint64_t", "SINT64": "int64_t",
    "FLOAT32": "float", "FLOAT64": "double",
}
BITMASKS = {"BITMASK8": "uint8_t", "BITMASK16": "uint16_t", "BITMASK32": "uint32_t", "BITMASK64": "uint64_t"}
CPP_KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield
decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if
inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public
register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch
template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile
wchar_t while xor xor_eq""".split())
# Common Windows / CRT macros that would break identifiers when windows.h is included first.
WIN_MACROS = set("""DELETE ERROR IN OUT OPTIONAL TRUE FALSE VOID CONST NEAR FAR PASCAL CALLBACK WINAPI interface
small min max near far ABSOLUTE RELATIVE TRANSPARENT OPAQUE ALTERNATE WINDING BLACKNESS WHITENESS SYNCHRONIZE
STANDARD_RIGHTS_READ IGNORE INFINITE DOMAIN OVERFLOW UNDERFLOW PLOSS TLOSS SING EOF NULL BOOL BYTE WORD DWORD
INT UINT LONG ULONG SHORT USHORT CHAR UCHAR FLOAT DOUBLE HANDLE MOUSE_MOVED DOUBLE_CLICK MOUSE_WHEELED
KEY_EVENT MOUSE_EVENT FOCUS_EVENT MENU_EVENT FILE_DEVICE_MASS_STORAGE IDLE_PRIORITY_CLASS HIGH_PRIORITY_CLASS
NORMAL_PRIORITY_CLASS REALTIME_PRIORITY_CLASS PURGE GENERIC_READ GENERIC_WRITE GENERIC_ALL DIFFERENCE RGB
GetObject GetMessage SendMessage PostMessage CreateWindow LoadImage DrawText GetClassName CreateFont LoadIcon
LoadCursor LoadBitmap LoadString CreateEvent CreateMutex CreateSemaphore OpenEvent SetProp GetProp RemoveProp
Yield PlaySound CopyFile MoveFile DeleteFile CreateFile CreateDirectory RemoveDirectory GetCurrentTime
NOERROR S_OK S_FALSE E_FAIL FAILED SUCCEEDED THIS RELEASE CONTEXT_FULL REG_NONE AF_UNSPEC FILE_ATTRIBUTE_NORMAL
DM_COPY DM_UPDATE DM_IN_BUFFER DM_OUT_BUFFER OF_READ OF_WRITE IDOK IDCANCEL IDABORT IDRETRY IDIGNORE IDYES IDNO
MB_OK HELP_INDEX CAPTUREBLT MIRROR_BITMAP NOMIRRORBITMAP SRCCOPY SRCPAINT DSTINVERT PATCOPY POINT RECT SIZE
NotEqual Equal Always Never Less Greater""".split())


def ident(name):
    s = re.sub(r"[^0-9A-Za-z_]", "_", name)
    if not s or s[0].isdigit():
        s = "_" + s
    if s in CPP_KEYWORDS or s in WIN_MACROS or s.startswith("__"):
        s += "_"
    return s


class Gen:
    def __init__(self, types):
        self.by_index = {t["index"]: t for t in types}
        self.emitted = set()
        self.out = []
        self.short_names = set()  # types that get a kName (events)

    def cpp_name(self, t):
        ns = [ident(n) for n in t["namespace"][1:]]  # drop leading keen
        return "::".join(["emf", "gen"] + ns + [ident(t["name"])])

    def resolve(self, idx):
        """Returns (cpp_type, dims, dep_index) for a field type, or None for an opaque type."""
        t = self.by_index.get(idx)
        if t is None:
            return None
        p = t.get("primitiveType")
        if p in PRIMS:
            return PRIMS[p], [], None
        if p in BITMASKS:
            return BITMASKS[p], [], None
        if p == "TYPEDEF":
            return self.resolve(t.get("innerType")) if t.get("innerType") is not None else None
        if p in ("GUID", "OBJECT_REFERENCE") and t["size"] == 16:
            return "::emf::gen::Guid", [], None
        if p == "ENUM":
            return self.cpp_name(t), [], idx
        if p == "STRUCT":
            return self.cpp_name(t), [], idx
        if p == "STATIC_ARRAY":
            elem = self.by_index.get(t.get("innerType"))
            inner = self.resolve(t.get("innerType"))
            if inner is None or elem is None or elem["size"] == 0:
                return None
            count = t.get("fieldCount") or (t["size"] // elem["size"])
            if count * elem["size"] != t["size"]:
                return None
            ctype, dims, dep = inner
            return ctype, [count] + dims, dep
        return None  # BlobArray, DS types, variants, strings: opaque bytes

    def fields_of(self, t):
        """All fields including those of non-empty base structs, sorted by offset."""
        fields = []
        base = self.by_index.get(t.get("innerType")) if t.get("primitiveType") == "STRUCT" else None
        if base is not None and base.get("primitiveType") == "STRUCT" and base["index"] != COMPONENT_BASE:
            fields += self.fields_of(base)
        fields += list((t.get("structFields") or {}).values())
        return sorted(fields, key=lambda f: f["dataOffset"])

    def emit(self, idx):
        if idx in self.emitted:
            return
        t = self.by_index[idx]
        self.emitted.add(idx)
        p = t.get("primitiveType")
        if p == "ENUM":
            self.emit_enum(t)
        elif p == "STRUCT":
            # dependencies first
            for f in self.fields_of(t):
                r = self.resolve(f["type"])
                if r and r[2] is not None:
                    self.emit(r[2])
            self.emit_struct(t)

    def open_ns(self, t):
        ns = [ident(n) for n in t["namespace"][1:]]
        return "namespace emf::gen" + "".join("::" + n for n in ns) + " {"

    def emit_enum(self, t):
        under = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}.get(t["size"], "uint32_t")
        inner = self.by_index.get(t.get("innerType"))
        if inner is not None and inner.get("primitiveType") in PRIMS:
            under = PRIMS[inner["primitiveType"]]
        values = sorted((t.get("enumFields") or {}).values(), key=lambda v: v["value"])
        lines = [self.open_ns(t), f"enum class {ident(t['name'])} : {under} {{"]
        seen = set()
        for v in values:
            n = ident(v["name"])
            while n in seen:
                n += "_"
            seen.add(n)
            lines.append(f"    {n} = {v['value']},")
        lines += ["};", "}", ""]
        self.out.append("\n".join(lines))

    def emit_struct(self, t):
        name = ident(t["name"])
        size = t["size"]
        lines = [self.open_ns(t), f"// {t['qualifiedName']}", f"struct {name} {{"]
        lines.append(f"    static constexpr uint32_t kHash = 0x{t['qualifiedHash']:08x}u;")
        lines.append(f"    static constexpr uint32_t kInternalHash = 0x{t['internalHash']:08x}u;")
        lines.append(f"    static constexpr uint32_t kSize = {size};")
        if t["index"] in self.short_names:
            lines.append(f"    static constexpr const char* kName = \"{t['name']}\";")
        cur = 0
        asserts = []
        used = set()
        pad = 0
        for f in self.fields_of(t):
            off = f["dataOffset"]
            ft = self.by_index.get(f["type"])
            fsize = ft["size"] if ft else 0
            if off < cur or fsize == 0 or off + fsize > size:
                continue  # overlapping or empty: skip, keep layout exact via padding
            if off > cur:
                lines.append(f"    uint8_t _pad{pad}[{off - cur}];")
                pad += 1
            fname = ident(f["name"])
            while fname in used or fname in ("kHash", "kInternalHash", "kSize", "kName"):
                fname += "_"
            used.add(fname)
            r = self.resolve(f["type"])
            if r is None:
                lines.append(f"    uint8_t {fname}[{fsize}];  // {ft['qualifiedName']}")
            else:
                ctype, dims, _ = r
                lines.append(f"    {ctype} {fname}{''.join(f'[{d}]' for d in dims)};")
            asserts.append(f"static_assert(offsetof({self.cpp_name(t)}, {fname}) == {off});")
            cur = off + fsize
        if cur < size:
            lines.append(f"    uint8_t _pad{pad}[{size - cur}];")
        if size == 0:
            lines.append("    // empty")
        lines += ["};", "}", f"static_assert(sizeof({self.cpp_name(t)}) == {size});"] + asserts + [""]
        self.out.append("\n".join(lines))


REFLECT_KINDS = {"bool": "Bool", "uint8_t": "U8", "int8_t": "I8", "uint16_t": "U16", "int16_t": "I16",
                 "uint32_t": "U32", "int32_t": "I32", "uint64_t": "U64", "int64_t": "I64", "float": "F32",
                 "double": "F64"}


def reflection(gen, components, dynamics, events):
    """sdk/include/emf/generated/reflection.hpp: every emitted struct with its fields (names, offsets, kinds), so
    tools and script hosts can read and write fields by name."""
    structs = [i for i in sorted(gen.emitted, key=lambda i: gen.by_index[i]["qualifiedName"])
               if gen.by_index[i].get("primitiveType") == "STRUCT"]
    slot = {idx: n for n, idx in enumerate(structs)}
    comp = {t["index"] for t in components} | {t["index"] for t in dynamics}
    evts = {t["index"] for t in events}
    type_lines, field_lines = [], []
    nfield = 0
    for idx in structs:
        t = gen.by_index[idx]
        first, count, cur, used = nfield, 0, 0, set()
        for f in gen.fields_of(t):
            off = f["dataOffset"]
            ft = gen.by_index.get(f["type"])
            fsize = ft["size"] if ft else 0
            if off < cur or fsize == 0 or off + fsize > t["size"]:
                continue
            fname = ident(f["name"])
            while fname in used or fname in ("kHash", "kInternalHash", "kSize", "kName"):
                fname += "_"
            used.add(fname)
            r = gen.resolve(f["type"])
            kind, ref, elems = "Bytes", 0xFFFF, 1
            if r is not None:
                ctype, dims, dep = r
                for d in dims:
                    elems *= d
                if ctype == "::emf::gen::Guid":
                    kind = "Guid"
                elif ctype in REFLECT_KINDS:
                    kind = REFLECT_KINDS[ctype]
                elif dep is not None and gen.by_index[dep].get("primitiveType") == "STRUCT":
                    kind, ref = "Struct", slot[dep]
                elif dep is not None:  # an enum: its underlying integer
                    e = gen.by_index[dep]
                    under = {1: "U8", 2: "U16", 4: "U32", 8: "U64"}.get(e["size"], "U32")
                    inner = gen.by_index.get(e.get("innerType"))
                    if inner is not None and inner.get("primitiveType") in PRIMS:
                        under = REFLECT_KINDS[PRIMS[inner["primitiveType"]]]
                    kind = under
            field_lines.append(f'    {{"{f["name"]}", {off}, {fsize}, Kind::{kind}, 0x{ref:04x}, {elems}}},')
            nfield += 1
            count += 1
            cur = off + fsize
        flags = (1 if idx in comp else 0) | (2 if idx in evts else 0)
        type_lines.append(f'    {{"{t["qualifiedName"]}", "{t["name"]}", 0x{t["qualifiedHash"]:08x}u, {t["size"]}, '
                          f'{first}, {count}, {flags}}},')
    head = [
        "// GENERATED by tools/gen_headers.py from EML types.json. Do not edit.",
        "// Every struct of components.hpp with its fields, for reading and writing fields by name at run time.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace emf::gen::reflect {",
        "enum class Kind : uint8_t { Bool, U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, Struct, Guid, Bytes };",
        "struct Field {",
        "    const char* name;",
        "    uint32_t offset, size;  // size of the whole field (all array elements)",
        "    Kind kind;              // enums appear as their integer kind",
        "    uint16_t type;          // Struct: index into kTypes, else 0xffff",
        "    uint32_t count;         // array elements (1 = not an array)",
        "};",
        "struct Type {",
        "    const char* name;        // keen::ecs::CurrentTransform",
        "    const char* short_name;  // CurrentTransform",
        "    uint32_t hash, size;",
        "    uint32_t first_field, field_count;  // into kFields",
        "    uint32_t flags;          // 1 = a component (or a Dynamic half), 2 = an event",
        "};",
        f"inline constexpr uint32_t kTypeCount = {len(type_lines)};",
        f"inline constexpr uint32_t kFieldCount = {max(nfield, 1)};",
        "inline constexpr Type kTypes[kTypeCount] = {",
    ]
    empty_field = '    {"", 0, 0, Kind::Bytes, 0xffff, 1},'
    tail = ["};", "inline constexpr Field kFields[kFieldCount] = {"] + (field_lines or [empty_field]) + \
           ["};", "}  // namespace emf::gen::reflect", ""]
    return "\n".join(head + type_lines + tail)


def default_types():
    """EML's types.json in the game folder EMF_GAME_DIR names; stops with what to pass when it is not set."""
    game = os.environ.get("EMF_GAME_DIR")
    if not game:
        sys.exit("gen_headers.py: where is EML's types.json? Pass it (python tools/gen_headers.py <types.json> "
                 "[out.hpp]), or set EMF_GAME_DIR to the game folder (its .cache\\types.json is read).")
    return os.path.join(game, ".cache", "types.json")


def main():
    types_path = sys.argv[1] if len(sys.argv) > 1 else default_types()
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT
    data = json.load(open(types_path, encoding="utf-8"))
    types = [t for t in data["types"] if not t["qualifiedName"].startswith("keen::ds::")]
    gen = Gen(types)
    def derives_from_component(t):
        cur, seen = t, 0
        while cur is not None and cur.get("primitiveType") == "STRUCT" and seen < 32:
            if cur.get("innerType") == COMPONENT_BASE:
                return True
            cur = gen.by_index.get(cur.get("innerType"))
            seen += 1
        return False

    def is_abstract(t):
        return "abstract" in (t.get("attributes") or {})

    # Components derive from keen::ecs::Component directly or through a base (Actor <- BaseActor, Health <- FloatAttribute).
    component_like = [t for t in types if t.get("primitiveType") == "STRUCT" and derives_from_component(t)]
    components = sorted((t for t in component_like if not is_abstract(t)), key=lambda t: t["qualifiedName"])
    abstract_bases = sorted((t for t in component_like if is_abstract(t)), key=lambda t: t["qualifiedName"])
    dynamics = sorted((t for t in types if t["qualifiedName"].startswith("keen::ecs::Dynamic") and t.get("primitiveType") == "STRUCT"),
                      key=lambda t: t["qualifiedName"])
    by_name = {t["qualifiedName"]: t for t in types}
    event_bases = {by_name[n]["index"] for n in EVENT_BASE_NAMES if n in by_name}

    def is_event(t):
        seen = 0
        cur = t
        while cur is not None and cur.get("primitiveType") == "STRUCT" and seen < 32:
            base = cur.get("innerType")
            if base in event_bases:
                return True
            cur = gen.by_index.get(base)
            seen += 1
        return False

    events = sorted((t for t in types if t["qualifiedName"].startswith("keen::ecs::") and t["index"] not in event_bases
                     and t.get("primitiveType") == "STRUCT" and is_event(t)), key=lambda t: t["qualifiedName"])
    gen.short_names = {t["index"] for t in events}
    for t in abstract_bases + components + dynamics + events:
        gen.emit(t["index"])

    head = [
        "// GENERATED by tools/gen_headers.py from EML types.json. Do not edit.",
        f"// Source version: {data.get('version', '?')}",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace emf::gen {",
        "struct Guid { uint8_t bytes[16]; };",
        "struct ComponentInfo { const char* name; uint32_t hash; uint32_t size; };",
        "struct EventInfo { const char* name; uint32_t hash; uint32_t size; };  // name = short name",
        "}",
        "",
        "#pragma pack(push, 1)",
        "#pragma warning(push)",
        "#pragma warning(disable : 4200 4201)",
        "",
    ]
    reg = ["#pragma warning(pop)", "#pragma pack(pop)", "", "namespace emf::gen {",
           f"inline constexpr size_t kComponentCount = {len(components)};",
           "inline constexpr ComponentInfo kComponents[kComponentCount] = {"]
    for t in components:
        reg.append(f"    {{\"{t['qualifiedName']}\", 0x{t['qualifiedHash']:08x}u, {t['size']}}},")
    reg += ["};", f"inline constexpr size_t kDynamicComponentCount = {len(dynamics)};",
            "// The per-entity halves of split components (DynamicX for X), keyed by their own hash.",
            "inline constexpr ComponentInfo kDynamicComponents[kDynamicComponentCount] = {"]
    for t in dynamics:
        reg.append(f"    {{\"{t['qualifiedName']}\", 0x{t['qualifiedHash']:08x}u, {t['size']}}},")
    reg += ["};", f"inline constexpr size_t kEventCount = {len(events)};",
            "inline constexpr EventInfo kEvents[kEventCount] = {"]
    for t in events:
        reg.append(f"    {{\"{t['name']}\", 0x{t['qualifiedHash']:08x}u, {t['size']}}},")
    reg += ["};", "}  // namespace emf::gen", ""]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(head + gen.out + reg), encoding="utf-8")
    refl_path = out_path.parent / "reflection.hpp"
    refl_path.write_text(reflection(gen, components, dynamics, events), encoding="utf-8")
    print(f"wrote {out_path}: {len(components)} components, {len(dynamics)} dynamic structs, {len(events)} events, "
          f"{len(gen.emitted)} types; and {refl_path.name}")


if __name__ == "__main__":
    main()
