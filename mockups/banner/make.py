# Banner ideas for the HOME Menu, round 2 (Alex: fix 1 (cut off, its shadow not lining up), keep 2 and
# take it further, more takes on 3). Round 1 is make.round1.py / round1.html.
# Each is a 256x128 svg with a transparent background (bannertool keeps alpha, as RGBA4444).
#   python -I mockups/banner/make.py  -> ideas/*.svg + index.html
import math, os, re
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = open(os.path.join(HERE, '..', '..', 'player', 'cia', 'banner.svg'), encoding='utf-8').read()
DEFS = re.search(r'<defs>.*?</defs>', SRC).group()
GREEN, DEEP, CREAM, FOOT, INK, WHITE = '#1ed760', '#0a6e39', '#f2e6cf', '#d9caa8', '#121212', '#ffffff'
TAPE, TAPE_HI = '#4a2e1a', '#8a6440'
PENCIL, PENCIL_D, WOOD, LEAD = '#f2b632', '#b9821a', '#e8c99a', '#2a2a2a'
WORD_W = 4297 / 700   # the wordmark's width, in cap heights
D = 0.62              # depth: every shadow goes the same way on screen, down and right, this far per layer


def word(x, base, size, ink=WHITE, three=GREEN, depth=DEEP, layers=None):
    """3DStereo: left edge x, baseline base, cap height size px, extruded down-right like everything else."""
    k = size / 700
    n = layers if layers is not None else max(2, round(size * .07 / D))
    out = []
    for i in list(range(n, 0, -1)) + [0]:
        c3, ci = (depth, depth) if i else (three, ink)
        X, B = x + i * D, base + i * D
        if i and depth is None:
            continue
        out.append(f'<g transform="translate({X:.2f} {B:.2f}) scale({k:.5f} {-k:.5f})" fill="{c3}"><use href="#b33"/></g>')
        out.append(f'<g transform="translate({X + 600 * k:.2f} {B:.2f}) scale({k:.5f} {-k:.5f})" fill="{ci}"><use href="#b44"/><use href="#b53" x="697"/></g>')
        out.append(f'<g transform="translate({X + 1921 * k:.2f} {B:.2f}) scale({k:.5f} {-k:.5f})" fill="{ci}"><use href="#r74"/>'
                   f'<use href="#r65" x="351"/><use href="#r72" x="900"/><use href="#r65" x="1267"/><use href="#r6f" x="1816"/></g>')
    return ''.join(out)


def rot_pt(px, py, cx, cy, deg):
    a = math.radians(deg)
    dx, dy = px - cx, py - cy
    return cx + dx * math.cos(a) - dy * math.sin(a), cy + dx * math.sin(a) + dy * math.cos(a)


class Tape:
    """The icon's cassette, w wide, centred on cx, cy, turned rot degrees; its depth is cast in screen
    space (down-right, like the wordmark's), so every shadow in a banner lines up."""
    def __init__(s, cx, cy, w, rot=-8):
        s.cx, s.cy, s.w, s.rot, s.k = cx, cy, w, rot, w / 59.2
        s.h = 37.888 * s.k
        s.x0, s.y0 = cx - w / 2, cy - s.h / 2

    def at(s, lx, ly):   # a point in the cassette's own units (0..59.2, 0..37.9) -> the screen
        return rot_pt(s.x0 + lx * s.k, s.y0 + ly * s.k, s.cx, s.cy, s.rot)

    def reel(s, right):
        return s.at(42.103 if right else 17.097, 17.807)

    def draw(s, shell=CREAM, stripe=GREEN, reel_l=GREEN, reel_r=CREAM, depth=DEEP, label=None, layers=None, hole=False):
        k, x0, y0, w, h = s.k, s.x0, s.y0, s.w, s.h
        n = layers if layers is not None else max(2, round(w * .055 / D))
        R = f'rotate({s.rot} {s.cx:.2f} {s.cy:.2f})'
        body = f'<rect x="{x0:.2f}" y="{y0:.2f}" width="{w:.2f}" height="{h:.2f}" rx="{4.736 * k:.2f}"'
        g = [f'<g transform="translate({i * D:.2f} {i * D:.2f}) {R}">{body} fill="{depth}"/></g>' for i in range(n, 0, -1)]
        g.append(f'<g transform="{R}">{body} fill="{shell}"/>')
        if stripe:
            g.append(f'<rect x="{x0 + 5.92 * k:.2f}" y="{y0 + 4.925 * k:.2f}" width="{47.36 * k:.2f}" height="{3.789 * k:.2f}" rx="{1.894 * k:.2f}" fill="{stripe}"/>')
        if label:
            g.append(label(x0, y0, k))
        g.append(f'<rect x="{x0 + 10.656 * k:.2f}" y="{y0 + 11.366 * k:.2f}" width="{37.888 * k:.2f}" height="{12.882 * k:.2f}" rx="{6.441 * k:.2f}" fill="{INK}"/>')
        for rx_, col in ((17.097, reel_l), (42.103, reel_r)):
            g.append(f'<circle cx="{x0 + rx_ * k:.2f}" cy="{y0 + 17.807 * k:.2f}" r="{4.637 * k:.2f}" fill="{col}"/>'
                     f'<circle cx="{x0 + rx_ * k:.2f}" cy="{y0 + 17.807 * k:.2f}" r="{1.948 * k:.2f}" fill="{INK}"/>')
        g.append(f'<path d="M{x0 + 13.024 * k:.2f} {y0 + h:.2f}L{x0 + 17.168 * k:.2f} {y0 + 29.553 * k:.2f}'
                 f'L{x0 + 42.032 * k:.2f} {y0 + 29.553 * k:.2f}L{x0 + 46.176 * k:.2f} {y0 + h:.2f}Z" fill="{FOOT}"/>')
        g.append('</g>')
        return ''.join(g)


def tape_path(d, wd=3.2):
    """Loose cassette tape: dark brown, a thin warm sheen along it."""
    return (f'<path d="{d}" fill="none" stroke="{TAPE}" stroke-width="{wd}" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<path d="{d}" fill="none" stroke="{TAPE_HI}" stroke-width="{wd * .3:.2f}" stroke-linecap="round" transform="translate(-.35 -.55)" opacity=".8"/>')


def label_word(ink=INK, three=DEEP, frac=.82):
    def lab(x0, y0, k):   # the wordmark across the top of the shell, where the stripe would be
        size = 59.2 * k * frac / WORD_W
        return word(x0 + (59.2 * k - WORD_W * size) / 2, y0 + 4.2 * k + size, size, ink=ink, three=three, depth=None)
    return lab


def svg(body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 128" width="256" height="128">{DEFS}{body}</svg>'


IDEAS = []


def idea(key, name, why):
    def deco(f):
        IDEAS.append((key, name, why, svg(f())))
        return f
    return deco


# ---- 1, fixed ----------------------------------------------------------------------------------

@idea('1', 'The loose tape', 'Fixed: the whole cassette fits now, and its depth falls down-right like every other shadow.')
def _():
    return Tape(124, 60, 150, rot=-6).draw(stripe=None, label=label_word(frac=.62))


# ---- 2, the stereo pair --------------------------------------------------------------------------

@idea('2', 'A stereo pair', 'As it was, shadows lined up: green left, cream right, fanned like a hand of cards.')
def _():
    a = Tape(84, 50, 112, rot=-12).draw(shell=GREEN, stripe=CREAM, reel_l=CREAM, reel_r=GREEN)
    b = Tape(166, 52, 112, rot=7).draw()
    size = 15
    return a + b + word(128 - WORD_W * size / 2, 118, size)


@idea('2b', 'The pile', 'The pair dropped on a desk: green underneath, cream on top wearing the wordmark as its label.')
def _():
    a = Tape(146, 56, 128, rot=9).draw(shell=GREEN, stripe=CREAM, reel_l=CREAM, reel_r=GREEN)
    b = Tape(112, 66, 140, rot=-7).draw(stripe=None, label=label_word(frac=.6))
    return a + b


@idea('2c', 'Left, right, one tape', 'The pair apart, joined by one brown tape running from the left one\'s reel to the right one\'s, under the wordmark.')
def _():
    L, R = Tape(38, 54, 66, rot=-10), Tape(218, 54, 66, rot=10)
    (ax, ay), (bx, by) = L.at(29.6, 37.9), R.at(29.6, 37.9)
    t = tape_path(f'M{ax:.1f} {ay:.1f} C {ax + 14:.1f} {ay + 50:.1f}, {bx - 14:.1f} {by + 50:.1f}, {bx:.1f} {by:.1f}')
    size = 13
    return t + L.draw(shell=GREEN, stripe=CREAM, reel_l=CREAM, reel_r=GREEN) + R.draw() + word(128 - WORD_W * size / 2, 64, size)


# ---- 3, the tape pulled out -------------------------------------------------------------------------

@idea('3a', 'Signature', 'The tape leaves the cassette from the bottom, where it really would, loops once and underlines the wordmark in one stroke.')
def _():
    T = Tape(50, 46, 86, rot=-10)
    sx, sy = T.at(29.6, 37.9)
    d = (f'M{sx:.1f} {sy:.1f} C {sx + 4:.1f} {sy + 22:.1f}, 64 106, 84 104 '
         f'C 104 102, 110 84, 98 82 C 86 80, 86 100, 108 102 C 150 106, 206 100, 244 90')
    size = 18
    return tape_path(d, 3.4) + T.draw() + word(104, 70, size)


@idea('3b', 'Lasso', 'Small cassette in the corner, and its tape thrown in a loose loop all the way round the wordmark.')
def _():
    T = Tape(38, 92, 64, rot=-14)
    sx, sy = T.at(29.6, 37.9)
    d = (f'M{sx:.1f} {sy:.1f} C {sx + 30:.1f} {sy + 14:.1f}, 236 112, 244 70 '
         f'C 250 30, 160 16, 110 22 C 50 30, 40 56, 72 80 C 96 98, 150 100, 190 92')
    size = 17
    return tape_path(d, 3) + T.draw() + word(84, 70, size)


@idea('3c', 'The pencil fix', 'Everyone who had tapes knows it: a pencil through the reel to wind the slack back in, the loose loop hanging under the wordmark.')
def _():
    T = Tape(74, 60, 112, rot=-6)
    rx, ry = T.reel(False)
    a = math.radians(-128)   # the pencil, up and to the left out of the left reel
    ux, uy = math.cos(a), math.sin(a)
    L, Wd = 70, 7
    tipx, tipy = rx - ux * 10, ry - uy * 10            # the lead, just past the reel's hole
    endx, endy = rx + ux * (L - 10), ry + uy * (L - 10)
    nx, ny = -uy * Wd / 2, ux * Wd / 2
    sh = f'translate({D * 3:.2f} {D * 3:.2f})'
    def pencil(fill_body, fill_wood, fill_lead, extra=''):
        bx, by = tipx + ux * 12, tipy + uy * 12   # where the sharpened wood starts
        return (f'<g {extra}><path d="M{bx + nx:.1f} {by + ny:.1f} L{endx + nx:.1f} {endy + ny:.1f} L{endx - nx:.1f} {endy - ny:.1f} L{bx - nx:.1f} {by - ny:.1f}Z" fill="{fill_body}"/>'
                f'<path d="M{bx + nx:.1f} {by + ny:.1f} L{tipx:.1f} {tipy:.1f} L{bx - nx:.1f} {by - ny:.1f}Z" fill="{fill_wood}"/>'
                f'<circle cx="{tipx + ux * 2:.1f}" cy="{tipy + uy * 2:.1f}" r="1.6" fill="{fill_lead}"/>'
                f'<path d="M{endx + nx:.1f} {endy + ny:.1f} L{endx + ux * 7 + nx:.1f} {endy + uy * 7 + ny:.1f} L{endx + ux * 7 - nx:.1f} {endy + uy * 7 - ny:.1f} L{endx - nx:.1f} {endy - ny:.1f}Z" fill="#e88a9a"/></g>')
    sx, sy = T.at(29.6, 37.9)
    d = f'M{sx - 6:.1f} {sy:.1f} C {sx - 10:.1f} {sy + 30:.1f}, {sx + 40:.1f} {sy + 30:.1f}, {sx + 34:.1f} {sy - 2:.1f}'
    size = 14
    return (tape_path(d, 3) + T.draw() + pencil(DEEP, DEEP, DEEP, f'transform="{sh}"') + pencil(PENCIL, WOOD, LEAD)
            + word(150, 66, size))


@idea('3d', 'Open reels', 'No shell at all: a green reel and a cream reel, the tape strung between them, the wordmark riding on it.')
def _():
    g = []
    size = 15
    for cx, rim, pack in ((38, GREEN, TAPE), (218, CREAM, TAPE)):
        for i in range(4, 0, -1):
            g.append(f'<circle cx="{cx + i * D:.2f}" cy="{62 + i * D:.2f}" r="34" fill="{DEEP}"/>')
        g.append(f'<circle cx="{cx}" cy="62" r="34" fill="{rim}"/><circle cx="{cx}" cy="62" r="24" fill="{pack}"/>'
                 f'<circle cx="{cx}" cy="62" r="11" fill="{rim}"/><circle cx="{cx}" cy="62" r="4" fill="{INK}"/>')
        for a in (90, 210, 330):   # the hub's three windows
            hx, hy = cx + 17 * math.cos(math.radians(a)), 62 + 17 * math.sin(math.radians(a))
            g.append(f'<circle cx="{hx:.1f}" cy="{hy:.1f}" r="4.4" fill="{rim}"/>')
    t = tape_path('M38 86 C 80 96, 176 96, 218 86', 3.2)
    return t + ''.join(g) + word(128 - WORD_W * size / 2, 76, size)


if __name__ == '__main__':
    os.makedirs(os.path.join(HERE, 'ideas'), exist_ok=True)
    cards = []
    for key, name, why, s in IDEAS:
        open(os.path.join(HERE, 'ideas', f'{key}.svg'), 'w', encoding='utf-8').write(s)
        cards.append(f'<figure><div class="top">{s}</div><figcaption><b>{key}</b> {name}<span>{why}</span></figcaption></figure>')
    css = '''body{margin:0;padding:28px;background:#1b1b1b;font:13px/1.4 "IBM Plex Sans",system-ui,sans-serif;color:#ddd}
main{display:grid;grid-template-columns:repeat(4,auto);gap:26px 22px;justify-content:center}
figure{margin:0}
.top{width:400px;height:240px;position:relative;border-radius:6px;overflow:hidden;background:linear-gradient(#5d6066,#3e4045);box-shadow:0 0 0 6px #0d0d0d}
.top::before{content:"";position:absolute;inset:0;background:repeating-linear-gradient(90deg,#ffffff07 0 2px,transparent 2px 9px)}
.top svg{position:absolute;left:72px;top:40px;width:256px;height:128px;outline:1px dashed #ffffff22}
figcaption{width:400px;margin-top:10px} figcaption b{color:#1ed760;margin-right:6px}
figcaption span{display:block;color:#999;font-size:12px}'''
    page = (f'<!doctype html><meta charset="utf-8"><title>3DStereo banners, round 2</title><style>{css}</style>'
            f'<main>{"".join(cards)}</main>')
    open(os.path.join(HERE, 'index.html'), 'w', encoding='utf-8').write(page)
    print(len(IDEAS), 'ideas')
