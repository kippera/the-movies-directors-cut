# SPDX-License-Identifier: GPL-3.0-or-later
"""usage: scaleplace.py src dst sprite_id char_id fx fy -- in DefineSprite <sprite_id>, multiply the scale of every placement of <char_id> by fx,fy (translation untouched)"""
import struct,sys
from swfdump import matrix,tags,rect
from mflsqueeze import enc_matrix,raw_matrix_flags
from mflspread2 import lenhdr
def rewrite(d,a,b,char,fx,fy,st):
    out=bytearray(); dl={}
    for code,hp,q,ln in tags(d,a,b):
        if code==26 and d[q]&4:
            flags=d[q]; depth=struct.unpack_from("<H",d,q+1)[0]; r=q+3
            if flags&2: ch=struct.unpack_from("<H",d,r)[0]; r+=2; dl[depth]=ch
            else: ch=dl.get(depth)
            if ch==char:
                m,mend=matrix(d,r); hs,hr=raw_matrix_flags(d,r); sx,sy,r0,r1,tx,ty=m
                nb=d[q:r]+enc_matrix(sx*fx,sy*fy,r0,r1,tx,ty,True,hr)+d[mend:q+ln]; out+=lenhdr(26,nb); st[0]+=1; continue
        elif code==26 and d[q]&2: dl[struct.unpack_from("<H",d,q+1)[0]]=struct.unpack_from("<H",d,q+3)[0]
        if code==28: dl.pop(struct.unpack_from("<H",d,q)[0],None)
        out+=d[hp:q+ln]
    return bytes(out)
if __name__=="__main__":
    d=open(sys.argv[1],"rb").read(); sid,char,fx,fy=int(sys.argv[3]),int(sys.argv[4]),float(sys.argv[5]),float(sys.argv[6])
    (x0,x1,y0,y1),p=rect(d,8); p+=4; out=bytearray(d[:p]); st=[0]
    for code,hp,q,ln in tags(d,p,len(d)):
        if code==39 and struct.unpack_from("<H",d,q)[0]==sid: inner=rewrite(d,q+4,q+ln,char,fx,fy,st); out+=lenhdr(39,d[q:q+4]+inner)
        else: out+=d[hp:q+ln]
    out[4:8]=struct.pack("<I",len(out)); open(sys.argv[2],"wb").write(out); print("placements",st[0])
