"""The HOME Menu banner as a real 3D model: the two cassettes of mockups/banner3d, built as a CGFX.

    uv run --with numpy --with pillow python -I tools/cia/make_banner3d.py

The banner CGFX bannertool writes (tools/cia/banner_tpl.cgfx) is one flat 26x13 rectangle with a 256x128
RGBA4 picture on it. We keep every byte of its structure (model, mesh, material, the picture's TXOB) and
change three things: the mesh's vertex and index streams (new ones, appended after the picture), the
picture itself (an atlas: the two cassette faces, and a grid of flat swatches for every other surface),
and the mesh's bounding box. Lighting is baked: every surface is a swatch picked for its angle to the
light, because the material has the one texture and nothing else.

Writes player/cia/banner3d.cgfx and a preview, tools/cia/banner3d_preview.png (a software render of what
it just wrote: its own vertices, indices and picture, read back from the file).
"""
import math, os, struct, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
TPL = os.path.join(HERE, 'banner_tpl.cgfx')
OUT = os.path.join(ROOT, 'player', 'cia', 'banner3d.cgfx')
PREVIEW = os.path.join(HERE, 'banner3d_preview.png')
SVG2PNG = os.path.join(HERE, 'svg2png.sh')

RADIUS = 8.0   # the most it may sweep from the spin axis, in banner units (the flat picture is 26 across)
AW, AH = 256, 128   # the picture
GREEN, DEEP, CREAM, FOOT, INK, TAPE, METAL = (0x1ed760, 0x0a6e39, 0xf2e6cf, 0xd9caa8, 0x121212, 0x4a2e1a, 0xa8a8a8)

# ---- small vector helpers --------------------------------------------------------------------------------------
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    l = math.sqrt(dot(a, a)) or 1.0
    return (a[0] / l, a[1] / l, a[2] / l)
def mmul(A, B): return [[sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
def mapply(M, v): return tuple(sum(M[i][k] * v[k] for k in range(3)) for i in range(3))
def rx(a): c, s = math.cos(a), math.sin(a); return [[1, 0, 0], [0, c, -s], [0, s, c]]
def ry(a): c, s = math.cos(a), math.sin(a); return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
def rz(a): c, s = math.cos(a), math.sin(a); return [[c, -s, 0], [s, c, 0], [0, 0, 1]]

# ---- the shapes (all in a cassette's own units: 5.92 wide, 3.7888 tall, 0.72 thick, front at +z) --------------
W, H, T = 5.92, 3.7888, 0.72
BEV, RAD = 0.06, 0.4736
CSEG, PSEG = 4, 8   # arc segments a corner / a window end

def rrect(w, h, r, seg=CSEG):   # counter-clockwise from the right edge's foot
    out = []
    for (cx, cy, a0) in ((w / 2 - r, -h / 2 + r, -90), (w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90), (-w / 2 + r, -h / 2 + r, 180)):
        for i in range(seg + 1):
            a = math.radians(a0 + 90 * i / seg)
            out.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return out

def pill(cx, cy, w, h, seg=PSEG):
    r = h / 2
    out = []
    for (px, a0) in ((cx + w / 2 - r, -90), (cx - w / 2 + r, 90)):
        for i in range(seg + 1):
            a = math.radians(a0 + 180 * i / seg)
            out.append((px + r * math.cos(a), cy + r * math.sin(a)))
    return out

def circle(cx, cy, r, n):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]

def area2(p): return sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p)))

def seg_hit(a, b, c, d):   # do segments ab and cd cross (not merely touch)?
    def o(p, q, r): return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    return o(a, b, c) * o(a, b, d) < -1e-12 and o(c, d, a) * o(c, d, b) < -1e-12

def triangulate(outer, hole=None):
    """Ear-clip a counter-clockwise polygon, with an optional hole (bridged in). Returns triples of (x, y)."""
    poly = list(outer)
    if hole:
        hole = list(hole)
        if area2(hole) > 0: hole.reverse()   # holes run clockwise
        best = None
        for i, o in enumerate(poly):
            for j, h in enumerate(hole):
                d = (o[0] - h[0]) ** 2 + (o[1] - h[1]) ** 2
                if best and d >= best[0]: continue
                ok = True
                for k in range(len(poly)):
                    if k in (i, (i - 1) % len(poly)): continue
                    if seg_hit(o, h, poly[k], poly[(k + 1) % len(poly)]): ok = False; break
                if ok:
                    for k in range(len(hole)):
                        if k in (j, (j - 1) % len(hole)): continue
                        if seg_hit(o, h, hole[k], hole[(k + 1) % len(hole)]): ok = False; break
                if ok: best = (d, i, j)
        _, i, j = best
        poly = poly[:i + 1] + hole[j:] + hole[:j + 1] + poly[i:]
    idx = list(range(len(poly)))
    tris = []
    def inside(p, a, b, c):
        def s(p1, p2, p3): return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
        d1, d2, d3 = s(p, a, b), s(p, b, c), s(p, c, a)
        return not ((d1 < -1e-12 or d2 < -1e-12 or d3 < -1e-12) and (d1 > 1e-12 or d2 > 1e-12 or d3 > 1e-12))
    guard = 0
    while len(idx) > 3 and guard < 100000:
        guard += 1
        for n in range(len(idx)):
            a, b, c = poly[idx[n - 1]], poly[idx[n]], poly[idx[(n + 1) % len(idx)]]
            if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) <= 1e-12: continue   # reflex or flat
            if any(inside(poly[m], a, b, c) and poly[m] not in (a, b, c) for m in idx if m not in (idx[n - 1], idx[n], idx[(n + 1) % len(idx)])): continue
            tris.append((a, b, c))
            del idx[n]
            break
        else:
            raise SystemExit('triangulate: no ear found (the outline crosses itself?)')
    if len(idx) == 3: tris.append((poly[idx[0]], poly[idx[1]], poly[idx[2]]))
    want = abs(area2(outer)) / 2 - (abs(area2(hole)) / 2 if hole else 0)
    got = sum(abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2 for a, b, c in tris)
    if abs(got - want) > 1e-6 * max(1.0, want): raise SystemExit('triangulate: area %.6f, wanted %.6f' % (got, want))
    return tris

# ---- the atlas swatches and the mesh ---------------------------------------------------------------------------
LIGHT = norm((-5, 7, 10))
def lit(n):   # the mockup's hemisphere plus one sun, as one number
    return min(1.0, 0.56 + 0.16 * (n[1] * 0.5 + 0.5) + 0.46 * max(0.0, dot(n, LIGHT)))

class Atlas:
    """Faces occupy the top; flat swatches (8x8 px, sampled at the middle) fill the rest."""
    FACES = {'cream': (0, 0, 160, 102), 'green': (160, 0, 96, 61)}
    def __init__(self):
        self.cells = {}
        free = []
        for y in range(104, AH, 8):
            for x in range(0, AW, 8): free.append((x, y))
        for y in range(64, 104, 8):
            for x in range(160, AW, 8): free.append((x, y))
        self.free = free
    def swatch(self, rgb, k):   # rgb 0xRRGGBB scaled by light k, quantised to the picture's 4 bits a channel
        c = tuple(min(15, int(round(((rgb >> s) & 255) * k / 17))) for s in (16, 8, 0))
        if c not in self.cells:
            if not self.free: raise SystemExit('atlas out of swatches')
            self.cells[c] = self.free.pop(0)
        x, y = self.cells[c]
        return ((x + 4) / AW, 1 - (y + 4) / AH)
    def face_uv(self, name, fx, fy):   # fx, fy in 0..1 across the face, y down
        x0, y0, w, h = self.FACES[name]
        return ((x0 + fx * w) / AW, 1 - (y0 + fy * h) / AH)

class Mesh:
    def __init__(self, atlas):
        self.v, self.i, self.atlas = [], [], atlas
        self.tris = 0
    def tri(self, P, uv):   # P three world points, uv three (u, v)
        base = len(self.v)
        for p, t in zip(P, uv): self.v.append((p[0], p[1], p[2], t[0], t[1]))
        self.i += [base, base + 1, base + 2]
        self.tris += 1

class Piece:
    """Builds triangles in a cassette's local space; world() puts them where the pile has them."""
    def __init__(self, mesh, M, off, scale):
        self.mesh, self.M, self.off, self.s = mesh, M, off, scale
    def w(self, p): return tuple(self.s * c for c in add(mapply(self.M, p), self.off))
    def emit(self, a, b, c, uv, ref=None, outward=True, normal=None):
        n = norm(cross(sub(b, a), sub(c, a)))
        if normal is not None: flip = dot(n, normal) < 0
        else:
            mid = ((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3)
            flip = (dot(n, sub(mid, ref)) < 0) == outward
        if flip: b, c, uv = c, b, (uv[0], uv[2], uv[1])
        n = norm(cross(sub(b, a), sub(c, a)))
        self.mesh.tri([self.w(a), self.w(b), self.w(c)], uv)
        return mapply(self.M, n)   # the normal, in the world
    def flat(self, rgb, a, b, c, **kw):
        # the light needs the world normal, which emit returns after it has made its swatch: so find it first
        n = norm(cross(sub(b, a), sub(c, a)))
        ref = kw.get('ref'); outward = kw.get('outward', True); normal = kw.get('normal')
        if normal is not None: flip = dot(n, normal) < 0
        else:
            mid = ((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3)
            flip = (dot(n, sub(mid, ref)) < 0) == outward
        if flip: n = (-n[0], -n[1], -n[2])
        k = lit(mapply(self.M, n))
        t = self.mesh.atlas.swatch(rgb, k)
        self.emit(a, b, c, (t, t, t), **kw)
    def quad(self, rgb, a, b, c, d, **kw):   # a b c d round the quad
        self.flat(rgb, a, b, c, **kw); self.flat(rgb, a, c, d, **kw)
    def ring(self, rgb, top, bot, ref, outward=True):   # two equal-length loops: a skirt between them
        n = len(top)
        for k in range(n):
            j = (k + 1) % n
            self.quad(rgb, top[k], bot[k], bot[j], top[j], ref=ref, outward=outward)
    def cap(self, rgb, pts, z, normal, ref=None):
        pts3 = [(x, y, z) for x, y in pts]
        for k in range(1, len(pts3) - 1): self.flat(rgb, pts3[0], pts3[k], pts3[k + 1], normal=normal)
    def cylinder(self, rgb, cx, cy, r, z0, z1, n, cap=None):
        top = [(cx + r * math.cos(2 * math.pi * k / n), cy + r * math.sin(2 * math.pi * k / n), z1) for k in range(n)]
        bot = [(p[0], p[1], z0) for p in top]
        self.cap(rgb, [(p[0], p[1]) for p in bot], z0, (0, 0, -1))
        self.ring(rgb, top, bot, ref=(cx, cy, (z0 + z1) / 2))
        self.cap(cap if cap is not None else rgb, [(p[0], p[1]) for p in top], z1, (0, 0, 1))

def build_cassette(mesh, face, M, off, scale, shell_rgb, reel_l, reel_r, back=None):
    P = Piece(mesh, M, off, scale)
    ctr = (0, 0, T / 2)
    full = rrect(W, H, RAD)
    inset = rrect(W - 2 * BEV, H - 2 * BEV, RAD - BEV)
    wx, wy, ww, wh = 0.0, 0.1137, 3.7888, 1.2882
    win = pill(wx, wy, ww, wh)
    z = lambda pts, zz: [(x, y, zz) for x, y in pts]
    # the back, then the walls, then the face -- back to front
    if back is None:
        P.cap(shell_rgb, full, 0.0, (0, 0, -1))
    else:   # a picture on the back, seen from behind: left and right swap, so u runs against x
        for a, b, c in triangulate(full):
            pts = [(p[0], p[1], 0.0) for p in (a, b, c)]
            uv = tuple(mesh.atlas.face_uv(back, (W / 2 - p[0]) / W, (H / 2 - p[1]) / H) for p in (a, b, c))
            P.emit(*pts, uv, normal=(0, 0, -1))
    P.ring(DEEP, z(full, T - BEV), z(full, 0.0), ref=ctr)                  # the wall
    P.ring(DEEP, z(inset, T), z(full, T - BEV), ref=ctr)                   # the chamfer round the face
    # the face: the picture, with the window cut through it
    for a, b, c in triangulate(inset, win):
        pts = [(p[0], p[1], T) for p in (a, b, c)]
        uv = tuple(mesh.atlas.face_uv(face, (p[0] + W / 2) / W, (H / 2 - p[1]) / H) for p in (a, b, c))
        P.emit(*pts, uv, normal=(0, 0, 1))
    # inside the window: its wall, a dark plate halfway down, the tape packs and the reels in front of it
    P.ring(DEEP, z(win, T), z(win, T * 0.3), ref=(wx, wy, T * 0.65), outward=False)
    P.cap(INK, win, T * 0.3, (0, 0, 1))
    for (x, col, pack) in ((-1.25, reel_l, 0.62), (1.25, reel_r, 0.38)):
        P.cylinder(TAPE, x, wy, pack, T * 0.3, T * 0.6, 20)
        P.cylinder(col, x, wy, 0.4637, T * 0.3, T * 0.85, 20)
        P.cap(INK, circle(x, wy, 0.195, 6), T * 0.85 + 0.06, (0, 0, 1))   # the hex the spindle grips
    # the foot: the raised trapezoid where the heads go
    foot = [(-2.96 + 1.3024, -H / 2 + 0.0), (-2.96 + 1.7168, -H / 2 + 0.8335), (-2.96 + 4.2032, -H / 2 + 0.8335), (-2.96 + 4.6176, -H / 2 + 0.0)]
    fz = T + 0.08
    ctr_f = (0, -H / 2 + 0.4, (T + fz) / 2)
    P.cap(FOOT, [(p[0], p[1] + 0.0) for p in foot], fz, (0, 0, 1))
    P.ring(FOOT, z(foot, fz), z(foot, T), ref=ctr_f)
    # screws: the four corners, one on the foot
    for (sx, sy, sz) in ((-2.96 + 0.31, H / 2 - 0.31, T), (2.96 - 0.31, H / 2 - 0.31, T), (-2.96 + 0.31, -H / 2 + 0.31, T), (2.96 - 0.31, -H / 2 + 0.31, T), (0.0, -H / 2 + 0.39, fz)):
        P.cylinder(METAL, sx, sy, 0.12, sz, sz + 0.06, 8)

# ---- the face pictures: drawn as svg, rendered by Chrome ---------------------------------------------------------
def faces_svg():
    glyph = open(os.path.join(ROOT, 'player', 'cia', 'banner.svg'), encoding='utf-8').read()
    defs = glyph[glyph.index('<defs>') + 6:glyph.index('</defs>')]
    k = 0.008266
    word = (f'<g transform="translate(11.84 9.99) scale({k} {-k})" fill="#{DEEP:06x}"><use href="#b33"/></g>'
            f'<g transform="translate(16.80 9.99) scale({k} {-k})" fill="#{INK:06x}"><use href="#b44"/><use href="#b53" x="697"/></g>'
            f'<g transform="translate(27.72 9.99) scale({k} {-k})" fill="#{INK:06x}"><use href="#r74"/><use href="#r65" x="351"/><use href="#r72" x="900"/><use href="#r65" x="1267"/><use href="#r6f" x="1816"/></g>')
    stripe = f'<rect x="5.92" y="4.925" width="47.36" height="3.789" rx="1.8944" fill="#{CREAM:06x}"/>'
    def face(x, y, w, h, fill, body): return f'<svg x="{x}" y="{y}" width="{w}" height="{h}" viewBox="0 0 59.2 37.888"><rect width="59.2" height="37.888" fill="#{fill:06x}"/>{body}</svg>'
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {AW} {AH}" width="{AW}" height="{AH}"><defs>{defs}</defs>'
            + face(0, 0, 160, 102.4, CREAM, word) + face(160, 0, 96, 61.4, GREEN, stripe) + '</svg>')

def render_faces(tmp):
    svg = os.path.join(tmp, 'faces.svg')
    open(svg, 'w', encoding='utf-8').write(faces_svg())
    png = os.path.join(tmp, 'faces.png')
    subprocess.run(['sh', SVG2PNG, svg, str(AW), str(AH), png], check=True, stdout=subprocess.DEVNULL)
    return Image.open(png).convert('RGBA')

# ---- RGBA4, 8x8 tiles, Morton order inside a tile -------------------------------------------------------------------
def morton(i):
    return ((i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)), (((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4))

def encode_tex(img):   # img: HxWx4 uint8, row 0 the top: what this CGFX's mesh reads as v = 1
    q = (img.astype(np.int32) + 8) // 17
    q = np.clip(q, 0, 15)
    v = (q[..., 0] << 12) | (q[..., 1] << 8) | (q[..., 2] << 4) | q[..., 3]
    out = np.zeros(AW * AH, '<u2')
    k = 0
    for ty in range(AH // 8):
        for tx in range(AW // 8):
            for i in range(64):
                x, y = morton(i)
                out[k] = v[ty * 8 + y, tx * 8 + x]; k += 1
    return out.tobytes()

def decode_tex(buf):
    d = np.frombuffer(buf, '<u2')
    img = np.zeros((AH, AW, 4), np.uint8)
    k = 0
    for ty in range(AH // 8):
        for tx in range(AW // 8):
            for i in range(64):
                x, y = morton(i); w = int(d[k]); k += 1
                img[ty * 8 + y, tx * 8 + x] = (((w >> 12) & 15) * 17, ((w >> 8) & 15) * 17, ((w >> 4) & 15) * 17, (w & 15) * 17)
    return img

# ---- patching the template --------------------------------------------------------------------------------------
def patch(tpl, verts, idx, tex):
    b = bytearray(tpl)
    assert b[:4] == b'CGFX' and len(b) == 0x11580
    def u32(o): return struct.unpack_from('<I', b, o)[0]
    # the places this template keeps them (checked against what's there before touching anything)
    assert b[0x14f8:0x14fc] == b'IMAG' and u32(0x14fc) == 0x11580 - 0x14f8
    assert struct.unpack_from('<3f', b, 0xd98) == (0.0, -1.0, 0.0) and struct.unpack_from('<3f', b, 0xdc8) == (26.0, 13.0, 0.0)
    assert u32(0x1000) == 5   # the bone's billboard mode: the flat template's is 5, which keeps it facing the viewer and cancels HOME's spin
    assert u32(0xe08) == 0x1401 and u32(0xe10) == 6 and u32(0xe48) == 0x50 and u32(0xe58) == 0x14
    assert 0xe14 + u32(0xe14) == 0x1500 and 0xe4c + u32(0xe4c) == 0x1508
    assert u32(0x1054) + 0x1054 == 0x1580 and u32(0x1050) == 0x10000
    b[0x1580:0x1580 + len(tex)] = tex
    ibuf = struct.pack('<%dH' % len(idx), *idx)
    ibuf += b'\0' * (-len(ibuf) % 16)
    vbuf = b''.join(struct.pack('<5f', *v) for v in verts)
    vbuf += b'\0' * (-len(vbuf) % 16)
    ipos = len(b)
    vpos = ipos + len(ibuf)
    b += ibuf + vbuf
    struct.pack_into('<I', b, 0x1000, 0)                      # no billboarding: HOME turns the model
    struct.pack_into('<I', b, 0xe08, 0x1403)                  # unsigned short indices
    struct.pack_into('<I', b, 0xe10, len(idx) * 2)
    struct.pack_into('<I', b, 0xe14, ipos - 0xe14)
    struct.pack_into('<I', b, 0xe48, len(verts) * 20)
    struct.pack_into('<I', b, 0xe4c, vpos - 0xe4c)
    xs, ys, zs = ([v[k] for v in verts] for k in range(3))
    struct.pack_into('<3f', b, 0xd98, (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2)
    struct.pack_into('<3f', b, 0xdc8, max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
    struct.pack_into('<I', b, 0x14fc, u32(0x14fc) + len(ibuf) + len(vbuf))   # IMAG's size
    struct.pack_into('<I', b, 0xc, len(b))
    return bytes(b)

# ---- the preview: read the file back and draw it --------------------------------------------------------------------
def preview(cg, path, scale=3, cull=True, yaw=0.0):
    b = cg
    u32 = lambda o: struct.unpack_from('<I', b, o)[0]
    isz, ioff = u32(0xe10), 0xe14 + u32(0xe14)
    vsz, voff = u32(0xe48), 0xe4c + u32(0xe4c)
    idx = np.frombuffer(b[ioff:ioff + isz], '<u2').astype(int)
    vv = np.frombuffer(b[voff:voff + vsz], '<f4').reshape(-1, 5)
    tex = decode_tex(b[0x1580:0x1580 + 65536])
    Wp, Hp = 256 * scale, 128 * scale
    # the camera: looks down -z from +z, the picture plane z=0 spanning x -13..13, y -7.5..5.5
    fov = math.radians(24); d = 6.5 / math.tan(fov / 2); cy = -1.0
    img = np.zeros((Hp, Wp, 3), np.float32); img[:] = (0x5d, 0x60, 0x66)
    zbuf = np.full((Hp, Wp), 1e9, np.float32)
    def proj(p):
        s = d / (d - p[2])
        return ((p[0] * s + 13) / 26 * Wp, (5.5 - (p[1] - cy) * s - cy * 0 - 0) / 13 * Hp, d - p[2])
    for t in range(0, len(idx), 3):
        a, bb, c = (vv[idx[t + k]] for k in range(3))
        if yaw:
            cy_, sy_ = math.cos(yaw), math.sin(yaw)
            a, bb, c = ([v[0] * cy_ + v[2] * sy_, v[1], -v[0] * sy_ + v[2] * cy_, v[3], v[4]] for v in (a, bb, c))
        pa, pb, pc = proj(a), proj(bb), proj(c)
        e = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0])
        if e == 0: continue
        if cull and e > 0: continue   # screen y runs down: counter-clockwise from the viewer is e < 0
        minx, maxx = int(max(0, min(pa[0], pb[0], pc[0]))), int(min(Wp - 1, max(pa[0], pb[0], pc[0])))
        miny, maxy = int(max(0, min(pa[1], pb[1], pc[1]))), int(min(Hp - 1, max(pa[1], pb[1], pc[1])))
        if maxx < minx or maxy < miny: continue
        xs, ys = np.meshgrid(np.arange(minx, maxx + 1) + 0.5, np.arange(miny, maxy + 1) + 0.5)
        w0 = ((pb[0] - xs) * (pc[1] - ys) - (pb[1] - ys) * (pc[0] - xs)) / e
        w1 = ((pc[0] - xs) * (pa[1] - ys) - (pc[1] - ys) * (pa[0] - xs)) / e
        w2 = 1 - w0 - w1
        m = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
        if not m.any(): continue
        z = w0 * pa[2] + w1 * pb[2] + w2 * pc[2]
        u = w0 * a[3] + w1 * bb[3] + w2 * c[3]
        v = w0 * a[4] + w1 * bb[4] + w2 * c[4]
        tx = np.clip((u * AW).astype(int), 0, AW - 1); ty = np.clip(((1 - v) * AH).astype(int), 0, AH - 1)
        col = tex[ty, tx, :3]
        sl = (slice(miny, maxy + 1), slice(minx, maxx + 1))
        ok = m & (z < zbuf[sl])
        zbuf[sl] = np.where(ok, z, zbuf[sl])
        for ch in range(3): img[sl + (ch,)] = np.where(ok, col[..., ch], img[sl + (ch,)])
    Image.fromarray(img.astype(np.uint8)).save(path)

def finish(mesh):
    """Identical (position, uv) vertices merged; the triangles keep the order they were built in."""
    tris = [mesh.v[i:i + 3] for i in range(0, len(mesh.v), 3)]
    table, verts, idx = {}, [], []
    for t in tris:
        for v in t:
            k = tuple(round(c, 5) for c in v)
            if k not in table: table[k] = len(verts); verts.append(v)
            idx.append(table[k])
    if len(verts) > 65535: raise SystemExit('too many vertices for 16-bit indices')
    return verts, idx

def main():
    import tempfile
    tpl = open(TPL, 'rb').read()
    atlas = Atlas()
    mesh = Mesh(atlas)
    # the pile (mockups/banner3d): green underneath, cream on top, the whole thing tipped toward the viewer
    pile = mmul(rx(-0.32), ry(0.22))
    under_M = mmul(pile, rz(math.radians(-9)))
    top_M = mmul(pile, rz(math.radians(7)))
    # the offsets are in the pile's own frame (so the two stay parallel, the top one just in front), then the pile tips
    under_off, top_off = mapply(pile, (0.95, 0.32, 0.0)), mapply(pile, (-0.55, -0.32, T + 0.24))
    # fit it: build once at scale 1 to measure, then scale to the room the banner has
    probe = Mesh(Atlas())
    for (M, off) in ((under_M, under_off), (top_M, top_off)):
        build_cassette(probe, 'cream', M, off, 1.0, CREAM, GREEN, CREAM)
    pts = [(v[0], v[1], v[2]) for v in probe.v]
    ext = [(min(p[k] for p in pts), max(p[k] for p in pts)) for k in range(3)]
    cx_, cz_ = (ext[0][0] + ext[0][1]) / 2, (ext[2][0] + ext[2][1]) / 2
    radius = max(math.hypot(p[0] - cx_, p[2] - cz_) for p in pts)   # how far it sweeps when HOME turns it about the vertical axis through the origin
    K = min(RADIUS / radius, 11.2 / (ext[1][1] - ext[1][0]))
    off0 = (-cx_, -(ext[1][0] + ext[1][1]) / 2 + (-1.0 / K), -cz_)   # the middle of it on the spin axis
    print('scale', round(K, 3), 'extent', [round((e[1] - e[0]) * K, 2) for e in ext])
    for (M, off, face, shell, rl, rr_) in ((under_M, under_off, 'green', GREEN, CREAM, GREEN), (top_M, top_off, 'cream', CREAM, GREEN, CREAM)):
        build_cassette(mesh, face, M, add(off, off0), K, shell, rl, rr_)
    print('triangles', mesh.tris, 'vertices', len(mesh.v), 'swatches', len(atlas.cells))

    with tempfile.TemporaryDirectory() as tmp:
        img = np.array(render_faces(tmp))
    # light the two faces the way their surfaces are lit
    for name, M in (('green', under_M), ('cream', top_M)):
        x0, y0, w, h = Atlas.FACES[name]
        k = lit(mapply(M, (0, 0, 1)))
        img[y0:y0 + h, x0:x0 + w, :3] = (img[y0:y0 + h, x0:x0 + w, :3] * k).astype(np.uint8)
    for c, (x, y) in atlas.cells.items():
        img[y:y + 8, x:x + 8] = (c[0] * 17, c[1] * 17, c[2] * 17, 255)
    tex = encode_tex(img)
    verts, idx = finish(mesh)
    print('after merging', len(verts), 'vertices,', len(idx) // 3, 'triangles')
    out = patch(tpl, verts, idx, tex)
    open(OUT, 'wb').write(out)
    print('wrote', OUT, len(out), 'bytes')
    from PIL import Image as _I
    sheet = _I.new('RGB', (768 * 2, 384 * 2))
    for n, deg in enumerate((0, 60, 120, 180)):
        preview(out, PREVIEW, yaw=math.radians(deg))
        sheet.paste(_I.open(PREVIEW), ((n % 2) * 768, (n // 2) * 384))
    sheet.save(PREVIEW)
    print('preview', PREVIEW)

if __name__ == '__main__':
    main()
