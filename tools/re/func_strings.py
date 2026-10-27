import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from funcs import *
def func_strings(f):
    va0,t=text(); out=[]
    for b,e,u in RF:
        c=containing(b)
        if not c or c[0]!=f: continue
        p=b-va0
        while p<e-va0:
            if t[p] in (0x48,0x4c) and t[p+1]==0x8d and (t[p+2]&0xc7)==5:
                tgt=va0+p+7+struct.unpack_from('<i',t,p+3)[0]
                o=va2off(tgt)
                if o and 0x1413b0000<=tgt<0x141fb5000:
                    ss=data[o:o+120].split(b'\0')[0]
                    if len(ss)>=4 and all(32<=c<127 for c in ss): out.append((hex(va0+p), ss.decode()))
                p+=7; continue
            p+=1
    return out
if __name__=='__main__':
    for a in sys.argv[1:]:
        print('==',a)
        for x in func_strings(int(a,16)): print('  ',x)
