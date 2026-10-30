# SPDX-License-Identifier: GPL-3.0-or-later
"""mkprofile.py <src.prx> <dst.prx> <width> <height> <video_bitrate> [label]: derive an export profile from a stock one (UTF-16 XML): new video size and bitrate"""
import re,sys
src,dst,W,H,BR=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5]); label=sys.argv[6] if len(sys.argv)>6 else None
t=open(src,'rb').read().decode('utf-16')
t=re.sub(r'(rcsource left="0"\s+top="0"\s+right=")\d+("\s+bottom=")\d+',lambda m:m.group(1)+str(W)+m.group(2)+str(H),t)
t=re.sub(r'(rctarget left="0"\s+top="0"\s+right=")\d+("\s+bottom=")\d+',lambda m:m.group(1)+str(W)+m.group(2)+str(H),t)
t=re.sub(r'(biwidth=")\d+("\s+biheight=")\d+',lambda m:m.group(1)+str(W)+m.group(2)+str(H),t,flags=re.I)
t=re.sub(r'(dwbitrate=")\d+',lambda m:m.group(1)+str(BR),t)
# the video stream's bitrate attribute (the one after Video Stream)
t=re.sub(r'(streamname="Video Stream"\s+inputname="Video\d+"\s+bitrate=")\d+',lambda m:m.group(1)+str(BR),t)
if label: t=re.sub(r'(<profile[^>]*?name=")[^"]*"',lambda m:m.group(1)+label+'"',t,count=1,flags=re.S)
open(dst,'wb').write(b'\xff\xfe'+t.replace('﻿','').encode('utf-16-le'))
print(dst,W,H,BR)
