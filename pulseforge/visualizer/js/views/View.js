/*
 * View.js - the contract every screen implements.
 *
 * A view owns its own THREE.Scene. The Navigator decides which view is on
 * screen; the Engine renders it and forwards input. To add a new drill-down
 * screen: subclass View, build your scene in the constructor, override the
 * hooks you need, and push it with app.nav.push(new YourView(app)).
 *
 *   enter()          became the active view: fly the camera, set HUD controls
 *   exit()           another view took over
 *   update(dt, t)    called every frame while active
 *   onPick(pick)     user clicked something you registered with addPickable()
 *   onHover(pick)    pointer moved onto / off a pickable (pick may be null)
 *   back()           Esc pressed: return true if you handled it internally
 *   crumbs()         breadcrumb entries for this view: [{ label, onClick? }]
 */
import * as THREE from 'three';

export class View {
  constructor(app, title) {
    this.app = app;
    this.title = title;
    this.scene = new THREE.Scene();
    this.pickables = [];
    this.active = false;
  }

  addPickable(obj, pick) {
    obj.userData.pick = pick;
    this.pickables.push(obj);
    return obj;
  }

  enter() {}
  exit() {}
  update() {}
  onPick() {}
  onHover() {}
  back() { return false; }
  crumbs() { return [{ label: this.title }]; }

  /* Standard lighting + fog so every view feels like the same world. */
  addAtmosphere({ fogDensity = 0.012, floorY = -0.02, floor = true } = {}) {
    this.scene.background = new THREE.Color(0x030509);
    this.scene.fog = new THREE.FogExp2(0x030509, fogDensity);
    this.scene.add(new THREE.AmbientLight(0x2a3a5a, 1.1));
    const key = new THREE.DirectionalLight(0xbfe8ff, 0.7);
    key.position.set(6, 16, 10);
    this.scene.add(key);
    this.rim = new THREE.PointLight(0x7c5cff, 3, 80, 2);
    this.rim.position.set(0, 8, -14);
    this.scene.add(this.rim);
    if (floor) {
      const grid = new THREE.GridHelper(160, 80, 0x113049, 0x0a1a28);
      grid.position.y = floorY;
      grid.material.transparent = true;
      grid.material.opacity = 0.45;
      this.scene.add(grid);
    }
    this.addStarfield();
  }

  /* Faint drifting particles - atmosphere, and a sense of scale. */
  addStarfield(count = 700) {
    const positions = new Float32Array(count * 3);
    for (let i = 0; i < count; i++) {
      positions[i * 3] = (Math.random() - 0.5) * 140;
      positions[i * 3 + 1] = Math.random() * 30;
      positions[i * 3 + 2] = (Math.random() - 0.5) * 90;
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    this.stars = new THREE.Points(geo, new THREE.PointsMaterial({ color: 0x4ab3ff, size: 0.06, transparent: true, opacity: 0.55 }));
    this.scene.add(this.stars);
  }

  animateAtmosphere(t) {
    if (this.stars) this.stars.rotation.y = t * 0.008;
    if (this.rim) this.rim.intensity = 2.6 + Math.sin(t * 0.8) * 0.7;
  }
}
