import json, sys
from gamefiles import types_path
d=json.load(open(types_path()))  # --types <path>, else EMF_GAME_DIR's .cache\types.json
T=d['types']; byname={t['qualifiedName']:t for t in T}; byidx={t['index']:t for t in T}
for q in sys.argv[1:]:
    t=byname.get(q) or byidx.get(int(q)) if q.isdigit() else byname.get(q)
    if not t: print('missing', q); continue
    print(f"== {t['qualifiedName']} size={t['size']} align={t['alignment']} prim={t.get('primitiveType')} inner={t.get('innerType')} flags={t.get('flags')} hash={t.get('qualifiedHash')}/{t.get('internalHash')}")
    a=t.get('attributes') or {}
    if a: print('   attrs:', {k:v.get('value',True) for k,v in a.items()})
    sf=t.get('structFields') or {}
    for f in sorted(sf.values(), key=lambda f:f['dataOffset']):
        ft=byidx.get(f['type'],{})
        print(f"   +{f['dataOffset']:4d} (0x{f['dataOffset']:x}) {f['name']:32s} {ft.get('qualifiedName')} [{ft.get('size')}]")
    ev=t.get('enumFields') or t.get('enumValues') or t.get('values')
    if ev: print('   enum:', json.dumps(ev)[:1500])
    other={k:v for k,v in t.items() if k not in ('structFields','defaultValue','attributes','index','name','impactName','qualifiedName','namespace','size','alignment','elementAlignment','fieldCount','primitiveType','flags','nameHash','impactHash','qualifiedHash','internalHash','innerType')}
    if other: print('   other:', json.dumps(other)[:800])
