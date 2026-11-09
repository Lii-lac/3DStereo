"""A software look at player/cia/banner3d.glb through HOME's banner camera, turned about the vertical axis the way HOME
turns it: yaw 0, 40, 140, 180 degrees, textured, one light. Not what the 3DS draws (its lighting is its own), but it
shows which way every picture faces.

    uv run --with numpy --with pillow python -I tools/cia/preview_glb.py   ->  tools/cia/banner3d_preview.png
"""
import io, json, math, os, struct
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GLB = os.path.join(HERE, '..', '..', 'player', 'cia', 'banner3d.glb')
OUT = os.path.join(HERE, 'banner3d_preview.png')


def load(path):
    b = open(path, 'rb').read()
    jl = struct.unpack_from('<I', b, 12)[0]
    doc = json.loads(b[20:20 + jl]); bin_ = b[20 + jl + 8:]
    bv = doc['bufferViews']
    def acc(i):
        a = doc['accessors'][i]; v = bv[a['bufferView']]
        dt = {5126: '<f4', 5123: '<u2'}[a['componentType']]; n = {'VEC3': 3, 'VEC2': 2, 'SCALAR': 1}[a['type']]
        return np.frombuffer(bin_[v['byteOffset']:v['byteOffset'] + v['byteLength']], dt)[:a['count'] * n].reshape(-1, n)
    v = bv[doc['images'][0]['bufferView']]
    img = np.array(Image.open(io.BytesIO(bin_[v['byteOffset']:v['byteOffset'] + v['byteLength']])).convert('RGBA'))
    return acc(0), acc(1), acc(2), acc(3)[:, 0].astype(int), img


def render(P, N, U, I, tex, yaw, W=600, H=360):
    th = math.radians(yaw); c, s = math.cos(th), math.sin(th)
    R = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    Pw = P @ R.T; Nw = N @ R.T
    cam = np.array([0, 1, 44.786]); fov = math.radians(30)
    f = (H / 2) / math.tan(fov / 2)
    img = np.zeros((H, W, 3), np.float32); img[:] = (0x5d, 0x60, 0x66)
    zb = np.full((H, W), 1e9, np.float32)
    light = np.array([-0.4, 0.6, 0.7]); light /= np.linalg.norm(light)
    th_, tw_ = tex.shape[:2]
    for t in range(0, len(I), 3):
        ids = I[t:t + 3]
        p = Pw[ids] - cam
        if p[:, 2].max() >= 0: continue
        sx = W / 2 + f * p[:, 0] / -p[:, 2]; sy = H / 2 - f * p[:, 1] / -p[:, 2]
        e = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0])
        if e >= 0 or e == 0: continue   # back-facing (screen y runs down)
        x0, x1 = int(max(0, sx.min())), int(min(W - 1, sx.max())); y0, y1 = int(max(0, sy.min())), int(min(H - 1, sy.max()))
        if x1 < x0 or y1 < y0: continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((sx[1] - xs) * (sy[2] - ys) - (sy[1] - ys) * (sx[2] - xs)) / e
        w1 = ((sx[2] - xs) * (sy[0] - ys) - (sy[2] - ys) * (sx[0] - xs)) / e
        w2 = 1 - w0 - w1
        m = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
        if not m.any(): continue
        z = w0 * -p[0, 2] + w1 * -p[1, 2] + w2 * -p[2, 2]
        u = w0 * U[ids[0], 0] + w1 * U[ids[1], 0] + w2 * U[ids[2], 0]
        v = w0 * U[ids[0], 1] + w1 * U[ids[1], 1] + w2 * U[ids[2], 1]
        tx = np.clip((u * tw_).astype(int), 0, tw_ - 1); ty = np.clip((v * th_).astype(int), 0, th_ - 1)
        col = tex[ty, tx, :3].astype(np.float32)
        k = 0.55 + 0.45 * max(0.0, float(Nw[ids[0]] @ light))
        sl = (slice(y0, y1 + 1), slice(x0, x1 + 1))
        ok = m & (z < zb[sl]); zb[sl] = np.where(ok, z, zb[sl])
        for ch in range(3): img[sl + (ch,)] = np.where(ok, col[..., ch] * k, img[sl + (ch,)])
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def main():
    P, N, U, I, tex = load(GLB)
    sheet = Image.new('RGB', (1200, 720))
    for n, yaw in enumerate((0, 40, 140, 180)):
        sheet.paste(render(P, N, U, I, tex, yaw), ((n % 2) * 600, (n // 2) * 360))
    sheet.save(OUT)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
