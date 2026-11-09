"""The launch logo as the app's own first frame, so HOME's splash and the app's first picture are one picture.

    uv run --with pillow python -I tools/cia/make_launch_logo.py

HOME draws a logo darc on both screens before the app starts. This one is the "one picture per screen, no
animation" layout of yellows8's ctr-logobuilder (Anemone3DS ships the same): six empty .bclan, two .bclyt, one
texture a screen. Here the pictures are the app's first frame: the top screen is player/cia/splash.png (the same
file main.c draws first), the bottom screen is the flat background, both panes stretched over the whole screen.
The textures are ETC1 (what HOME's own logos use). Then the darc is signed (tools/cia/sign_logo.py: HOME checks the
HMAC at its end, without a valid one it panics) and LZ11-packed into the 8 KB file makerom takes with -logo.

The layout files come from a CIA that is known to launch: tools/cia/dl/Anemone3DS.cia (the release from
astronautlevel2/Anemone3DS; fetch it with: gh release download -R astronautlevel2/Anemone3DS --pattern '*.cia').
Writes player/cia/logo.bin and tools/cia/launch_logo_preview.png.
"""
import hashlib, os, struct, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import etc1, logo, sign_logo

TEMPLATE = os.path.join(HERE, 'dl', 'Anemone3DS.cia')
SPLASH = os.path.join(ROOT, 'player', 'cia', 'splash.png')
OUT = os.path.join(ROOT, 'player', 'cia', 'logo.bin')
PREVIEW = os.path.join(HERE, 'launch_logo_preview.png')
BG = (0x12, 0x12, 0x12)
TOP_W, TOP_H = 512, 256      # the picture's texture; the pane stretches it over 400x240, so it is squeezed to fit first
BOT_W, BOT_H = 64, 64


def al(x, a): return (x + a - 1) // a * a


def read_darc(raw):
    """-> (header fields, entries as [name_off_flags, a, b] lists, names block, [(index, name)] for the files)."""
    magic, bom, hsz, ver, fsz, toff, tsz, doff = struct.unpack_from('<4sHHIIIII', raw, 0)
    assert magic == b'darc' and hsz == 0x1C
    count = struct.unpack_from('<III', raw, toff)[2]
    ent = [list(struct.unpack_from('<III', raw, toff + 12 * k)) for k in range(count)]
    names = raw[toff + 12 * count:toff + tsz]
    files = []
    for k, (no, off, sz) in enumerate(ent):
        if not no >> 24 & 1:
            s = names[no & 0xFFFFFF:]
            e = 0
            while s[e:e + 2] != b'\0\0': e += 2
            files.append((k, s[:e].decode('utf-16le')))
    return (fsz, toff, tsz, doff), ent, names, files


def build_darc(template, contents):
    """The template's darc with every file's bytes replaced by contents[name], offsets and sizes redone
    (textures on 0x80, everything else on 0x20, the way darctool lays them out)."""
    (fsz, toff, tsz, doff), ent, names, files = read_darc(template)
    pos = doff
    data = b''
    for k, nm in files:
        body = contents[nm]
        pos = al(pos, 0x80 if nm.endswith('.bclim') else 0x20)
        data += bytes(pos - doff - len(data)) + body
        ent[k][1], ent[k][2] = pos, len(body)
        pos += len(body)
    head = bytearray(template[:doff])
    struct.pack_into('<I', head, 0xC, pos)
    for k, e in enumerate(ent): struct.pack_into('<III', head, toff + 12 * k, *e)
    return bytes(head) + data


def bclim(w, h, rows, footer):
    """ETC1 data (rows of (r, g, b), top row first, as etc1.decode hands them back) + the CLIM footer."""
    d = etc1.encode(rows, w, h)
    f = bytearray(footer)
    struct.pack_into('<I', f, 0xC, len(d) + 0x28)
    struct.pack_into('<HHII', f, 0x1C, w, h, 0x0A, len(d))
    return d + bytes(f)


def full_screen(lyt, w, h):
    """The layout's one picture pane covers the whole screen: centred, no offset, w x h (a stock logo's own
    full-screen background pane does the same: origin 4, 0,0, 400x240)."""
    b = bytearray(lyt)
    off = struct.unpack_from('<H', b, 6)[0]
    for _ in range(struct.unpack_from('<H', b, 0x10)[0]):
        mg, sz = struct.unpack_from('<4sI', b, off)
        if mg == b'pic1':
            body = off + 8
            b[body + 1] = 4
            struct.pack_into('<3f', b, body + 28, 0.0, 0.0, 0.0)
            struct.pack_into('<2f', b, body + 60, float(w), float(h))
        off += sz
    return bytes(b)


def main():
    if not os.path.exists(TEMPLATE):
        raise SystemExit('no %s: gh release download -R astronautlevel2/Anemone3DS --pattern "*.cia" and put it there' % TEMPLATE)
    tl = logo.lz11_dec(logo.exefs_file(logo.ncch(TEMPLATE), b'logo'))
    darc_t, _tail = sign_logo.split(tl)
    (fsz, toff, tsz, doff), ent, names, files = read_darc(darc_t)
    tc = {nm: darc_t[ent[k][1]:ent[k][1] + ent[k][2]] for k, nm in files}
    # the writer must give the template back when nothing changes
    assert build_darc(darc_t, tc) == darc_t, 'darc writer does not reproduce the template'

    c = dict(tc)
    c['NintendoLogo_U_00.bclyt'] = full_screen(tc['NintendoLogo_U_00.bclyt'], 400, 240)
    c['NintendoLogo_D_00.bclyt'] = full_screen(tc['NintendoLogo_D_00.bclyt'], 320, 240)

    # top: the app's first frame, squeezed into the texture so the 400x240 pane stretches it back
    top = Image.open(SPLASH).convert('RGB')
    assert top.size == (400, 240), top.size
    tex = top.resize((TOP_W, TOP_H), Image.LANCZOS)
    rows = [[tex.getpixel((x, y)) for x in range(TOP_W)] for y in range(TOP_H)]
    c['hblogo_top.bclim'] = bclim(TOP_W, TOP_H, rows, tc['hblogo_top.bclim'][-0x28:])
    c['hblogo_bottom.bclim'] = bclim(BOT_W, BOT_H, [[BG] * BOT_W for _ in range(BOT_H)], tc['hblogo_bottom.bclim'][-0x28:])

    darc = build_darc(darc_t, c)
    out = sign_logo.sign_darc(darc, sign_logo.key())   # exits if the LZ11 doesn't fit in 8 KB
    open(OUT, 'wb').write(out)
    used = len(logo.lz11_enc(darc + bytes(32)))
    print('darc %d bytes, LZ11 about %d of 8192' % (len(darc), used))

    # read the file back and draw what HOME would: both screens
    back = logo.lz11_dec(out)
    d2, tail = sign_logo.split(back)
    assert d2 == darc
    (_, toff2, _, _), ent2, _, files2 = read_darc(d2)
    got = {nm: d2[ent2[k][1]:ent2[k][1] + ent2[k][2]] for k, nm in files2}
    tb = got['hblogo_top.bclim']
    w, h = struct.unpack_from('<HH', tb[-0x28:], 0x1C)
    px = etc1.decode(tb[:-0x28], w, h)
    im = Image.new('RGB', (w, h)); im.putdata([p for r in px for p in r])
    im = im.resize((400, 240), Image.LANCZOS)
    sheet = Image.new('RGB', (400 + 320 + 10, 240), (60, 60, 60)); sheet.paste(im, (0, 0))
    sheet.paste(Image.new('RGB', (320, 240), BG), (410, 0))
    sheet.save(PREVIEW)
    # how far each pixel is from the app's own first frame
    diff = max(max(abs(a - b) for a, b in zip(p, q)) for p, q in zip(top.getdata(), im.getdata()))
    print('largest channel difference from splash.png after the round trip: %d' % diff)


if __name__ == '__main__':
    main()
