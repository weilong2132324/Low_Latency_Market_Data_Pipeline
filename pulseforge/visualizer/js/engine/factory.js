/*
 * factory.js - the small set of 3D building blocks every view is made of:
 * glowing/glass materials, outlined boxes, tubes along curves, text labels,
 * and procedurally drawn textures (circuit boards, silicon).
 *
 * Nothing here knows about PulseForge. If you want to learn three.js, this
 * file is the place to start: each helper is a few lines of core API.
 */
import * as THREE from 'three';
import { CSS2DObject } from 'three/addons/renderers/CSS2DRenderer.js';

export const COLORS = {
  hot: 0xff5a3c,
  cold: 0x26e5ff,
  neutral: 0x9aa3b2,
  accent: 0x7c5cff,
  ok: 0x39ffb0,
  warn: 0xffd23f,
  bad: 0xff3366,
  pcb: 0x0b2a2a,
  silicon: 0x10141f,
  receiver: 0xff7a3c,
  workers: 0xd05cff,
  both: 0xffd23f,
  pad: 0x2a3344,
};

/* Emissive surfaces are what the bloom pass makes glow. */
export function glow(color, intensity = 0.6, extra = {}) {
  return new THREE.MeshStandardMaterial({
    color, emissive: color, emissiveIntensity: intensity, roughness: 0.35, metalness: 0.25, ...extra,
  });
}

/* Translucent "hologram glass". depthWrite:false avoids transparent
 * objects hiding each other in the wrong order. */
export function glass(color, opacity = 0.18, extra = {}) {
  return new THREE.MeshStandardMaterial({
    color, emissive: color, emissiveIntensity: 0.12, transparent: true, opacity,
    roughness: 0.15, metalness: 0.1, depthWrite: false, ...extra,
  });
}

export function box(w, h, d, material) {
  return new THREE.Mesh(new THREE.BoxGeometry(w, h, d), material);
}

/* Adds a glowing wireframe outline as a child, so it moves with the mesh. */
export function outline(mesh, color, opacity = 0.8) {
  const edges = new THREE.LineSegments(
    new THREE.EdgesGeometry(mesh.geometry),
    new THREE.LineBasicMaterial({ color, transparent: true, opacity }),
  );
  mesh.add(edges);
  return edges;
}

export function line(points, color, { opacity = 0.7, dashed = false, dash = 0.25, gap = 0.18 } = {}) {
  const geo = new THREE.BufferGeometry().setFromPoints(points.map((p) => new THREE.Vector3(...p)));
  const mat = dashed
    ? new THREE.LineDashedMaterial({ color, transparent: true, opacity, dashSize: dash, gapSize: gap })
    : new THREE.LineBasicMaterial({ color, transparent: true, opacity });
  const l = new THREE.Line(geo, mat);
  if (dashed) l.computeLineDistances();
  return l;
}

/* A glowing cable along a smooth curve through `points`. Returns the curve
 * too, so packets can ride it with curve.getPointAt(t). */
export function tube(points, radius, color, opacity = 0.55) {
  const curve = new THREE.CatmullRomCurve3(points.map((p) => new THREE.Vector3(...p)));
  const mesh = new THREE.Mesh(
    new THREE.TubeGeometry(curve, 96, radius, 10, false),
    new THREE.MeshStandardMaterial({
      color, emissive: color, emissiveIntensity: 0.7, transparent: true, opacity, depthWrite: false,
    }),
  );
  return { mesh, curve };
}

/*
 * Text label pinned to a 3D point, rendered as real HTML text by the
 * engine's CSS2DRenderer - NOT drawn into the 3D scene. That matters:
 * anything drawn into the scene goes through the bloom pass, so text used
 * to get blurred and drowned by glowing objects behind it. HTML text sits
 * on top of the finished frame, sharp, on a dark backing plate.
 *
 * `height` (in the old world-unit scale) picks the font size, so call
 * sites keep reading as "big title" vs "small tag".
 */
export function label(text, { height = 0.42, color = '#e6f2ff', weight = 600, sub = null, subColor = '#9db3c8' } = {}) {
  const el = document.createElement('div');
  el.className = 'lbl';
  const px = Math.max(10, Math.min(19, Math.round(8 + height * 10.5)));
  el.style.fontSize = `${px}px`;
  el.style.fontWeight = String(weight);
  el.style.color = color;
  el.style.borderColor = `${color}55`;
  const main = document.createElement('div');
  main.className = 'lbl-main';
  main.textContent = text;
  el.appendChild(main);
  if (sub) {
    const s = document.createElement('div');
    s.className = 'lbl-sub';
    s.style.color = subColor;
    s.textContent = sub;
    el.appendChild(s);
  }
  return new CSS2DObject(el);
}

export function setLabelText(lbl, text) {
  lbl.element.querySelector('.lbl-main').textContent = text;
}

/* Make an HTML label clickable (labels sit above the canvas, so the 3D
 * picking ray never sees them). */
export function onLabelClick(lbl, handler) {
  lbl.element.classList.add('clickable');
  lbl.element.addEventListener('click', (e) => {
    e.stopPropagation();
    handler();
  });
}

export function canvasTexture(w, h, draw) {
  const canvas = document.createElement('canvas');
  canvas.width = w;
  canvas.height = h;
  draw(canvas.getContext('2d'), w, h);
  const tex = new THREE.CanvasTexture(canvas);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 4;
  return tex;
}

/* Deterministic pseudo-random so textures look the same every load. */
function rng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (s * 1664525 + 1013904223) >>> 0;
    return s / 4294967296;
  };
}

/* A dark-teal printed circuit board with copper traces and vias. */
export function pcbTexture(seed = 7) {
  return canvasTexture(1024, 1024, (ctx, w, h) => {
    const r = rng(seed);
    ctx.fillStyle = '#06201f';
    ctx.fillRect(0, 0, w, h);
    ctx.lineCap = 'round';
    for (let i = 0; i < 260; i++) {
      let x = r() * w;
      let y = r() * h;
      ctx.strokeStyle = r() < 0.15 ? 'rgba(80,255,220,0.55)' : 'rgba(40,160,140,0.35)';
      ctx.lineWidth = r() < 0.2 ? 3 : 1.5;
      ctx.beginPath();
      ctx.moveTo(x, y);
      for (let s = 0; s < 4; s++) {
        const len = 30 + r() * 120;
        const dir = Math.floor(r() * 4);
        if (dir === 0) x += len; else if (dir === 1) x -= len; else if (dir === 2) y += len; else y -= len;
        ctx.lineTo(x, y);
      }
      ctx.stroke();
      ctx.fillStyle = 'rgba(120,255,230,0.6)';
      ctx.beginPath();
      ctx.arc(x, y, 3, 0, Math.PI * 2);
      ctx.fill();
    }
  });
}

/* Silicon die: a faint grid of standard cells. */
export function siliconTexture(seed = 3) {
  return canvasTexture(512, 512, (ctx, w, h) => {
    const r = rng(seed);
    ctx.fillStyle = '#0c1220';
    ctx.fillRect(0, 0, w, h);
    for (let i = 0; i < 1400; i++) {
      ctx.fillStyle = `rgba(${90 + r() * 60},${120 + r() * 80},255,${0.05 + r() * 0.12})`;
      ctx.fillRect(Math.floor(r() * 64) * 8, Math.floor(r() * 64) * 8, 7, 3 + r() * 4);
    }
  });
}

/* Mark an object as clickable. The engine walks up from whatever mesh the
 * mouse hit until it finds userData.pick. */
export function pickable(obj, pick) {
  obj.userData.pick = pick;
  return obj;
}

/* Recursively set opacity on everything in a group (for fade in/out).
 * HTML labels only show while their group is close to fully visible -
 * half-faded layers behind the one you're looking at shouldn't keep
 * their text on screen. */
export function setGroupOpacity(group, opacity) {
  group.traverse((o) => {
    if (o.isCSS2DObject) {
      o.visible = opacity > 0.7;
      o.element.style.opacity = String(Math.min(1, opacity));
      return;
    }
    const mats = o.material ? (Array.isArray(o.material) ? o.material : [o.material]) : [];
    for (const m of mats) {
      if (m.userData.baseOpacity === undefined) m.userData.baseOpacity = m.opacity;
      m.transparent = true;
      m.opacity = m.userData.baseOpacity * opacity;
    }
  });
  group.visible = opacity > 0.01;
}
