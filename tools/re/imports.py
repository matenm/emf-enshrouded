from pe import *
def imports():
    imp_rva=struct.unpack_from('<I',data,opt+120)[0]
    o=va2off(BASE+imp_rva); res={}
    while True:
        oft,ts,fc,name_rva,ft=struct.unpack_from('<IIIII',data,o)
        if name_rva==0: break
        dll=data[va2off(BASE+name_rva):].split(b'\0')[0].decode()
        t=va2off(BASE+(oft or ft)); i=0
        while True:
            th=struct.unpack_from('<Q',data,t+8*i)[0]
            if th==0: break
            if not th>>63:
                nm=data[va2off(BASE+(th&0x7fffffff))+2:].split(b'\0')[0].decode()
            else: nm=f'ord{th&0xffff}'
            res[(dll,nm)]=BASE+ft+8*i; i+=1
        o+=20
    return res
