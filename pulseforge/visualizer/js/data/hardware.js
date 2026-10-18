/*
 * hardware.js - the machine model the "open up the machine" view draws.
 *
 * Source of truth, in order:
 *   1. data/hw_topology.json, written by `make topology` (src/hw_topology.cpp
 *      reading Linux sysfs) - YOUR real CPU, caches, NUMA nodes and NICs.
 *   2. DEFAULT_TOPOLOGY below - a generic single-socket server, used when
 *      the file hasn't been generated yet.
 *
 * Also decides where each PulseForge thread *should* run on that hardware
 * (placementFor). That is a teaching suggestion, not what the code does:
 * only the receiver thread is actually pinned (--pin-cpu, src/cpu_util.cpp).
 */

export const DEFAULT_TOPOLOGY = {
  source: 'built-in generic model - run `make topology` to load your real machine',
  cpu_model: 'Generic 8-core x86-64 server CPU',
  logical_cpus: 16,
  physical_cores: 8,
  mem_total_kb: 64 * 1024 * 1024,
  isolated_cpus: [],
  cores: Array.from({ length: 8 }, (_, i) => ({ package: 0, core_id: i, threads: [2 * i, 2 * i + 1] })),
  caches: [
    ...Array.from({ length: 8 }, (_, i) => ({ level: 1, type: 'Data', size_kb: 48, line_size: 64, ways: 12, shared_cpus: [2 * i, 2 * i + 1] })),
    ...Array.from({ length: 8 }, (_, i) => ({ level: 1, type: 'Instruction', size_kb: 32, line_size: 64, ways: 8, shared_cpus: [2 * i, 2 * i + 1] })),
    ...Array.from({ length: 8 }, (_, i) => ({ level: 2, type: 'Unified', size_kb: 2048, line_size: 64, ways: 16, shared_cpus: [2 * i, 2 * i + 1] })),
    { level: 3, type: 'Unified', size_kb: 32768, line_size: 64, ways: 16, shared_cpus: Array.from({ length: 16 }, (_, i) => i) },
  ],
  numa_nodes: [{ id: 0, cpus: Array.from({ length: 16 }, (_, i) => i) }],
  nics: [{ name: 'eth0', driver: 'mlx5_core (example kernel-bypass capable NIC)', mac: '', mtu: 1500, speed_mbps: 25000, numa_node: 0, rx_queues: 8, tx_queues: 8 }],
};

export async function loadTopology() {
  try {
    const res = await fetch('data/hw_topology.json', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const topo = await res.json();
    if (!Array.isArray(topo.cores) || !topo.cores.length || !Array.isArray(topo.caches)) {
      throw new Error('unexpected shape');
    }
    return { topo, real: true };
  } catch (e) {
    return { topo: DEFAULT_TOPOLOGY, real: false, reason: e.message };
  }
}

/* Flatten sysfs data into "per physical core, which caches does it own,
 * and which big shared cache sits behind them". */
export function summarize(topo) {
  const cacheFor = (cpu, level, type) =>
    topo.caches.find((c) => c.level === level && (type ? c.type === type : true) && c.shared_cpus.includes(cpu));

  const packages = [...new Set(topo.cores.map((c) => c.package))].sort((a, b) => a - b);
  const cores = topo.cores.map((c) => {
    const cpu = c.threads[0];
    return {
      package: c.package,
      coreId: c.core_id,
      threads: c.threads,
      l1d: cacheFor(cpu, 1, 'Data'),
      l1i: cacheFor(cpu, 1, 'Instruction'),
      l2: cacheFor(cpu, 2),
      l3: cacheFor(cpu, 3),
    };
  });
  const l3 = topo.caches.filter((c) => c.level === 3);
  const physicalNics = (topo.nics || []).filter((n) => !/virtual/.test(n.driver) && n.name !== 'lo');
  const nic = physicalNics[0] || (topo.nics || [])[0] || DEFAULT_TOPOLOGY.nics[0];
  const memGb = topo.mem_total_kb ? topo.mem_total_kb / (1024 * 1024) : null;
  return { packages, cores, l3, nic, memGb, numa: topo.numa_nodes || [] };
}

export function fmtKb(kb) {
  if (kb == null) return '?';
  return kb >= 1024 ? `${(kb / 1024).toFixed(kb % 1024 ? 1 : 0)} MB` : `${kb} KB`;
}

/*
 * Suggested thread placement per machine role. The rules every HFT box
 * follows, in miniature:
 *   - keep logical CPU 0 for the OS / interrupts ("housekeeping");
 *   - give each hot thread its own PHYSICAL core (use one SMT sibling,
 *     leave the other idle) so it gets the whole L1/L2;
 *   - keep everything on the NIC's NUMA node.
 */
export function placementFor(role, summary, workerCount = 3) {
  const cores = summary.cores;
  const pick = (i) => cores[Math.min(i, cores.length - 1)];
  const out = [{ cpu: cores[0].threads[0], thread: 'OS + IRQs (housekeeping)', short: 'OS + IRQs', kind: 'os' }];
  if (role === 'A') {
    out.push({ cpu: pick(1).threads[0], thread: 'publisher main loop (sendto)', short: 'publisher', kind: 'hot' });
    out.push({ cpu: pick(2).threads[0], thread: 'snapshot server (TCP accept)', short: 'snapshot srv', kind: 'cold' });
  } else if (role === 'B') {
    out.push({ cpu: pick(1).threads[0], thread: 'receiver loop (recvfrom/recvmmsg)', short: 'receiver', kind: 'hot' });
    for (let w = 0; w < workerCount; w++) {
      out.push({ cpu: pick(2 + w).threads[0], thread: `worker ${w}`, short: `worker ${w}`, kind: 'hot' });
    }
  } else {
    out.push({ cpu: pick(1).threads[0], thread: 'telemetry_bridge poll loop', short: 'telemetry', kind: 'cold' });
    out.push({ cpu: pick(2).threads[0], thread: 'WebSocket accept thread', short: 'ws accept', kind: 'cold' });
  }
  return out;
}
