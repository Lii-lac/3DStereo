"""Our launch logo (makerom -logo): devkitPro's Homebrew splash with its pictures swapped for ours, so
its layout and animation, which the HOME Menu is known to accept, stay as they are. A CIA with no logo
at all dies at launch ("the SD card was removed", Alex, build 63).

    uv run --with pillow python tools/cia/make_logo.py <unpacked dir> player/cia/logo.bin
(<unpacked dir> from: python -I tools/cia/logo.py unpack <a CIA built with Logo: Homebrew> <dir>)

    logo.bclim     "homebrew" (white on black, 225x40 of 256x64) -> player/cia/logo/logo.png
    bubbles.bclim  the drifting squares -> black: none
    waves.bclim    the bottom screen's blue waves -> the same waves in our greens
"""
import os, struct, subprocess, sys
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import etc1, logo

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')


def bclim_size(b):
    w, h = struct.unpack_from('<HH', b[-0x28:], 0x1C)
    p2 = lambda v: next(n for n in (8, 16, 32, 64, 128, 256, 512, 1024) if n >= v)
    return w, h, p2(w), p2(h)


def put(path, rows):
    b = open(path, 'rb').read()
    w, h, W, H = bclim_size(b)
    data = etc1.encode(rows, W, H)
    assert len(data) == len(b) - 0x28
    open(path, 'wb').write(data + b[-0x28:])


def rows_of(im):
    im = im.convert('RGB')
    return [[im.getpixel((x, y)) for x in range(im.width)] for y in range(im.height)]


if __name__ == '__main__':
    d, out = sys.argv[1], sys.argv[2]
    files = os.path.join(d, 'files')
    # the name
    put(os.path.join(files, 'logo.bclim'), rows_of(Image.open(os.path.join(ROOT, 'player', 'cia', 'logo', 'logo.png'))))
    # no bubbles
    b = open(os.path.join(files, 'bubbles.bclim'), 'rb').read()
    _, _, W, H = bclim_size(b)
    put(os.path.join(files, 'bubbles.bclim'), [[(0, 0, 0)] * W for _ in range(H)])
    # the waves, recoloured: the blues' brightness picks a point between our deep green and our green,
    # the bright crest line goes cream
    b = open(os.path.join(files, 'waves.bclim'), 'rb').read()
    _, _, W, H = bclim_size(b)
    src = etc1.decode(b[:-0x28], W, H)
    deep, green, cream = (0x0a, 0x6e, 0x39), (0x1e, 0xd7, 0x60), (0xf2, 0xe6, 0xcf)
    def mix(a, c, t): return tuple(round(a[i] + (c[i] - a[i]) * t) for i in range(3))
    rows = []
    for row in src:
        r2 = []
        for (r, g, bb) in row:
            if g > 195:   # the crest's bright line (g ~220; the two blues sit at ~130 and ~175)
                r2.append(mix(green, cream, min(1, (g - 195) / 25)))
            else:
                t = max(0, min(1, (g - 128) / 48))   # the darker blue (g ~128) -> deep, the lighter (g ~176) -> green
                r2.append(mix(deep, green, t))
        rows.append(r2)
    put(os.path.join(files, 'waves.bclim'), rows)
    Image.new('RGB', (W, H)).save(os.path.join(d, 'waves.preview.png'))
    im = Image.new('RGB', (W, H)); im.putdata([p for r in etc1.decode(open(os.path.join(files, 'waves.bclim'), 'rb').read()[:-0x28], W, H) for p in r]); im.save(os.path.join(d, 'waves.preview.png'))
    im = Image.new('RGB', (256, 64)); im.putdata([p for r in etc1.decode(open(os.path.join(files, 'logo.bclim'), 'rb').read()[:-0x28], 256, 64) for p in r]); im.save(os.path.join(d, 'logo.preview.png'))
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'logo.py'), 'pack', d, out])
