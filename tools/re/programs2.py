from funcs import *
import json, collections
P=json.load(open('programs.json'))
def strlist(lp,lc):
    o=va2off(lp)
    if o is None or lc==0 or lc>200: return None
    items=[]
    for k in range(lc):
        sp,sl=struct.unpack_from('<QQ',data,o+16*k)
        so=va2off(sp)
        if so is None or sl==0 or sl>120: return None
        s=data[so:so+sl]
        if not all(32<=c<127 for c in s): return None
        items.append(s.decode())
    return items
out=[]
for p in P:
    if not p.get('name'): continue
    o=va2off(p['desc'])
    slots={}
    off=0x18
    while off<0x180:
        lp,lc=struct.unpack_from('<QQ',data,o+off)
        sl=strlist(lp,lc) if (BASE<lp<BASE+0x3000000) else None
        if sl is not None:
            slots[off]=sl; off+=16
        else: off+=8
    p['slots2']={hex(k):v for k,v in slots.items()}
    out.append(p)
c=collections.Counter()
for p in out:
    for k in p['slots2']: c[k]+=1
print(sorted(c.items(), key=lambda x:int(x[0],16)))
json.dump(out,open('programs2.json','w'),indent=1)
# sample of each slot
ex={}
for p in out:
    for k,v in p['slots2'].items():
        ex.setdefault(k,[]).append((p['name'],v))
for k in sorted(ex,key=lambda x:int(x,16)):
    print(k, ex[k][:3])
