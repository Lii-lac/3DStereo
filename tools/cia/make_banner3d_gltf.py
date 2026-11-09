"""The HOME Menu banner as a real 3D model, the proper way: write the two cassettes of mockups/banner3d as a glTF
(.glb) and let pycgfx (skyfloogle/pycgfx, the converter people use for real banners) turn it into the CGFX.

    uv run --with numpy --with pillow --with gltflib python -I tools/cia/make_banner3d_gltf.py

Why not tools/cia/make_banner3d.py any more: that one patched a model into the flat banner's CGFX bytes by hand, and
on the 3DS its textures came out garbled. pycgfx writes everything a banner model needs (a model named COMMON, its
materials, light environment, lookup tables), and HOME turns the model by itself, so nothing here animates.

It reuses make_banner3d.py's shapes and picture atlas, with two changes: the atlas swatches are the plain colours
(HOME lights the model, so baked light would be counted twice) and every triangle keeps its own normal.

Writes player/cia/banner3d.glb, player/cia/banner3d.cgfx and tools/cia/banner3d_preview.png.
"""
import importlib.util, json, math, os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
PYCGFX = os.path.join(HERE, 'dl', 'pycgfx')
GLB = os.path.join(ROOT, 'player', 'cia', 'banner3d.glb')
CGFX = os.path.join(ROOT, 'player', 'cia', 'banner3d.cgfx')
PREVIEW = os.path.join(HERE, 'banner3d_preview.png')

spec = importlib.util.spec_from_file_location('mb', os.path.join(HERE, 'make_banner3d.py'))
mb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mb)
mb.lit = lambda n: 1.0   # plain colours: HOME's lights do the shading
mb.AW, mb.AH = 256, 256   # the biggest picture a CGFX takes; the backs get their own room


class Atlas(mb.Atlas):
    """Four faces (both cassettes' fronts and backs) on top, flat 8x8 swatches for every other surface below."""
    FACES = {'cream': (0, 0, 160, 102), 'green': (160, 0, 96, 61), 'greenback': (0, 104, 160, 102), 'creamback': (160, 104, 96, 61)}

    def __init__(self):
        self.cells = {}
        self.free = [(x, y) for y in range(208, 256, 8) for x in range(0, 256, 8)]


def faces_svg():
    glyph = open(os.path.join(ROOT, 'player', 'cia', 'banner.svg'), encoding='utf-8').read()
    defs = glyph[glyph.index('<defs>') + 6:glyph.index('</defs>')]
    hx = lambda c: '#%06x' % c

    def word(x, y, k, c3, cds, cr):   # the wordmark, 3DStereo, from the banner's own glyphs (k 0.008266 is the front's size)
        s = k / 0.008266
        return (f'<g transform="translate({x:.2f} {y:.2f}) scale({k} {-k})" fill="{hx(c3)}"><use href="#b33"/></g>'
                f'<g transform="translate({x + 4.96 * s:.2f} {y:.2f}) scale({k} {-k})" fill="{hx(cds)}"><use href="#b44"/><use href="#b53" x="697"/></g>'
                f'<g transform="translate({x + 15.88 * s:.2f} {y:.2f}) scale({k} {-k})" fill="{hx(cr)}"><use href="#r74"/><use href="#r65" x="351"/><use href="#r72" x="900"/><use href="#r65" x="1267"/><use href="#r6f" x="1816"/></g>')

    front_word = word(11.84, 9.99, 0.008266, mb.DEEP, mb.INK, mb.INK)
    stripe = f'<rect x="5.92" y="4.925" width="47.36" height="3.789" rx="1.8944" fill="{hx(mb.CREAM)}"/>'

    def back(shell, label, c3, cink, bar, foot):
        """What you see turning a cassette over: the label with the wordmark, a tracklist and a row of level bars,
        the corner screws, the foot's two holes. (The 59.2 x 37.888 box is the cassette, 10 units a cassette unit.)"""
        o = [f'<rect width="59.2" height="37.888" fill="{hx(shell)}"/>',
             f'<rect x="4.4" y="3.6" width="50.4" height="20" rx="2.4" fill="{hx(label)}"/>',
             word(8.6, 12.2, 0.0058, c3, cink, cink)]
        for y in (16.2, 18.9, 21.6):
            o.append(f'<line x1="8.6" x2="29.5" y1="{y}" y2="{y}" stroke="{hx(cink)}" stroke-width="0.55" stroke-opacity="0.45" stroke-linecap="round"/>')
        for i, h in enumerate((4, 7, 10, 6, 12, 8, 5, 9)):
            o.append(f'<rect x="{36.5 + i * 2.2:.1f}" y="{22.0 - h}" width="1.2" height="{h}" rx="0.5" fill="{hx(bar)}"/>')
        o.append(f'<polygon points="13.02,37.9 17.17,29.6 42.03,29.6 46.18,37.9" fill="{hx(foot)}"/>')
        for x in (24.5, 34.7):
            o.append(f'<circle cx="{x}" cy="34.2" r="1.25" fill="{hx(mb.INK)}"/>')
        for (x, y) in ((3.1, 3.1), (56.1, 3.1), (3.1, 34.78), (56.1, 34.78)):
            o.append(f'<circle cx="{x}" cy="{y}" r="1.3" fill="{hx(mb.METAL)}" stroke="{hx(mb.INK)}" stroke-width="0.25"/>'
                     f'<line x1="{x - 0.8}" x2="{x + 0.8}" y1="{y}" y2="{y}" stroke="{hx(mb.INK)}" stroke-width="0.3"/>')
        return ''.join(o)

    def face(x, y, w, h, body):
        return f'<svg x="{x}" y="{y}" width="{w}" height="{h}" viewBox="0 0 59.2 37.888">{body}</svg>'

    cream_front = f'<rect width="59.2" height="37.888" fill="{hx(mb.CREAM)}"/>' + front_word
    green_front = f'<rect width="59.2" height="37.888" fill="{hx(mb.GREEN)}"/>' + stripe
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256"><defs>{defs}</defs>'
            + face(0, 0, 160, 102.4, cream_front) + face(160, 0, 96, 61.4, green_front)
            + face(0, 104, 160, 102.4, back(mb.GREEN, mb.CREAM, mb.DEEP, mb.INK, mb.DEEP, mb.DEEP))
            + face(160, 104, 96, 61.4, back(mb.CREAM, mb.GREEN, mb.CREAM, mb.INK, mb.INK, mb.FOOT))
            + '</svg>')


mb.faces_svg = faces_svg


def build():
    """-> (positions, normals, uvs, indices, atlas image RGBA) with one vertex per triangle corner."""
    atlas = Atlas()
    mesh = mb.Mesh(atlas)
    pile = mb.mmul(mb.rx(-0.32), mb.ry(0.22))
    under_M = mb.mmul(pile, mb.rz(math.radians(-9)))
    top_M = mb.mmul(pile, mb.rz(math.radians(7)))
    under_off, top_off = mb.mapply(pile, (0.95, 0.32, 0.0)), mb.mapply(pile, (-0.55, -0.32, mb.T + 0.24))
    probe = mb.Mesh(Atlas())
    for (M, off) in ((under_M, under_off), (top_M, top_off)):
        mb.build_cassette(probe, 'cream', M, off, 1.0, mb.CREAM, mb.GREEN, mb.CREAM)
    pts = [(v[0], v[1], v[2]) for v in probe.v]
    ext = [(min(p[k] for p in pts), max(p[k] for p in pts)) for k in range(3)]
    cx_, cz_ = (ext[0][0] + ext[0][1]) / 2, (ext[2][0] + ext[2][1]) / 2
    radius = max(math.hypot(p[0] - cx_, p[2] - cz_) for p in pts)
    K = min(mb.RADIUS / radius, 11.2 / (ext[1][1] - ext[1][0]))
    off0 = (-cx_, -(ext[1][0] + ext[1][1]) / 2 + (-1.0 / K), -cz_)
    for (M, off, face, shell, rl, rr_, back) in ((under_M, under_off, 'green', mb.GREEN, mb.CREAM, mb.GREEN, 'greenback'), (top_M, top_off, 'cream', mb.CREAM, mb.GREEN, mb.CREAM, 'creamback')):
        mb.build_cassette(mesh, face, M, mb.add(off, off0), K, shell, rl, rr_, back=back)
    print('scale', round(K, 3), 'triangles', mesh.tris, 'swatches', len(atlas.cells))

    with tempfile.TemporaryDirectory() as tmp:
        img = np.array(mb.render_faces(tmp))
    for c, (x, y) in atlas.cells.items():
        img[y:y + 8, x:x + 8] = (c[0] * 17, c[1] * 17, c[2] * 17, 255)

    pos, nor, uv, idx = [], [], [], []
    for t in range(0, len(mesh.v), 3):
        a, b, c = mesh.v[t], mesh.v[t + 1], mesh.v[t + 2]
        n = mb.norm(mb.cross(mb.sub(b[:3], a[:3]), mb.sub(c[:3], a[:3])))
        for v in (a, b, c):
            idx.append(len(pos))
            pos.append(v[:3]); nor.append(n)
            uv.append((v[3], 1 - v[4]))   # glTF's v runs down the picture, the mesh's runs up
    if len(pos) > 65535: raise SystemExit('too many vertices for 16-bit indices')
    return pos, nor, uv, idx, Image.fromarray(img, 'RGBA')


def write_glb(path, pos, nor, uv, idx, image):
    import io
    png = io.BytesIO(); image.save(png, 'PNG'); png = png.getvalue()
    P = np.array(pos, '<f4'); N = np.array(nor, '<f4'); U = np.array(uv, '<f4'); I = np.array(idx, '<u2')
    blobs = []
    def add(b):
        off = sum(len(x) for x in blobs); blobs.append(b + bytes(-len(b) % 4)); return off, len(b)
    po, pl = add(P.tobytes()); no, nl = add(N.tobytes()); uo, ul = add(U.tobytes()); io_, il = add(I.tobytes()); go, gl = add(png)
    doc = {
        'asset': {'version': '2.0', 'generator': 'make_banner3d_gltf.py'},
        'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'name': 'cassettes', 'mesh': 0}],
        'meshes': [{'name': 'cassettes', 'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}],
        'materials': [{'name': 'atlas', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0}}],
        'textures': [{'sampler': 0, 'source': 0}],
        'samplers': [{'magFilter': 9729, 'minFilter': 9729, 'wrapS': 33071, 'wrapT': 33071}],
        'images': [{'name': 'atlas', 'bufferView': 4, 'mimeType': 'image/png'}],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': len(pos), 'type': 'VEC3', 'min': P.min(0).tolist(), 'max': P.max(0).tolist()},
            {'bufferView': 1, 'componentType': 5126, 'count': len(pos), 'type': 'VEC3'},
            {'bufferView': 2, 'componentType': 5126, 'count': len(pos), 'type': 'VEC2'},
            {'bufferView': 3, 'componentType': 5123, 'count': len(idx), 'type': 'SCALAR'}],
        'bufferViews': [{'buffer': 0, 'byteOffset': o, 'byteLength': l, **({'target': tg} if tg else {})}
                        for (o, l, tg) in ((po, pl, 34962), (no, nl, 34962), (uo, ul, 34962), (io_, il, 34963), (go, gl, 0))],
        'buffers': [{'byteLength': sum(len(x) for x in blobs)}]}
    js = json.dumps(doc, separators=(',', ':')).encode(); js += b' ' * (-len(js) % 4)
    bin_ = b''.join(blobs)
    total = 12 + 8 + len(js) + 8 + len(bin_)
    open(path, 'wb').write(struct.pack('<4sII', b'glTF', 2, total) + struct.pack('<I4s', len(js), b'JSON') + js + struct.pack('<I4s', len(bin_), b'BIN\0') + bin_)
    print('wrote', path, total, 'bytes')


def main():
    pos, nor, uv, idx, image = build()
    write_glb(GLB, pos, nor, uv, idx, image)
    image.save(os.path.join(HERE, 'banner3d_atlas.png'))
    subprocess.run([sys.executable, os.path.join(PYCGFX, 'main.py'), GLB, CGFX], check=True, cwd=PYCGFX)
    print('wrote', CGFX, os.path.getsize(CGFX), 'bytes (HOME takes at most 512 KiB)')


if __name__ == '__main__':
    main()
