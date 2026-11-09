"""Give a .3dsx a different title in the Homebrew Launcher (its embedded SMDH), so a fallback build
on the card doesn't look like the current one.

    python tools/retitle_3dsx.py in.3dsx out.3dsx "Pocket (build 16)"
"""
import struct
import sys

src, dst, title = sys.argv[1], sys.argv[2], sys.argv[3]
b = bytearray(open(src, "rb").read())
assert b[:4] == b"3DSX", "not a 3dsx"
hsize = struct.unpack_from("<H", b, 4)[0]
assert hsize > 32, "no extended header, so no SMDH"
off, size = struct.unpack_from("<II", b, 0x20)
assert b[off:off + 4] == b"SMDH" and size >= 0x2040
enc = title.encode("utf-16-le")[:0x7E]
for k in range(16):   # every language's short title
    at = off + 8 + k * 0x200
    b[at:at + 0x80] = enc + bytes(0x80 - len(enc))
open(dst, "wb").write(b)
print(f"{dst}: {title}")
