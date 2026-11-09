# The HOME Menu banner as a real 3D model (Alex: "make it a 3d thing, 3d cassettes"): banner 2b's pile,
# the two tapes built in three.js. Writes index.html beside this file, the wordmark's glyphs taken
# from player/cia/banner.svg so the label is the real one.   python -I mockups/banner3d/make.py
import json, os, re
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = open(os.path.join(HERE, '..', '..', 'player', 'cia', 'banner.svg'), encoding='utf-8').read()
GLYPHS = dict(re.findall(r'<path id="(\w+)" d="([^"]+)"', SRC))

PAGE = r'''<!doctype html><meta charset="utf-8"><title>3DStereo banner, in 3D</title>
<style>
body{margin:0;padding:28px;background:#1b1b1b;font:13px/1.4 "IBM Plex Sans",system-ui,sans-serif;color:#ddd;display:flex;flex-direction:column;align-items:center;gap:14px}
.top{width:800px;height:480px;position:relative;border-radius:8px;overflow:hidden;background:linear-gradient(#5d6066,#3e4045);box-shadow:0 0 0 8px #0d0d0d}
.top::before{content:"";position:absolute;inset:0;background:repeating-linear-gradient(90deg,#ffffff07 0 4px,transparent 4px 18px)}
#view{position:absolute;left:144px;top:80px;width:512px;height:256px;cursor:grab}
#view.native{image-rendering:pixelated}
.bar{display:flex;gap:18px;align-items:center;color:#999}
.bar label{display:flex;gap:6px;align-items:center;cursor:pointer}
.bar b{color:#1ed760}
</style>
<div class="top"><canvas id="view" class="native"></canvas></div>
<div class="bar"><span><b>2b</b> in 3D: drag to turn it, it settles back</span>
<label><input type="checkbox" id="flip"> the back</label>
<label><input type="checkbox" id="crisp"> crisp (the 3DS draws it at the size shown unticked)</label></div>
<script type="importmap">{"imports":{"three":"https://cdn.jsdelivr.net/npm/three@0.170.0/build/three.module.js"}}</script>
<script type="module">
import * as THREE from 'three';
const G = __GLYPHS__;
const C = {green: 0x1ed760, deep: 0x0a6e39, cream: 0xf2e6cf, foot: 0xd9caa8, ink: 0x121212, tape: 0x4a2e1a};

// ---- the label: the wordmark, drawn from its own glyphs --------------------------------------------
function wordmark(ink, three) {
  const cv = document.createElement('canvas'); cv.width = 1024; cv.height = 192;
  const g = cv.getContext('2d');
  const k = 150 / 700, W = 4297 * k, x0 = (cv.width - W) / 2, base = 168;
  const put = (id, x, col) => { g.save(); g.translate(x, base); g.scale(k, -k); g.fillStyle = col; g.fill(new Path2D(G[id])); g.restore(); };
  put('b33', x0, three);
  put('b44', x0 + 600 * k, ink); put('b53', x0 + 1297 * k, ink);
  [['r74', 0], ['r65', 351], ['r72', 900], ['r65', 1267], ['r6f', 1816]].forEach(([id, dx]) => put(id, x0 + (1921 + dx) * k, ink));
  const t = new THREE.CanvasTexture(cv); t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 4;
  return t;
}

// ---- one cassette, in the icon's units / 10: 5.92 wide, 3.79 tall, 0.72 thick -----------------------
const W = 5.92, H = 3.7888, T = 0.72;
const P = (lx, ly) => [lx / 10 - W / 2, H / 2 - ly / 10];     // the icon's coordinates (y down) -> ours
function rrect(s, x, y, w, h, r, hole) {   // a rounded rectangle path, x/y its lower-left
  s.moveTo(x + r, y); s.lineTo(x + w - r, y); s.quadraticCurveTo(x + w, y, x + w, y + r);
  s.lineTo(x + w, y + h - r); s.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  s.lineTo(x + r, y + h); s.quadraticCurveTo(x, y + h, x, y + h - r);
  s.lineTo(x, y + r); s.quadraticCurveTo(x, y, x + r, y);
}
function pill(x, y, w, h) { const p = new THREE.Path(); const r = h / 2;
  p.moveTo(x + r, y); p.lineTo(x + w - r, y); p.absarc(x + w - r, y + r, r, -Math.PI / 2, Math.PI / 2, false);
  p.lineTo(x + r, y + h); p.absarc(x + r, y + r, r, Math.PI / 2, Math.PI * 1.5, false); return p; }
const mat = (c, o = {}) => new THREE.MeshStandardMaterial({color: c, roughness: .62, metalness: 0, ...o});

function cassette({shell, stripe, label, reelL, reelR}) {
  const grp = new THREE.Group();
  // the shell, its window cut through, its sides in the deep green every extrusion of ours wears
  const s = new THREE.Shape(); rrect(s, -W / 2, -H / 2, W, H, .4736);
  const [wx, wy] = P(10.656, 11.366 + 12.882), ww = 3.7888, wh = 1.2882;
  s.holes.push(pill(wx, wy, ww, wh));
  const bev = .06;
  const geo = new THREE.ExtrudeGeometry(s, {depth: T - 2 * bev, bevelEnabled: true, bevelThickness: bev, bevelSize: bev, bevelSegments: 3, curveSegments: 10});
  geo.translate(0, 0, bev);
  const body = new THREE.Mesh(geo, [mat(shell), mat(C.deep)]);
  body.castShadow = body.receiveShadow = true; grp.add(body);
  // both faces get the same details: back(m) puts a copy of a front-face piece on the back, facing out
  const back = (m, zf) => { const c = m.clone(); c.rotation.y += Math.PI; c.position.z = T - zf; grp.add(c); return c; };
  // inside, through both windows: a dark plate in the middle, the tape packs and the reels through it
  const plate = new THREE.Mesh(new THREE.PlaneGeometry(ww + .2, wh + .2), mat(0x0c0c0c, {roughness: .9, side: THREE.DoubleSide}));
  plate.position.set(0, wy + wh / 2, T * .5); grp.add(plate);
  const reelY = wy + wh / 2;
  [[-1.25, reelL, .62], [1.25, reelR, .38]].forEach(([x, col, pack]) => {
    const tp = new THREE.Mesh(new THREE.CylinderGeometry(pack, pack, T * .8, 40), mat(C.tape, {roughness: .35}));
    tp.rotation.x = Math.PI / 2; tp.position.set(x, reelY, T * .5); grp.add(tp);
    const reel = new THREE.Group();
    const hub = new THREE.Mesh(new THREE.CylinderGeometry(.4637, .4637, T * .88, 36), mat(col));
    hub.rotation.x = Math.PI / 2; reel.add(hub);
    const hole = new THREE.Mesh(new THREE.CylinderGeometry(.195, .195, T * .92, 6), mat(C.ink));   // the hex the spindle grips
    hole.rotation.x = Math.PI / 2; reel.add(hole);
    for (let i = 0; i < 6; i++) {   // the teeth round the hole
      const t = new THREE.Mesh(new THREE.BoxGeometry(.05, .09, T * .92), mat(col));
      const a = i * Math.PI / 3; t.position.set(Math.cos(a) * .17, Math.sin(a) * .17, 0); t.rotation.z = a; reel.add(t);
    }
    reel.position.set(x, reelY, T * .5); reel.userData.spin = true; grp.add(reel);
  });
  // the foot: the raised bit along the bottom where the heads go in, both sides
  const f = new THREE.Shape(); const a = P(13.024, 37.888), b = P(17.168, 29.553), c = P(42.032, 29.553), d = P(46.176, 37.888);
  f.moveTo(a[0], a[1] + .02); f.lineTo(b[0], b[1]); f.lineTo(c[0], c[1]); f.lineTo(d[0], d[1] + .02); f.closePath();
  const fg = new THREE.ExtrudeGeometry(f, {depth: .07, bevelEnabled: true, bevelThickness: .02, bevelSize: .02, bevelSegments: 2});
  const foot = new THREE.Mesh(fg, [mat(C.foot), mat(C.foot)]); foot.position.z = T; foot.castShadow = true; grp.add(foot);
  back(foot, T + .07 + .02).position.z = -.07;
  // five screws a side: the four corners, and one on the foot
  const metal = mat(0xa8a8a8, {roughness: .35, metalness: .6}), slot = mat(0x3a3a3a);
  const screw = (lx, ly, z) => {
    const g = new THREE.Group(); const [x, y] = P(lx, ly);
    const head = new THREE.Mesh(new THREE.CylinderGeometry(.12, .12, .03, 20), metal); head.rotation.x = Math.PI / 2; g.add(head);
    [0, Math.PI / 2].forEach(r => { const sl = new THREE.Mesh(new THREE.BoxGeometry(.17, .025, .01), slot); sl.position.z = .016; sl.rotation.z = r + .4; g.add(sl); });
    g.position.set(x, y, z + .015); grp.add(g);
    const bk = g.clone(); bk.rotation.y = Math.PI; bk.position.z = T - (z + .015); grp.add(bk);
  };
  [[3.1, 3.1], [56.1, 3.1], [3.1, 34.8], [56.1, 34.8]].forEach(([x, y]) => screw(x, y, T));
  screw(29.6, 33.9, T + .09);
  // the edges: the openings along the bottom (capstans, pinch roller, the head), the write-protect
  // tabs' notches along the top
  const hole = mat(0x161616, {roughness: .9});
  [[-1.62, .34], [-.78, .26], [0, .5], [.78, .26], [1.62, .34]].forEach(([x, w]) => {
    const o = new THREE.Mesh(new THREE.BoxGeometry(w, .03, T * .55), hole); o.position.set(x, -H / 2 - bev + .012, T / 2); grp.add(o);
  });
  [-2.35, 2.35].forEach(x => {
    const o = new THREE.Mesh(new THREE.BoxGeometry(.32, .03, T * .3), hole); o.position.set(x, H / 2 + bev - .012, T * .62); grp.add(o);
  });
  // the faces: a stripe, or the wordmark where the stripe goes; the back always gets a stripe
  const stripeMesh = col => {
    const st = new THREE.Shape(); const [sx, sy] = P(5.92, 4.925 + 3.789); rrect(st, sx, sy, 4.736, .3789, .1894);
    const sm = new THREE.Mesh(new THREE.ShapeGeometry(st, 8), mat(col)); sm.position.z = T + .004; return sm;
  };
  if (stripe !== undefined) grp.add(stripeMesh(stripe));
  back(stripeMesh(stripe !== undefined ? stripe : C.green), T + .004);
  if (label) {
    const lw = W * .74, lh = lw * 192 / 1024;
    const lm = new THREE.Mesh(new THREE.PlaneGeometry(lw, lh), new THREE.MeshStandardMaterial({map: label, transparent: true, roughness: .62}));
    lm.position.set(0, H / 2 - .16 - lh / 2, T + .004); grp.add(lm);
  }
  return grp;
}

// ---- the pile: green underneath, cream on top wearing the wordmark --------------------------------
const pile = new THREE.Group();
const under = cassette({shell: C.green, stripe: C.cream, reelL: C.cream, reelR: C.green});
under.position.set(.95, .32, 0); under.rotation.z = THREE.MathUtils.degToRad(-9);
const top = cassette({shell: C.cream, label: wordmark('#121212', '#0a6e39'), reelL: C.green, reelR: C.cream});
top.position.set(-.55, -.32, T + .03); top.rotation.z = THREE.MathUtils.degToRad(7);
pile.add(under, top);
pile.rotation.set(-.32, .22, 0);   // tipped toward you a little: enough to read as thick
const rest = pile.rotation.clone();

// ---- the stage ------------------------------------------------------------------------------------
const cv = document.getElementById('view');
const r = new THREE.WebGLRenderer({canvas: cv, antialias: true, alpha: true});
r.shadowMap.enabled = true; r.shadowMap.type = THREE.PCFSoftShadowMap;
r.outputColorSpace = THREE.SRGBColorSpace; r.toneMapping = THREE.NoToneMapping;
const scene = new THREE.Scene(); scene.add(pile);
const cam = new THREE.PerspectiveCamera(24, 2, .1, 100); cam.position.set(0, 0, 15.5); cam.lookAt(0, 0, 0);
scene.add(new THREE.HemisphereLight(0xffffff, 0x404040, 1.6));
const sun = new THREE.DirectionalLight(0xffffff, 2.2); sun.position.set(-5, 7, 10);   // from up-left: shadows fall down-right
sun.castShadow = true; sun.shadow.mapSize.set(1024, 1024); sun.shadow.radius = 3; sun.shadow.bias = -.0005;
Object.assign(sun.shadow.camera, {left: -6, right: 6, top: 4, bottom: -4, near: 1, far: 30});
scene.add(sun);
function size() {
  const crisp = document.getElementById('crisp').checked;
  cv.classList.toggle('native', !crisp);
  r.setPixelRatio(1);
  r.setSize(crisp ? 512 : 256, crisp ? 256 : 128, false);   // unticked: the 3DS's own 256x128 pixels, shown at 2x
}
if (location.hash.includes('back')) document.getElementById('flip').checked = true;
if (location.hash.includes('crisp')) document.getElementById('crisp').checked = true;
document.getElementById('crisp').onchange = size; size();

// drag to turn it; let go and it eases back
let drag = null, rx = 0, ry = 0;
cv.onpointerdown = e => { drag = [e.clientX, e.clientY, rx, ry]; cv.setPointerCapture(e.pointerId); cv.style.cursor = 'grabbing'; };
cv.onpointermove = e => { if (drag) { ry = drag[3] + (e.clientX - drag[0]) * .01; rx = drag[2] + (e.clientY - drag[1]) * .01; } };
cv.onpointerup = () => { drag = null; cv.style.cursor = 'grab'; };
const clock = new THREE.Clock();
(function frame() {
  const dt = Math.min(clock.getDelta(), .05);
  if (!drag) { rx *= .9; ry *= .9; }
  pile.rotation.set(rest.x + rx, rest.y + ry + (document.getElementById('flip').checked ? Math.PI : 0), 0);
  pile.traverse(o => { if (o.userData.spin) o.rotation.z -= dt * 1.1; });   // the one thing that moves: the reels, slowly
  r.render(scene, cam);
  requestAnimationFrame(frame);
})();
window.__ready = true;
</script>'''

if __name__ == '__main__':
    page = PAGE.replace('__GLYPHS__', json.dumps(GLYPHS))
    open(os.path.join(HERE, 'index.html'), 'w', encoding='utf-8').write(page)
    print('index.html', len(GLYPHS), 'glyphs')
