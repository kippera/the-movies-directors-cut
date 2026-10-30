# SPDX-License-Identifier: GPL-3.0-or-later
"""Estimate the x extent (px) of an .mfl's content: union of shape bounds through all placements (all frames)."""
import struct,sys
from swfdump import rect,matrix,tags
FIRST=False
def parse(d):
    (_,_,_,_),p=rect(d,8); p+=4
    shapes={}; sprites={}
    def walk(start,end):
        out=[]; done=False
        for code,hp,q,ln in tags(d,start,end):
            if code==1 and FIRST: done=True
            if done and code in (26,): continue
            if code in(2,22,32,83):
                cid=struct.unpack_from("<H",d,q)[0]; r,_=rect(d,q+2); shapes[cid]=r
            elif code==39:
                cid,fc=struct.unpack_from("<HH",d,q); sprites[cid]=walk(q+4,q+ln)
            elif code==26:
                fl=d[q]; r=q+3
                if fl&2: ch=struct.unpack_from("<H",d,r)[0]; r+=2
                else: ch=None
                m=matrix(d,r)[0] if fl&4 else None
                if ch is not None: out.append((ch,m))
        return out
    root=walk(p,len(d)); return root,shapes,sprites
def ext(ch,m,shapes,sprites,seen=()):
    # returns (xmin,xmax) px
    if m is None: m=(1,1,0,0,0,0)
    sx,sy,r0,r1,tx,ty=m
    if ch in shapes:
        x0,x1,y0,y1=shapes[ch]; xs=[sx*x+r1*y+tx for x in (x0,x1) for y in (y0,y1)]
        return min(xs)/20 if False else min(xs),max(xs)
    if ch in sprites and ch not in seen:
        lo,hi=1e18,-1e18
        for c2,m2 in sprites[ch]:
            r=ext(c2,m2,shapes,sprites,seen+(ch,))
            if r: 
                # r in twips of child space -> parent: apply this matrix
                a,b=r; xs=[sx*a+tx,sx*b+tx]; lo=min(lo,min(xs)); hi=max(hi,max(xs))
        return (lo,hi) if lo<hi else None
    return None
if __name__=="__main__":
    FIRST=len(sys.argv)>1 and sys.argv[1]=="-1"
    sys.argv=[a for a in sys.argv if a!="-1"]
    for f in sys.argv[1:]:
        d=open(f,'rb').read(); root,shapes,sprites=parse(d)
        lo,hi=1e18,-1e18
        for ch,m in root:
            r=ext(ch,m,shapes,sprites)
            if r: lo=min(lo,r[0]); hi=max(hi,r[1])
        print(f,"x %.1f .. %.1f px"%(lo/20,hi/20))
