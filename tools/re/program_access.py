"""Program descriptors with the real field layout (docs/engine_parts/re_custom_programs.md section 2).
Finds registerProgram from its log string, so it works on any build without an address table.

Usage (run from tools/re):
  python program_access.py <name> [<name> ...]     print the named programs' access lists
  python program_access.py --uses <Component>      every program that lists the name, with the slot
  python program_access.py --addremove             every program with an add or remove list
"""
import struct, sys
from funcs import *

SLOTS = [  # (offset, label) of the NameList fields; masks are skipped
    (0x18, 'tpl'), (0x28, 'tpl?'), (0x38, 'read'), (0x48, 'read?'), (0x58, 'write'), (0x68, 'write?'),
    (0x78, 'ADD'), (0x88, 'REMOVE'), (0x98, 'sys-read'), (0xB0, 'sys-write'),
    (0xD0, 'other-tpl'), (0xE0, 'other-read'), (0xF0, 'other-write'), (0x100, 'other-write-unser'),
    (0x110, 'with'), (0x120, 'without'), (0x130, 'any-of'), (0x140, 'all-write'), (0x150, 'all-read'),
    (0x160, 'events-in'), (0x170, 'events-out'),
]

def register_program():
    s = find_str("[ecs%c] entity program '%s' already registered")
    for v in s:
        for r in rip_refs(v):
            c = containing(r)
            if c: return c[0]

def names(lp, lc):
    if not lc: return []
    o = va2off(lp)
    if o is None or lc > 400: return None
    out = []
    for k in range(lc):
        sp, sl = struct.unpack_from('<QQ', data, o + 16 * k)
        so = va2off(sp)
        out.append(data[so:so + sl].decode(errors='replace') if so is not None and sl < 200 else f'?{sp:#x}')
    return out

def getter_desc(g):
    o = va2off(g)
    if o is not None and data[o:o + 3] == b'\x48\x8d\x05' and data[o + 7] == 0xc3:
        return g + 7 + struct.unpack_from('<i', data, o + 3)[0]

def programs():
    va0, t = text()
    out = []
    for site in sorted(call_index().get(register_program(), [])):
        p = site - va0
        if t[p - 14] != 0xe8: continue
        g = va0 + p - 14 + 5 + struct.unpack_from('<i', t, p - 13)[0]
        d = getter_desc(g)
        if d is None: continue
        o = va2off(d)
        np_, nl, fn = struct.unpack_from('<QQQ', data, o)
        name = data[va2off(np_):va2off(np_) + nl].decode()
        acc = {}
        for off, lab in SLOTS:
            lp, lc = struct.unpack_from('<QQ', data, o + off)
            n = names(lp, lc)
            if n: acc[lab] = n
        flags, interval = struct.unpack_from('<II', data, o + 0x180)
        out.append({'name': name, 'desc': d, 'fn': fn, 'flags': flags, 'acc': acc})
    return out

def show(p):
    print(f"{p['name']}  update {p['fn']:#x}  desc {p['desc']:#x}  flags {p['flags']:#x}")
    for lab, n in p['acc'].items(): print(f"    {lab:18s} {', '.join(n)}")

if __name__ == '__main__':
    P = programs()
    a = sys.argv[1:]
    if a and a[0] == '--uses':
        for p in P:
            hits = [lab for lab, n in p['acc'].items() if a[1] in n]
            if hits: print(f"{p['name']:45s} {p['fn']:#x} {','.join(hits)}")
    elif a and a[0] == '--addremove':
        for p in P:
            if 'ADD' in p['acc'] or 'REMOVE' in p['acc']: show(p)
    else:
        for p in P:
            if p['name'] in a: show(p)
