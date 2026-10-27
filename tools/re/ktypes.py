import json, sys
from gamefiles import types_path
d=json.load(open(types_path()))  # --types <path>, else EMF_GAME_DIR's .cache\types.json
T=d['types']
byname={t['qualifiedName']:t for t in T}
byidx={t['index']:t for t in T}
def show(q, depth=0):
    t=byname.get(q)
    if not t: print('missing',q); return
    keys={k:v for k,v in t.items() if k not in ('fields',)}
    print(json.dumps(keys))
    for f in t.get('fields',[]) or []:
        print('   ', json.dumps(f))
if __name__=='__main__':
    mode=sys.argv[1]
    if mode=='grep':
        import re
        r=re.compile(sys.argv[2])
        for t in T:
            if r.search(t['qualifiedName']): print(t['index'], t['qualifiedName'], t['size'], t.get('primitiveType'), t.get('fieldCount'))
    elif mode=='show':
        for q in sys.argv[2:]: show(q); print()
