# SPDX-License-Identifier: GPL-3.0-or-later
"""usage: shiftplace.py src dst sprite_id char_id dx_px  -- inside DefineSprite <sprite_id>, add dx_px to the x translation of every placement of character <char_id> (incl. later move-only placements at the same depth)"""
import struct,sys
from swfdump import matrix,tags
from mflsqueeze import enc_matrix,raw_matrix_flags
from mflspread2 import lenhdr
def rewrite(d,a,b,char,dx,stats):
    out=bytearray(); dl={}
    for code,hp,q,ln in tags(d,a,b):
        if code==26 and d[q]&4:
            flags=d[q]; depth=struct.unpack_from("<H",d,q+1)[0]; r=q+3
            if flags&2: ch=struct.unpack_from("<H",d,r)[0]; r+=2; dl[depth]=ch
            else: ch=dl.get(depth)
            if ch==char:
                m,mend=matrix(d,r); hs,hr=raw_matrix_flags(d,r); sx,sy,r0,r1,tx,ty=m
                nb=d[q:r]+enc_matrix(sx,sy,r0,r1,tx+dx,ty,True,hr)+d[mend:q+ln]; out+=lenhdr(26,nb); stats[0]+=1; continue
        elif code==26 and d[q]&2: dl[struct.unpack_from("<H",d,q+1)[0]]=struct.unpack_from("<H",d,q+3)[0]
        if code==28: dl.pop(struct.unpack_from("<H",d,q)[0],None)
        out+=d[hp:q+ln]
    return bytes(out)
def run(d,sid,char,dx):
    from swfdump import rect
    (x0,x1,y0,y1),p=rect(d,8); p+=4; out=bytearray(d[:p]); stats=[0]
    for code,hp,q,ln in tags(d,p,len(d)):
        if code==39 and struct.unpack_from("<H",d,q)[0]==sid:
            inner=rewrite(d,q+4,q+ln,char,dx,stats); out+=lenhdr(39,d[q:q+4]+inner)
        else: out+=d[hp:q+ln]
    out[4:8]=struct.pack("<I",len(out)); return bytes(out),stats[0]
if __name__=="__main__":
    d=open(sys.argv[1],"rb").read(); o,n=run(d,int(sys.argv[3]),int(sys.argv[4]),float(sys.argv[5])); open(sys.argv[2],"wb").write(o); print("placements shifted",n,len(d),"->",len(o))
