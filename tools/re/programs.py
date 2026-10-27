from funcs import *
import dump, json, collections
sites=sorted(call_index()[0x1409d5770])
va0,t=text()
def getter_desc(g):
    o=va2off(g)
    if data[o:o+3]==b'\x48\x8d\x05' and data[o+7]==0xc3:
        return g+7+struct.unpack_from('<i',data,o+3)[0]
progs=[]
for s in sites:
    p=s-va0
    if t[p-14]!=0xe8: progs.append({'site':s,'desc':None}); continue
    g=va0+p-14+5+struct.unpack_from('<i',t,p-13)[0]
    d=getter_desc(g)
    if d is None: progs.append({'site':s,'getter':g,'desc':None}); continue
    o=va2off(d)
    np,nl,fn=struct.unpack_from('<QQQ',data,o)
    name=data[va2off(np):va2off(np)+nl].decode()
    slots={}
    for so in range(0x18,0x180,0x10):
        lp,lc=struct.unpack_from('<QQ',data,o+so)
        if lc and lc<1000 and BASE<lp<BASE+0x3000000:
            items=[]
            for k in range(lc):
                sp,sl=struct.unpack_from('<QQ',data,va2off(lp)+16*k)
                so2=va2off(sp)
                items.append(data[so2:so2+sl].decode(errors='replace') if so2 and sl<200 else f'?{sp:#x}')
            slots[hex(so)]=items
        elif lp or lc:
            slots[hex(so)]=f'raw {lp:#x} {lc:#x}'
    flags,f2=struct.unpack_from('<II',data,o+0x180)
    fns=[hex(q) for q in struct.unpack_from('<QQQ',data,o+0x10)]
    progs.append({'site':s,'getter':g,'desc':d,'name':name,'fn':fns,'flags':flags,'f2':f2,'slots':slots,'caller':containing(s)[0]})
json.dump(progs,open('programs.json','w'),indent=1, default=str)
print(len(progs), 'with desc', sum(1 for p in progs if p.get('name')))
c=collections.Counter(); 
for p in progs:
    for k in p.get('slots',{}): c[k]+=1
print(sorted(c.items()))
print(collections.Counter(hex(p.get('caller',0)) for p in progs))
