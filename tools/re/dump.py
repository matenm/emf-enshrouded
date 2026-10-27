from pe import *
import sys
def strat(v):
    o=va2off(v)
    if o is None: return None
    s=data[o:o+100].split(b'\0')[0]
    if len(s)>=3 and all(32<=c<127 for c in s): return s.decode()
def dumpq(v, n):
    o=va2off(v)
    for i in range(0,n,8):
        q=struct.unpack_from('<Q',data,o+i)[0]
        lo,hi=struct.unpack_from('<II',data,o+i)
        note=''
        if BASE<=q<BASE+0x3000000:
            s=strat(q); note = f'-> "{s}"' if s else f'-> {q:#x}'
        print(f"{v+i:#x} +{i:#05x}: {q:016x}  ({lo:#010x} {hi:#010x}) {note}")
if __name__=='__main__':
    dumpq(int(sys.argv[1],16), int(sys.argv[2],0))
