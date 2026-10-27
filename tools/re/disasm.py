"""Annotated disassembly of a function (or a range) without Ghidra. Needs capstone
(pip install capstone, for example in a venv; then: python disasm.py ...).
Not called dis.py on purpose: that name hides the standard library module capstone imports.

Usage (run from tools/re):
  python disasm.py 0x14027b4e0            whole function (bounds from .pdata, chained parts included)
  python disasm.py 0x14027b4e0 0x80       only the first 0x80 bytes
Annotations: call targets that are program update functions get the program name; RIP-relative
operands get their target address and the string there, if any.
Set EMF_EXE to read another exe (e.g. the dedicated server).
"""
import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from funcs import *

def strat(v):
    o = va2off(v)
    if o is None: return None
    s = data[o:o + 90].split(b'\0')[0]
    if len(s) >= 4 and all(32 <= c < 127 for c in s): return s.decode()

_progs = None
def prog_names():
    global _progs
    if _progs is None:
        try:
            import program_access as pa
            _progs = {p['fn']: p['name'] for p in pa.programs()}
        except Exception:
            _progs = {}
    return _progs

def parts_of(f):
    """all .pdata ranges whose primary function is f"""
    return sorted((b, e) for b, e, u in RF if primary((b, e, u)) == f)

def dis(start, length=None):
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = False
    names = prog_names()
    if length is None:
        c = containing(start)
        f = c[0] if c else start
        ranges = parts_of(f) or [(start, start + 0x200)]
    else:
        ranges = [(start, start + length)]
    for b, e in ranges:
        o = va2off(b)
        print(f'--- {b:#x}..{e:#x}')
        for ins in md.disasm(data[o:o + (e - b)], b):
            note = ''
            op = ins.op_str
            if ins.mnemonic in ('call', 'jmp') and op.startswith('0x'):
                t = int(op, 16); n = names.get(t)
                if n: note = f'  ; {n}'
            if 'rip' in op:
                try:
                    disp = op.split('rip')[1].split(']')[0].replace(' ', '')
                    tgt = ins.address + ins.size + int(disp, 16) if disp else ins.address + ins.size
                    s = strat(tgt)
                    note = f'  ; [{tgt:#x}]' + (f' "{s}"' if s else '')
                except ValueError:
                    pass
            print(f'{ins.address:#x}: {ins.mnemonic:7s} {op}{note}')

if __name__ == '__main__':
    a = int(sys.argv[1], 16)
    n = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    dis(a, n)
