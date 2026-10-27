from pe import *
import bisect, pickle, os, re
# parse .pdata
n,va,vs,rp,rs=sec('.pdata')
RF=[]
for i in range(0, vs//12):
    b,e,u=struct.unpack_from('<III',data,rp+12*i)
    if b==0: break
    RF.append((BASE+b,BASE+e,BASE+u))
RF.sort()
starts=[r[0] for r in RF]
def primary(entry):
    # follow chained unwind info to primary function
    b,e,u=entry
    seen=0
    while seen<16:
        o=va2off(u)
        if o is None: break
        verflags=data[o]; flags=verflags>>3; cnt=data[o+2]
        if flags & 0x4:  # UNW_FLAG_CHAININFO
            off=o+4+((cnt+1)&~1)*2
            cb,ce,cu=struct.unpack_from('<III',data,off)
            b,e,u=BASE+cb,BASE+ce,BASE+cu; seen+=1
        else: break
    return b
def containing(v):
    i=bisect.bisect_right(starts,v)-1
    if i<0: return None
    b,e,u=RF[i]
    if b<=v<e: return primary(RF[i]), (b,e)
    return None
_calls=None
def call_index():
    """map target -> list of call sites (E8 rel32)"""
    global _calls
    if _calls is not None: return _calls
    if os.path.exists('calls.pkl'):
        _calls=pickle.load(open('calls.pkl','rb')); return _calls
    va0,t=text(); d={}
    sset=set(starts)
    for m in re.finditer(rb'[\xe8\xe9]', t):
        p=m.start()
        if p+5>len(t): break
        tgt=va0+p+5+struct.unpack_from('<i',t,p+1)[0]
        if tgt in sset:
            d.setdefault(tgt,[]).append(va0+p)
    pickle.dump(d,open('calls.pkl','wb')); _calls=d; return d
def callers(f):
    res=set()
    for site in call_index().get(f,[]):
        c=containing(site)
        if c: res.add(c[0])
    return sorted(res)
