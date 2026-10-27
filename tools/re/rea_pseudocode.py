import json, sys, re
def compact(code):
    out=[]
    for ln in code.replace('\r','').split('\n'):
        s=ln.strip()
        if not s: continue
        if re.match(r'^[\w\s\*\[\]]+;$', s) and '(' not in s and '=' not in s and not s.startswith('return') and not s.startswith('goto') and s not in ('break;','continue;'): continue
        if s.startswith('/* WARNING: Globals'): continue
        out.append(ln.rstrip())
    return '\n'.join(out)
d=json.load(open(sys.argv[1], encoding='utf-8'))
nr=d.get('normalized_result')
if isinstance(nr,dict) and 'items' in nr:
    for it in nr['items']:
        print('=====', it.get('address'), it.get('status'))
        print(compact(it.get('pseudocode') or json.dumps(it)[:300]))
elif isinstance(nr,str): print(compact(nr))
else: print(json.dumps(nr)[:2000])
