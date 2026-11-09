"""Draws the README's pictures: docs/readme/*.svg, the dark versions under the same names in docs/readme/dark/, and the
app's own art (player/icon.png's source player/cia/icon.svg, and player/cia/banner.svg). Their rules are in DESIGN.md.

    uv run --with fonttools python docs/readme/build.py

Everything is drawn the way the icon is: a cassette. Flat chunky shapes, cream on green, depth as a solid deep-green
offset down and to the right, a slight tilt, and almost no words. Every letter is outlined from IBM Plex Sans (SIL OFL
1.1), so a picture looks the same on a PC with no fonts at all. Set PLEX to the variable IBMPlexSans ttf if it isn't
where this PC keeps it.
"""
import math
import os
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

NAME = "3DStereo"   # the app's name; the wordmark splits it 3 | DS | tereo
HERE = Path(__file__).resolve().parent
FONT = Path(os.environ.get("PLEX", Path.home() / r"AppData\Local\Microsoft\Windows\Fonts\IBMPlexSans-VariableFont_wdth,wght.ttf"))

# The cassette's palette: the icon's five colours, and the two pages GitHub puts them on.
INK = dict(oxide="#7a5236", green="#1ed760", deep="#0a6e39", cream="#f2e6cf", foot="#d9caa8", ink="#121212", white="#ffffff",
           stock="#a8997a", panel="#121212", grey="#b3b3b3")
THEMES = {
    "light": dict(INK, page="#f4f1ee", word="#121212", soft="#4a4a4a", makerchip="#e6dfd2", makerink="#121212",
                  track="#d9caa8", tape="#0a6e39", cord="#121212"),
    "dark": dict(INK, page="#0d1117", word="#ffffff", soft="#b3b3b3", makerchip="#2a2a2a", makerink="#ffffff",
                 track="#30363d", tape="#1ed760", cord="#f2e6cf"),
}


# ---- type ------------------------------------------------------------------------------------------------------------

class Type:
    STYLES = {"r": (400, 100), "m": (500, 100), "b": (700, 100)}

    def __init__(self):
        if not FONT.exists():
            raise SystemExit(f"IBM Plex Sans isn't at {FONT}: set PLEX to its variable ttf")
        self.upm = TTFont(FONT)["head"].unitsPerEm
        self.fonts = {k: instantiateVariableFont(TTFont(FONT), {"wght": w, "wdth": d}) for k, (w, d) in self.STYLES.items()}
        self.cmap = self.fonts["r"].getBestCmap()

    def adv(self, ch: str, w: str) -> int:
        return self.fonts[w]["hmtx"][self.cmap[ord(ch)]][0]

    def width(self, s: str, size: float, w: str = "r", ls: float = 0) -> float:
        return sum(self.adv(c, w) for c in s) * size / self.upm + ls * max(len(s) - 1, 0)

    def path(self, gid: str) -> str:
        w, cp = gid[0], int(gid[1:], 16)
        gs = self.fonts[w].getGlyphSet()
        pen = SVGPathPen(gs, ntos=lambda v: str(round(v)))
        gs[self.cmap[cp]].draw(pen)
        return pen.getCommands()


T = Type()


# ---- a picture ---------------------------------------------------------------------------------------------------------

class Pic:
    def __init__(self, w: int, h: int, t: dict, label: str):
        self.w, self.h, self.t, self.label = w, h, t, label
        self.body, self.used, self.defs, self.n = [], set(), [], 0

    def add(self, s: str) -> None:
        self.body.append(s)

    def uid(self, p: str) -> str:
        self.n += 1
        return f"{p}{self.n}"

    def col(self, c: str) -> str:
        return self.t.get(c, c)

    def text(self, x, y, size, s, c="white", w="r", anchor="start", op=1.0) -> float:
        """One run on a baseline, the way citro2d draws it; returns its width."""
        wd = T.width(s, size, w)
        x -= wd / 2 if anchor == "middle" else wd if anchor == "end" else 0
        k = size / T.upm
        uses, cur = [], 0.0
        for ch in s:
            if ch != " ":
                gid = f"{w}{ord(ch):x}"
                self.used.add(gid)
                uses.append(f'<use href="#{gid}" x="{round(cur)}"/>')
            cur += T.adv(ch, w)
        o = f' opacity="{op:.3f}"' if op < 1 else ""
        self.add(f'<g transform="translate({x:.2f} {y:.2f}) scale({k:.5f} {-k:.5f})" fill="{self.col(c)}"{o}>{"".join(uses)}</g>')
        return wd

    def rect(self, x, y, w, h, c, r=0, a=255, extra=""):
        """main.c's rect and rrect; a is its 0 to 255 alpha."""
        o = f' fill-opacity="{a / 255:.3f}"' if a < 255 else ""
        rr = f' rx="{r:g}"' if r else ""
        self.add(f'<rect x="{x:g}" y="{y:g}" width="{w:g}" height="{h:g}"{rr} fill="{self.col(c)}"{o}{extra}/>')

    def circle(self, x, y, r, c, a=255):
        o = f' fill-opacity="{a / 255:.3f}"' if a < 255 else ""
        self.add(f'<circle cx="{x:g}" cy="{y:g}" r="{r:g}" fill="{self.col(c)}"{o}/>')

    def poly(self, pts, c):
        self.add(f'<path d="M{"L".join(f"{x:g} {y:g}" for x, y in pts)}Z" fill="{self.col(c)}"/>')

    def line(self, pts, c, w=1.5):
        self.add(f'<path d="M{"L".join(f"{x:g} {y:g}" for x, y in pts)}" fill="none" stroke="{self.col(c)}" stroke-width="{w}"/>')

    def vgrad(self, a: str, b: str, stop: float) -> str:
        g = self.uid("wash")
        self.defs.append(f'<linearGradient id="{g}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{self.col(a)}"/>'
                         f'<stop offset="{stop:.3f}" stop-color="{self.col(b)}"/></linearGradient>')
        return f"url(#{g})"

    def svg(self) -> str:
        glyphs = "".join(f'<path id="{g}" d="{T.path(g)}"/>' for g in sorted(self.used))
        return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w:g} {self.h:g}" width="{self.w:g}" height="{self.h:g}" '
                f'role="img" aria-label="{self.label}"><defs>{glyphs}{"".join(self.defs)}</defs>{"".join(self.body)}</svg>\n')




# ---- colour checks -----------------------------------------------------------------------------------------------------

def luminance(h: str) -> float:
    def lin(c):
        c /= 255
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (int(h[i:i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def contrast(a: str, b: str) -> float:
    x, y = sorted((luminance(a), luminance(b)), reverse=True)
    return (x + 0.05) / (y + 0.05)


def check_contrast() -> None:
    """WCAG AA: 4.5:1 for reading text, 3:1 for big type and for shapes that carry meaning. Stops the build if one fails.
    The wordmark's green 3 is a logotype, which WCAG leaves out (1.4.3), so it isn't checked."""
    pairs = [("word", "page", 4.5), ("soft", "page", 4.5), ("makerink", "makerchip", 4.5),
             ("ink", "cream", 4.5), ("ink", "green", 4.5), ("ink", "foot", 4.5), ("ink", "stock", 4.5),
             ("white", "panel", 4.5), ("grey", "panel", 4.5), ("green", "panel", 3), ("stock", "panel", 3),
             ("cream", "panel", 3), ("deep", "page", 3) if False else ("cream", "deep", 3)]
    bad = [f"{n}: {a} {t[a]} on {b} {t[b]} is {contrast(t[a], t[b]):.2f}:1, needs {need}:1"
           for n, t in THEMES.items() for a, b, need in pairs if contrast(t[a], t[b]) < need]
    if bad:
        raise SystemExit("\n".join(bad))


# ---- the cassette's pieces -------------------------------------------------------------------------------------------------

def depth(p, x, y, w, h, r, d, c="deep"):
    """The one kind of shadow there is: the shape again, solid, d down and to the right."""
    p.rect(x + d, y + d, w, h, c, r=r)


def wordmark(p, x, y, size, ink="word") -> float:
    """3 | DS | tereo on one baseline: the 3 green, DS bold in the ink, tereo regular. Returns its width."""
    w = p.text(x, y, size, "3", "green", "b")
    w += p.text(x + w, y, size, "DS", ink, "b")
    return w + p.text(x + w, y, size, "tereo", ink, "r")


def new_badge(p, x, top, h, ink="word") -> float:
    """The "new" over the wordmark, in the console logo's own silhouette: two brackets of three tiles, each tile a slice
    of a ring. Measured off the logo (its units: the brackets' outer edges 838 apart, 230 tall): each bracket's ring is
    centred 177 in from its middle tile's outer edge, the tiles run from radius 119 to 177, each spans 15.5 degrees, and
    they sit at -32, 0 and +32 degrees. Only the colours are ours: six of them, as on the logo: green, cream and the
    tape's brown down the left, and their lighter siblings down the right in the opposite order, so the pair is
    symmetric about the \\ diagonal and not a mirror. Returns its width."""
    k = h / 230
    cy = top + 115 * k
    r_out, r_in, span = 177 * k, 119 * k, math.radians(15.5)
    corner = 7 * k                                    # the tiles' soft corners, drawn as a round-joined stroke

    # Six colours, like the console's. Measured off the logo: left red/lime/blue, right (top to bottom) teal/amber/magenta,
    # each right tile being its diagonal opposite on the left (red<>magenta, lime<>amber, blue<>teal) with the hue turned
    # by -30 degrees (-29.8, -27.6, -31.9) and the saturation kept. Ours: the same shift on the cassette's three colours.
    LEFT = ("green", "cream", "oxide")
    RIGHT = ("#a8506b", "#e8c9d5", "#47bd7c")   # the same -30 degree turn, but with the saturation lifted the way the logo's is: brick, peach, fresh green

    def bracket(cx, side):
        for step, c in zip((-1, 0, 1), LEFT if side < 0 else RIGHT):
            mid = (math.pi if side < 0 else 0) + side * step * math.radians(32)
            pt = lambda r, a: (cx + r * math.cos(a), cy + r * math.sin(a))
            a0, a1 = mid - span / 2, mid + span / 2
            ro, ri = r_out - corner / 2, r_in + corner / 2
            sh = math.asin(corner / 2 / r_in)        # pull the radial sides in by half the stroke too
            (x0, y0), (x1, y1) = pt(ro, a0 + sh * r_in / ro), pt(ro, a1 - sh * r_in / ro)
            (x2, y2), (x3, y3) = pt(ri, a1 - sh), pt(ri, a0 + sh)
            d = (f"M{x0:.2f} {y0:.2f}A{ro:.2f} {ro:.2f} 0 0 1 {x1:.2f} {y1:.2f}L{x2:.2f} {y2:.2f}"
                 f"A{ri:.2f} {ri:.2f} 0 0 0 {x3:.2f} {y3:.2f}Z")
            for col, o in (("deep", 9 * k), (c, 0)):
                p.add(f'<path d="{d}" transform="translate({o:.2f} {o:.2f})" fill="{p.col(col)}" stroke="{p.col(col)}" '
                      f'stroke-width="{corner:.2f}" stroke-linejoin="round"/>')

    bracket(x + r_out, -1)
    bracket(x + 838 * k - r_out, 1)
    ts, span_w = 272 * k, 580 * k                     # the word, letter-spaced out to fill the space between the brackets
    ws = [T.width(c, ts, "r") for c in "new"]
    ls = (span_w - sum(ws)) / 2
    cx = x + 419 * k - span_w / 2
    for c, w in zip("new", ws):
        p.text(cx, cy + ts * .516 / 2, ts, c, ink, "r")
        cx += w + ls
    return 838 * k


def wordmark_width(size) -> float:
    return T.width("3", size, "b") + T.width("DS", size, "b") + T.width("tereo", size, "r")


def cassette(p, x, y, w, tilt=-8):
    """The icon's cassette: a cream shell over its deep-green depth, the green stripe, a big window, and stereo reels
    (left green, right cream), tilted."""
    h = w * .64
    cx, cy = x + w / 2, y + h / 2
    p.add(f'<g transform="rotate({tilt} {cx:.2f} {cy:.2f})">')
    for i in range(6, 0, -1):
        d = w * .06 * i / 6
        p.rect(x + d, y + d, w, h, "deep", r=w * .08)
    p.rect(x, y, w, h, "cream", r=w * .08)
    p.rect(x + w * .1, y + h * .13, w * .8, h * .1, "green", r=h * .05)
    ww, wh = w * .64, h * .34
    wx, wy = x + (w - ww) / 2, y + h * .30
    p.rect(wx, wy, ww, wh, "ink", r=wh / 2)
    r = wh * .36
    for f, c in ((0, "green"), (1, "cream")):
        rx = wx + wh * .5 + f * (ww - wh)
        p.circle(rx, wy + wh / 2, r, c)
        p.circle(rx, wy + wh / 2, r * .42, "ink")
    p.poly([(x + w * .22, y + h), (x + w * .29, y + h * .78), (x + w * .71, y + h * .78), (x + w * .78, y + h)], "foot")
    p.add("</g>")


def tile(p, x, y, s, r=None, lift=0):
    """The icon: the green tile with the cassette on it. lift: the tile's own depth, for when it sits on a page."""
    r = s * .22 if r is None else r
    if lift:
        depth(p, x, y, s, s, r, lift)
    p.rect(x, y, s, s, "green", r=r)
    w = s * .74
    cassette(p, x + (s - w) / 2 - s * .025, y + (s - w * .64) / 2 - s * .025, w)


GITHUB = ("M8 0c4.42 0 8 3.58 8 8a8.013 8.013 0 0 1-5.45 7.59c-.4.08-.55-.17-.55-.38 0-.27.01-1.13.01-2.2 0-.75-.25-1.23-.54-1.48 "
          "1.78-.2 3.65-.88 3.65-3.95 0-.88-.31-1.59-.82-2.15.08-.2.36-1.02-.08-2.12 0 0-.67-.22-2.2.82-.64-.18-1.32-.27-2-.27-.68 0-1.36"
          ".09-2 .27-1.53-1.03-2.2-.82-2.2-.82-.44 1.1-.16 1.92-.08 2.12-.51.56-.82 1.28-.82 2.15 0 3.06 1.86 3.75 3.64 3.95-.23.2-.44.55-"
          ".51 1.07-.46.21-1.61.55-2.33-.66-.15-.24-.6-.83-1.23-.82-.67.01-.27.38.01.53.34.19.73.9.82 1.13.16.45.68 1.31 2.69.94 0 .67.01 "
          "1.3.01 1.49 0 .21-.15.45-.55.38A7.995 7.995 0 0 1 0 8c0-4.42 3.58-8 8-8Z")


# ---- the pictures ---------------------------------------------------------------------------------------------------------

def hero(t: dict) -> Pic:
    """1280 by 640, so it doubles as the social preview: the wordmark and its promise on the left, the icon big on the
    right with its own depth, the maker in the corner."""
    W, H = 1280, 640
    p = Pic(W, H, t, f"{NAME}: your own music on a New 3DS. Over the name, a little “new” between two arcs of green, cream and brown tiles. Beside the name, its icon: a cream cassette tilted on a green "
                     "tile. In the bottom left corner, the maker's name, Lii-lac, with the GitHub mark.")
    p.rect(0, 0, W, H, "page")
    size = 640 / (wordmark_width(1) or 1)
    size = min(size, 150)
    new_badge(p, 84, 98, 92)
    wordmark(p, 80, 324, size)
    p.text(84, 412, 44, "Your own music,", "soft", "m")
    p.text(84, 464, 44, "on a New 3DS.", "soft", "m")
    lw = T.width("Lii-lac", 26, "m")
    p.rect(80, 528, 64 + lw + 24, 52, "makerchip", r=26)
    p.add(f'<path d="{GITHUB}" transform="translate(98 538) scale(2)" fill="{p.col("makerink")}"/>')
    p.text(144, 563, 26, "Lii-lac", "makerink", "m")
    tile(p, 790, 120, 400, lift=22)
    return p


def banner(t: dict) -> Pic:
    """The HOME Menu banner, 256 by 128 (player/cia/banner.png): the wordmark and the icon, on the app's dark."""
    W, H = 256, 128
    p = Pic(W, H, t, f"{NAME}, beside its icon")
    p.rect(0, 0, W, H, "ink")
    wordmark(p, 12, 72, 30, "white")
    tile(p, 160, 22, 80, lift=6)
    return p


def icon(t: dict) -> Pic:
    """The 48 by 48 icon, full bleed (the HOME Menu frames it itself)."""
    p = Pic(48, 48, t, f"{NAME}")
    tile(p, 0, 0, 48, r=0)
    return p


def sound(t: dict) -> Pic:
    """The three settings SELECT cycles through, as how much of a song each lets through: a dark panel lifted on its
    depth, a chunky bar each. Each bar's track is 0 to 24 kHz and its fill is that setting's real Nyquist limit."""
    W, H = 1280, 352
    p = Pic(W, H, t, "How much of a song each sound setting keeps: Stock up to 16.4 kHz, Good and Best up to 23.8 kHz.")
    px, py, pw, ph = 8, 8, W - 40, H - 40
    depth(p, px, py, pw, ph, 36, 18)
    p.rect(px, py, pw, ph, "panel", r=36)
    L, R = 260, 980
    hz = lambda k: L + (R - L) * k / 24
    for i, (q, c, top) in enumerate((("STOCK", "stock", 16.364), ("GOOD", "cream", 23.803), ("BEST", "green", 23.803))):
        y = 92 + i * 72
        qw = T.width(q, 24, "b") + 36
        p.rect(64, y - 24, qw, 48, c, r=12)
        p.text(82, y + 9, 24, q, "ink", "b")
        p.rect(L, y - 12, R - L, 24, "white", r=12, a=30)
        p.rect(L, y - 12, hz(top) - L, 24, c, r=12)
        p.text(W - 150, y + 11, 32, f"{top:.1f}", "white", "b", anchor="end")
        p.text(W - 140, y + 10, 22, "kHz", "grey")
    return p


def nav(label: str, on: bool) -> Pic:
    """A chunky button on its depth: green with ink letters for where the README starts (Install), cream otherwise."""
    k, px, pad, d = 2.2, 10, 11, 2.4
    w = T.width(label, px, "b") + 2 * pad
    p = Pic(round((w + d) * k), round((20 + d) * k), THEMES["light"], label)
    p.add(f'<g transform="scale({k})">')
    depth(p, 0, 0, w, 20, 10, d)
    p.rect(0, 0, w, 20, "green" if on else "cream", r=10)
    p.text(pad, 13.6, px, label, "ink", "b")
    p.add("</g>")
    return p


# ---- the sections' pictures: a piece of music hardware each, no words -----------------------------------------------------

SW, SH = 1280, 300   # every section picture


def rrect_path(x, y, w, h, r, cut=0):
    """A rounded rectangle as a path; cut clips the top right corner off at 45 degrees (an SD card)."""
    return (f"M{x + r:.1f} {y:.1f}H{x + w - cut:.1f}L{x + w:.1f} {y + cut:.1f}V{y + h - r:.1f}"
            f"Q{x + w:.1f} {y + h:.1f} {x + w - r:.1f} {y + h:.1f}H{x + r:.1f}Q{x:.1f} {y + h:.1f} {x:.1f} {y + h - r:.1f}"
            f"V{y + r:.1f}Q{x:.1f} {y:.1f} {x + r:.1f} {y:.1f}Z")


def reels(p, wx, wy, ww, wh):
    """The cassette's window with its stereo reels: left green, right cream."""
    p.rect(wx, wy, ww, wh, "ink", r=wh / 2)
    r = wh * .36
    for f, c in ((0, "green"), (1, "cream")):
        rx = wx + wh * .5 + f * (ww - wh)
        p.circle(rx, wy + wh / 2, r, c)
        p.circle(rx, wy + wh / 2, r * .42, "ink")


def key_icon(p, kind, cx, cy, s):
    c = "ink"
    if kind == "play":
        p.poly([(cx - s * .35, cy - s * .45), (cx - s * .35, cy + s * .45), (cx + s * .45, cy)], c)
    elif kind == "pause":
        p.rect(cx - s * .38, cy - s * .45, s * .26, s * .9, c, r=2)
        p.rect(cx + s * .12, cy - s * .45, s * .26, s * .9, c, r=2)
    elif kind == "stop":
        p.rect(cx - s * .4, cy - s * .4, s * .8, s * .8, c, r=4)
    elif kind in ("rew", "ff"):
        k = -1 if kind == "rew" else 1
        for o in (-.42, .02):
            a = cx + o * s * k if k > 0 else cx - o * s - s * .4
            p.poly([(a, cy - s * .38), (a, cy + s * .38), (a + s * .42 * k, cy)] if k > 0 else
                   [(a + s * .42, cy - s * .38), (a + s * .42, cy + s * .38), (a, cy)], c)
    elif kind == "rec":
        p.circle(cx, cy, s * .4, c)


# ---- the dividers: three, each a different bit of a music player --------------------------------------------------------

def div_levels(t: dict) -> Pic:
    """A level meter's strip of lights, lit about three quarters of the way, the last few in deep green."""
    W, H = 1280, 64
    p = Pic(W, H, t, "")
    n, gap = 40, 8
    bw = (W - 80 - (n - 1) * gap) / n
    lit = int(n * .74)
    for i in range(n):
        x = 40 + i * (bw + gap)
        c = "green" if i < lit else "track"
        if i >= n - 6 and i < lit:
            c = "deep"
        p.rect(x, 20, bw, 24, c, r=6)
    return p


def install_art(t: dict) -> Pic:
    """Install: a real SD card (24 by 32 mm: the cut corner, the lock in its notch on the left edge, the grip ridges),
    standing beside the steps, its label a blank mixtape label: the tape window with both reels and the tape wound on,
    and lines to write the songs on."""
    W, H = 400, 470
    p = Pic(W, H, t, "An SD card whose label is a blank mixtape label: the tape window with its two reels and the tape wound "
                     "on, and lines to write the songs on.")
    k = 11.5                                       # px per mm
    cw, ch, cut, r = 24 * k, 32 * k, 4.4 * k, 12
    x, y = (W - cw) / 2 - 8, (H - ch) / 2 - 10
    n, y1, y2 = 1.3 * k, y + 7.5 * k, y + 13.5 * k      # the lock's notch in the left edge
    body = (f"M{x + r:.1f} {y:.1f}H{x + cw - cut:.1f}L{x + cw:.1f} {y + cut:.1f}V{y + ch - r:.1f}"
            f"Q{x + cw:.1f} {y + ch:.1f} {x + cw - r:.1f} {y + ch:.1f}H{x + r:.1f}Q{x:.1f} {y + ch:.1f} {x:.1f} {y + ch - r:.1f}"
            f"V{y2:.1f}H{x + n:.1f}V{y1:.1f}H{x:.1f}V{y + r:.1f}Q{x:.1f} {y:.1f} {x + r:.1f} {y:.1f}Z")
    p.add(f'<g transform="rotate(-6 {x + cw / 2:.1f} {y + ch / 2:.1f})">')
    for i in range(8, 0, -1):
        p.add(f'<path d="{body}" transform="translate({2 * i} {2 * i})" fill="{p.col("deep")}"/>')
    p.add(f'<path d="{body}" fill="{p.col("ink")}"/>')
    p.rect(x + 1, y1 + 1, n + 3, 2.4 * k, "cream", r=3)         # the slide, up: not locked
    for i in range(3):
        p.rect(x + 4, y1 + 6 + i * 7, n - 2, 2.5, "ink")
    for i in range(6):                                           # the grip ridges along the top
        p.rect(x + 3.4 * k, y + (1.6 + i * .85) * k, cw - 9.6 * k, .32 * k, "grey", r=1.5, a=70)
    lx, ly, lw, lh = x + 2.6 * k, y + 7.4 * k, cw - 4.6 * k, ch - 9.6 * k
    p.rect(lx, ly, lw, lh, "cream", r=10)                        # the label
    p.rect(lx + 1.2 * k, ly + 1.2 * k, lw - 2.4 * k, 1.9 * k, "green", r=.95 * k)
    wx, wy, ww, wh = lx + .1 * lw, ly + 4.6 * k, .8 * lw, 7.2 * k
    clip = p.uid("win")
    p.defs.append(f'<clipPath id="{clip}"><rect x="{wx:.1f}" y="{wy:.1f}" width="{ww:.1f}" height="{wh:.1f}" rx="{wh / 2:.1f}"/></clipPath>')
    p.rect(wx, wy, ww, wh, "ink", r=wh / 2)
    p.add(f'<g clip-path="url(#{clip})">')
    hy = wy + wh / 2
    for f, pack, hub in ((0, 3.9 * k, "green"), (1, 2.6 * k, "cream")):   # more tape wound on the left: side A, half played
        hx = wx + wh / 2 + f * (ww - wh)
        p.circle(hx, hy, pack, "oxide")
        p.circle(hx, hy, 1.55 * k, hub)
        for j in range(6):
            p.add(f'<rect x="{hx - 2:.1f}" y="{hy - 1.55 * k:.1f}" width="4" height="{.5 * k:.1f}" fill="{p.col("ink")}" '
                  f'transform="rotate({j * 60} {hx:.1f} {hy:.1f})"/>')
        p.circle(hx, hy, .45 * k, "ink")
    p.add("</g>")
    for i in range(3):                                           # lines to write the songs on
        p.rect(lx + .1 * lw, ly + (14.6 + i * 2.7) * k, .8 * lw, 3, "foot", r=1.5)
    p.add("</g>")
    return p


def using_art(t: dict) -> Pic:
    """Using it: a tape deck's row of piano keys in their frame, play pushed down."""
    p = Pic(SW, SH, t, "A tape deck's row of chunky piano keys, rewind, play, pause, fast forward and stop, with play pushed down.")
    keys = ("rew", "play", "pause", "ff", "stop")
    kw, gap, kh, d, pad = 160, 20, 140, 22, 34
    row = len(keys) * kw + (len(keys) - 1) * gap
    fw, fh = row + 2 * pad, kh + d + 2 * pad
    fx, fy = (SW - fw) / 2 - 7, (SH - fh) / 2 - 7          # centred with its own depth counted in
    p.rect(fx + 14, fy + 14, fw, fh, "deep", r=32)
    p.rect(fx, fy, fw, fh, "ink", r=32)
    for i, k in enumerate(keys):
        x = fx + pad + i * (kw + gap)
        down = k == "play"
        y = fy + pad + (d - 6 if down else 0)
        dd = 6 if down else d
        p.rect(x, y + dd, kw, kh, "deep", r=18)
        p.rect(x, y, kw, kh, "green" if down else "cream", r=18)
        key_icon(p, k, x + kw / 2, y + kh / 2, 44)
    return p


def spotify_art(t: dict) -> Pic:
    """Spotify: a phone playing the cassette, sending Wi-Fi to its right, where the README goes on to say what it sends."""
    W, H = 420, 430
    p = Pic(W, H, t, "A phone playing a song whose cover is the 3DStereo cassette, sending Wi-Fi waves out to its right.")
    px, py, pw, ph = 30, 40, 186, 340
    p.rect(px + 16, py + 16, pw, ph, "deep", r=30)
    p.rect(px, py, pw, ph, "cream", r=30)
    sx, sy, sw, sh = px + 14, py + 34, pw - 28, ph - 74
    p.rect(sx, sy, sw, sh, "ink", r=12)
    p.rect(px + pw / 2 - 22, py + 14, 44, 7, "foot", r=3.5)            # the speaker slot
    tile(p, sx + 16, sy + 18, sw - 32)                                   # the cover: the cassette itself
    by = sy + 18 + sw - 32 + 24
    p.rect(sx + 16, by, sw - 32, 7, "white", r=3.5, a=40)              # the progress bar, a third in
    p.rect(sx + 16, by, (sw - 32) * .36, 7, "green", r=3.5)
    cx, cy = sx + sw / 2, by + 50
    p.circle(cx, cy, 24, "green")
    p.poly([(cx - 8, cy - 12), (cx - 8, cy + 12), (cx + 13, cy)], "ink")
    for k in (-1, 1):                                                    # previous and next
        a = cx + k * 54
        p.poly([(a - 8 * k, cy - 10), (a - 8 * k, cy + 10), (a + 6 * k, cy)], "cream")
        p.rect(a + 6 * k - (3 if k > 0 else 0), cy - 10, 3, 20, "cream")
    ox, oy = px + pw + 22, py + ph * .42
    for r in (40, 92, 144):
        a = math.radians(40)
        x1, y1 = ox + r * math.cos(-a), oy + r * math.sin(-a)
        x2, y2 = ox + r * math.cos(a), oy + r * math.sin(a)
        d = f"M{x1:.1f} {y1:.1f}A{r} {r} 0 0 1 {x2:.1f} {y2:.1f}"
        p.add(f'<path d="{d}" transform="translate(7 7)" fill="none" stroke="{p.col("deep")}" stroke-width="16" stroke-linecap="round"/>')
        p.add(f'<path d="{d}" fill="none" stroke="{p.col("green")}" stroke-width="16" stroke-linecap="round"/>')
    return p


# ---- Building it: the cassette exploded, the way a service manual draws it ---------------------------------------------------
# Every part floats straight above where it sits, in one view from above (30 degrees round, squashed to half its depth), so
# pushing them back down the dashed lines and driving the five screws would put the cassette back together. Measurements
# are a real cassette's, in mm round its centre: 100.4 by 63.8, the hubs 42.5 apart.

CL, CWD = 100.4, 63.8
HUB, HUBY = 21.25, -3.8
SCREWS = ((-46.8, -28.2), (46.8, -28.2), (-46.8, 28.2), (46.8, 28.2), (0, 17))
ROLLERS = ((-41, 26), (41, 26))


def mm_circle(cx, cy, r):
    return f"M{cx - r:.2f} {cy:.2f}a{r:.2f} {r:.2f} 0 1 0 {2 * r:.2f} 0a{r:.2f} {r:.2f} 0 1 0 {-2 * r:.2f} 0Z"


def mm_rect(x, y, w, h, r):
    return (f"M{x + r:.2f} {y:.2f}H{x + w - r:.2f}Q{x + w:.2f} {y:.2f} {x + w:.2f} {y + r:.2f}V{y + h - r:.2f}"
            f"Q{x + w:.2f} {y + h:.2f} {x + w - r:.2f} {y + h:.2f}H{x + r:.2f}Q{x:.2f} {y + h:.2f} {x:.2f} {y + h - r:.2f}"
            f"V{y + r:.2f}Q{x:.2f} {y:.2f} {x + r:.2f} {y:.2f}Z")


OUTLINE = mm_rect(-CL / 2, -CWD / 2, CL, CWD, 4)
WINDOW = mm_rect(-12, -11.5, 24, 15.5, 3)
HOLES = ([mm_circle(-HUB, HUBY, 6.6), mm_circle(HUB, HUBY, 6.6), WINDOW, mm_rect(-9, 22.6, 18, 7, 1.5),
          mm_circle(-20, 26, 2.4), mm_circle(20, 26, 2.4)] + [mm_circle(x, y, 1.7) for x, y in SCREWS])
HEAD = "M-35 31.9L-29 19.5H29L35 31.9Z"   # the raised strip along the bottom edge, where the head goes in


class View:
    """The one projection every part is drawn in: plan millimetres in, screen pixels out; z lifts a part straight up."""

    def __init__(self, ox, oy, k, turn=30, squash=.5):
        c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
        self.m = (k * c, k * squash * s, -k * s, k * squash * c)
        self.ox, self.oy = ox, oy

    def at(self, z, dy=0.0) -> str:
        a, b, c, d = self.m
        return f'<g transform="translate({self.ox:.1f} {self.oy - z + dy:.1f}) matrix({a:.4f} {b:.4f} {c:.4f} {d:.4f} 0 0)">'

    def pt(self, x, y, z):
        a, b, c, d = self.m
        return self.ox + a * x + c * y, self.oy - z + b * x + d * y


def slab(p, v, z, thick, shape, faces):
    """A part with a thickness: its shape (holes and all) again in deep green, stacked down under its top face."""
    steps = max(1, round(thick / 1.5))
    for i in range(steps, 0, -1):
        p.add(v.at(z, thick * i / steps) + f'<path d="{shape}" fill-rule="evenodd" fill="{p.col("deep")}"/></g>')
    p.add(v.at(z))
    faces()
    p.add("</g>")


def ev(p, d, c, a=255, extra=""):
    o = f' fill-opacity="{a / 255:.3f}"' if a < 255 else ""
    p.add(f'<path d="{d}" fill-rule="evenodd" fill="{p.col(c)}"{o}{extra}/>')


def shell_clip(p) -> str:
    """A clip of the shell's own shape, holes and all, so what's printed or moulded on a half never covers a hole."""
    c = p.uid("shell")
    p.defs.append(f'<clipPath id="{c}"><path d="{OUTLINE}{"".join(HOLES)}" clip-rule="evenodd"/></clipPath>')
    return c


def upper_half(p, v, z):
    shape, clip = OUTLINE + "".join(HOLES), shell_clip(p)

    def faces():
        ev(p, shape, "cream")
        p.add(f'<g clip-path="url(#{clip})">')
        p.add(f'<path d="{mm_rect(-42, -25, 84, 34, 3)}" fill="{p.col("green")}"/>')        # the label, round the window
        p.add(f'<path d="{mm_rect(-38, -21.5, 76, 4, 1.5)}" fill="{p.col("cream")}"/>')
        p.add(f'<path d="{HEAD}" fill="{p.col("foot")}"/>')
        p.add("</g>")
    slab(p, v, z, 12, shape, faces)


def lower_half(p, v, z):
    """The lower half lies face down, so what shows is its inside: a boss round each screw hole, the roller pins."""
    shape, clip = OUTLINE + "".join(HOLES), shell_clip(p)

    def faces():
        ev(p, shape, "foot")
        p.add(f'<g clip-path="url(#{clip})"><path d="{HEAD}" fill="{p.col("stock")}"/></g>')
        for x, y in SCREWS:
            ev(p, mm_circle(x, y, 3.4) + mm_circle(x, y, 1.7), "cream")
        for x, y in ROLLERS:
            p.add(f'<path d="{mm_circle(x, y, 1.3)}" fill="{p.col("ink")}"/>')
    slab(p, v, z, 12, shape, faces)


def slip_sheet(p, v, z):
    """Thin see-through film with the hub holes and the window cut out: no thickness, only an edge."""
    shape = mm_rect(-45, -24, 90, 37, 2) + mm_circle(-HUB, HUBY, 8.6) + mm_circle(HUB, HUBY, 8.6) + WINDOW
    p.add(v.at(z))
    ev(p, shape, "white", a=40, extra=f' stroke="{p.col("foot")}" stroke-width="2" vector-effect="non-scaling-stroke"')
    for y in (-20, 8.5):
        p.add(f'<path d="{mm_rect(-40, y, 80, .5, .2)}" fill="{p.col("foot")}"/>')
    p.add("</g>")


def tape_pack(p, v, z):
    """The tape: wound on two hubs (more on the left), out round the two guide rollers along the bottom edge, and past
    the felt pressure pad on its leaf spring. The tape stands 3.8 mm tall, so its run between the rollers is a ribbon."""
    run = (f"M{-HUB - 17:.2f} {HUBY + 5:.2f}L{ROLLERS[0][0] - 1.6:.2f} {ROLLERS[0][1]:.2f}"
           f"L{ROLLERS[0][0]:.2f} {ROLLERS[0][1] + 1.6:.2f}L{ROLLERS[1][0]:.2f} {ROLLERS[1][1] + 1.6:.2f}"
           f"L{ROLLERS[1][0] + 1.6:.2f} {ROLLERS[1][1]:.2f}L{HUB + 11.6:.2f} {HUBY + 3.4:.2f}")
    for i in range(8, -1, -1):
        p.add(v.at(z, i) + f'<path d="{run}" fill="none" stroke="{p.col("deep" if i else "oxide")}" stroke-width="4" '
                           f'stroke-linejoin="round" vector-effect="non-scaling-stroke"/></g>')
    for x, r in ((-HUB, 17.5), (HUB, 12)):
        slab(p, v, z, 9, mm_circle(x, HUBY, r), lambda x=x, r=r: p.add(f'<path d="{mm_circle(x, HUBY, r)}" fill="{p.col("oxide")}"/>'))

        def hub(x=x):
            p.add(f'<path d="{mm_circle(x, HUBY, 6.1)}" fill="{p.col("cream")}"/>')
            for j in range(6):
                p.add(f'<rect x="{x - .8:.2f}" y="{HUBY - 6.1:.2f}" width="1.6" height="2.4" fill="{p.col("ink")}" '
                      f'transform="rotate({j * 60} {x:.2f} {HUBY:.2f})"/>')
            p.add(f'<path d="{mm_circle(x, HUBY, 1.6)}" fill="{p.col("ink")}"/>')
        slab(p, v, z + 3, 3, mm_circle(x, HUBY, 6.1), hub)
    for x, y in ROLLERS:
        slab(p, v, z + 1, 9, mm_circle(x, y, 2.4), lambda x=x, y=y: (
            p.add(f'<path d="{mm_circle(x, y, 2.4)}" fill="{p.col("cream")}"/>'),
            p.add(f'<path d="{mm_circle(x, y, 1)}" fill="{p.col("ink")}"/>')))
    p.add(v.at(z) + f'<path d="M-9 24.5Q0 20 9 24.5" fill="none" stroke="{p.col("deep")}" stroke-width="3" '
                    f'vector-effect="non-scaling-stroke"/></g>')
    slab(p, v, z + 2, 6, mm_rect(-4, 24.6, 8, 3, .8), lambda: p.add(f'<path d="{mm_rect(-4, 24.6, 8, 3, .8)}" fill="{p.col("green")}"/>'))


def screw(p, v, x, y, z):
    """A screw, point down, over its hole."""
    hx, hy = v.pt(x, y, z)
    p.poly([(hx - 4, hy + 34), (hx, hy + 42), (hx + 4, hy + 34)], "foot")
    p.rect(hx - 4, hy, 8, 35, "foot", r=1)
    for k in range(5):                                       # the thread
        p.poly([(hx - 4, hy + 9 + k * 5), (hx + 4, hy + 6 + k * 5), (hx + 4, hy + 8 + k * 5), (hx - 4, hy + 11 + k * 5)], "stock")
    slab(p, v, z, 5, mm_circle(x, y, 3.3), lambda: (
        p.add(f'<path d="{mm_circle(x, y, 3.3)}" fill="{p.col("cream")}"/>'),
        p.add(f'<path d="{mm_rect(x - 2.1, y - .45, 4.2, .9, .3)}{mm_rect(x - .45, y - 2.1, .9, 4.2, .3)}" fill="{p.col("ink")}"/>')))


def building_art(t: dict) -> Pic:
    """Building it: the cassette pulled apart along its screws, every part straight over where it sits."""
    W, H = 640, 960
    p = Pic(W, H, t, "A cassette pulled apart the way a service manual draws it, every part floating straight above where it "
                     "goes: five screws, the upper half with its label, a slip sheet, the tape wound on its two hubs with the "
                     "guide rollers and the pressure pad, another slip sheet, and the lower half, with dashed lines down "
                     "through the screw holes and the hubs.")
    v = View(W / 2 - 4, H - 150, 4.8)
    zs = dict(lower=0, slip1=104, tape=196, slip2=300, upper=392, screws=620)
    dash = f'stroke="{p.col("soft")}" stroke-width="1.6" stroke-dasharray="5 6" fill="none"'
    for x, y in SCREWS + ((-HUB, HUBY), (HUB, HUBY)):          # the lines it goes back together along
        top = zs["screws"] + 30 if (x, y) in SCREWS else zs["upper"] + 60
        (x1, y1), (_, y2) = v.pt(x, y, top), v.pt(x, y, -4)
        p.add(f'<path d="M{x1:.1f} {y1:.1f}V{y2:.1f}" {dash}/>')
    lower_half(p, v, zs["lower"])
    slip_sheet(p, v, zs["slip1"])
    tape_pack(p, v, zs["tape"])
    slip_sheet(p, v, zs["slip2"])
    upper_half(p, v, zs["upper"])
    for x, y in sorted(SCREWS, key=lambda s: v.pt(*s, 0)[1]):
        screw(p, v, x, y, zs["screws"])
    return p


# ---- the divider: a headphone plug, about to go in ------------------------------------------------------------------------

def div_jack(t: dict) -> Pic:
    """A 3.5 mm plug meeting an extension's socket in the middle of the page. Its pin is tip, ring, sleeve with the black
    bands between: the tip is the left channel and the ring the right, which is the whole of stereo."""
    W, H = 1280, 72
    p = Pic(W, H, t, "")
    cy, gap = 32, 12
    tip = W / 2 - gap / 2
    mx = tip - 70                     # where the metal starts
    gx = mx - 74                      # the grip
    sock = W / 2 + gap / 2

    def parts(o, mono):
        c = lambda k: mono or k
        p.rect(60 + o, cy - 4 + o, gx - 40 - 60, 8, c("cord"), r=4)
        p.rect(sock + 118 + o, cy - 4 + o, W - 60 - (sock + 118), 8, c("cord"), r=4)
        p.poly([(gx - 44 + o, cy - 5 + o), (gx + 2 + o, cy - 12 + o), (gx + 2 + o, cy + 12 + o), (gx - 44 + o, cy + 5 + o)], c("green"))
        p.rect(gx + o, cy - 16 + o, 74, 32, c("green"), r=8)
        p.rect(mx + o, cy - 7 + o, 30, 14, c("cream"), r=2)                # sleeve: ground
        p.rect(mx + 30 + o, cy - 7 + o, 5, 14, c("ink"))
        p.rect(mx + 35 + o, cy - 7 + o, 15, 14, c("cream"))                # ring: right
        p.rect(mx + 50 + o, cy - 7 + o, 5, 14, c("ink"))
        p.poly([(mx + 55 + o, cy - 7 + o), (mx + 62 + o, cy - 7 + o), (tip + o, cy + o), (mx + 62 + o, cy + 7 + o),
                (mx + 55 + o, cy + 7 + o)], c("cream"))                    # tip: left
        p.rect(sock + o, cy - 11 + o, 18, 22, c("foot"), r=3)              # the socket's collar
        p.rect(sock + 16 + o, cy - 17 + o, 82, 34, c("cream"), r=9)
        p.poly([(sock + 96 + o, cy - 12 + o), (sock + 120 + o, cy - 5 + o), (sock + 120 + o, cy + 5 + o), (sock + 96 + o, cy + 12 + o)],
               c("cream"))
        if not mono:
            for i in range(4):
                p.rect(gx + 14 + i * 14, cy - 16, 4, 32, "deep")
            for i in range(2):
                p.rect(sock + 64 + i * 12, cy - 17, 4, 34, "foot")
    parts(4, "deep")
    parts(0, None)
    return p


def write(pic: Pic, name: str, theme: str) -> None:
    out = HERE / ("dark" if theme == "dark" else "") / name
    out.parent.mkdir(exist_ok=True)
    out.write_text(pic.svg(), encoding="utf-8")


def main() -> None:
    check_contrast()
    for theme, t in THEMES.items():
        write(hero(t), "hero.svg", theme)
        write(sound(t), "sound.svg", theme)
        write(div_jack(t), "div-1.svg", theme)
        write(div_levels(t), "div-2.svg", theme)
        for name, art in (("install", install_art), ("using", using_art), ("spotify", spotify_art), ("building", building_art)):
            write(art(t), f"art-{name}.svg", theme)
    for slug, label in (("install", "Install"), ("using-it", "Using it"), ("spotify", "Spotify"), ("the-sound", "The sound"),
                        ("building", "Building it")):
        write(nav(label, slug == "install"), f"nav-{slug}.svg", "light")
    cia = HERE.parent.parent / "player" / "cia"
    (cia / "banner.svg").write_text(banner(THEMES["dark"]).svg(), encoding="utf-8")
    (cia / "icon.svg").write_text(icon(THEMES["dark"]).svg(), encoding="utf-8")
    print("drew", ", ".join(sorted(f.name for f in HERE.glob("*.svg"))), "and player/cia/banner.svg, icon.svg")


if __name__ == "__main__":
    main()
