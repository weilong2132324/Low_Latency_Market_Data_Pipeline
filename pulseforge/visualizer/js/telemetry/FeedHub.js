/*
 * FeedHub.js - where the visualizer's data comes from.
 *
 *   LIVE  a WebSocket to src/telemetry_bridge.cpp. Each JSON frame is
 *         { stats: {...}, config: {...}, events: [...] } (see that file's
 *         build_snapshot_json()).
 *   DEMO  when no bridge is reachable, DemoFeed synthesises the same
 *         shapes so the scene is never empty. It is always labelled DEMO
 *         in the HUD - simulated numbers must never pass for real ones.
 *
 * Views subscribe with hub.on('stats' | 'config' | 'events' | 'source', fn).
 * The hub keeps retrying the WebSocket in the background, so starting the
 * backend later flips the page to LIVE without a reload.
 */

const RETRY_MS = 4000;

export class FeedHub {
  constructor(url) {
    this.url = url;
    this.listeners = { stats: [], config: [], events: [], source: [] };
    this.source = 'connecting';
    this.lastConfig = null;
    this.lastStats = null;
    this.ws = null;
    this.retryTimer = null;
    this.demo = new DemoFeed(this);
    this.connect(url);
  }

  on(type, fn) {
    this.listeners[type].push(fn);
  }

  emit(type, data) {
    if (type === 'config') this.lastConfig = data;
    if (type === 'stats') this.lastStats = data;
    for (const fn of this.listeners[type]) fn(data);
  }

  setSource(source, detail = '') {
    this.source = source;
    this.emit('source', { source, detail });
  }

  connect(url) {
    this.url = url;
    clearTimeout(this.retryTimer);
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.close();
    }
    let ws;
    try {
      ws = new WebSocket(url);
    } catch (e) {
      this.setSource('demo', `bad URL: ${e.message}`);
      this.demo.start();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      this.demo.stop();
      this.setSource('live', url);
    };
    ws.onmessage = (ev) => {
      let msg;
      try {
        msg = JSON.parse(ev.data);
      } catch (e) {
        return;
      }
      if (msg.config) this.emit('config', msg.config);
      if (msg.stats) this.emit('stats', msg.stats);
      if (Array.isArray(msg.events) && msg.events.length) this.emit('events', msg.events);
    };
    ws.onclose = () => {
      if (this.source !== 'demo') this.setSource('demo', `no telemetry_bridge at ${url} - retrying`);
      this.demo.start();
      this.retryTimer = setTimeout(() => this.connect(this.url), RETRY_MS);
    };
  }
}

/*
 * DemoFeed - synthetic stats/config/events with roughly realistic shapes:
 * log-normal latencies (most fast, a long slow tail - like real systems),
 * a gap every ~70 ticks that gets "recovered" ~300 ms later.
 */
class DemoFeed {
  constructor(hub) {
    this.hub = hub;
    this.timer = null;
    this.seq = 0;
    this.stats = null;
    this.pendingRecoveries = [];
    this.config = {
      transport: 'multicast', multicast_group: '239.255.0.5', queue_kind: 'disruptor', workers: 3,
      batch: 16, busy_poll_us: 50, bpf_filter: true, recovery_enabled: true, port: 9000,
    };
  }

  start() {
    if (this.timer) return;
    this.stats = {
      received: 0, invalid: 0, gaps: 0, reorders: 0, queue_full: 0, recovered: 0,
      processed: 0, queue_depth: 0, avg_latency_ns: 0, max_latency_ns: 0,
    };
    this.latTotal = 0;
    this.hub.emit('config', this.config);
    this.timer = setInterval(() => this.tick(), 50);
  }

  stop() {
    clearInterval(this.timer);
    this.timer = null;
  }

  latencyNs() {
    const gaussian = Math.sqrt(-2 * Math.log(Math.random() + 1e-12)) * Math.cos(2 * Math.PI * Math.random());
    return Math.round(Math.exp(Math.log(38000) + 0.55 * gaussian));
  }

  tick() {
    const now = performance.now() * 1e6;
    const events = [];
    const s = this.stats;
    const n = 8 + Math.floor(Math.random() * 6);
    for (let i = 0; i < n; i++) {
      this.seq++;
      if (this.seq % 71 === 0) {
        s.gaps++;
        events.push({ type: 'gap', seq: this.seq, ts_ns: now, latency_ns: 1, worker_id: 0 });
        this.pendingRecoveries.push({ seq: this.seq, at: performance.now() + 300 });
        continue;
      }
      s.received++;
      events.push({ type: 'received', seq: this.seq, ts_ns: now, latency_ns: 0, worker_id: 0 });
      const lat = this.latencyNs();
      s.processed++;
      this.latTotal += lat;
      s.max_latency_ns = Math.max(s.max_latency_ns, lat);
      events.push({ type: 'processed', seq: this.seq, ts_ns: now, latency_ns: lat, worker_id: this.seq % this.config.workers });
    }
    this.pendingRecoveries = this.pendingRecoveries.filter((r) => {
      if (performance.now() < r.at) return true;
      s.recovered++;
      events.push({ type: 'recovered', seq: r.seq, ts_ns: now, latency_ns: 0, worker_id: 0 });
      return false;
    });
    if (Math.random() < 0.01) {
      s.queue_full++;
      events.push({ type: 'queue_full', seq: this.seq, ts_ns: now, latency_ns: 0, worker_id: 0 });
    }
    s.queue_depth = Math.max(0, Math.min(48, s.queue_depth + Math.round((Math.random() - 0.5) * 6)));
    s.avg_latency_ns = s.processed ? Math.round(this.latTotal / s.processed) : 0;
    this.hub.emit('stats', { ...s });
    this.hub.emit('events', events);
  }
}
