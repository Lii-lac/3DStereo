# Banner ideas for the HOME Menu (Alex, build 63: "not a rectangle, more creative").
# Each is a 256x128 svg with a transparent background (bannertool keeps alpha, as RGBA4444), drawn
# from the icon's pieces.   python -I mockups/banner/make.py  -> ideas/*.svg + index.html
import os, re
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = open(os.path.join(HERE, '..', '..', 'player', 'cia', 'banner.svg'), encoding='utf-8').read()
DEFS = re.search(r'<defs>.*?</defs>', SRC).group()
GREEN, DEEP, CREAM, FOOT, INK, WHITE, TAPE = '#1ed760', '#0a6e39', '#f2e6cf', '#d9caa8', '#121212', '#ffffff', '#5a3a22'
WORD_W = 4297 / 700   # the wordmark's width, in cap heights


def word(x, base, size, ink=WHITE, three=GREEN, depth=None):
    """3DStereo: left edge x, baseline base, cap height size px."""
    k = size / 700
    out = []
    for dx, c3, ci in ([(size * .07, depth, depth)] if depth else []) + [(0, three, ink)]:
        X, B = x + dx, base + dx
        out.append(f'<g transform="translate({X:.2f} {B:.2f}) scale({k:.5f} {-k:.5f})" fill="{c3}"><use href="#b33"/></g>')
        out.append(f'<g transform="translate({X + 600 * k:.2f} {B:.2f}) scale({k:.5f} {-k:.5f})" fill="{ci}"><use href="#b44"/><use href="#b53" x="697"/></g>')
        out.append(f'<g transform="translate({X + 1921 * k:.2f} {B:.2f}) scale({k:.5f} {-k:.5f})" fill="{ci}"><use href="#r74"/>'
                   f'<use href="#r65" x="351"/><use href="#r72" x="900"/><use href="#r65" x="1267"/><use href="#r6f" x="1816"/></g>')
    return ''.join(out)


def cassette(cx, cy, w, rot=-8, shell=CREAM, stripe=GREEN, reel_l=GREEN, reel_r=CREAM, depth=DEEP, label=None, dd=6):
    """The icon's cassette, w wide, centred on cx, cy; label(x0, y0, k) draws on the shell."""
    k = w / 59.2
    h = 37.888 * k
    x0, y0 = cx - w / 2, cy - h / 2
    g = [f'<g transform="rotate({rot} {cx} {cy})">']
    for i in range(dd, 0, -1):
        g.append(f'<rect x="{x0 + i * .6 * k:.2f}" y="{y0 + i * .6 * k:.2f}" width="{w:.2f}" height="{h:.2f}" rx="{4.736 * k:.2f}" fill="{depth}"/>')
    g.append(f'<rect x="{x0:.2f}" y="{y0:.2f}" width="{w:.2f}" height="{h:.2f}" rx="{4.736 * k:.2f}" fill="{shell}"/>')
    if stripe:
        g.append(f'<rect x="{x0 + 5.92 * k:.2f}" y="{y0 + 4.925 * k:.2f}" width="{47.36 * k:.2f}" height="{3.789 * k:.2f}" rx="{1.894 * k:.2f}" fill="{stripe}"/>')
    if label:
        g.append(label(x0, y0, k))
    wx, wy = x0 + 10.656 * k, y0 + 11.366 * k
    g.append(f'<rect x="{wx:.2f}" y="{wy:.2f}" width="{37.888 * k:.2f}" height="{12.882 * k:.2f}" rx="{6.441 * k:.2f}" fill="{INK}"/>')
    for rx_, col in ((17.097, reel_l), (42.103, reel_r)):
        g.append(f'<circle cx="{x0 + rx_ * k:.2f}" cy="{y0 + 17.807 * k:.2f}" r="{4.637 * k:.2f}" fill="{col}"/>'
                 f'<circle cx="{x0 + rx_ * k:.2f}" cy="{y0 + 17.807 * k:.2f}" r="{1.948 * k:.2f}" fill="{INK}"/>')
    g.append(f'<path d="M{x0 + 13.024 * k:.2f} {y0 + h:.2f}L{x0 + 17.168 * k:.2f} {y0 + 29.553 * k:.2f}'
             f'L{x0 + 42.032 * k:.2f} {y0 + 29.553 * k:.2f}L{x0 + 46.176 * k:.2f} {y0 + h:.2f}Z" fill="{FOOT}"/>')
    g.append('</g>')
    return ''.join(g)


def svg(body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 128" width="256" height="128">{DEFS}{body}</svg>'


IDEAS = []


def idea(key, name, why):
    def deco(f):
        IDEAS.append((key, name, why, svg(f())))
        return f
    return deco


@idea('loose', 'The loose tape', 'No card, no tile: the cassette is the banner, tilted, the wordmark on its own label.')
def _():
    def lab(x0, y0, k):
        size = 2.9 * k
        return word(x0 + (59.2 * k - WORD_W * size) / 2, y0 + 9.6 * k, size, ink=INK, three=DEEP)
    return cassette(126, 60, 178, rot=-6, stripe=None, label=lab, dd=7)


@idea('pair', 'A stereo pair', 'Two tapes fanned like a hand of cards, the green one left and the cream one right, the wordmark under them.')
def _():
    a = cassette(84, 50, 118, rot=-12, shell=GREEN, stripe=CREAM, reel_l=CREAM, reel_r=GREEN)
    b = cassette(166, 52, 118, rot=7)
    size = 15
    return a + b + word(128 - WORD_W * size / 2, 118, size, depth=DEEP)


@idea('ribbon', 'The tape pulled out', 'The cassette on the left, its brown tape looping out and running under the wordmark like a signature.')
def _():
    c = cassette(52, 58, 92, rot=-10)
    p = 'M70 60 C 92 56, 104 30, 92 22 C 80 14, 70 34, 88 46 C 104 58, 120 92, 150 92 C 190 92, 222 90, 246 84'
    tape = (f'<path d="{p}" fill="none" stroke="{TAPE}" stroke-width="3.4" stroke-linecap="round"/>'
            f'<path d="{p}" fill="none" stroke="#7a5534" stroke-width="1.2" stroke-linecap="round" transform="translate(-.4 -.8)"/>')
    return tape + c + word(112, 80, 17, depth=DEEP)


@idea('phones', 'Headphones on', 'The wordmark wearing headphones: a cream band over it and a green cup on each side.')
def _():
    size = 19
    band = 'M34 78 C 34 18, 222 18, 222 78'
    g = [f'<path d="{band}" fill="none" stroke="{DEEP}" stroke-width="9" stroke-linecap="round" transform="translate(2.5 2.5)"/>',
         f'<path d="{band}" fill="none" stroke="{CREAM}" stroke-width="9" stroke-linecap="round"/>']
    for cx in (30, 226):
        g.append(f'<rect x="{cx - 12}" y="61" width="30" height="44" rx="13" fill="{DEEP}"/>'
                 f'<rect x="{cx - 15}" y="58" width="30" height="44" rx="13" fill="{GREEN}"/>'
                 f'<rect x="{cx - 8 + (6 if cx < 128 else -6)}" y="65" width="16" height="30" rx="8" fill="{INK}" opacity=".9"/>')
    g.append(word(128 - WORD_W * size / 2, 88, size, depth=DEEP))
    return ''.join(g)


@idea('boombox', 'The boombox', 'A chunky cream boombox with a handle: two green speakers and our cassette in the deck between them.')
def _():
    handle = 'M70 36 V20 Q70 12 78 12 H178 Q186 12 186 20 V36'
    g = [f'<path d="{handle}" fill="none" stroke="{DEEP}" stroke-width="8" transform="translate(3 3)"/>',
         f'<path d="{handle}" fill="none" stroke="{FOOT}" stroke-width="8"/>',
         f'<rect x="18" y="38" width="228" height="86" rx="14" fill="{DEEP}"/>',
         f'<rect x="14" y="34" width="228" height="86" rx="14" fill="{CREAM}"/>']
    for cx in (58, 198):
        g.append(f'<circle cx="{cx}" cy="78" r="33" fill="{INK}"/><circle cx="{cx}" cy="78" r="27" fill="{GREEN}"/>'
                 f'<circle cx="{cx}" cy="78" r="16" fill="{DEEP}"/><circle cx="{cx}" cy="78" r="6" fill="{INK}"/>')
    g.append(f'<rect x="96" y="56" width="64" height="44" rx="5" fill="{INK}"/>')
    g.append(cassette(128, 77, 54, rot=0, dd=0))
    size = 7
    g.append(word(128 - WORD_W * size / 2, 49, size, ink=INK, three=DEEP))
    return ''.join(g)


@idea('solid', 'Solid letters', 'The wordmark is the object: thick cream letters extruded deep into green, nothing around them.')
def _():
    size = 30
    x = 128 - WORD_W * size / 2 - 4
    layers = ''.join(word(x + i * .55, 82 + i * .55, size, ink=DEEP, three=DEEP) for i in range(12, 0, -1))
    return layers + word(x, 82, size, ink=CREAM, three=GREEN)


if __name__ == '__main__':
    os.makedirs(os.path.join(HERE, 'ideas'), exist_ok=True)
    cards = []
    for i, (key, name, why, s) in enumerate(IDEAS, 1):
        open(os.path.join(HERE, 'ideas', f'{i}-{key}.svg'), 'w', encoding='utf-8').write(s)
        cards.append(f'<figure><div class="top">{s}</div><figcaption><b>{i}</b> {name}<span>{why}</span></figcaption></figure>')
    css = '''body{margin:0;padding:28px;background:#1b1b1b;font:13px/1.4 "IBM Plex Sans",system-ui,sans-serif;color:#ddd}
main{display:grid;grid-template-columns:repeat(3,auto);gap:26px 22px;justify-content:center}
figure{margin:0}
.top{width:400px;height:240px;position:relative;border-radius:6px;overflow:hidden;background:linear-gradient(#5d6066,#3e4045);box-shadow:0 0 0 6px #0d0d0d}
.top::before{content:"";position:absolute;inset:0;background:repeating-linear-gradient(90deg,#ffffff07 0 2px,transparent 2px 9px)}
.top svg{position:absolute;left:72px;top:40px;width:256px;height:128px}
figcaption{width:400px;margin-top:10px} figcaption b{color:#1ed760;margin-right:6px}
figcaption span{display:block;color:#999;font-size:12px}'''
    page = f'<!doctype html><meta charset="utf-8"><title>3DStereo banners</title><style>{css}</style><main>{"".join(cards)}</main>'
    open(os.path.join(HERE, 'index.html'), 'w', encoding='utf-8').write(page)
    print(len(IDEAS), 'ideas')
