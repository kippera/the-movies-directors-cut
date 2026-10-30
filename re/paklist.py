# SPDX-License-Identifier: GPL-3.0-or-later
import re, struct, sys, zlib, os
pak, outdir, pat = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(outdir, exist_ok=True)
d = open(pak, "rb").read()
n = 0
for m in re.finditer(rb"[A-Za-z0-9_./\\ -]+\.(?:%s)(?=\x00)" % pat.encode(), d):
    s = m.start()
    if s < 16: continue
    off, cs, us, tag = struct.unpack_from("<IIII", d, s - 16)
    if off + cs > len(d) or cs < 16 or us > 50_000_000: continue
    blob = d[off + 16:off + cs]
    data = None
    for wbits in (15, -15):
        try: data = zlib.decompress(blob, wbits); break
        except Exception: pass
    if data is None: continue
    name = m.group().decode().replace("\\", "/")
    p = os.path.join(outdir, name); os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    open(p, "wb").write(data); n += 1
print(pak, n, "files")
