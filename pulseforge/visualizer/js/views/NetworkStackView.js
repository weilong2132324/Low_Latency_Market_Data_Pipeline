/*
 * NetworkStackView.js - one network link, opened up into its layers.
 *
 *   SENDER                                  RECEIVER
 *   L7 application  -- user space --        L7 application
 *   socket API      -- syscall line --      socket API
 *   L4 UDP/TCP                              L4 UDP/TCP
 *   L3 IP                                   L3 IP
 *   L2 Ethernet                             L2 Ethernet
 *   driver / NAPI   -- kernel / hw line --  driver / NAPI
 *   NIC (DMA)                               NIC (DMA)
 *   L1 PHY  ========== wire + switch ====== L1 PHY
 *
 * A packet capsule walks down the sender (each layer prepends its header -
 * encapsulation), crosses the wire as bits, and walks up the receiver (each
 * layer strips its header). The capsule's segment widths are the real byte
 * counts from protocols.js. KERNEL BYPASS reroutes it NIC <-> application
 * directly, which is exactly what DPDK / ef_vi / AF_XDP do.
 */
import * as THREE from 'three';
import { View } from './View.js';
import { COLORS, glow, glass, box, outline, line, label, setLabelText, tube } from '../engine/factory.js';
import {
  LINKS, IPV4_HEADER, ETHERNET_HEADER, frameBudget, serializationNs, multicastMac,
} from '../data/protocols.js';
import { lesson } from '../data/lessons.js';

const ROWS = [
  { id: 'app', name: 'L7 APPLICATION', color: 0xffb08a, lesson: 'net-app' },
  { id: 'socket', name: 'SOCKET API', color: COLORS.warn, lesson: 'net-socket' },
  { id: 'l4', name: 'L4 TRANSPORT', color: COLORS.ok, lesson: 'net-udp' },
  { id: 'ip', name: 'L3 NETWORK (IP)', color: COLORS.cold, lesson: 'net-ip' },
  { id: 'eth', name: 'L2 DATA LINK', color: COLORS.accent, lesson: 'net-eth' },
  { id: 'driver', name: 'DRIVER / NAPI', color: 0xd05cff, lesson: 'net-driver' },
  { id: 'nic', name: 'NIC (DMA rings)', color: COLORS.hot, lesson: 'net-nic' },
  { id: 'phy', name: 'L1 PHY', color: COLORS.bad, lesson: 'net-phy' },
];
const KERNEL_ROWS = ['socket', 'l4', 'ip', 'eth', 'driver'];
const SX = -5.6;
const RX = 5.6;
const SLAB_W = 6.4;
const rowY = (i) => 11 - i * 1.45;
const ROW_INDEX = Object.fromEntries(ROWS.map((r, i) => [r.id, i]));
const BYTE = 0.04; /* world units per byte in the capsule */
const MAX_DRAWN_BYTES = 110; /* a 354-byte JSON payload is drawn capped (and labelled with its true size) */
const CAPSULE_Z = 2.15;

/* Capsule segments, front of the frame first. */
const SEGMENTS = [
  { id: 'eth', short: 'ETH', color: COLORS.accent },
  { id: 'ip', short: 'IP', color: COLORS.cold },
  { id: 'l4', short: 'L4', color: COLORS.ok },
  { id: 'payload', short: 'DATA', color: 0xffb08a },
  { id: 'fcs', short: 'FCS', color: 0x5a3cff },
];

export class NetworkStackView extends View {
  constructor(app, linkId) {
    super(app, 'Protocol stack');
    this.addAtmosphere({ fogDensity: 0.008, floorY: -1.6 });
    this.bypass = false;
    this.playing = true;
    this.speed = 1;
    this.slabs = { sender: {}, receiver: {} };
    this.badges = [];
    this.bits = [];

    this._buildStacks();
    this._buildWire();
    this._buildCapsule();
    this.setLink(linkId, false);

    app.feed.on('config', () => this.active && this._buildBadges());
  }

  /* ============================================================ build */

  _buildStacks() {
    for (const [side, x] of [['sender', SX], ['receiver', RX]]) {
      ROWS.forEach((row, i) => {
        const slab = box(SLAB_W, 1.0, 3.0, glass(row.color, 0.2));
        slab.position.set(x, rowY(i), 0);
        const edge = outline(slab, row.color, 0.85);
        this.scene.add(slab);
        this.slabs[side][row.id] = { slab, edge, row };
        this.addPickable(slab, { kind: 'layer', row: row.id, side, tip: `${row.name} (${side}) — click for its bytes` });
      });
    }
    /* row names once, in the corridor between the two stacks */
    ROWS.forEach((row, i) => {
      const l = label(row.name, { height: 0.44, color: '#' + row.color.toString(16).padStart(6, '0') });
      l.position.set(0, rowY(i) + 0.05, 0);
      this.scene.add(l);
    });

    /* boundary tags sit at the outer left edge, clear of the row names */
    const boundary = (y, text, color) => {
      this.scene.add(line([[SX - 4, y, 1.6], [RX + 4, y, 1.6]], color, { dashed: true, opacity: 0.9, dash: 0.35, gap: 0.2 }));
      const l = label(text, { height: 0.22, color: '#' + color.toString(16).padStart(6, '0') });
      l.position.set(SX - SLAB_W / 2 - 1.2, y, 1.7);
      this.scene.add(l);
    };
    boundary((rowY(0) + rowY(1)) / 2, '▲ user │ kernel ▼', COLORS.warn);
    boundary((rowY(5) + rowY(6)) / 2, '▲ kernel │ hw ▼', COLORS.hot);

    this.senderTitle = null;
    this.receiverTitle = null;
  }

  _buildWire() {
    const y = rowY(ROW_INDEX.phy);
    this.switchPos = new THREE.Vector3(0, y - 2.0, 0.6);
    const sw = box(3.2, 0.7, 1.8, glow(0x1a2233, 0.35));
    sw.position.copy(this.switchPos);
    outline(sw, COLORS.cold, 0.9);
    for (let p = 0; p < 8; p++) {
      const led = box(0.12, 0.12, 0.05, glow(COLORS.ok, 1.4));
      led.position.set(-1.2 + p * 0.34, 0.1, 0.92);
      sw.add(led);
    }
    this.scene.add(sw);
    this.addPickable(sw, { kind: 'switch', tip: 'the switch between the two machines' });
    const swl = label('SWITCH', { height: 0.34, sub: 'cut-through, IGMP snooping' });
    swl.position.set(0, this.switchPos.y - 0.9, 0.6);
    this.scene.add(swl);

    const w = tube([[SX + 2.5, y - 0.5, 0.6], [SX + 3.5, y - 1.8, 0.6], [-1.6, this.switchPos.y, 0.6],
      [1.6, this.switchPos.y, 0.6], [RX - 3.5, y - 1.8, 0.6], [RX - 2.5, y - 0.5, 0.6]], 0.06, 0xff6688, 0.6);
    this.wire = w;
    this.scene.add(w.mesh);
    this.addPickable(w.mesh, { kind: 'wire', tip: 'fibre / twinax: ~5 ns per metre' });
    const note = label('in this lab: loopback only - this is the path between two real machines', { height: 0.26, color: '#7f9bb3' });
    note.position.set(0, this.switchPos.y - 1.6, 0.6);
    this.scene.add(note);

    this.bitGeo = new THREE.SphereGeometry(0.06, 8, 8);
    this.bitMat = glow(0xff6688, 2.0);
  }

  _buildCapsule() {
    this.capsule = new THREE.Group();
    this.capsule.position.set(SX, rowY(0), CAPSULE_Z);
    this.scene.add(this.capsule);
    this.segs = SEGMENTS.map((s) => {
      /* modest emissive: at full intensity bloom turns every header white
       * and you can no longer see which layer added which bytes */
      const mesh = box(1, 0.48, 0.5, glow(s.color, 0.42));
      outline(mesh, 0xffffff, 0.6);
      const l = label(s.short, { height: 0.34, color: '#ffffff' });
      this.capsule.add(mesh);
      this.capsule.add(l);
      return { ...s, mesh, label: l, width: 0, target: 0 };
    });
  }

  /* ===================================================== link setup */

  setLink(linkId, refreshHud = true) {
    this.link = LINKS[linkId] || LINKS.feed;
    this.title = this.link.title;
    const sizes = {
      eth: ETHERNET_HEADER.size, ip: IPV4_HEADER.size, l4: this.link.l4Header.size,
      payload: this.link.app.size, fcs: ETHERNET_HEADER.trailer,
    };
    for (const s of this.segs) {
      s.bytes = sizes[s.id];
      let text = s.id === 'l4' ? this.link.l4.toUpperCase() : s.short;
      if (s.id === 'payload' && s.bytes > MAX_DRAWN_BYTES) text = `DATA ${s.bytes} B ▸`;
      setLabelText(s.label, text);
    }
    ROWS[ROW_INDEX.l4].lesson = this.link.l4 === 'tcp' ? 'net-tcp' : 'net-udp';

    if (this.senderTitle) this.scene.remove(this.senderTitle, this.receiverTitle);
    const ends = {
      feed: ['SENDER · Machine A', 'publisher → sendto()', 'RECEIVER · Machine B', 'recvmmsg() → receiver loop'],
      recovery: ['SENDER · Machine B', 'snapshot client (std::async)', 'RECEIVER · Machine A', 'snapshot server'],
      telemetry: ['SENDER · Machine C', 'telemetry_bridge → ws_server', 'RECEIVER · your browser', 'WebSocket onmessage'],
    }[this.link.id];
    this.senderTitle = label(ends[0], { height: 0.5, color: '#ffffff', weight: 700, sub: ends[1] });
    this.senderTitle.position.set(SX, rowY(0) + 1.5, 0);
    this.receiverTitle = label(ends[2], { height: 0.5, color: '#ffffff', weight: 700, sub: ends[3] });
    this.receiverTitle.position.set(RX, rowY(0) + 1.5, 0);
    this.scene.add(this.senderTitle, this.receiverTitle);

    this._buildBadges();
    this._restart();
    if (refreshHud) {
      this._showOverviewLesson();
      this._setControls();
      this.app.nav.refresh();
    }
  }

  _cfg() {
    return this.app.feed.lastConfig || {};
  }

  _buildBadges() {
    for (const b of this.badges) this.scene.remove(b);
    this.badges = [];
    const c = this._cfg();
    const add = (side, rowId, text, color = '#ffe27a') => {
      const l = label(text, { height: 0.24, color });
      l.position.set(side === 'sender' ? SX - SLAB_W / 2 - 1.5 : RX + SLAB_W / 2 + 1.5, rowY(ROW_INDEX[rowId]), 0.4);
      this.scene.add(l);
      this.badges.push(l);
    };
    const budget = frameBudget(this.link.app.size, this.link.l4Header.size);
    add('sender', 'app', `${this.link.app.size}-byte payload`);
    add('sender', 'phy', `${budget.onWire} B on the wire`, '#ff9ab0');
    add('receiver', 'phy', `${serializationNs(budget.onWire, 10).toFixed(1)} ns @10G`, '#ff9ab0');
    if (this.link.id === 'feed') {
      if (c.transport === 'multicast') {
        add('sender', 'ip', `→ ${c.multicast_group}`, '#7ff6ff');
        add('receiver', 'ip', `joined ${c.multicast_group}`, '#7ff6ff');
        add('sender', 'eth', multicastMac(c.multicast_group), '#c7b8ff');
      }
      if (c.batch > 1) add('receiver', 'socket', `recvmmsg x${c.batch}`);
      if (c.bpf_filter) add('receiver', 'socket', 'BPF filter attached', '#ffe27a');
      add('receiver', 'socket', 'SO_RCVBUF 4 MiB', '#ffe27a');
      if (c.busy_poll_us >= 0) add('receiver', 'driver', `SO_BUSY_POLL ${c.busy_poll_us} µs`, '#e7a6ff');
    }
    /* stack badges vertically if two land on the same row */
    const seen = {};
    for (const b of this.badges) {
      const key = `${b.position.x}|${b.position.y}`;
      seen[key] = (seen[key] || 0) + 1;
      b.position.y -= (seen[key] - 1) * 0.42;
    }
  }

  /* ================================================== packet journey */

  /*
   * The journey is a list of stops: { side, row, headers, caption }.
   * `headers` is the set of segments present on arrival.
   */
  _journey() {
    const l4 = this.link.l4.toUpperCase();
    const app = ['payload'];
    const withL4 = ['l4', 'payload'];
    const withIp = ['ip', 'l4', 'payload'];
    const frame = ['eth', 'ip', 'l4', 'payload', 'fcs'];
    if (this.bypass) {
      return [
        { side: 'sender', row: 'app', headers: app, caption: `application builds the ${this.link.app.size}-byte payload` },
        { side: 'sender', row: 'app', headers: frame, caption: 'KERNEL BYPASS: a userspace library builds the Ethernet/IP/L4 headers itself' },
        { side: 'sender', row: 'nic', headers: frame, caption: 'writes the frame straight into the NIC’s TX ring - no syscall, no kernel' },
        { side: 'sender', row: 'phy', headers: frame, caption: 'PHY serialises the frame onto the wire' },
        { wire: true, caption: 'bits on the wire → switch → receiver' },
        { side: 'receiver', row: 'phy', headers: frame, caption: 'PHY recovers the bits' },
        { side: 'receiver', row: 'nic', headers: frame, caption: 'NIC DMAs the frame into an RX ring mapped into the APPLICATION' },
        { side: 'receiver', row: 'app', headers: frame, caption: 'a spinning userspace thread sees the new descriptor - no interrupt, no syscall' },
        { side: 'receiver', row: 'app', headers: app, caption: 'the library parses Eth/IP/L4 in userspace and hands over the payload' },
      ];
    }
    return [
      { side: 'sender', row: 'app', headers: app, caption: `application builds the ${this.link.app.size}-byte payload` },
      { side: 'sender', row: 'socket', headers: app, caption: 'sendto(): syscall into the kernel, payload copied into a kernel buffer' },
      { side: 'sender', row: 'l4', headers: withL4, caption: `${l4} prepends its ${this.link.l4Header.size}-byte header (ports, length, checksum)` },
      { side: 'sender', row: 'ip', headers: withIp, caption: 'IP prepends 20 bytes: TTL, protocol, source and destination address' },
      { side: 'sender', row: 'eth', headers: frame, caption: 'Ethernet adds 14 bytes of MACs + EtherType, and a 4-byte FCS trailer' },
      { side: 'sender', row: 'driver', headers: frame, caption: 'driver posts the frame to the NIC’s TX ring' },
      { side: 'sender', row: 'nic', headers: frame, caption: 'NIC DMAs the frame out of host memory over PCIe' },
      { side: 'sender', row: 'phy', headers: frame, caption: 'PHY adds preamble + gap and serialises the bits' },
      { wire: true, caption: 'bits on the wire → switch → receiver' },
      { side: 'receiver', row: 'phy', headers: frame, caption: 'PHY recovers the bits, NIC checks the FCS' },
      { side: 'receiver', row: 'nic', headers: frame, caption: 'NIC DMAs the frame into an RX ring slot, raises an interrupt' },
      { side: 'receiver', row: 'driver', headers: frame, caption: 'interrupt → NAPI softirq polls the ring, builds an sk_buff' },
      { side: 'receiver', row: 'eth', headers: ['ip', 'l4', 'payload'], caption: 'Ethernet checked and stripped (EtherType 0x0800 → IPv4)' },
      { side: 'receiver', row: 'ip', headers: withL4, caption: 'IP checked and stripped (protocol field → UDP/TCP)' },
      { side: 'receiver', row: 'l4', headers: app, caption: `${l4} stripped; destination port picks the socket` },
      { side: 'receiver', row: 'socket', headers: app, caption: 'payload queued in the socket receive buffer - waiting for recvfrom()' },
      { side: 'receiver', row: 'app', headers: app, caption: 'recvfrom()/recvmmsg() copies it out: the application finally has its tick' },
    ];
  }

  _stopPos(stop) {
    return new THREE.Vector3(stop.side === 'sender' ? SX : RX, rowY(ROW_INDEX[stop.row]), CAPSULE_Z);
  }

  _restart() {
    this.stops = this._journey();
    this.stepIdx = 0;
    this.stepT = 0;
    this.pauseT = 0;
    this._enterStop(0);
    this.capsule.position.copy(this._stopPos(this.stops[0]));
    this.capsule.visible = true;
    this._applyBypassDim();
  }

  _enterStop(i) {
    const stop = this.stops[i];
    if (!stop.wire) {
      for (const s of this.segs) s.target = stop.headers.includes(s.id) ? Math.min(s.bytes, MAX_DRAWN_BYTES) * BYTE : 0;
    }
    this.app.hud.caption(`${i + 1}/${this.stops.length} · ${stop.caption}`);
    for (const side of ['sender', 'receiver']) {
      for (const id of Object.keys(this.slabs[side])) {
        const sl = this.slabs[side][id];
        const here = !stop.wire && stop.side === side && stop.row === id;
        sl.slab.material.emissiveIntensity = here ? 0.9 : 0.12;
        sl.edge.material.opacity = here ? 1 : (this.bypass && KERNEL_ROWS.includes(id) ? 0.2 : 0.75);
      }
    }
  }

  _applyBypassDim() {
    for (const side of ['sender', 'receiver']) {
      for (const id of KERNEL_ROWS) {
        const sl = this.slabs[side][id];
        sl.slab.material.opacity = this.bypass ? 0.05 : 0.2;
      }
    }
  }

  _spawnBits() {
    for (let i = 0; i < 26; i++) {
      const m = new THREE.Mesh(this.bitGeo, this.bitMat);
      this.scene.add(m);
      this.bits.push({ mesh: m, delay: i * 0.035 });
    }
    this.bitT = 0;
  }

  update(dt, t) {
    this.animateAtmosphere(t);
    const k = 1 - Math.pow(0.002, dt * this.speed);
    for (const s of this.segs) s.width += (s.target - s.width) * k;
    let total = 0;
    for (const s of this.segs) total += s.width;
    let x = -total / 2;
    for (const s of this.segs) {
      const w = Math.max(0.0001, s.width);
      s.mesh.scale.x = w;
      s.mesh.position.x = x + w / 2;
      s.mesh.visible = s.width > 0.02;
      s.label.position.set(x + w / 2, 0.5, 0);
      s.label.visible = s.width > 0.35;
      x += s.width;
    }

    if (!this.playing) return;
    const stop = this.stops[this.stepIdx];
    const next = this.stops[this.stepIdx + 1];

    if (stop.wire) {
      this.bitT += dt * this.speed;
      const dur = 1.4;
      for (const b of this.bits) {
        const u = Math.min(1, Math.max(0, (this.bitT - b.delay) / (dur - 0.9)));
        b.mesh.position.copy(this.wire.curve.getPointAt(u));
        b.mesh.visible = u > 0 && u < 1;
      }
      if (this.bitT >= dur) {
        for (const b of this.bits) this.scene.remove(b.mesh);
        this.bits = [];
        this.stepIdx++;
        this.capsule.visible = true;
        this.capsule.position.copy(this._stopPos(this.stops[this.stepIdx]));
        this._enterStop(this.stepIdx);
      }
      return;
    }

    if (this.pauseT > 0) {
      this.pauseT -= dt * this.speed;
      return;
    }
    if (!next) {
      this.pauseT = 0;
      this.stepT += dt * this.speed;
      if (this.stepT > 1.6) this._restart();
      return;
    }
    if (next.wire) {
      this.stepIdx++;
      this.capsule.visible = false;
      this._spawnBits();
      this._enterStop(this.stepIdx);
      return;
    }
    this.stepT += dt * this.speed;
    const moveDur = stop.row === next.row && stop.side === next.side ? 0.01 : 0.75;
    const u = Math.min(1, this.stepT / moveDur);
    const e = u < 0.5 ? 2 * u * u : 1 - Math.pow(-2 * u + 2, 2) / 2;
    this.capsule.position.lerpVectors(this._stopPos(stop), this._stopPos(next), e);
    if (u >= 1) {
      this.stepIdx++;
      this.stepT = 0;
      this.pauseT = 0.85; /* dwell so the header change is readable */
      this._enterStop(this.stepIdx);
    }
  }

  /* ========================================================= HUD */

  enter() {
    this.app.engine.flyTo([-2.0, 7.4, 28], [-2.0, 5.2, 0], 1400);
    this._showOverviewLesson();
    this._setControls();
    this._enterStop(this.stepIdx);
  }

  exit() {
    this.app.hud.caption('');
  }

  _budgetTable() {
    const b = frameBudget(this.link.app.size, this.link.l4Header.size);
    const ser = serializationNs(b.onWire, 10);
    return {
      title: 'Typical one-way latency budget, two machines, one switch',
      head: ['hop', 'kernel path', 'kernel bypass'],
      rows: [
        ['send syscall + copy', '~1 µs', '~0.1 µs (TX ring write)'],
        ['kernel L4/IP/Eth (TX)', '~1 µs', '0 - userspace builds headers'],
        ['NIC DMA + PCIe (TX)', '~0.5 µs', '~0.5 µs'],
        [`serialization @10G (${b.onWire} B)`, `${ser.toFixed(1)} ns`, `${ser.toFixed(1)} ns`],
        ['switch, cut-through', '~0.4 µs', '~0.4 µs'],
        ['2 m of fibre', '~10 ns', '~10 ns'],
        ['NIC DMA + PCIe (RX)', '~0.5 µs', '~0.5 µs'],
        ['interrupt + softirq + wakeup', '~3-5 µs', '0 - thread spin-polls'],
        ['kernel RX stack', '~1 µs', '~0.1 µs (userspace parse)'],
        ['recv syscall + copy', '~1 µs', '0'],
        { cells: ['TOTAL (roughly)', '~9-11 µs', '~2 µs'], cls: 'total' },
      ],
      note: 'Illustrative orders of magnitude for modern servers, not measurements. The wire, switch and serialization costs are identical on both paths - bypass only removes software.',
    };
  }

  _showOverviewLesson() {
    const b = frameBudget(this.link.app.size, this.link.l4Header.size);
    const base = lesson('net-stack');
    this.app.hud.showLesson({ ...base, title: `Encapsulation: how ${b.payload} bytes become ${b.onWire}` }, {
      live: [
        ['link', this.link.title],
        ['payload', `${b.payload} B`], ['headers + FCS', `${b.headers} B`], ['frame', `${b.frame} B`],
        ['on the wire (+preamble, gap)', `${b.onWire} B`],
        ['efficiency', `${((b.payload / b.onWire) * 100).toFixed(0)}% of the bits are your data`],
      ],
      table: this._budgetTable(),
    });
  }

  _setControls() {
    this.app.hud.setControls([
      { type: 'buttons', label: 'Link', items: Object.values(LINKS).map((L) => ({
        label: L.id === 'feed' ? 'Feed (UDP)' : L.id === 'recovery' ? 'Recovery (TCP)' : 'Telemetry (WS)',
        active: L.id === this.link.id,
        onClick: () => this.setLink(L.id),
      })) },
      { type: 'toggle', label: 'Kernel bypass', value: this.bypass, onChange: (v) => this._setBypass(v) },
      { type: 'buttons', items: [{ label: this.playing ? 'Pause' : 'Play', onClick: () => { this.playing = !this.playing; this._setControls(); } },
        { label: 'Restart', onClick: () => this._restart() }] },
      { type: 'slider', label: 'Speed', min: 0.25, max: 2.5, step: 0.25, value: this.speed, fmt: (v) => `${v}x`, onInput: (v) => { this.speed = v; } },
    ]);
  }

  _setBypass(on) {
    this.bypass = on;
    this._restart();
    this._setControls();
    if (on) this.app.hud.showLesson('net-bypass');
    else this._showOverviewLesson();
  }

  crumbs() {
    return [{ label: this.link.title }];
  }

  onPick(pick) {
    if (!pick) return;
    const hud = this.app.hud;
    const c = this._cfg();
    const ctx = { config: c, link: this.link };
    if (pick.kind === 'switch') return hud.showLesson('net-switch');
    if (pick.kind === 'wire') return this._phyLesson();
    if (pick.kind !== 'layer') return;
    const row = ROWS[ROW_INDEX[pick.row]];
    switch (pick.row) {
      case 'app':
        return hud.showLesson(this.link.id === 'telemetry' ? 'link-telemetry' : 'net-app', { header: { def: this.link.app, ctx } });
      case 'l4':
        return hud.showLesson(row.lesson, { header: { def: this.link.l4Header, ctx } });
      case 'ip':
        return hud.showLesson('net-ip', { header: { def: IPV4_HEADER, ctx } });
      case 'eth':
        return hud.showLesson('net-eth', { header: { def: ETHERNET_HEADER, ctx } });
      case 'socket':
        return hud.showLesson('net-socket', {
          live: [
            ['receive buffer', 'SO_RCVBUF = 4 MiB (src/receiver.cpp)'],
            ['batching', c.batch > 1 ? `recvmmsg, ${c.batch} datagrams per syscall` : 'off: one recvfrom() per datagram', c.batch > 1 ? 'ok' : ''],
            ['socket filter', c.bpf_filter ? 'classic BPF attached (drops quantity == 0)' : 'none', c.bpf_filter ? 'ok' : ''],
            ['side', pick.side],
          ],
        });
      case 'driver':
        return hud.showLesson('net-driver', {
          live: [['SO_BUSY_POLL', c.busy_poll_us >= 0 ? `${c.busy_poll_us} µs budget` : 'off (interrupt driven)', c.busy_poll_us >= 0 ? 'ok' : ''],
            ['your NIC driver', this.app.hw.nic.driver]],
        });
      case 'nic': {
        const n = this.app.hw.nic;
        return hud.showLesson('net-nic', {
          live: [['interface', n.name], ['driver', n.driver, /hv_netvsc|virtio/.test(n.driver) ? 'warn' : 'ok'],
            ['RX queues', String(n.rx_queues)], ['speed', n.speed_mbps > 0 ? `${n.speed_mbps} Mb/s` : 'n/a']],
        });
      }
      case 'phy':
        return this._phyLesson();
      default:
        return hud.showLesson(row.lesson);
    }
  }

  _phyLesson() {
    const b = frameBudget(this.link.app.size, this.link.l4Header.size);
    this.app.hud.showLesson('net-phy', {
      table: {
        title: `Serialization of this ${b.onWire}-byte frame`,
        head: ['link speed', 'time on the wire'],
        rows: [1, 10, 25, 100].map((g) => [`${g} Gb/s`, `${serializationNs(b.onWire, g).toFixed(1)} ns`]),
        note: '(frame + 8-byte preamble + 12-byte inter-frame gap) x 8 bits / link speed.',
      },
    });
  }
}
