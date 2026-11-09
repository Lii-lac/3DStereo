#!/usr/bin/env python3
"""Splash -> home transition mockups. Reads the wordmark glyphs out of player/cia/splash.svg so the
letters are the real ones, then writes index.html (one self-contained page, native 400x240 + 320x240)."""
import json, re, pathlib

here = pathlib.Path(__file__).parent
svg = (here / "../../player/cia/splash.svg").read_text(encoding="utf-8")
glyphs = dict(re.findall(r'<path id="(\w+)" d="([^"]+)"', svg))

PAGE = r'''<!doctype html><meta charset="utf-8"><title>3DStereo splash to home</title>
<style>
body{margin:0;padding:26px;background:#1b1b1b;color:#ddd;font:13px/1.4 "Segoe UI",system-ui,sans-serif}
.bar{display:flex;gap:20px;align-items:center;margin:0 0 22px}
.bar button{font:inherit;color:#ddd;background:#2a2a2a;border:0;border-radius:15px;padding:5px 15px;cursor:pointer}
.bar button:hover{background:#353535}
.bar label{display:flex;gap:6px;align-items:center;cursor:pointer;color:#999}
.decks{display:grid;grid-template-columns:repeat(auto-fit,minmax(620px,1fr));gap:34px 20px;justify-items:center}
.deck{display:flex;flex-direction:column;align-items:center;cursor:pointer}
.deck h2{margin:0 0 12px;font-size:15px;font-weight:600;color:#ddd;display:flex;gap:10px;align-items:baseline}
.deck h2 b{color:#1ed760;font-size:20px}
.deck h2 label{font-size:12px;font-weight:400;color:#999;display:flex;gap:5px;align-items:center}
canvas{display:block;image-rendering:pixelated;box-shadow:0 0 0 7px #0b0b0b;background:#121212}
.deck canvas+canvas{margin-top:34px}
.sheet{display:grid;gap:10px 8px}
.sheet .cell{display:flex;flex-direction:column;gap:4px;align-items:center}
.sheet .cell canvas{box-shadow:none}
.sheet h3{margin:14px 0 0;color:#1ed760;font-size:16px}
</style>
<div id="app"></div>
<script>
const G = /*GLYPHS*/;
const FONT = '"Segoe UI","IBM Plex Sans",system-ui,sans-serif';
const BG = '#121212', GREEN = '#1ed760', DEEP = '#0a6e39', CREAM = '#f2e6cf', FOOT = '#d9caa8', GREY = '#b3b3b3', CHIP = '#2a2a2a';
const SPIN = 0.0062;   // radians per ms: about one turn a second, the reels while we wait

// ---- helpers ---------------------------------------------------------------------------------------
const clamp = (v, a = 0, b = 1) => Math.min(b, Math.max(a, v));
const seg = (t, a, b) => clamp((t - a) / (b - a));
const lerp = (a, b, p) => a + (b - a) * p;
const ease = {
  io: p => p < .5 ? 4 * p * p * p : 1 - Math.pow(-2 * p + 2, 3) / 2,
  out: p => 1 - Math.pow(1 - p, 4),
  in: p => p * p * p,
};
const rr = (g, x, y, w, h, r) => { g.beginPath(); g.roundRect(x, y, w, h, Math.max(0, Math.min(r, w / 2, h / 2))); };
const clr = (g, w) => { g.fillStyle = BG; g.fillRect(0, 0, w, 240); };
const P2 = {}; for (const k in G) P2[k] = new Path2D(G[k]);

// ---- the splash, in pieces (the same numbers as player/cia/splash.svg) -----------------------------------
const WORD = [['b33', 12, GREEN], ['b44', 30, '#fff'], ['b53', 50.91, '#fff'], ['r74', 69.63, '#fff'],
              ['r65', 80.16, '#fff'], ['r72', 96.63, '#fff'], ['r65', 107.64, '#fff'], ['r6f', 124.11, '#fff']];
function word(g, o = {}) {   // o.glyph(i) -> {x, y, s} in the wordmark's own units
  g.save(); g.translate(200, 188); g.scale(1.2, 1.2); g.translate(-85, -64);
  WORD.forEach(([id, x, col], i) => {
    const q = o.glyph ? o.glyph(i) : {}, s = q.s ?? 1;
    g.save(); g.translate(x + (q.x || 0), 72 + (q.y || 0)); g.scale(.03 * s, -.03 * s);
    g.fillStyle = col; g.fill(P2[id]); g.restore();
  });
  g.restore();
}
function tile(g, o = {}) {
  const {cx = 196.55, cy = 94.55, w = 92, h = 92, r = 20.24, sd = 6.9} = o;
  g.fillStyle = DEEP; rr(g, cx - w / 2 + sd, cy - h / 2 + sd, w, h, r); g.fill();
  g.fillStyle = GREEN; rr(g, cx - w / 2, cy - h / 2, w, h, r); g.fill();
}
function reel(g, x, y, col, spin) {
  g.fillStyle = col; g.beginPath(); g.arc(x, y, 4.63749, 0, 7); g.fill();
  g.fillStyle = BG; g.beginPath();
  for (let i = 0; i < 6; i++) { const a = spin + i * Math.PI / 3; g.lineTo(x + 2.4 * Math.cos(a), y + 2.4 * Math.sin(a)); }
  g.fill();
}
const WIN = [179.056, 52.4224, 37.888, 12.8819, 6.44096];
function cassette(g, o = {}) {   // drawn in the tile's own space, so it sits on the tile like the icon
  const {rot = -8, s = 1, spin = 0, ext = 1, reelOut = 0, inside = null} = o;
  if (s <= 0.002) return;
  g.save(); g.translate(200, 98); g.scale(1.15, 1.15); g.translate(-203, -65);
  g.translate(198, 60); g.scale(s, s); g.rotate(rot * Math.PI / 180); g.translate(-198, -60);
  g.fillStyle = DEEP;
  for (let i = 6; i >= 1; i--) { rr(g, 168.4 + .592 * i * ext, 41.056 + .592 * i * ext, 59.2, 37.888, 4.736); g.fill(); }
  g.fillStyle = CREAM; rr(g, 168.4, 41.056, 59.2, 37.888, 4.736); g.fill();
  g.fillStyle = GREEN; rr(g, 174.32, 45.9814, 47.36, 3.7888, 1.8944); g.fill();
  g.fillStyle = BG; rr(g, ...WIN); g.fill();
  g.save(); rr(g, ...WIN); g.clip();
  if (inside) { const m = g.getTransform(); g.setTransform(1, 0, 0, 1, 0, 0); inside(g); g.setTransform(m); }
  reel(g, 185.497 - reelOut, 58.8634, GREEN, spin); reel(g, 210.503 + reelOut, 58.8634, CREAM, spin);
  g.restore();
  g.fillStyle = FOOT; g.beginPath(); g.moveTo(181.424, 78.944); g.lineTo(185.568, 70.6086); g.lineTo(210.432, 70.6086); g.lineTo(214.576, 78.944); g.fill();
  g.restore();
}
function splash(g, o = {}) {   // the whole thing, as it is today (plus turning reels)
  g.save(); g.translate(o.dx || 0, o.dy || 0);
  word(g, o.word); tile(g, o.tile); cassette(g, o.cas); g.restore();
}

// ---- the home screens: player up top, the library on the touchscreen -----------------------------------
const hide = {dx: 0, dy: 0};
const HT = {
  art(g, {dx = 0, dy = 0} = hide) {
    g.save(); g.translate(30 + dx, 44 + dy);
    const gr = g.createLinearGradient(0, 0, 148, 148); gr.addColorStop(0, '#b85a45'); gr.addColorStop(1, '#6a2b27');
    g.fillStyle = gr; rr(g, 0, 0, 148, 148, 10); g.fill(); g.restore();
  },
  text(g, {dx = 0, dy = 0} = hide) {
    g.fillStyle = '#fff'; g.font = '700 24px ' + FONT; g.fillText('madeleine', 206 + dx, 88 + dy);
    g.fillStyle = GREY; g.font = '400 15px ' + FONT; g.fillText('brakence', 206 + dx, 112 + dy);
  },
  ctl(g, {dx = 0, dy = 0} = hide) {
    const y = 160 + dy;
    g.fillStyle = '#fff'; g.beginPath(); g.arc(252 + dx, y, 21, 0, 7); g.fill();
    g.fillStyle = BG; g.beginPath(); g.moveTo(246 + dx, y - 10); g.lineTo(246 + dx, y + 10); g.lineTo(262 + dx, y); g.fill();
    g.fillStyle = GREY;
    g.beginPath(); g.moveTo(322 + dx, y - 9); g.lineTo(322 + dx, y + 9); g.lineTo(336 + dx, y); g.fill(); g.fillRect(337 + dx, y - 9, 3, 18);
    g.beginPath(); g.moveTo(182 + dx, y - 9); g.lineTo(182 + dx, y + 9); g.lineTo(168 + dx, y); g.fill(); g.fillRect(165 + dx, y - 9, 3, 18);
  },
  bar(g, {dx = 0, dy = 0} = hide) {
    g.fillStyle = '#3a3a3a'; rr(g, 30 + dx, 200 + dy, 340, 5, 2.5); g.fill();
    g.fillStyle = GREEN; rr(g, 30 + dx, 200 + dy, 130, 5, 2.5); g.fill();
    g.fillStyle = '#fff'; g.beginPath(); g.arc(160 + dx, 202.5 + dy, 6, 0, 7); g.fill();
    g.fillStyle = GREY; g.font = '400 11px ' + FONT; g.fillText('1:12', 30 + dx, 224 + dy);
    g.textAlign = 'right'; g.fillText('3:04', 370 + dx, 224 + dy); g.textAlign = 'left';
  },
};
function homeTop(g, o = {}) {   // o.art, o.text, o.ctl, o.bar: each {dx, dy}
  g.fillStyle = BG; g.fillRect(0, 0, 400, 240);
  for (const k of ['art', 'text', 'ctl', 'bar']) HT[k](g, o[k] || hide);
}
const TABS = [['ham', 22], ['home', 58], ['person', 190], ['disc', 222], ['note', 254], ['search', 298]];
const ICON = {
  ham(g, x, y) { for (const d of [-4.5, 0, 4.5]) g.fillRect(x - 6, y + d - 1, 12, 2); },
  home(g, x, y) { g.beginPath(); g.moveTo(x, y - 7); g.lineTo(x + 8, y); g.lineTo(x + 5.5, y); g.lineTo(x + 5.5, y + 6.5); g.lineTo(x - 5.5, y + 6.5); g.lineTo(x - 5.5, y); g.lineTo(x - 8, y); g.fill(); },
  person(g, x, y) { g.beginPath(); g.arc(x, y - 3.5, 3.6, 0, 7); g.fill(); g.beginPath(); g.arc(x, y + 8, 7, Math.PI, 0); g.fill(); },
  disc(g, x, y) { g.beginPath(); g.arc(x, y, 7, 0, 7); g.fill(); g.fillStyle = CHIP; g.beginPath(); g.arc(x, y, 2.2, 0, 7); g.fill(); },
  note(g, x, y) { g.beginPath(); g.ellipse(x - 2.5, y + 4.5, 3.4, 2.7, -.4, 0, 7); g.fill(); g.fillRect(x + .2, y - 6.5, 1.9, 11); g.fillRect(x + .2, y - 6.5, 5, 2); },
  search(g, x, y) { g.lineWidth = 2; g.beginPath(); g.arc(x - 1.5, y - 1.5, 5, 0, 7); g.stroke(); g.beginPath(); g.moveTo(x + 2, y + 2); g.lineTo(x + 6.5, y + 6.5); g.stroke(); },
};
function tabs(g, el = () => hide) {
  TABS.forEach(([k, bx], i) => {
    const {dx = 0, dy = 0} = el(i), x = bx + dx, y = 26 + dy;
    g.fillStyle = CHIP; g.beginPath(); g.arc(x, y, 15, 0, 7); g.fill();
    g.fillStyle = g.strokeStyle = '#fff'; ICON[k](g, x, y);
  });
}
function pillTab(g, {dx = 0, dy = 0, rv = 1} = hide) {   // the green "Playlists" pill, its label wiped in left to right
  g.fillStyle = GREEN; rr(g, 76 + dx, 11 + dy, 96, 30, 15); g.fill();
  if (rv <= 0) return;
  g.save(); g.beginPath(); g.rect(76 + dx, 11 + dy, 96 * rv, 30); g.clip();
  g.fillStyle = '#0b0b0b'; g.fillRect(88 + dx, 21.5 + dy, 11, 2); g.fillRect(88 + dx, 25.5 + dy, 11, 2); g.fillRect(88 + dx, 29.5 + dy, 7, 2);
  g.font = '700 14px ' + FONT; g.fillText('Playlists', 106 + dx, 31.5 + dy); g.restore();
}
const ROWS = [['Liked Songs', 'Playlist · 412 songs', '#1c9a4d'], ['swing and a Miss', 'Playlist · Lilac', '#8a82a6'],
              ['foulwintercore', 'Playlist · Lilac', '#7a3b30'], ['Pylon', 'Playlist · Lilac', '#5f6f55'], ['Dearie', 'Playlist · Lilac', '#a89a78']];
function rows(g, el = () => hide) {
  ROWS.forEach(([a, b, col], i) => {
    const {dx = 0, dy = 0} = el(i), y = 60 + i * 36 + dy, x = 14 + dx;
    g.fillStyle = col; rr(g, x, y, 30, 30, 4); g.fill();
    g.fillStyle = '#fff'; g.font = '600 14px ' + FONT; g.fillText(a, x + 40, y + 13);
    g.fillStyle = GREY; g.font = '400 11px ' + FONT; g.fillText(b, x + 40, y + 27);
  });
}
function homeBottom(g, o = {}) {   // o.tabs(i), o.pill, o.rows(i)
  g.fillStyle = BG; g.fillRect(0, 0, 320, 240);
  tabs(g, o.tabs); if (o.pill !== false) pillTab(g, o.pill); rows(g, o.rows);
}

// ---- the four ways out --------------------------------------------------------------------------------
// t = ms since the app started, R = when the library is read (the animation starts then, never later)
const rowStagger = (x, t0, d, dur, dy = 0, dxs = 0) => i => { const p = 1 - ease.out(seg(x, t0 + i * d, t0 + i * d + dur)); return {dx: dxs * p, dy: dy * p}; };

// 1. push: how the swap gesture already moves things: the strip slides up one screen
function cPush(top, bot, t, R) {
  const x = t - R, p = ease.io(seg(x, 0, 540)), q = ease.io(seg(x, 70, 610));
  clr(top, 400); clr(bot, 320);
  splash(top, {dy: -258 * p, cas: {spin: t * SPIN}});
  top.save(); top.translate(0, 258 * (1 - p)); homeTop(top); top.restore();
  if (x > 0) { bot.save(); bot.translate(0, 258 * (1 - q)); homeBottom(bot); bot.restore(); }
}

// 2. window: we fall into the cassette's window and the player is what is behind it
function cWindow(top, bot, t, R) {
  const x = t - R, spin = t * SPIN;
  clr(top, 400); clr(bot, 320);
  const e = ease.io(seg(x, 110, 650)), s = Math.pow(26, e), cw = [194.25, 91];
  const tx = lerp(cw[0], 200, e), ty = lerp(cw[1], 120, e);
  word(top, {glyph: i => ({y: 150 * ease.in(seg(x, i * 14, i * 14 + 240))})});
  top.save(); top.translate(tx, ty); top.scale(s, s); top.translate(-cw[0], -cw[1]);
  tile(top);
  cassette(top, {rot: -8 * (1 - e), spin, reelOut: ease.io(seg(x, 30, 190)) * 17, inside: x > 0 ? g => {
    g.save(); g.translate(tx, ty); const hs = Math.pow(.11, 1 - e); g.scale(hs, hs); g.translate(-200, -120);
    homeTop(g); g.restore();
  } : null});
  top.restore();
  if (x > 340) homeBottomIn(bot, x, 360);
}
function homeBottomIn(bot, x, t0) {   // the touchscreen: tabs drop in, rows come up under them
  homeBottom(bot, {
    tabs: rowStagger(x, t0, 25, 280, -34), pill: {...{dx: 0, dy: -34 * (1 - ease.out(seg(x, t0, t0 + 280)))}},
    rows: rowStagger(x, t0 + 60, 38, 300, 48),
  });
}

// 3. pill: everything folds into the green tile, the tile drops through the hinge and becomes the tab you are on
const PILL = {cx: 124 + 40, cy: 258 + 26, w: 96, h: 30, r: 15};   // in the strip: the touchscreen sits 40 right, 258 down
function cPill(top, bot, t, R) {
  const x = t - R, spin = t * SPIN;
  clr(top, 400); clr(bot, 320);
  // the player rises into the top screen once the tile has left it, a part at a time
  if (x > 430) {
    const up = t0 => ({dx: 0, dy: 44 * (1 - ease.out(seg(x, t0, t0 + 300)))});
    HT.art(top, up(430)); HT.text(top, up(470)); HT.ctl(top, up(510)); HT.bar(top, up(550));
  }
  const letter = i => { const g = ease.in(seg(x, i * 14, i * 14 + 150)); return {x: (82.1 - WORD[i][1]) * g, y: (-13.9 - 72) * g, s: 1 - g}; };
  if (x < 270) word(top, {glyph: letter});
  const m = ease.io(seg(x, 280, 640)), m2 = ease.in(seg(x, 400, 640));
  const T = {cx: lerp(196.55, PILL.cx, m), cy: lerp(94.55, PILL.cy, m), w: lerp(92, PILL.w, m), h: lerp(92, PILL.h, m2),
             r: lerp(20.24, PILL.r, m2), sd: lerp(6.9, 0, seg(x, 280, 460))};
  T.r = Math.min(T.r, T.h / 2);
  tile(top, T);
  const cs = 1 - ease.in(seg(x, 80, 260));
  cassette(top, {s: cs, rot: -8 - 200 * (1 - cs), spin});
  // the touchscreen: the tabs arrive around the pill, the rows come up
  const side = i => { const p = 1 - ease.out(seg(x, 430 + i * 35, 430 + i * 35 + 300)); return {dx: (i < 2 ? -80 : 110) * p, dy: 0}; };
  if (x > 420) { tabs(bot, side); rows(bot, rowStagger(x, 600, 40, 300, 48)); }
  bot.save(); bot.translate(-40, -258); tile(bot, T); bot.restore();   // the tile, seen from the touchscreen
  if (x > 640) pillTab(bot, {rv: ease.out(seg(x, 640, 820))});
}

// 4. depth: the layers of the logo are really at different depths (3D slider up) and slide off at different speeds
function cDepth(top, bot, t, R, o = {}) {
  const x = t - R, spin = t * SPIN;
  const k = 1 - ease.io(seg(x, 560, 780));   // the depth sinks back into the screen as home lands
  const render = (g, eye) => {
    clr(g, 400);
    const dz = z => -eye * 8 * z * k / 2;                      // px of shift for this eye
    const out = (z, t0) => -(470 + 90 * z) * ease.io(seg(x, t0, t0 + 430));
    const come = (z, t0) => (430 + 60 * z) * (1 - ease.out(seg(x, t0, t0 + 480)));
    if (x > 120) {   // home first, so the splash slides over it
      const z = {art: .3, text: .6, ctl: .75, bar: .45};
      const lay = (kk, t0) => ({dx: dz(z[kk]) + come(z[kk], t0), dy: 0});
      HT.art(g, lay('art', 120)); HT.text(g, lay('text', 170)); HT.ctl(g, lay('ctl', 210)); HT.bar(g, lay('bar', 250));
    }
    g.save(); g.translate(dz(0) + out(0, 130), 0); tile(g); g.restore();
    g.save(); g.translate(dz(.45) + out(.45, 65), 0); cassette(g, {spin}); g.restore();
    g.save(); g.translate(dz(1) + out(1, 0), 0); word(g); g.restore();
  };
  if (o.stereo) {
    const L = cv2(0), Rr = cv2(1);
    render(L.g, -1); render(Rr.g, 1);
    const a = L.g.getImageData(0, 0, 400, 240), b = Rr.g.getImageData(0, 0, 400, 240), d = top.createImageData(400, 240);
    for (let i = 0; i < d.data.length; i += 4) {   // red from the left eye, green and blue from the right
      d.data[i] = .3 * a.data[i] + .59 * a.data[i + 1] + .11 * a.data[i + 2];
      d.data[i + 1] = b.data[i + 1]; d.data[i + 2] = b.data[i + 2]; d.data[i + 3] = 255;
    }
    top.putImageData(d, 0, 0);
  } else render(top, 0);
  clr(bot, 320);
  if (x > 80) {
    const side = (t0, i) => { const p = 1 - ease.out(seg(x, t0 + i * 35, t0 + i * 35 + 420)); return {dx: 340 * p, dy: 0}; };
    tabs(bot, i => side(120, i)); pillTab(bot, side(120, 2)); rows(bot, i => side(190, i));
  }
}
const scratch = [];
function cv2(i) { if (!scratch[i]) { const c = document.createElement('canvas'); c.width = 400; c.height = 240; scratch[i] = {c, g: c.getContext('2d', {willReadFrequently: true})}; } return scratch[i]; }

// ---- the page ------------------------------------------------------------------------------------------
const DECKS = [{n: 1, name: 'push', f: cPush}, {n: 2, name: 'window', f: cWindow}, {n: 3, name: 'pill', f: cPill}, {n: 4, name: 'depth', f: cDepth, stereo: true}];
const S = 1.5, st = {R: 500, speed: 1};
const app = document.getElementById('app');

function mk(w, scale = S) { const c = document.createElement('canvas'); c.width = w; c.height = 240; c.style.width = w * scale + 'px'; c.style.height = 240 * scale + 'px'; return c; }

if (location.hash.startsWith('#at=')) {   // one frame at native size: #at=<deck>,<ms after the library is read>
  const [n, xx] = location.hash.slice(4).split(',').map(Number), d = DECKS[n - 1];
  const a = mk(400, 1), b = mk(320, 1); app.append(a, b); app.style.display = 'flex'; app.style.gap = '20px';
  d.f(a.getContext('2d'), b.getContext('2d'), st.R + xx, st.R, {stereo: false}); document.title = 'frame';
} else if (location.hash.startsWith('#sheet')) {   // stills: the exit at a few moments, for a quick look (and for checking the frames)
  const XS = [-150, 90, 200, 320, 450, 700];
  app.className = 'sheet'; app.style.gridTemplateColumns = 'repeat(6,1fr)';
  for (const d of DECKS) {
    const h = document.createElement('h3'); h.textContent = d.n + ' ' + d.name; h.style.gridColumn = '1/-1'; app.append(h);
    for (const x of XS) {
      const cell = document.createElement('div'); cell.className = 'cell';
      const a = mk(400, .5), b = mk(320, .5); cell.append(a, b); app.append(cell);
      d.f(a.getContext('2d'), b.getContext('2d'), st.R + x, st.R, {stereo: d.stereo && location.hash === '#sheet3d'});
    }
  }
  document.title = 'sheet';
} else {
  const bar = document.createElement('div'); bar.className = 'bar';
  bar.innerHTML = '<button id="all">replay</button><label><input type="checkbox" id="slow"> slow scan</label><label><input type="checkbox" id="qt"> ¼×</label>';
  app.append(bar);
  const wrap = document.createElement('div'); wrap.className = 'decks'; app.append(wrap);
  for (const d of DECKS) {
    const el = document.createElement('div'); el.className = 'deck';
    const h = document.createElement('h2'); h.innerHTML = `<b>${d.n}</b>${d.name}` + (d.stereo ? '<label><input type="checkbox"> 3D</label>' : '');
    d.top = mk(400); d.bot = mk(320); el.append(h, d.top, d.bot); wrap.append(el);
    d.g = [d.top.getContext('2d', {willReadFrequently: true}), d.bot.getContext('2d')];
    d.opts = {stereo: false};
    if (d.stereo) h.querySelector('input').onchange = e => { d.opts.stereo = e.target.checked; };
    el.onclick = e => { if (e.target.tagName !== 'INPUT' && e.target.tagName !== 'LABEL') d.t0 = performance.now(); };
    d.t0 = performance.now();
  }
  document.getElementById('all').onclick = () => DECKS.forEach(d => d.t0 = performance.now());
  document.getElementById('slow').onchange = e => { st.R = e.target.checked ? 2200 : 500; DECKS.forEach(d => d.t0 = performance.now()); };
  document.getElementById('qt').onchange = e => { st.speed = e.target.checked ? .25 : 1; DECKS.forEach(d => d.t0 = performance.now()); };
  const tick = now => {
    for (const d of DECKS) d.f(d.g[0], d.g[1], Math.min((now - d.t0) * st.speed, st.R + 1200), st.R, d.opts);
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
  // the title reports whether every frame of every one draws, at a spread of times
  try {
    for (const d of DECKS) for (let t = 0; t <= 2400; t += 37) d.f(d.g[0], d.g[1], t, 500, {stereo: !!d.stereo});
    document.title = 'ok';
  } catch (e) { document.title = 'ERR ' + e.message; }
}
</script>
'''

(here / "index.html").write_text(PAGE.replace("/*GLYPHS*/", json.dumps(glyphs)), encoding="utf-8")
print("wrote", here / "index.html", len(glyphs), "glyphs")
