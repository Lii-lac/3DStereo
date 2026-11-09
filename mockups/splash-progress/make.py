#!/usr/bin/env python3
"""The splash while new songs are read: four ways to show the progress, over the real splash picture (player/cia/splash.svg).
Writes index.html: one self-contained page, four native 400x240 top screens on one shared, believable scan
(tags read in bursts, the bar stepping about ten times a second like the real loop), then a hold, then again."""
import pathlib, re

here = pathlib.Path(__file__).parent
svg = (here / "../../player/cia/splash.svg").read_text(encoding="utf-8")
defs = re.search(r"<defs>(.*?)</defs>", svg, re.S).group(1)
body = svg[svg.index("</defs>") + 7:svg.rindex("</svg>")].replace('<rect width="400" height="240" fill="#121212"/>', "")

PAGE = r"""<!doctype html><meta charset="utf-8"><title>3DStereo splash, reading songs</title>
<style>
html{background:#171717}
body{margin:0;padding:34px 30px 40px;color:#d8d2c4;font:14px/1.4 "Segoe UI",system-ui,sans-serif}
.top{display:flex;gap:18px;align-items:center;margin:0 0 30px}
button{font:inherit;color:#d8d2c4;background:#262626;border:0;border-radius:16px;padding:6px 18px;cursor:pointer}
button:hover{background:#303030}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(430px,1fr));gap:44px 26px;justify-items:center}
.cell{display:flex;flex-direction:column;align-items:center;gap:16px}
.cell b{font-weight:600;color:#1ed760;font-size:20px}
svg.scr{width:400px;height:240px;display:block;background:#121212;box-shadow:0 0 0 8px #0a0a0a,0 14px 34px #000a}
</style>
<div class="top"><button id="replay">again</button><span id="pct" style="color:#7d786c;font-variant-numeric:tabular-nums"></span></div>
<div class="grid" id="grid"></div>
<svg width="0" height="0" style="position:absolute"><defs>@@DEFS@@<g id="art">@@ART@@</g></defs></svg>
<script>
const G = '#1ed760', CREAM = '#f2e6cf', INK = '#121212', TAPE = '#4a2e1a';
const NS = 'http://www.w3.org/2000/svg';
const el = (p, n, a) => { const e = document.createElementNS(NS, n); for (const k in (a || {})) e.setAttribute(k, a[k]); p.appendChild(e); return e; };
const lerp = (a, b, t) => a + (b - a) * t;
const ease = t => t < .5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;

// the scan: what the splash would be told, in bursts (tags read from the card) with pauses between
const SCAN_MS = 6200, HOLD_MS = 1500, TICK = 100;
function scanAt(ms) {
  const t = Math.min(1, ms / SCAN_MS);
  const w = [0, .07, .09, .31, .36, .62, .66, .93, 1];
  const x = Math.min(w.length - 2, t * (w.length - 1) | 0), f = t * (w.length - 1) - x;
  return lerp(w[x], w[x + 1], f < .5 ? 0 : ease((f - .5) * 2));
}

const variants = [
  { n: '1', make(svg) {   // a thin rule under the wordmark, a light sweeping along the fill
      const track = el(svg, 'rect', { x: 120, y: 216, width: 160, height: 4, rx: 2, fill: '#242424', opacity: 0 });
      const clip = el(svg, 'clipPath', { id: 'v1c' }); const cr = el(clip, 'rect', { x: 120, y: 214, width: 0, height: 8 });
      el(svg, 'rect', { x: 120, y: 216, width: 160, height: 4, rx: 2, fill: G, 'clip-path': 'url(#v1c)' });
      const sw = el(svg, 'rect', { x: 120, y: 216, width: 26, height: 4, rx: 2, fill: '#c9ffe0', opacity: 0, 'clip-path': 'url(#v1c)' });
      return (p, t) => { track.setAttribute('opacity', p > 0 ? 1 : 0); cr.setAttribute('width', 160 * p);
        sw.setAttribute('x', 120 - 26 + (160 + 26) * ((t / 1300) % 1)); sw.setAttribute('opacity', p > 0 && p < 1 ? .45 : 0); }; } },
  { n: '2', make(svg) {   // the tape: the left reel winds down into the right one
      const y = 220, x0 = 126, x1 = 274;
      const base = el(svg, 'line', { x1: x0, x2: x1, y1: y, y2: y, stroke: '#2a2a2a', 'stroke-width': 2, 'stroke-linecap': 'round', opacity: 0 });
      const run = el(svg, 'line', { x1: x0, x2: x0, y1: y, y2: y, stroke: G, 'stroke-width': 2, 'stroke-linecap': 'round' });
      const L = el(svg, 'g'), R = el(svg, 'g');
      const lp = el(L, 'circle', { cx: x0, cy: y, fill: TAPE }); el(L, 'circle', { cx: x0, cy: y, r: 4.4, fill: CREAM });
      const rp = el(R, 'circle', { cx: x1, cy: y, fill: TAPE }); el(R, 'circle', { cx: x1, cy: y, r: 4.4, fill: G });
      const spokes = g => [0, 1, 2].map(() => el(g, 'line', { stroke: INK, 'stroke-width': 1.2, 'stroke-linecap': 'round' }));
      const ls = spokes(L), rs = spokes(R);
      const turn = (arr, cx, a) => arr.forEach((s, i) => { const q = a + i * 2.0944; s.setAttribute('x1', cx); s.setAttribute('y1', y); s.setAttribute('x2', cx + Math.cos(q) * 3.4); s.setAttribute('y2', y + Math.sin(q) * 3.4); });
      return (p, t) => {
        const o = p > 0 ? 1 : 0; base.setAttribute('opacity', o); L.setAttribute('opacity', o); R.setAttribute('opacity', o);
        lp.setAttribute('r', lerp(11, 5.6, p)); rp.setAttribute('r', lerp(5.6, 11, p));
        run.setAttribute('x2', lerp(x0, x1, p)); turn(ls, x0, -t / 260); turn(rs, x1, -t / 260);
      }; } },
  { n: '3', make(svg) {   // a line drawn round the tile itself
      const x = 140.6, y = 38.6, w = 112, h = 112;
      const tr = el(svg, 'rect', { x, y, width: w, height: h, rx: 30, fill: 'none', stroke: '#242424', 'stroke-width': 3, opacity: 0 });
      const ring = el(svg, 'rect', { x, y, width: w, height: h, rx: 30, fill: 'none', stroke: G, 'stroke-width': 3, 'stroke-linecap': 'round', pathLength: 100, 'stroke-dasharray': '0 100', opacity: 0 });
      return p => { tr.setAttribute('opacity', p > 0 ? 1 : 0); ring.setAttribute('stroke-dasharray', (p * 100) + ' 100'); ring.setAttribute('opacity', p > 0 ? 1 : 0); }; } },
  { n: '4', make(svg) {   // the level bars from the back of the banner, climbing one by one
      const N = 9, gap = 10, x0 = 200 - (N - 1) * gap / 2, base = 226, bars = [];
      for (let i = 0; i < N; i++) bars.push(el(svg, 'rect', { x: x0 + i * gap - 2.5, width: 5, rx: 2.5, fill: '#262626', opacity: 0 }));
      return (p, t) => {
        const lit = p * N;
        bars.forEach((b, i) => {
          const on = lit > i, a = Math.max(0, Math.min(1, lit - i));
          const h = on ? 5 + 11 * (.5 + .5 * Math.sin(t / 170 + i * 1.7)) * a : 5;
          b.setAttribute('opacity', p > 0 ? 1 : 0); b.setAttribute('fill', on ? G : '#262626');
          b.setAttribute('height', h); b.setAttribute('y', base - h);
        });
      }; } },
];

const grid = document.getElementById('grid');
const cells = variants.map(v => {
  const c = document.createElement('div'); c.className = 'cell'; c.innerHTML = '<b>' + v.n + '</b>'; grid.appendChild(c);
  const s = document.createElementNS(NS, 'svg'); s.setAttribute('class', 'scr'); s.setAttribute('viewBox', '0 0 400 240'); c.appendChild(s);
  el(s, 'rect', { width: 400, height: 240, fill: INK });
  el(s, 'use', { href: '#art' });
  return v.make(s);
});

let t0 = performance.now();
const fix = /t=(\d+)/.exec(location.hash);          // page.html#t=3200 freezes it at that moment (for stills)
document.getElementById('replay').onclick = () => { t0 = performance.now(); };
function frame(now) {
  if (fix) now = t0 + +fix[1];
  const ms = (now - t0) % (SCAN_MS + HOLD_MS);
  const q = Math.floor(ms / TICK) * TICK;             // the device redraws about ten times a second: the bar steps like that
  const p = ms >= SCAN_MS ? 1 : scanAt(q);
  cells.forEach(f => f(p, now));
  document.getElementById('pct').textContent = ms >= SCAN_MS ? '' : Math.round(p * 100) + '%';
  if (!fix) requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script>
"""
out = PAGE.replace("@@DEFS@@", defs).replace("@@ART@@", body)
(here / "index.html").write_text(out, encoding="utf-8")
print("wrote", here / "index.html", len(out), "bytes")
