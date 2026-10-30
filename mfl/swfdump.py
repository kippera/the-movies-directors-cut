# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal SWF (uncompressed) tag walker: prints tags, PlaceObject matrices, shape bounds."""
import struct,sys
NAMES={0:"End",1:"ShowFrame",2:"DefineShape",4:"PlaceObject",5:"RemoveObject",6:"DefineBits",7:"DefineButton",8:"JPEGTables",9:"SetBackgroundColor",10:"DefineFont",11:"DefineText",12:"DoAction",13:"DefineFontInfo",14:"DefineSound",15:"StartSound",17:"DefineButtonSound",18:"SoundStreamHead",19:"SoundStreamBlock",20:"DefineBitsLossless",21:"DefineBitsJPEG2",22:"DefineShape2",23:"DefineButtonCxform",24:"Protect",26:"PlaceObject2",28:"RemoveObject2",32:"DefineShape3",33:"DefineText2",34:"DefineButton2",35:"DefineBitsJPEG3",36:"DefineBitsLossless2",37:"DefineEditText",39:"DefineSprite",43:"FrameLabel",45:"SoundStreamHead2",46:"DefineMorphShape",48:"DefineFont2",56:"ExportAssets",59:"DoInitAction",70:"PlaceObject3",75:"DefineFont3",83:"DefineShape4"}
class Bits:
    def __init__(s,d,p=0): s.d=d; s.p=p; s.bit=0
    def u(s,n):
        v=0
        for _ in range(n):
            v=(v<<1)|((s.d[s.p]>>(7-s.bit))&1); s.bit+=1
            if s.bit==8: s.bit=0; s.p+=1
        return v
    def sg(s,n):
        v=s.u(n)
        return v-(1<<n) if n and v>>(n-1) else v
    def align(s):
        if s.bit: s.bit=0; s.p+=1
def rect(d,p):
    b=Bits(d,p); n=b.u(5); v=[b.sg(n) for _ in range(4)]; b.align(); return v,b.p
def matrix(d,p):
    b=Bits(d,p); sx=sy=1.0; r0=r1=0.0
    if b.u(1): n=b.u(5); sx=b.sg(n)/65536; sy=b.sg(n)/65536
    if b.u(1): n=b.u(5); r0=b.sg(n)/65536; r1=b.sg(n)/65536
    n=b.u(5); tx=b.sg(n); ty=b.sg(n); b.align(); return (sx,sy,r0,r1,tx,ty),b.p
def tags(d,p,end=None):
    end=end or len(d)
    while p<end:
        h=struct.unpack_from("<H",d,p)[0]; code=h>>6; ln=h&63; hp=p; p+=2
        if ln==63: ln=struct.unpack_from("<I",d,p)[0]; p+=4
        yield code,hp,p,ln
        p+=ln
        if code==0: break
def dump(d,indent=0,start=None,end=None,depth=0):
    if start is None:
        (x0,x1,y0,y1),p=rect(d,8); fr,fc=struct.unpack_from("<HH",d,p); p+=4
        print("stage %.1fx%.1f px, frame rate %.2f, frames %d, version %d, len %d"%((x1-x0)/20,(y1-y0)/20,fr/256,fc,d[3],len(d)))
        start=p; end=len(d)
    for code,hp,p,ln in tags(d,start,end):
        nm=NAMES.get(code,"tag%d"%code); pad="  "*depth
        extra=""
        if code in (2,22,32,83):
            cid=struct.unpack_from("<H",d,p)[0]; (a,b_,c,e),_=rect(d,p+2); extra="id=%d bounds x %.1f..%.1f y %.1f..%.1f px"%(cid,a/20,b_/20,c/20,e/20)
        elif code in (26,70):
            flags=d[p]; depth_=struct.unpack_from("<H",d,p+1)[0]; q=p+3; info="depth=%d"%depth_
            if flags&2: info+=" char=%d"%struct.unpack_from("<H",d,q)[0]; q+=2
            if flags&4: m,q=matrix(d,q); info+=" matrix scale=(%.3f,%.3f) rot=(%.3f,%.3f) t=(%.1f,%.1f)px"%(m[0],m[1],m[2],m[3],m[4]/20,m[5]/20)
            if flags&16:
                nmst=d.index(b"\0",q); info+=" name=%s"%d[q:nmst].decode("latin1") if False else ""
            extra=info+" flags=0x%02x"%flags
        elif code==39:
            cid,fc=struct.unpack_from("<HH",d,p); extra="id=%d frames=%d"%(cid,fc)
        elif code in (20,36,21,35):
            extra="id=%d"%struct.unpack_from("<H",d,p)[0]
        elif code==11 or code==33:
            cid=struct.unpack_from("<H",d,p)[0]; (a,b_,c,e),_=rect(d,p+2); extra="id=%d bounds x %.1f..%.1f y %.1f..%.1f"%(cid,a/20,b_/20,c/20,e/20)
        print("%s%-18s len=%-5d %s"%(pad,nm,ln,extra))
        if code==39 and depth<3: dump(d,indent,p+4,p+ln,depth+1)
if __name__=="__main__": dump(open(sys.argv[1],"rb").read())
