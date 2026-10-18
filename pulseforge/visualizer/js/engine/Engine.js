/*
 * Engine.js - owns the WebGL renderer, the camera, the bloom pass, the
 * render loop and mouse picking. Knows nothing about PulseForge: it just
 * renders whichever View is active and forwards clicks/hovers to it.
 *
 * The render loop, once per frame:
 *   tickAnimations()  -> camera fly-throughs, layer peels (tween.js)
 *   view.update()     -> the active view animates its packets etc.
 *   controls.update() -> orbit damping
 *   render            -> through the bloom composer if it loaded
 */
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { CSS2DRenderer } from 'three/addons/renderers/CSS2DRenderer.js';
import { animate, tickAnimations, ease } from './tween.js';

export class Engine {
  constructor(container) {
    this.container = container;
    this.renderer = new THREE.WebGLRenderer({ antialias: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.setSize(window.innerWidth, window.innerHeight);
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = 1.05;
    container.appendChild(this.renderer.domElement);

    /* Text labels: a second renderer that positions HTML elements over
     * the canvas (see factory.label). pointer-events:none on the layer so
     * orbit drags and 3D clicks pass through to the canvas underneath. */
    this.labelRenderer = new CSS2DRenderer();
    this.labelRenderer.setSize(window.innerWidth, window.innerHeight);
    this.labelRenderer.domElement.className = 'label-layer';
    container.appendChild(this.labelRenderer.domElement);

    this.camera = new THREE.PerspectiveCamera(48, window.innerWidth / window.innerHeight, 0.05, 600);
    this.camera.position.set(0, 12, 34);

    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enableDamping = true;
    this.controls.dampingFactor = 0.08;
    this.controls.maxPolarAngle = Math.PI * 0.49; /* never go under the floor */

    this.view = null;
    this.composer = null;
    this.renderPass = null;
    this.bloomPass = null;
    this.flight = null;
    this.onHover = null; /* (pick, event) => void, set by main.js for the tooltip */

    this.raycaster = new THREE.Raycaster();
    this.pointer = new THREE.Vector2();
    this.hovered = null;
    this.downAt = null;

    this._bindEvents();
    this._setupBloom();
    window.addEventListener('resize', () => this._resize());

    this.clock = new THREE.Clock();
    const loop = () => {
      requestAnimationFrame(loop);
      this._frame();
    };
    loop();
  }

  /*
   * Bloom is loaded with a dynamic import inside try/catch: if it fails
   * (CDN hiccup), the scene still renders, just without glow. A failing
   * STATIC import would take the whole module - and the whole page - down.
   */
  async _setupBloom() {
    try {
      const base = 'three/addons/postprocessing/';
      const [{ EffectComposer }, { RenderPass }, { UnrealBloomPass }, { OutputPass }] = await Promise.all([
        import(base + 'EffectComposer.js'),
        import(base + 'RenderPass.js'),
        import(base + 'UnrealBloomPass.js'),
        import(base + 'OutputPass.js'),
      ]);
      const size = new THREE.Vector2(window.innerWidth, window.innerHeight);
      this.composer = new EffectComposer(this.renderer);
      this.renderPass = new RenderPass(this.view ? this.view.scene : new THREE.Scene(), this.camera);
      this.composer.addPass(this.renderPass);
      this.bloomPass = new UnrealBloomPass(size, 0.75, 0.5, 0.32);
      this.composer.addPass(this.bloomPass);
      this.composer.addPass(new OutputPass());
    } catch (e) {
      console.warn('[PulseForge] bloom unavailable, rendering without it:', e.message);
    }
  }

  setView(view) {
    /* Each view has its own scene; the previous scene's label elements
     * would otherwise stay stuck on screen. (The renderer re-attaches
     * them if that view becomes active again.) */
    if (this.view && this.view !== view) {
      this.view.scene.traverse((o) => {
        if (o.isCSS2DObject && o.element.parentNode) o.element.remove();
      });
    }
    this.view = view;
    if (this.renderPass) this.renderPass.scene = view.scene;
    this._setHovered(null, null);
  }

  /* Smoothly move the camera and its orbit target. */
  flyTo(pos, target, ms = 1300) {
    if (this.flight) this.flight.cancel();
    const fromPos = this.camera.position.clone();
    const fromTarget = this.controls.target.clone();
    const toPos = new THREE.Vector3(...pos);
    const toTarget = new THREE.Vector3(...target);
    this.flight = animate(ms, (t) => {
      this.camera.position.lerpVectors(fromPos, toPos, t);
      this.controls.target.lerpVectors(fromTarget, toTarget, t);
    }, { easing: ease.inOutCubic, onComplete: () => { this.flight = null; } });
  }

  /* First visible hit (walking up to an ancestor with userData.pick). */
  pickAt(clientX, clientY) {
    if (!this.view) return null;
    const rect = this.renderer.domElement.getBoundingClientRect();
    this.pointer.x = ((clientX - rect.left) / rect.width) * 2 - 1;
    this.pointer.y = -((clientY - rect.top) / rect.height) * 2 + 1;
    this.raycaster.setFromCamera(this.pointer, this.camera);
    const hits = this.raycaster.intersectObjects(this.view.pickables, true);
    for (const hit of hits) {
      let o = hit.object;
      let visible = true;
      let pick = null;
      while (o) {
        if (!o.visible) visible = false;
        if (!pick && o.userData && o.userData.pick) pick = o.userData.pick;
        o = o.parent;
      }
      if (visible && pick) return { pick, hit };
    }
    return null;
  }

  _bindEvents() {
    const el = this.renderer.domElement;
    el.addEventListener('pointerdown', (e) => { this.downAt = [e.clientX, e.clientY]; });
    el.addEventListener('pointerup', (e) => {
      if (!this.downAt) return;
      const moved = Math.hypot(e.clientX - this.downAt[0], e.clientY - this.downAt[1]);
      this.downAt = null;
      if (moved > 6) return; /* that was an orbit drag, not a click */
      const result = this.pickAt(e.clientX, e.clientY);
      if (this.view) this.view.onPick(result ? result.pick : null, result ? result.hit : null);
    });
    el.addEventListener('pointermove', (e) => {
      const result = this.pickAt(e.clientX, e.clientY);
      this._setHovered(result ? result.pick : null, e);
    });
    el.addEventListener('pointerleave', () => this._setHovered(null, null));
  }

  _setHovered(pick, event) {
    if (pick !== this.hovered && this.view) this.view.onHover(pick);
    this.hovered = pick;
    this.renderer.domElement.style.cursor = pick ? 'pointer' : 'grab';
    if (this.onHover) this.onHover(pick, event);
  }

  _resize() {
    const w = window.innerWidth;
    const h = window.innerHeight;
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
    this.renderer.setSize(w, h);
    this.labelRenderer.setSize(w, h);
    if (this.composer) {
      this.composer.setSize(w, h);
      this.bloomPass.setSize(w, h);
    }
  }

  _frame() {
    const now = performance.now();
    /* capped so a backgrounded tab doesn't jump animations forward by seconds */
    const dt = Math.min(0.1, this.clock.getDelta());
    const t = this.clock.elapsedTime;
    tickAnimations(now);
    if (this.view) this.view.update(dt, t);
    this.controls.update();
    if (!this.view) return;
    if (this.composer) this.composer.render();
    else this.renderer.render(this.view.scene, this.camera);
    this._renderLabels();
  }

  /*
   * CSS2DRenderer (three r160) only checks a label's OWN `visible` flag,
   * not its parents', so a label inside a hidden group would still show.
   * Temporarily fold ancestor visibility into each label, render, restore.
   */
  _renderLabels() {
    const touched = [];
    const walk = (obj, parentVisible) => {
      const effective = parentVisible && obj.visible;
      if (obj.isCSS2DObject && obj.visible && !effective) {
        obj.visible = false;
        touched.push(obj);
      }
      for (const child of obj.children) walk(child, effective);
    };
    walk(this.view.scene, true);
    this.labelRenderer.render(this.view.scene, this.camera);
    for (const o of touched) o.visible = true;
  }
}
