from funcs import *
import pickle, sys, re
S=pickle.load(open('rdata_strs.pkl','rb'))
def anchor(pat, regex=False, lim=6):
    r=re.compile(pat if regex else re.escape(pat))
    hits=[(v,s) for v,s in S if r.search(s)]
    out=[]
    for v,s in hits[:lim]:
        # string may start before match start in the extracted run; use run start v
        refs=rip_refs(v)
        fs=[]
        for x in refs:
            c=containing(x); fs.append((x, c[0] if c else None))
        out.append((v,s,fs))
    return out
if __name__=='__main__':
    for p in sys.argv[1:]:
        for v,s,fs in anchor(p):
            print(f'{v:#x} "{s[:100]}"')
            for x,f in fs: print(f'     ref {x:#x} in func {f:#x}' if f else f'     ref {x:#x} (no func)')
