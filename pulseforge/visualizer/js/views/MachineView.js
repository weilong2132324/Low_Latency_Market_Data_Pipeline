/*
 * MachineView.js - open a machine up, one layer at a time.
 *
 *   level 0  chassis       a closed 1U server
 *   level 1  motherboard   lid off: CPU socket, DRAM, PCIe slot + NIC, NUMA node
 *   level 2  CPU die       the CPU lifts out: cores, L1/L2/L3, which thread runs where
 *   level 3  cache line    one core's L1d: a real PulseForge struct mapped
 *                          onto 64-byte lines, coloured by writer thread
 *
 * The cores and caches come from data/hw_topology.json (your machine, via
 * `make topology`) when present, otherwise a generic model.
 *
 * Every layer change is one tween over this.state = { lift, die, dim, line }
 * (0..1 each), so going back up runs the same animation in reverse.
 */
import * as THREE from 'three';
import { View } from './View.js';
import { animate, lerp } from '../engine/tween.js';
import {
  COLORS, glow, glass, box, outline, line, label, pcbTexture, siliconTexture, setGroupOpacity,
} from '../engine/factory.js';
import { fmtKb, placementFor } from '../data/hardware.js';
import {
  SHARED_STATS, SHARED_STATS_UNPADDED, SPSC_RING, tickMessageRing, eventRing, writersPerLine, CACHE_LINE,
} from '../data/layouts.js';

const LEVELS = [
  { label: 'Chassis', lesson: 'hw-chassis', cam: { pos: [-2.5, 10, 25], target: [-2.5, 1, 0] } },
  { label: 'Motherboard', lesson: 'hw-board', cam: { pos: [-3, 16, 16.5], target: [-3, 0, 0.5] } },
  { label: 'CPU die', lesson: 'hw-die', cam: { pos: [-5.4, 17.5, 13.5], target: [-5.4, 6, 0] } },
  { label: 'Cache line', lesson: null, cam: { pos: [-4.4, 35, 15], target: [-4.4, 14.5, 0.4] } },
];

const ROLE_NAMES = { A: 'Machine A — exchange gateway', B: 'Machine B — trading receiver', C: 'Machine C — monitoring host' };
const ROLE_COLORS = { A: COLORS.neutral, B: COLORS.hot, C: COLORS.cold };

const STRUCTS = {
  A: [{ label: 'TickMessage ring', def: tickMessageRing(8), lesson: 'hw-cacheline-A' }],
  B: [
    { label: 'SharedStats', def: SHARED_STATS, lesson: 'hw-cacheline' },
    { label: 'No padding (broken)', def: SHARED_STATS_UNPADDED, lesson: 'hw-cacheline' },
    { label: 'SpscRing', def: SPSC_RING, lesson: 'hw-cacheline' },
  ],
  C: [
    { label: 'Event ring', def: eventRing(6), lesson: 'hw-cacheline-C' },
    { label: 'SharedStats', def: SHARED_STATS, lesson: 'hw-cacheline' },
  ],
};

const SOCKET_POS = new THREE.Vector3(-3, 0.75, -0.6);
const DIE_POS = new THREE.Vector3(-3.2, 6, 0);
const LINE_POS = new THREE.Vector3(-3.2, 14.5, 0);

export class MachineView extends View {
  constructor(app, role) {
    super(app, `Machine ${role}`);
    this.role = role;
    this.level = 0;
    this.structIdx = 0;
    this.color = ROLE_COLORS[role];
    this.hw = app.hw;
    this.placement = placementFor(role, this.hw, (app.feed.lastConfig && app.feed.lastConfig.workers) || 3);
    this.state = { lift: 0, die: 0, dim: 0, line: 0 };
    this.tween = null;
    this.tokens = [];
    this.coreTiles = [];

    this.addAtmosphere({ fogDensity: 0.009 });
    this._buildBoard();
    this._buildChassis();
    this._buildDie();
    this.lineGroup = new THREE.Group();
    this.lineGroup.position.copy(LINE_POS);
    this.scene.add(this.lineGroup);
    this._buildCacheLines();
    this._apply();
  }

  /* ============================================================ build */

  _buildBoard() {
    this.board = new THREE.Group();
    this.scene.add(this.board);

    const pcbMat = new THREE.MeshStandardMaterial({ map: pcbTexture(this.role.charCodeAt(0)), roughness: 0.8, metalness: 0.2,
      emissive: 0x0a3a36, emissiveIntensity: 0.4 });
    const pcb = box(20, 0.18, 13, pcbMat);
    outline(pcb, 0x2fd9c4, 0.5);
    this.board.add(pcb);

    /* CPU socket + heat spreader */
    const socket = box(4.6, 0.3, 4.6, glow(0x1a2233, 0.2));
    socket.position.set(SOCKET_POS.x, 0.25, SOCKET_POS.z);
    outline(socket, COLORS.accent, 0.8);
    this.board.add(socket);
    const ihs = box(3.7, 0.22, 3.7, glow(0x9aa8bb, 0.25, { metalness: 0.9, roughness: 0.25 }));
    ihs.position.set(SOCKET_POS.x, 0.5, SOCKET_POS.z);
    this.board.add(ihs);
    this.ihs = ihs;
    const cpuLabel = label('CPU', { height: 0.5, color: '#c7b8ff', sub: this._short(this.app.topo.cpu_model) });
    cpuLabel.position.set(SOCKET_POS.x, 1.6, SOCKET_POS.z + 2.9);
    this.board.add(cpuLabel);
    this.addPickable(socket, { kind: 'socket', tip: 'CPU package — click to lift the CPU out and look inside' });
    this.addPickable(ihs, { kind: 'socket', tip: 'CPU package — click to lift the CPU out and look inside' });

    /* DRAM: two banks of DIMMs, each wired to the socket's memory controller */
    const dimmXs = [-8.7, -8.05, -7.4, -6.75, 0.75, 1.4, 2.05, 2.7];
    for (const x of dimmXs) {
      const dimm = box(0.2, 1.15, 6, glow(0x0d5c3a, 0.25));
      dimm.position.set(x, 0.68, SOCKET_POS.z);
      outline(dimm, 0x39ffb0, 0.35);
      for (let c = 0; c < 6; c++) {
        const chip = box(0.06, 0.55, 0.7, glow(0x111111, 0.05));
        chip.position.set(0.12, 0, -2.4 + c * 0.95);
        dimm.add(chip);
      }
      this.board.add(dimm);
      this.addPickable(dimm, { kind: 'dram', tip: 'DRAM — ~80-100 ns away' });
      const sx = x < SOCKET_POS.x ? SOCKET_POS.x - 2.3 : SOCKET_POS.x + 2.3;
      this.board.add(line([[sx, 0.12, SOCKET_POS.z - 1.2 + (x % 1)], [x, 0.12, SOCKET_POS.z - 1.2 + (x % 1)]], 0x39ffb0, { opacity: 0.6 }));
    }
    const memGb = this.hw.memGb ? `${this.hw.memGb.toFixed(1)} GB total` : 'memory';
    const memLabel = label('DRAM', { height: 0.45, color: '#7dffc8', sub: memGb });
    memLabel.position.set(-7.7, 2.1, SOCKET_POS.z - 3.4);
    this.board.add(memLabel);

    /* PCIe slot + NIC card standing in it */
    const slot = box(7.2, 0.22, 0.45, glow(0x222b3a, 0.3));
    slot.position.set(3.4, 0.2, 4.7);
    outline(slot, COLORS.cold, 0.6);
    this.board.add(slot);
    this.addPickable(slot, { kind: 'pcie', tip: 'PCIe x16 slot — the NIC’s road to memory' });
    this.board.add(line([[SOCKET_POS.x + 1.6, 0.13, SOCKET_POS.z + 2.3], [0, 0.13, 4.7]], COLORS.cold, { opacity: 0.8 }));
    this.board.add(line([[SOCKET_POS.x + 1.2, 0.13, SOCKET_POS.z + 2.3], [-0.4, 0.13, 4.5]], COLORS.cold, { opacity: 0.5 }));

    const nic = this.hw.nic;
    const card = box(6.6, 2.3, 0.12, glow(0x0d4a3a, 0.35));
    card.position.set(3.4, 1.45, 4.7);
    outline(card, COLORS.cold, 0.8);
    const chip = box(1.4, 1.2, 0.12, glow(0x1a1f2b, 0.25));
    chip.position.set(-0.6, 0.1, 0.1);
    outline(chip, COLORS.cold, 0.6);
    card.add(chip);
    for (let p = 0; p < 2; p++) {
      const cage = box(0.55, 0.5, 1.0, glow(0x8090a0, 0.3, { metalness: 0.9 }));
      cage.position.set(3.0, 0.55 - p * 0.75, 0.3);
      const led = box(0.12, 0.12, 0.05, glow(COLORS.ok, 1.5));
      led.position.set(-0.15, 0.3, 0.52);
      cage.add(led);
      card.add(cage);
    }
    this.board.add(card);
    this.nicCard = card;
    this.addPickable(card, { kind: 'nic', tip: `NIC ${nic.name} (${nic.driver})` });
    const speed = nic.speed_mbps > 0 ? `${nic.speed_mbps >= 1000 ? nic.speed_mbps / 1000 + ' Gb/s' : nic.speed_mbps + ' Mb/s'}` : 'speed n/a';
    const nicLabel = label(`NIC ${nic.name}`, { height: 0.45, color: '#7ff6ff', sub: `${nic.driver} · ${speed} · ${nic.rx_queues} RX queues` });
    nicLabel.position.set(3.4, 3.3, 4.7);
    this.board.add(nicLabel);

    /* chipset */
    const chipset = box(1.7, 0.25, 1.7, glow(0x1a2233, 0.25));
    chipset.position.set(7.3, 0.25, -3.6);
    outline(chipset, COLORS.accent, 0.5);
    this.board.add(chipset);
    this.addPickable(chipset, { kind: 'board', tip: 'chipset (slow I/O: USB, SATA)' });

    /* NUMA node boundary: socket + its memory + the NIC on its lanes */
    const numa = line([[-9.6, 0.15, -4.4], [3.6, 0.15, -4.4], [3.6, 0.15, 3.9], [7.2, 0.15, 3.9], [7.2, 0.15, 5.4],
      [-9.6, 0.15, 5.4], [-9.6, 0.15, -4.4]], COLORS.warn, { dashed: true, opacity: 0.8 });
    this.board.add(numa);
    const numaCount = this.hw.numa.length || 1;
    const numaLabel = label('NUMA node 0', { height: 0.4, color: '#ffd23f', sub: numaCount > 1 ? `${numaCount} nodes on this machine` : 'the only node on this machine' });
    numaLabel.position.set(-6.5, 0.9, 5.9);
    this.board.add(numaLabel);
  }

  _buildChassis() {
    this.lid = new THREE.Group();
    this.walls = new THREE.Group();
    this.scene.add(this.lid);
    this.scene.add(this.walls);

    const lid = box(20.6, 0.12, 13.6, glass(this.color, 0.32));
    lid.position.y = 3.0;
    outline(lid, this.color, 0.9);
    this.lid.add(lid);
    this.addPickable(lid, { kind: 'chassis', tip: `${ROLE_NAMES[this.role]}\nclick to take the lid off` });
    const title = label(`MACHINE ${this.role}`, { height: 0.8, color: '#ffffff', weight: 700, sub: 'click the lid to open it' });
    title.position.set(0, 4.3, 0);
    this.lid.add(title);

    /* heatsink over the CPU, removed with the lid */
    const sink = new THREE.Group();
    for (let i = 0; i < 14; i++) {
      const fin = box(0.08, 1.8, 4.0, glow(0xaab6c6, 0.12, { metalness: 0.9, roughness: 0.3 }));
      fin.position.set(SOCKET_POS.x - 1.95 + i * 0.3, 1.6, SOCKET_POS.z);
      sink.add(fin);
    }
    this.lid.add(sink);
    this.addPickable(sink, { kind: 'chassis', tip: 'heatsink — click to open the machine' });

    const wallMat = glass(this.color, 0.16);
    for (const x of [-10.3, 10.3]) {
      const w = box(0.12, 2.9, 13.6, wallMat);
      w.position.set(x, 1.5, 0);
      outline(w, this.color, 0.6);
      this.walls.add(w);
    }
    const back = box(20.6, 2.9, 0.12, wallMat);
    back.position.set(0, 1.5, -6.8);
    outline(back, this.color, 0.6);
    this.walls.add(back);
    const bezel = box(20.6, 2.9, 0.16, glass(this.color, 0.22));
    bezel.position.set(0, 1.5, 6.8);
    outline(bezel, this.color, 0.9);
    this.walls.add(bezel);
    this.leds = [];
    for (let i = 0; i < 12; i++) {
      const led = box(0.16, 0.16, 0.05, glow(i % 4 ? COLORS.ok : this.color, 1.2));
      led.position.set(-9.4 + i * 0.4, 2.4, 6.9);
      led.userData.phase = Math.random() * 10;
      this.walls.add(led);
      this.leds.push(led);
    }
    for (let i = 0; i < 6; i++) {
      const bay = box(2.2, 1.1, 0.06, glass(this.color, 0.3));
      bay.position.set(8.6 - i * 2.5, 1.2, 6.9);
      outline(bay, this.color, 0.5);
      this.walls.add(bay);
    }
  }

  _buildDie() {
    this.die = new THREE.Group();
    this.scene.add(this.die);
    const cores = this.hw.cores;
    const n = cores.length;
    const cols = Math.min(8, Math.max(2, Math.ceil(n / 2)));
    const rows = Math.ceil(n / cols);
    const tileW = 1.55;
    const tileD = 1.95;
    const gap = 0.2;
    const gridW = cols * (tileW + gap) - gap;
    const corridor = 1.9;
    const gridD = rows * (tileD + gap) - gap + corridor;
    this.dieSize = { w: gridW + 3.6, d: gridD + 1.6 };

    const plate = box(this.dieSize.w, 0.16, this.dieSize.d,
      new THREE.MeshStandardMaterial({ map: siliconTexture(5), emissive: 0x141a33, emissiveIntensity: 0.5, roughness: 0.4, metalness: 0.6 }));
    outline(plate, COLORS.accent, 0.9);
    this.die.add(plate);

    const dieTitle = label(this._short(this.app.topo.cpu_model), {
      height: 0.55, color: '#c7b8ff', weight: 700,
      sub: `${n} cores · ${this.app.topo.logical_cpus} logical CPUs · ${this.app.topoReal ? 'your machine (sysfs)' : 'generic model'}`,
    });
    dieTitle.position.set(0, 1.2, -this.dieSize.d / 2 - 0.4);
    this.die.add(dieTitle);

    const half = Math.ceil(rows / 2);
    const assigned = new Map(this.placement.map((p) => [p.cpu, p]));
    cores.forEach((core, i) => {
      const r = Math.floor(i / cols);
      const c = i % cols;
      const x = -gridW / 2 + tileW / 2 + c * (tileW + gap);
      const z = -gridD / 2 + tileD / 2 + r * (tileD + gap) + (r >= half ? corridor : 0);
      const tile = new THREE.Group();
      tile.position.set(x, 0.12, z);
      this.die.add(tile);

      const job = core.threads.map((t) => assigned.get(t)).find(Boolean);
      const tileColor = job ? (job.kind === 'hot' ? COLORS.hot : job.kind === 'os' ? COLORS.warn : COLORS.cold) : 0x3a4a6a;
      const base = box(tileW, 0.08, tileD, glass(tileColor, job ? 0.35 : 0.2));
      outline(base, tileColor, job ? 1 : 0.5);
      tile.add(base);
      this.addPickable(base, { kind: 'core', idx: i, tip: `core ${core.coreId} — logical CPUs ${core.threads.join(', ')}` });

      const l1d = box(0.66, 0.14, 0.46, glow(COLORS.hot, 0.45));
      l1d.position.set(-0.36, 0.1, -0.62);
      tile.add(l1d);
      this.addPickable(l1d, { kind: 'l1d', idx: i, tip: `L1d ${fmtKb(core.l1d && core.l1d.size_kb)} — click to zoom into a cache line` });
      const l1i = box(0.66, 0.14, 0.46, glow(0xffa25a, 0.3));
      l1i.position.set(0.36, 0.1, -0.62);
      tile.add(l1i);
      this.addPickable(l1i, { kind: 'l1i', idx: i, tip: `L1i ${fmtKb(core.l1i && core.l1i.size_kb)} (instructions)` });
      const l2 = box(1.38, 0.14, 0.62, glow(COLORS.accent, 0.4));
      l2.position.set(0, 0.1, 0.05);
      tile.add(l2);
      this.addPickable(l2, { kind: 'l2', idx: i, tip: `L2 ${fmtKb(core.l2 && core.l2.size_kb)} (private to this core)` });
      const exec = box(1.38, 0.14, 0.42, glow(0x2a3a5a, 0.3));
      exec.position.set(0, 0.1, 0.66);
      tile.add(exec);
      core.threads.forEach((t, k) => {
        const pill = new THREE.Mesh(new THREE.CylinderGeometry(0.1, 0.1, 0.12, 14),
          glow(assigned.get(t) ? tileColor : 0x5a6a8a, assigned.get(t) ? 1.4 : 0.4));
        pill.position.set(-0.45 + k * 0.3, 0.2, 0.66);
        tile.add(pill);
      });

      const tag = label(`core ${core.coreId}`, { height: 0.26, sub: `cpu ${core.threads.join(',')}` });
      tag.position.set(0, 0.55, 0.15);
      tile.add(tag);
      if (job) {
        const jl = label(job.short, { height: 0.34, color: job.kind === 'hot' ? '#ff9a7c' : job.kind === 'os' ? '#ffe27a' : '#7ff6ff' });
        jl.position.set(0, 1.1, 0.15);
        tile.add(jl);
        const beam = new THREE.Mesh(new THREE.CylinderGeometry(0.02, 0.02, 0.6, 6), glow(tileColor, 1.2));
        beam.position.set(0, 0.7, 0.66);
        tile.add(beam);
      }
      this.coreTiles.push({ tile, core, job });
    });

    /* shared L3 in the corridor between the rows of cores */
    const l3 = this.hw.l3[0];
    const l3z = -gridD / 2 + half * (tileD + gap) - gap + corridor / 2;
    const l3mesh = box(gridW, 0.14, corridor - 0.5, glow(0x3b6cff, 0.45));
    l3mesh.position.set(0, 0.16, l3z);
    outline(l3mesh, 0x6f9bff, 0.8);
    this.die.add(l3mesh);
    this.addPickable(l3mesh, { kind: 'l3', tip: `L3 ${fmtKb(l3 && l3.size_kb)} shared by ${l3 ? l3.shared_cpus.length : '?'} logical CPUs` });
    const l3l = label(`L3 · ${fmtKb(l3 && l3.size_kb)} shared`, { height: 0.36, color: '#9bb8ff' });
    l3l.position.set(0, 0.55, l3z);
    this.die.add(l3l);

    /* uncore: memory controller + PCIe root complex */
    const mc = box(1.3, 0.14, gridD, glow(0x1f8a6a, 0.4));
    mc.position.set(-gridW / 2 - 1.05, 0.16, 0);
    outline(mc, 0x39ffb0, 0.7);
    this.die.add(mc);
    this.addPickable(mc, { kind: 'mc', tip: 'memory controller + PCIe I/O (where the NIC’s DMA enters)' });
    const mcl = label('mem ctrl + I/O', { height: 0.3, color: '#7dffc8' });
    mcl.position.set(-gridW / 2 - 1.05, 0.55, gridD / 2 + 0.4);
    this.die.add(mcl);
  }

  _short(model) {
    return String(model || 'CPU').replace(/\(R\)|\(TM\)|CPU|Processor|w\/.*$/gi, '').replace(/\s+/g, ' ').trim();
  }

  _buildCacheLines() {
    const g = this.lineGroup;
    for (const c of [...g.children]) g.remove(c);
    this.tokens = [];
    this.fieldPickables = this.fieldPickables || [];
    for (const p of this.fieldPickables) this.pickables.splice(this.pickables.indexOf(p), 1);
    this.fieldPickables = [];

    const def = STRUCTS[this.role][this.structIdx].def;
    const lines = writersPerLine(def);
    const CELL = 0.19;
    const W = CACHE_LINE * CELL;
    const zOf = (l) => (l - (lines.length - 1) / 2) * 1.3;
    const writerColor = { receiver: COLORS.receiver, 'receiver-alt': 0xffb070, workers: COLORS.workers, both: COLORS.both, pad: COLORS.pad };

    const title = label(def.id, { height: 0.5, color: '#ffffff', weight: 700, sub: `${def.file} · ${def.size} bytes · each bar is one 64-byte cache line` });
    title.position.set(0, 1.4, zOf(0) - 1.3);
    g.add(title);

    lines.forEach((l) => {
      const z = zOf(l.line);
      const bar = box(W + 0.1, 0.06, 0.78, glow(l.falseSharing ? 0x3a0a14 : 0x0e1626, 0.5));
      bar.position.set(0, 0, z);
      const edge = outline(bar, l.falseSharing ? COLORS.bad : 0x3d5a80, 0.9);
      if (l.falseSharing) bar.userData.pulse = edge;
      g.add(bar);
      for (let b = 0; b <= 64; b += 8) {
        g.add(line([[-W / 2 + b * CELL, 0.05, z - 0.42], [-W / 2 + b * CELL, 0.05, z + 0.42]], 0x4d6a90, { opacity: 0.5 }));
      }
      const ll = label(`line ${l.line}`, { height: 0.26, color: l.falseSharing ? '#ff7a9a' : '#9fb4cc', sub: `bytes ${l.line * 64}-${l.line * 64 + 63}` });
      ll.position.set(-W / 2 - 1.0, 0.3, z);
      g.add(ll);
    });

    for (const f of def.fields) {
      let start = f.offset;
      const end = f.offset + f.size;
      const straddles = Math.floor(start / 64) !== Math.floor((end - 1) / 64);
      while (start < end) {
        const li = Math.floor(start / 64);
        const segEnd = Math.min(end, (li + 1) * 64);
        const w = (segEnd - start) * CELL - 0.03;
        const isPad = f.writer === 'pad';
        const seg = box(Math.max(0.05, w), isPad ? 0.08 : 0.24, 0.6,
          isPad ? glass(COLORS.pad, 0.35) : glow(writerColor[f.writer] || 0xffffff, f.msg !== undefined && f.msg % 2 ? 0.5 : 0.32));
        seg.position.set(-W / 2 + ((start % 64) + (segEnd - start) / 2) * CELL, isPad ? 0.08 : 0.16, zOf(li));
        if (straddles) outline(seg, COLORS.bad, 1);
        g.add(seg);
        this.addPickable(seg, { kind: 'field', field: f, def, straddles, tip: `${f.name} — offset ${f.offset}, ${f.size} B${straddles ? ' (straddles two lines!)' : ''}` });
        this.fieldPickables.push(seg);
        if (w >= 1.4 && !isPad) { /* label only fields >= 8 bytes; click the rest */
          /* alternate label heights so neighbouring long names don't collide */
          const fl = label(f.name, { height: 0.3, color: '#ffffff' });
          const lift = this._labelFlip ? 0.95 : 0.5;
          this._labelFlip = !this._labelFlip;
          fl.position.set(seg.position.x, lift, seg.position.z - 0.1);
          g.add(fl);
        }
        start = segEnd;
      }
    }

    /* the cores that write these lines, and the line-ownership traffic */
    const recvCpu = (this.placement.find((p) => /receiver|publisher/.test(p.thread)) || this.placement[1] || {}).cpu;
    const hasWorkers = def.fields.some((f) => f.writer === 'workers' || f.writer === 'both');
    const left = new THREE.Vector3(-W / 2 - 3.2, 0.3, 0);
    const right = new THREE.Vector3(W / 2 + 2.6, 0.3, 0);
    const leftName = this.role === 'A' ? 'publisher thread' : 'receiver core';
    const lg = box(1.7, 0.5, 1.1, glow(COLORS.receiver, 0.7));
    lg.position.copy(left);
    g.add(lg);
    const lgl = label(leftName, { height: 0.26, color: '#ffb08a', sub: recvCpu !== undefined ? `cpu ${recvCpu}` : '' });
    lgl.position.set(left.x, 1.0, left.z);
    g.add(lgl);
    if (hasWorkers) {
      const rg = box(1.7, 0.5, 1.1, glow(COLORS.workers, 0.7));
      rg.position.copy(right);
      g.add(rg);
      const rgl = label('worker cores', { height: 0.26, color: '#e7a6ff', sub: 'other physical cores' });
      rgl.position.set(right.x, 1.0, right.z);
      g.add(rgl);
    }
    lines.forEach((l) => {
      const z = zOf(l.line);
      if (l.writers.includes('receiver')) g.add(line([[left.x + 0.85, 0.3, left.z], [-W / 2 - 0.05, 0.1, z]], COLORS.receiver, { opacity: 0.6 }));
      if (hasWorkers && l.writers.includes('workers')) g.add(line([[right.x - 0.85, 0.3, right.z], [W / 2 + 0.05, 0.1, z]], COLORS.workers, { opacity: 0.6 }));
      if (l.falseSharing) {
        const token = new THREE.Mesh(new THREE.SphereGeometry(0.17, 16, 16), glow(COLORS.bad, 2.2));
        g.add(token);
        this.tokens.push({ mesh: token, z, a: new THREE.Vector3(left.x + 0.9, 0.5, z), b: new THREE.Vector3(right.x - 0.9, 0.5, z) });
      }
    });
    if (this.tokens.length) {
      const warn = label('red ball = the cache line bouncing between cores on every write', { height: 0.36, color: '#ff7a9a' });
      warn.position.set(0, 0.2, zOf(lines.length - 1) + 1.5);
      g.add(warn);
    }
    this.lineInfo = lines;
  }

  /* ======================================================== layering */

  _targets(level) {
    return {
      lift: level >= 1 ? 1 : 0,
      die: level >= 2 ? 1 : 0,
      dim: level >= 2 ? (level >= 3 ? 1 : 0.6) : 0,
      line: level >= 3 ? 1 : 0,
    };
  }

  setLevel(level) {
    level = Math.max(0, Math.min(3, level));
    this.level = level;
    const from = { ...this.state };
    const to = this._targets(level);
    if (this.tween) this.tween.cancel();
    this.tween = animate(1100, (t) => {
      for (const k of Object.keys(to)) this.state[k] = lerp(from[k], to[k], t);
      this._apply();
    });
    const cam = LEVELS[level].cam;
    this.app.engine.flyTo(cam.pos, cam.target, 1300);
    this._showLevelLesson();
    this._setControls();
    this.app.nav.refresh();
  }

  _apply() {
    const s = this.state;
    this.lid.position.y = s.lift * 9;
    setGroupOpacity(this.lid, 1 - s.lift);
    setGroupOpacity(this.walls, 1 - 0.8 * s.lift);
    setGroupOpacity(this.board, 1 - 0.55 * s.dim);

    this.die.position.lerpVectors(SOCKET_POS, DIE_POS, s.die);
    this.die.scale.setScalar(lerp(0.2, 1, s.die));
    setGroupOpacity(this.die, s.die * (1 - 0.82 * s.line));
    this.ihs.visible = s.die < 0.5;

    this.lineGroup.scale.setScalar(lerp(0.3, 1, s.line));
    this.lineGroup.position.y = lerp(DIE_POS.y, LINE_POS.y, s.line);
    setGroupOpacity(this.lineGroup, s.line);
  }

  crumbs() {
    const items = [{ label: `Machine ${this.role}`, onClick: () => this.setLevel(0) }];
    for (let l = 1; l <= this.level; l++) items.push({ label: LEVELS[l].label, onClick: () => this.setLevel(l) });
    return items;
  }

  back() {
    if (this.level === 0) return false;
    this.setLevel(this.level - 1);
    return true;
  }

  enter() {
    this.setLevel(this.level);
  }

  /* ========================================================= lessons */

  _hwLive() {
    const t = this.app.topo;
    return [
      ['role in this lab', ROLE_NAMES[this.role]],
      ['CPU', t.cpu_model],
      ['logical CPUs / cores', `${t.logical_cpus} / ${t.physical_cores || this.hw.cores.length}`],
      ['memory', this.hw.memGb ? `${this.hw.memGb.toFixed(1)} GB` : '?'],
      ['isolated CPUs', t.isolated_cpus && t.isolated_cpus.length ? t.isolated_cpus.join(',') : 'none (no isolcpus=)'],
      ['topology source', this.app.topoReal ? 'your machine (make topology)' : 'generic model - run `make topology`', this.app.topoReal ? 'ok' : 'warn'],
    ];
  }

  _showLevelLesson() {
    const hud = this.app.hud;
    const hw = this.hw;
    const l3 = hw.l3[0];
    const c0 = hw.cores[0];
    if (this.level === 0) {
      hud.showLesson('hw-chassis', { live: this._hwLive() });
    } else if (this.level === 1) {
      hud.showLesson('hw-board', {
        live: [
          ['NUMA nodes', String(hw.numa.length || 1)],
          ['NIC', `${hw.nic.name} · ${hw.nic.driver}`],
          ['NIC NUMA node', hw.nic.numa_node >= 0 ? String(hw.nic.numa_node) : 'n/a (virtual)'],
          ['memory', hw.memGb ? `${hw.memGb.toFixed(1)} GB` : '?'],
        ],
      });
    } else if (this.level === 2) {
      hud.showLesson('hw-die', {
        live: [
          ['L1d / L1i per core', `${fmtKb(c0.l1d && c0.l1d.size_kb)} / ${fmtKb(c0.l1i && c0.l1i.size_kb)}`],
          ['L2 per core', `${fmtKb(c0.l2 && c0.l2.size_kb)} (${c0.l2 ? c0.l2.ways : '?'}-way)`],
          ['L3', `${fmtKb(l3 && l3.size_kb)} shared by ${l3 ? l3.shared_cpus.length : '?'} logical CPUs`],
          ['SMT', c0.threads.length > 1 ? `${c0.threads.length} logical CPUs per core` : 'off'],
        ],
        table: {
          title: 'Suggested thread placement (only --pin-cpu for the receiver is real)',
          head: ['logical CPU', 'thread'],
          rows: this.placement.map((p) => [String(p.cpu), p.thread]),
        },
      });
    } else {
      const s = STRUCTS[this.role][this.structIdx];
      hud.showLesson(s.lesson, { layout: { def: s.def, lines: this.lineInfo } });
    }
  }

  _setControls() {
    const specs = [{
      type: 'buttons', label: 'Layer',
      items: LEVELS.map((L, i) => ({ label: `${i}. ${L.label}`, active: i === this.level, onClick: () => this.setLevel(i) })),
    }];
    if (this.level === 3) {
      specs.push({
        type: 'buttons', label: 'Struct',
        items: STRUCTS[this.role].map((s, i) => ({
          label: s.label, active: i === this.structIdx,
          onClick: () => { this.structIdx = i; this._buildCacheLines(); this._apply(); this._showLevelLesson(); this._setControls(); },
        })),
      });
    }
    specs.push({ type: 'text', text: this.level < 3 ? 'click a part to dive in · Esc = up one layer' : 'click a field for its details · Esc = back to the die' });
    this.app.hud.setControls(specs);
  }

  onPick(pick) {
    if (!pick) return;
    const hud = this.app.hud;
    const hw = this.hw;
    switch (pick.kind) {
      case 'chassis':
        if (this.level === 0) this.setLevel(1);
        break;
      case 'socket':
        if (this.level <= 1) this.setLevel(2);
        break;
      case 'dram':
        hud.showLesson('hw-dram', { live: [['installed', hw.memGb ? `${hw.memGb.toFixed(1)} GB` : '?'], ['NUMA nodes', String(hw.numa.length || 1)]] });
        break;
      case 'pcie':
        hud.showLesson('hw-pcie', { live: [['device in slot', `${hw.nic.name} (${hw.nic.driver})`]] });
        break;
      case 'nic':
        hud.showLesson('hw-nic', {
          live: [
            ['interface', hw.nic.name], ['driver', hw.nic.driver, /hv_netvsc|virtio/.test(hw.nic.driver) ? 'warn' : 'ok'],
            ['link speed', hw.nic.speed_mbps > 0 ? `${hw.nic.speed_mbps} Mb/s` : 'n/a'],
            ['RX / TX queues', `${hw.nic.rx_queues} / ${hw.nic.tx_queues}`], ['MTU', String(hw.nic.mtu)],
            ['NUMA node', hw.nic.numa_node >= 0 ? String(hw.nic.numa_node) : 'n/a'],
          ],
        });
        break;
      case 'board':
        hud.showLesson('hw-board');
        break;
      case 'core': {
        const ct = this.coreTiles[pick.idx];
        const c = ct.core;
        hud.showLesson('hw-core', {
          live: [
            ['core id', String(c.coreId)], ['logical CPUs (SMT siblings)', c.threads.join(', ')],
            ['L1d / L1i', `${fmtKb(c.l1d && c.l1d.size_kb)} / ${fmtKb(c.l1i && c.l1i.size_kb)}`],
            ['L2', fmtKb(c.l2 && c.l2.size_kb)],
            ['suggested job', ct.job ? ct.job.thread : 'idle / available', ct.job ? (ct.job.kind === 'hot' ? 'hot' : '') : ''],
          ],
        });
        break;
      }
      case 'l1d':
        if (this.level === 2) this.setLevel(3);
        else hud.showLesson('hw-l1');
        break;
      case 'l1i':
        hud.showLesson('hw-l1', { live: [['note', 'this is the INSTRUCTION half of L1 - code, not data']] });
        break;
      case 'l2':
        hud.showLesson('hw-l2', { live: [['size', fmtKb(hw.cores[pick.idx].l2 && hw.cores[pick.idx].l2.size_kb)]] });
        break;
      case 'l3':
        hud.showLesson('hw-l3', { live: [['size', fmtKb(hw.l3[0] && hw.l3[0].size_kb)], ['shared by', `${hw.l3[0] ? hw.l3[0].shared_cpus.length : '?'} logical CPUs`]] });
        break;
      case 'mc':
        hud.showLesson('hw-memctrl');
        break;
      case 'field': {
        const f = pick.field;
        const first = Math.floor(f.offset / 64);
        const last = Math.floor((f.offset + f.size - 1) / 64);
        const writer = { receiver: 'receiver thread', 'receiver-alt': 'receiver thread', workers: 'worker threads', both: 'receiver AND workers', pad: 'nobody (padding)' }[f.writer];
        hud.showLesson(STRUCTS[this.role][this.structIdx].lesson, {
          live: [
            ['field', f.name], ['offset / size', `${f.offset} / ${f.size} bytes`], ['written by', writer, f.writer === 'both' ? 'warn' : ''],
            ['cache line(s)', first === last ? `line ${first}` : `lines ${first}-${last} (straddles!)`, first === last ? '' : 'bad'],
          ],
          layout: { def: pick.def, lines: this.lineInfo },
        });
        break;
      }
      default:
        break;
    }
  }

  update(dt, t) {
    this.animateAtmosphere(t);
    for (const led of this.leds) led.material.emissiveIntensity = 0.4 + (Math.sin(t * 5 + led.userData.phase * 3) > 0.5 ? 1.5 : 0);
    if (this.state.line > 0.05) {
      for (const tok of this.tokens) {
        const k = (Math.sin(t * 3.2 + tok.z) + 1) / 2;
        tok.mesh.position.lerpVectors(tok.a, tok.b, k);
        tok.mesh.position.y = 0.5 + Math.sin(k * Math.PI) * 0.9;
      }
      this.lineGroup.traverse((o) => {
        if (o.userData.pulse) o.userData.pulse.material.opacity = 0.5 + 0.5 * Math.abs(Math.sin(t * 4));
      });
    }
  }
}
