from pe import *
import dump
def is_desc(q):
    o=va2off(q)
    if o is None or not (0x1413b0000<=q<0x141fb5000): return False
    try:
        p=struct.unpack_from('<Q',data,o+0x20)[0]; n=struct.unpack_from('<Q',data,o+0x28)[0]
        s=dump.strat(p)
        return bool(s) and s.startswith('keen') and len(s)==n
    except Exception: return False
def desc_name(q):
    o=va2off(q); p=struct.unpack_from('<Q',data,o+0x20)[0]; return dump.strat(p)
def bounds(v):
    o=va2off(v); s=v
    while is_desc(struct.unpack_from('<Q',data,va2off(s-8))[0]): s-=8
    e=v
    while is_desc(struct.unpack_from('<Q',data,va2off(e))[0]): e+=8
    return s,e
