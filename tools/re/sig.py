"""Heuristic unique byte signature for a function start (or any address) in .text.
Wildcards rel32 after E8/E9/0F 8x and disp32 of RIP-relative ModRM (mod=00, rm=101).
Usage: python sig.py 0x140456790 [0x...]"""
from pe import *
import re, sys
def mask_bytes(b):
    m=[True]*len(b)  # True = keep
    i=0
    while i<len(b):
        c=b[i]
        if c in (0xE8,0xE9) and i+5<=len(b):
            for k in range(1,5): m[i+k]=False
            i+=5; continue
        if c==0x0F and i+1<len(b) and 0x80<=b[i+1]<=0x8F and i+6<=len(b):
            for k in range(2,6): m[i+k]=False
            i+=6; continue
        # RIP-relative: [prefix] opcode modrm where (modrm & 0xC7)==0x05
        if i+1<len(b) and (b[i+1] & 0xC7)==0x05 and c in (0x8B,0x8D,0x89,0x3B,0x39,0x03,0x2B,0x33,0x0B,0x23,0x85,0x87,0xFF,0x80,0x81,0x83,0xC7,0xC6,0x10,0x11,0x28,0x29,0x2E,0x2F,0x58,0x59,0x5C,0x5E,0x54,0x57,0x6F,0x7F,0xB6,0xB7,0xBE,0xBF,0x38,0x3A,0x02,0x0A,0x22,0x2A,0x32):
            if i+6<=len(b):
                for k in range(2,6): m[i+k]=False
                i+=6; continue
        i+=1
    return m
def to_pat(b,m): return ' '.join(f'{x:02X}' if k else '??' for x,k in zip(b,m))
def to_re(b,m): return b''.join(re.escape(bytes([x])) if k else b'.' for x,k in zip(b,m))
def make_sig(addr, minlen=12, maxlen=96):
    va0,t=text(); p=addr-va0
    for L in range(minlen, maxlen+1, 4):
        b=t[p:p+L]; m=mask_bytes(b)
        rx=re.compile(to_re(b,m), re.DOTALL)
        hits=[]
        for h in rx.finditer(t):
            hits.append(h.start())
            if len(hits)>1: break
        if len(hits)==1 and hits[0]==p:
            return to_pat(b,m), L
    return None, None
def verify(pat):
    va0,t=text()
    parts=pat.split()
    rx=re.compile(b''.join(b'.' if x=='??' else re.escape(bytes([int(x,16)])) for x in parts), re.DOTALL)
    return [hex(va0+h.start()) for h in rx.finditer(t)][:5]
if __name__=='__main__':
    for a in sys.argv[1:]:
        s,L=make_sig(int(a,16)); print(a, L, s)
