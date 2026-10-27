import struct, re, sys, functools, os
from gamefiles import exe_path
EXE=os.environ.get('EMF_EXE') or exe_path()  # EMF_EXE (another exe, e.g. the server), else --exe <path>, else EMF_GAME_DIR's enshrouded.exe
data=open(EXE,'rb').read()
e_lfanew=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,e_lfanew+6)[0]
optsz=struct.unpack_from('<H',data,e_lfanew+20)[0]
opt=e_lfanew+24
BASE=struct.unpack_from('<Q',data,opt+24)[0]
secs=[]
so=opt+optsz
for i in range(nsec):
    o=so+40*i
    name=data[o:o+8].rstrip(b'\0').decode()
    vsize,va,rawsz,rawptr=struct.unpack_from('<IIII',data,o+8)
    secs.append((name,BASE+va,vsize,rawptr,rawsz))
def off2va(off):
    for n,va,vs,rp,rs in secs:
        if rp<=off<rp+rs: return va+(off-rp)
def va2off(v):
    for n,va,vs,rp,rs in secs:
        if va<=v<va+rs: return rp+(v-va)
def sec(name):
    for s in secs:
        if s[0]==name: return s
def find_str(s, exact=True):
    b=s.encode()+(b'\0' if exact else b'')
    res=[]; i=data.find(b)
    while i!=-1:
        # require preceding NUL for exact
        if not exact or data[i-1]==0: res.append(off2va(i))
        i=data.find(b,i+1)
    return res
_text=None
def text():
    global _text
    if _text is None:
        n,va,vs,rp,rs=sec('.text'); _text=(va,data[rp:rp+rs])
    return _text
def rip_refs(target):
    """find instructions with rip-relative disp32 pointing at target (any opcode); returns candidate VA of disp field end"""
    va0,t=text(); res=[]
    # brute force: for every position p, disp=target-(va0+p+4) ; search for disp bytes then validate opcode heuristically
    # Efficient: target-(va0+p+4) varies with p, so scan linearly using numpy-free trick: check each 4-byte window
    # Use regex on candidate opcode prefixes instead
    for m in re.finditer(rb'(?:[\x48\x4c]\x8d|[\x48\x4c]\x8b|[\x48\x4c]\x89|[\x48\x4c]\x3b|[\x48\x4c]\x39|\x0f\x10|\x0f\x11|\x0f\x28|\x8b|\x89|\x8d|\xff|\x80|\x81|\x83|\xc7|\x3b|\x39|\xf3\x0f\x10)[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', t):
        p=m.end()
        if p+4>len(t): continue
        d=struct.unpack_from('<i',t,p)[0]
        # instruction end may include imm after disp; try +0 (most), +1, +4
        for extra in (0,1,4):
            if va0+p+4+extra+d==target:
                res.append(va0+m.start()); break
    return res
def ptr_refs(target, secnames=('.rdata','.data')):
    b=struct.pack('<Q',target); res=[]
    for n,va,vs,rp,rs in secs:
        if n not in secnames: continue
        seg=data[rp:rp+rs]; i=seg.find(b)
        while i!=-1:
            res.append(va+i); i=seg.find(b,i+1)
    return res
if __name__=='__main__':
    for s in secs: print(s[0], hex(s[1]), hex(s[2]), hex(s[3]), hex(s[4]))
    print('base',hex(BASE))
