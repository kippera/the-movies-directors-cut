# SPDX-License-Identifier: GPL-3.0-or-later
import re, struct, sys, zlib, os

pak, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
d = open(pak, "rb").read()
for m in re.finditer(rb"wmvprofile_[a-z_]+\.prx", d):
    s = m.start()
    # entry: [8 hash][u32 off][u32 csize][u32 usize][u32 ?][name]
    off, cs, us, tag = struct.unpack_from("<IIII", d, s - 16)
    blob = d[off + 16:off + cs]
    data = None
    for wbits in (15, -15):
        try:
            data = zlib.decompress(blob, wbits)
            break
        except Exception:
            pass
    name = m.group().decode()
    print(name, hex(off), cs, us, "->", None if data is None else len(data), blob[:4].hex())
    if data is not None:
        open(os.path.join(outdir, name), "wb").write(data)
