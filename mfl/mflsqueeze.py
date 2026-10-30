# SPDX-License-Identifier: GPL-3.0-or-later
"""Pre-squash an .mfl horizontally by K (default 0.75) by rewriting root-level PlaceObject2 matrices.
usage: python3 mflsqueeze.py src dst [K] [shift_px] [Ky]
x' = K*x (a, skew1, tx scaled by K), then + shift_px (applied to tx, for right-anchoring)."""
import struct,sys
from swfdump import Bits,rect,matrix,tags
def put(bits,v,n):
    for i in range(n-1,-1,-1): bits.append((v>>i)&1)
def nb(*vals):
    n=1
    for v in vals:
        v=int(v); 
        while not (-(1<<(n-1))<=v<(1<<(n-1))): n+=1
    return n
def enc_matrix(sx,sy,r0,r1,tx,ty,has_scale,has_rot):
    b=[]
    if has_scale:
        b.append(1); a=int(round(sx*65536)); c=int(round(sy*65536)); n=nb(a,c); put(b,n,5); put(b,a&((1<<n)-1),n); put(b,c&((1<<n)-1),n)
    else: b.append(0)
    if has_rot:
        b.append(1); a=int(round(r0*65536)); c=int(round(r1*65536)); n=nb(a,c); put(b,n,5); put(b,a&((1<<n)-1),n); put(b,c&((1<<n)-1),n)
    else: b.append(0)
    tx=int(round(tx)); ty=int(round(ty)); n=nb(tx,ty); put(b,n,5); put(b,tx&((1<<n)-1),n); put(b,ty&((1<<n)-1),n)
    while len(b)%8: b.append(0)
    return bytes(int("".join(map(str,b[i:i+8])),2) for i in range(0,len(b),8))
def raw_matrix_flags(d,p):
    b=Bits(d,p); hs=b.u(1); 
    if hs: n=b.u(5); b.sg(n); b.sg(n)
    hr=b.u(1)
    return hs,hr
def squeeze(d,K,shift_tw,Ky=1.0):
    (x0,x1,y0,y1),p=rect(d,8); p+=4
    out=bytearray(d[:p]); 
    for code,hp,q,ln in tags(d,p):
        tag=d[hp:q+ln]
        if code==26 and (d[q]&6)==2:   # char placed without matrix: add a pure squeeze
            flags=d[q]|4; r=q+5
            body=bytes([flags])+d[q+1:r]+enc_matrix(K,Ky,0,0,shift_tw,0,True,False)+d[r:q+ln]
            tag=(struct.pack("<H",(code<<6)|len(body)) if len(body)<63 else struct.pack("<HI",(code<<6)|63,len(body)))+body
        elif code==26 and d[q]&4:
            flags=d[q]; r=q+3+(2 if flags&2 else 0)
            m,mend=matrix(d,r); hs,hr=raw_matrix_flags(d,r)
            sx,sy,r0,r1,tx,ty=m
            sx*=K; r1*=K; tx=tx*K+shift_tw; sy*=Ky; r0*=Ky; ty*=Ky
            body=d[q:r]+enc_matrix(sx,sy,r0,r1,tx,ty,True,hr)+d[mend:q+ln]
            if len(body)<63: tag=struct.pack("<H",(code<<6)|len(body))+body
            else: tag=struct.pack("<HI",(code<<6)|63,len(body))+body
        out+=tag
    out[4:8]=struct.pack("<I",len(out))
    return bytes(out)
if __name__=="__main__":
    src,dst=sys.argv[1],sys.argv[2]; K=float(sys.argv[3]) if len(sys.argv)>3 else .75; sh=float(sys.argv[4]) if len(sys.argv)>4 else 0; Ky=float(sys.argv[5]) if len(sys.argv)>5 else 1.0
    d=open(src,"rb").read(); o=squeeze(d,K,sh*20,Ky); open(dst,"wb").write(o); print(len(d),"->",len(o))
