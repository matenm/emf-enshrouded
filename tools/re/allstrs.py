from pe import *
import re, pickle, os
# extract all printable ascii strings >=4 from .rdata with VA
n,va,vs,rp,rs=sec('.rdata')
seg=data[rp:rp+rs]
out=[]
for m in re.finditer(rb'[\x20-\x7e]{4,}', seg):
    out.append((va+m.start(), m.group().decode()))
pickle.dump(out, open('rdata_strs.pkl','wb'))
print(len(out))
