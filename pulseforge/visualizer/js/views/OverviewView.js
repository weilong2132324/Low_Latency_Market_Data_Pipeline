/*
 * OverviewView.js - the top level: three machines, the cables between
 * them, and live ticks flowing through the pipeline.
 *
 * Click targets (registered with addPickable):
 *   machine chassis  -> opens MachineView (hardware drill-down)
 *   cable / badge    -> opens NetworkStackView (protocol drill-down)
 *   software module  -> shows that component's lesson
 *   queue slot       -> the ring-buffer lesson + that slot's state
 *   worker sphere    -> that one worker's own latency numbers
 *
 * Every animation is driven by real events from the FeedHub: a 'received'
 * event sends a packet down the feed cable into a queue slot, a 'processed'
 * event carries it from that slot to the worker that consumed it.
 */
import * as THREE from 'three';
import { View } from './View.js';
import { COLORS, glow, glass, box, outline, tube, label, onLabelClick } from '../engine/factory.js';

const ROLE = {
  A: { x: -13.5, width: 8, color: COLORS.neutral, css: '#b8c4d4', name: 'MACHINE A', sub: 'EXCHANGE GATEWAY', lesson: 'machine-A' },
  B: { x: 0, width: 12.5, color: COLORS.hot, css: '#ff7a5c', name: 'MACHINE B', sub: 'TRADING RECEIVER · hot path', lesson: 'machine-B' },
  C: { x: 13.5, width: 8, color: COLORS.cold, css: '#4feaff', name: 'MACHINE C', sub: 'MONITORING HOST · cold path', lesson: 'machine-C' },
};
const SLOTS = 16;
const COMP_Y = 2.75;

export class OverviewView extends View {
  constructor(app) {
    super(app, 'Overview');
    this.addAtmosphere();
    this.dilation = 650; /* ms a packet takes per cable */
    this.spawnCap = 45; /* packets/s drawn, so a fast feed stays readable */
    this.spawnTokens = 0;
    this.packets = [];
    this.leds = [];
    this.workerStats = {};
    this.workerMeshes = [];
    this.selected = null;
    this.slotHeat = new Array(SLOTS).fill(0);
    this.flashes = [];
    this.lastLiveRefresh = 0;

    this.packetGeo = new THREE.SphereGeometry(0.1, 12, 12);
    this.packetMats = {
      tick: glow(0xffffff, 1.6),
      processed: glow(0xffb08a, 1.4),
      gap: glow(COLORS.bad, 1.8),
      recovered: glow(COLORS.cold, 1.8),
      telemetry: glow(0x7ff6ff, 1.2),
    };

    for (const role of ['A', 'B', 'C']) this._buildMachine(role);
    this._buildSoftware();
    this._buildLinks();

    app.feed.on('events', (evs) => this.active && this._onEvents(evs));
    app.feed.on('config', (cfg) => this._buildWorkers(cfg.workers || 3));
    app.feed.on('stats', () => this.active && this._refreshSelectedLive());
  }

  /* ----------------------------------------------------------- scene */

  _buildMachine(role) {
    const R = ROLE[role];
    const group = new THREE.Group();
    group.position.x = R.x;
    this.scene.add(group);

    /* chassis: translucent body, glowing edges, front bezel with LEDs */
    const chassis = box(R.width, 1.5, 5, glass(R.color, 0.14));
    chassis.position.y = 0.95;
    outline(chassis, R.color, 0.85);
    group.add(chassis);
    const bezel = box(R.width * 0.96, 0.14, 0.06, glow(R.color, 0.9));
    bezel.position.set(0, 0.55, 2.52);
    group.add(bezel);
    for (let i = 0; i < 10; i++) {
      const led = box(0.12, 0.12, 0.05, glow(i % 3 ? COLORS.ok : R.color, 1));
      led.position.set(-R.width / 2 + 0.6 + i * 0.32, 1.25, 2.53);
      led.userData.phase = Math.random() * 10;
      group.add(led);
      this.leds.push(led);
    }
    /* drive-bay slats on the front */
    for (let i = 0; i < 4; i++) {
      const bay = box(1.1, 0.5, 0.04, glass(R.color, 0.25));
      bay.position.set(R.width / 2 - 0.9 - i * 1.25, 1.05, 2.53);
      outline(bay, R.color, 0.4);
      group.add(bay);
    }

    /* pedestal ring */
    const ring = new THREE.Mesh(
      new THREE.RingGeometry(R.width * 0.62, R.width * 0.62 + 0.08, 96),
      new THREE.MeshBasicMaterial({ color: R.color, transparent: true, opacity: 0.55, side: THREE.DoubleSide }),
    );
    ring.rotation.x = -Math.PI / 2;
    ring.position.y = 0.02;
    group.add(ring);
    const disc = new THREE.Mesh(
      new THREE.CircleGeometry(R.width * 0.62, 96),
      new THREE.MeshBasicMaterial({ color: R.color, transparent: true, opacity: 0.05, side: THREE.DoubleSide }),
    );
    disc.rotation.x = -Math.PI / 2;
    disc.position.y = 0.01;
    group.add(disc);
    this[`ring${role}`] = ring;

    const title = label(R.name, { height: 1.05, color: R.css, weight: 700, sub: R.sub });
    title.position.set(0, 7.3, -0.5);
    group.add(title);

    this.addPickable(chassis, { kind: 'machine', role, tip: `${R.name} — ${R.sub}\nclick to open it up` });
  }

  _module(x, w, color, text, pick) {
    const m = box(w, 0.85, 1.3, glow(color, 0.55));
    m.position.set(x, COMP_Y, 0);
    outline(m, 0xffffff, 0.35);
    this.scene.add(m);
    const l = label(text, { height: 0.5 });
    l.position.set(x, COMP_Y + 1.05, 0);
    this.scene.add(l);
    this.addPickable(m, pick);
    return m;
  }

  _buildSoftware() {
    this.publisher = this._module(ROLE.A.x, 2.6, COLORS.neutral, 'publisher', { kind: 'comp', id: 'comp-publisher', tip: 'publisher process' });
    this.receiver = this._module(-4.4, 2.0, COLORS.hot, 'receiver', { kind: 'comp', id: 'comp-receiver', tip: 'receiver loop (single producer)' });
    this.telemetry = this._module(ROLE.C.x, 2.6, COLORS.cold, 'telemetry_bridge', { kind: 'comp', id: 'comp-telemetry', tip: 'telemetry bridge' });

    /* the queue as what it really is: a ring of slots */
    this.ringCenter = new THREE.Vector3(0, COMP_Y + 0.15, 0);
    this.slots = [];
    const radius = 1.15;
    for (let i = 0; i < SLOTS; i++) {
      const a = (i / SLOTS) * Math.PI * 2 + Math.PI / 2;
      const slot = box(0.3, 0.3, 0.3, glow(COLORS.hot, 0.05));
      slot.position.set(Math.cos(a) * radius, COMP_Y + 0.15 + Math.sin(a) * radius, 0);
      slot.rotation.z = a;
      outline(slot, COLORS.hot, 0.5);
      this.scene.add(slot);
      this.slots.push(slot);
      this.addPickable(slot, { kind: 'slot', index: i, tip: `queue slot ${i} — click for the ring-buffer lesson` });
    }
    const hub = new THREE.Mesh(new THREE.TorusGeometry(radius, 0.025, 8, 96), glow(COLORS.hot, 0.6));
    hub.position.copy(this.ringCenter);
    this.scene.add(hub);
    const ql = label('queue (ring buffer)', { height: 0.5 });
    ql.position.set(0, COMP_Y + 1.85, 0);
    this.scene.add(ql);
    this.addPickable(hub, { kind: 'comp', id: 'comp-queue', tip: 'queue backend' });

    this.workerLabel = label('workers', { height: 0.5 });
    this.workerLabel.position.set(4.4, COMP_Y + 1.3, 0);
    this.scene.add(this.workerLabel);
    this._buildWorkers(3);
  }

  _buildWorkers(n) {
    n = Math.max(1, Math.min(8, n));
    if (this.workerMeshes.length === n) return;
    for (const w of this.workerMeshes) {
      this.scene.remove(w);
      this.pickables.splice(this.pickables.indexOf(w), 1);
    }
    this.workerMeshes = [];
    for (let i = 0; i < n; i++) {
      const s = new THREE.Mesh(new THREE.SphereGeometry(0.28, 20, 20), glow(COLORS.hot, 0.6));
      s.position.set(4.4, COMP_Y, (i - (n - 1) / 2) * 0.72);
      this.scene.add(s);
      this.workerMeshes.push(s);
      this.addPickable(s, { kind: 'worker', id: i, tip: `worker ${i} — click for this thread's own latency` });
    }
  }

  _buildLinks() {
    const xa = ROLE.A.x;
    const xc = ROLE.C.x;
    this.links = {};
    const make = (id, points, color, radius, badgeText, sub) => {
      const t = tube(points, radius, color, 0.55);
      this.scene.add(t.mesh);
      this.addPickable(t.mesh, { kind: 'link', id, tip: `${badgeText}\nclick to see its protocol stack` });
      const mid = t.curve.getPointAt(0.5);
      const badge = label(`${badgeText} ▸`, { height: 0.32, color: '#ffffff' });
      badge.element.title = sub;
      badge.position.set(mid.x, mid.y + 0.45, mid.z);
      this.scene.add(badge);
      onLabelClick(badge, () => this.active && this.onPick({ kind: 'link', id }));
      this.links[id] = t;
    };
    make('feed', [[xa + 1.35, COMP_Y, 0], [xa + 3.8, 5.4, 0], [-7.4, 5.4, 0], [-5.4, COMP_Y, 0]], 0xffd0b0, 0.08,
      'UDP MULTICAST FEED', 'click → protocol stack');
    make('recovery', [[-4.6, COMP_Y - 0.35, 0.6], [-6.8, 0.9, 3.6], [xa + 4.2, 0.9, 3.6], [xa + 0.9, COMP_Y - 0.35, 0.6]], COLORS.cold, 0.06,
      'TCP GAP RECOVERY', 'B → A');
    make('telemetry', [[4.9, COMP_Y, 0], [7.0, 5.0, 0], [xc - 3.8, 5.0, 0], [xc - 1.35, COMP_Y, 0]], 0x7ff6ff, 0.06,
      'WEBSOCKET TELEMETRY', 'B → C → you');
  }

  /* ------------------------------------------------------- lifecycle */

  enter() {
    /* target shifted left so the scene centres in the gap between the side panels */
    this.app.engine.flyTo([-1.4, 15, 39], [-1.4, 2.4, 0], 1400);
    this.app.hud.showLesson('overview', { live: this._overviewLive() });
    this.app.hud.setControls([
      { type: 'buttons', label: 'Open', items: [
        { label: 'Machine A', onClick: () => this.app.open.machine('A') },
        { label: 'Machine B', onClick: () => this.app.open.machine('B') },
        { label: 'Machine C', onClick: () => this.app.open.machine('C') },
      ] },
      { type: 'buttons', label: 'Stack', items: [
        { label: 'Feed (UDP)', onClick: () => this.app.open.link('feed') },
        { label: 'Recovery (TCP)', onClick: () => this.app.open.link('recovery') },
        { label: 'Telemetry (WS)', onClick: () => this.app.open.link('telemetry') },
      ] },
      { type: 'slider', label: 'Slow-mo', min: 200, max: 2500, step: 50, value: this.dilation, fmt: (v) => `${v} ms/hop`, onInput: (v) => { this.dilation = v; } },
      { type: 'slider', label: 'Packets drawn', min: 5, max: 120, value: this.spawnCap, fmt: (v) => `${v}/s`, onInput: (v) => { this.spawnCap = v; } },
    ]);
  }

  _overviewLive() {
    const c = this.app.feed.lastConfig;
    const s = this.app.feed.lastStats;
    const rows = [['data source', this.app.feed.source.toUpperCase(), this.app.feed.source === 'live' ? 'ok' : 'warn']];
    if (c) rows.push(['feed', c.transport === 'multicast' ? `multicast ${c.multicast_group}:${c.port}` : `unicast :${c.port}`]);
    if (c) rows.push(['queue', `${c.queue_kind}, ${c.workers} worker(s)`]);
    if (s) rows.push(['ticks processed', s.processed.toLocaleString()]);
    if (s) rows.push(['avg / max latency', `${(s.avg_latency_ns / 1000).toFixed(1)} / ${(s.max_latency_ns / 1000).toFixed(1)} µs`]);
    return rows;
  }

  onHover(pick) {
    for (const r of ['A', 'B', 'C']) this[`ring${r}`].material.opacity = 0.55;
    if (pick && pick.kind === 'machine') this[`ring${pick.role}`].material.opacity = 1.0;
  }

  onPick(pick) {
    if (!pick) return;
    if (pick.kind === 'machine') return this.app.open.machine(pick.role);
    if (pick.kind === 'link') return this.app.open.link(pick.id);
    this.selected = pick;
    this._showSelected();
  }

  _showSelected() {
    const p = this.selected;
    if (p.kind === 'comp') this.app.hud.showLesson(p.id, { live: this._liveFor(p) });
    if (p.kind === 'slot') this.app.hud.showLesson('comp-queue', { live: this._liveFor(p) });
    if (p.kind === 'worker') this.app.hud.showLesson('comp-workers', { live: this._liveFor(p) });
  }

  _liveFor(p) {
    const s = this.app.feed.lastStats || {};
    const c = this.app.feed.lastConfig || {};
    const us = (ns) => `${((ns || 0) / 1000).toFixed(1)} µs`;
    if (p.kind === 'worker') {
      const w = this.workerStats[p.id];
      return [
        ['worker', `#${p.id} of ${this.workerMeshes.length}`],
        ['processed (this thread)', w ? w.n.toLocaleString() : '0'],
        ['avg latency (this thread)', w ? us(w.total / w.n) : '-'],
        ['max latency (this thread)', w ? us(w.max) : '-'],
        ['pool avg (all workers)', us(s.avg_latency_ns)],
      ];
    }
    if (p.kind === 'slot') {
      return [
        ['slot', `${p.index} of ${SLOTS} drawn (real capacity 4096, rounded to a power of 2)`],
        ['heat (recent traffic)', this.slotHeat[p.index] > 0.15 ? 'tick in flight' : 'idle', this.slotHeat[p.index] > 0.15 ? 'hot' : ''],
        ['queue depth (whole ring)', s.queue_depth ?? '-'],
        ['times found full', s.queue_full ?? '-'],
        ['backend', c.queue_kind || '-'],
      ];
    }
    if (p.id === 'comp-receiver') {
      return [['received', s.received ?? '-'], ['gaps detected', s.gaps ?? '-'], ['recovered over TCP', s.recovered ?? '-'],
        ['recvmmsg batch', c.batch > 1 ? `x${c.batch}` : 'off'], ['SO_BUSY_POLL', c.busy_poll_us >= 0 ? `${c.busy_poll_us} µs` : 'off'],
        ['BPF filter', c.bpf_filter ? 'attached' : 'off']];
    }
    if (p.id === 'comp-queue') return [['backend', c.queue_kind || '-'], ['depth', s.queue_depth ?? '-'], ['found full', s.queue_full ?? '-']];
    if (p.id === 'comp-publisher') return [['transport', c.transport || '-'], ['group', c.multicast_group || '-'], ['port', c.port || '-']];
    return [['processed', s.processed ?? '-'], ['avg latency', us(s.avg_latency_ns)]];
  }

  _refreshSelectedLive() {
    const now = performance.now();
    if (now - this.lastLiveRefresh < 250) return;
    this.lastLiveRefresh = now;
    if (this.selected) this.app.hud.updateLive(this._liveFor(this.selected));
    else this.app.hud.updateLive(this._overviewLive());
  }

  /* -------------------------------------------------------- packets */

  _slotPos(i) {
    return this.slots[i % SLOTS].position.clone();
  }

  _spawn(kind, { curve, path, dur, reverse = false, onDone }) {
    if (this.packets.length > 260) return;
    const mesh = new THREE.Mesh(this.packetGeo, this.packetMats[kind]);
    this.scene.add(mesh);
    this.packets.push({ mesh, curve, path, dur, reverse, onDone, t0: performance.now() });
  }

  _onEvents(events) {
    for (const e of events) {
      if (e.type === 'processed') {
        const w = this.workerStats[e.worker_id] || (this.workerStats[e.worker_id] = { n: 0, total: 0, max: 0 });
        w.n++;
        w.total += e.latency_ns;
        if (e.latency_ns > w.max) w.max = e.latency_ns;
      }
      if (e.type === 'gap') this._flash(this.receiver, COLORS.bad);
      if (e.type === 'queue_full') this._flash(this.slots[0], COLORS.warn);

      if (this.spawnTokens < 1) continue; /* drawing is rate-limited; the stats above are not */
      this.spawnTokens -= 1;

      const slot = e.seq % SLOTS;
      if (e.type === 'received') {
        this._spawn('tick', {
          curve: this.links.feed.curve, dur: this.dilation,
          onDone: () => {
            this.slotHeat[slot] = 1;
            this._spawn('tick', { path: [this.receiver.position.clone(), this._slotPos(slot)], dur: this.dilation * 0.4 });
          },
        });
      } else if (e.type === 'processed') {
        const w = this.workerMeshes[e.worker_id % this.workerMeshes.length];
        const latencyScale = Math.min(2.5, Math.max(0.4, e.latency_ns / 40000));
        this._spawn('processed', {
          path: [this._slotPos(slot), new THREE.Vector3(2.6, COMP_Y + 0.3, 0), w.position.clone()],
          dur: this.dilation * 0.5 * latencyScale,
          onDone: () => {
            this.slotHeat[slot] *= 0.3;
            w.scale.setScalar(1.6);
            if (Math.random() < 0.25) this._spawn('telemetry', { curve: this.links.telemetry.curve, dur: this.dilation });
          },
        });
      } else if (e.type === 'gap') {
        this._spawn('gap', { curve: this.links.recovery.curve, dur: this.dilation * 1.2 });
      } else if (e.type === 'recovered') {
        this._spawn('recovered', { curve: this.links.recovery.curve, dur: this.dilation * 1.2, reverse: true });
      }
    }
  }

  _flash(mesh, color) {
    /* remember the true resting colour once, so overlapping flashes can't
     * leave the mesh stuck on a flash colour */
    if (!mesh.userData.restEmissive) mesh.userData.restEmissive = mesh.material.emissive.clone();
    this.flashes = this.flashes.filter((f) => f.mesh !== mesh);
    this.flashes.push({ mesh, color: new THREE.Color(color), t0: performance.now(), base: mesh.userData.restEmissive });
  }

  update(dt, t) {
    this.animateAtmosphere(t);
    this.spawnTokens = Math.min(this.spawnCap, this.spawnTokens + this.spawnCap * dt);
    const now = performance.now();

    for (let i = this.packets.length - 1; i >= 0; i--) {
      const p = this.packets[i];
      const u = Math.min(1, (now - p.t0) / p.dur);
      if (p.curve) {
        p.mesh.position.copy(p.curve.getPointAt(p.reverse ? 1 - u : u));
      } else {
        const seg = Math.min(p.path.length - 2, Math.floor(u * (p.path.length - 1)));
        const local = u * (p.path.length - 1) - seg;
        p.mesh.position.lerpVectors(p.path[seg], p.path[seg + 1], local);
      }
      if (u >= 1) {
        this.scene.remove(p.mesh);
        this.packets.splice(i, 1);
        if (p.onDone) p.onDone();
      }
    }

    for (let i = 0; i < SLOTS; i++) {
      this.slotHeat[i] *= Math.pow(0.25, dt);
      this.slots[i].material.emissiveIntensity = 0.05 + this.slotHeat[i] * 1.6;
    }
    for (const w of this.workerMeshes) w.scale.setScalar(THREE.MathUtils.lerp(w.scale.x, 1, 0.12));
    for (const led of this.leds) led.material.emissiveIntensity = 0.4 + (Math.sin(t * 6 + led.userData.phase * 3) > 0.6 ? 1.4 : 0);

    this.flashes = this.flashes.filter((f) => {
      const k = (now - f.t0) / 500;
      if (k >= 1) {
        f.mesh.material.emissive.copy(f.base);
        return false;
      }
      f.mesh.material.emissive.copy(f.base).lerp(f.color, 1 - k);
      return true;
    });
  }
}
