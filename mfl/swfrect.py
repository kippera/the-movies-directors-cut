# SPDX-License-Identifier: GPL-3.0-or-later
"""Read/write the stage RECT (twips) in the header of an uncompressed SWF (.mfl)."""
import sys
def read_rect(d):
    n=d[8]>>3; total=5+4*n; nb=(total+7)//8; v=int.from_bytes(d[8:8+nb],"big")>>(nb*8-total); out=[]
    for i in range(4):
        x=(v>>(total-5-n*(i+1)))&((1<<n)-1)
        if x>=1<<(n-1): x-=1<<n
        out.append(x)
    return out,nb
def write_rect(d,vals):
    old_n=d[8]>>3; old_total=5+4*old_n; old_nb=(old_total+7)//8
    n=max(max(abs(v) for v in vals).bit_length()+1,1)
    total=5+4*n; nb=(total+7)//8
    bits=n
    acc=n
    for v in vals: acc=(acc<<n)|(v&((1<<n)-1))
    acc<<=(nb*8-total)
    new=acc.to_bytes(nb,"big")
    out=d[:8]+new+d[8+old_nb:]
    # fix the file length field (bytes 4..7, little endian)
    import struct
    return out[:4]+struct.pack("<I",len(out))+out[8:]
if __name__=="__main__":
    src,dst,w,h=sys.argv[1],sys.argv[2],float(sys.argv[3]),float(sys.argv[4])
    d=open(src,"rb").read(); (x0,x1,y0,y1),nb=read_rect(d)
    print("old stage twips",x0,x1,y0,y1,"->",(x1-x0)/20,"x",(y1-y0)/20,"px")
    out=write_rect(d,[0,int(round(w*20)),0,int(round(h*20))])
    open(dst,"wb").write(out)
    (a,b,c,e),_=read_rect(out); print("new stage",(b-a)/20,"x",(e-c)/20,"px; size",len(d),"->",len(out))
