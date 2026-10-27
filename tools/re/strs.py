from pe import *
import re, sys
for pat in sys.argv[1:]:
    pb=pat.encode()
    for m in re.finditer(re.escape(pb), data):
        o=m.start(); st=o
        while st>0 and 32<=data[st-1]<127: st-=1
        v=off2va(st)
        print(f"{pat!r:30s} {v:#x} {data[st:st+140].split(b'\0')[0]}")
