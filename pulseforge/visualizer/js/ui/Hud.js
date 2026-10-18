/*
 * Hud.js - every piece of 2D UI: breadcrumb, lesson panel, live stats,
 * per-view controls, tooltip, and the queue_bench paste panel.
 *
 * Views never touch the DOM directly; they call these methods. All
 * dynamic text goes through textContent (never innerHTML), so nothing
 * arriving over the WebSocket can inject markup into the page.
 */
import { lesson as getLesson } from '../data/lessons.js';

const $ = (id) => document.getElementById(id);

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = String(text);
  return e;
}

/* Renders `backtick spans` as <code>, everything else as plain text. */
function richText(parent, text) {
  String(text).split('`').forEach((part, i) => {
    if (!part) return;
    parent.appendChild(i % 2 ? el('code', null, part) : document.createTextNode(part));
  });
  return parent;
}

export class Hud {
  constructor() {
    this.lessonId = null;
    this.liveTable = null;
    this.onConnect = null;
    $('connect-btn').addEventListener('click', () => this.onConnect && this.onConnect($('ws-url').value.trim()));
    $('bench-toggle').addEventListener('click', () => $('bench').classList.toggle('hidden'));
    $('bench-close').addEventListener('click', () => $('bench').classList.add('hidden'));
    $('bench-parse').addEventListener('click', () => this._renderBench());
    $('lesson-toggle').addEventListener('click', () => $('lesson').classList.toggle('collapsed'));
  }

  /* ------------------------------------------------------------ crumbs */
  setCrumbs(items) {
    const nav = $('crumbs');
    nav.replaceChildren();
    items.forEach((item, i) => {
      if (i) nav.appendChild(el('span', 'sep', '›'));
      const last = i === items.length - 1;
      const b = el('button', last ? 'crumb current' : 'crumb', item.label);
      if (!last) b.addEventListener('click', item.onClick);
      nav.appendChild(b);
    });
  }

  /* ------------------------------------------------------------ lesson */
  /*
   * extras: {
   *   live:   [[label, value], ...]         live values, refreshed by updateLive()
   *   header: { def, ctx }                  protocol header byte table (protocols.js)
   *   layout: { def, lines }                cache-line struct table (layouts.js)
   *   table:  { title, head:[...], rows:[[...]] }  any extra table
   * }
   */
  showLesson(id, extras = {}) {
    const L = typeof id === 'string' ? getLesson(id) : id;
    this.lessonId = typeof id === 'string' ? id : L.title;
    const body = $('lesson-body');
    body.replaceChildren();
    $('lesson').classList.remove('collapsed');

    if (L.tag) body.appendChild(el('div', 'tag', L.tag));
    body.appendChild(el('h2', null, L.title));
    if (L.summary) body.appendChild(richText(el('p', 'summary'), L.summary));

    this.liveTable = null;
    if (extras.live) {
      body.appendChild(el('div', 'section', 'Live'));
      this.liveTable = el('table', 'kv live');
      body.appendChild(this.liveTable);
      this.updateLive(extras.live);
    }
    if (extras.header) body.appendChild(this._headerTable(extras.header.def, extras.header.ctx));
    if (extras.layout) body.appendChild(this._layoutTable(extras.layout.def, extras.layout.lines));
    if (extras.table) body.appendChild(this._genericTable(extras.table));

    if (L.points && L.points.length) {
      body.appendChild(el('div', 'section', 'Key ideas'));
      const ul = el('ul', 'points');
      for (const p of L.points) ul.appendChild(richText(el('li'), p));
      body.appendChild(ul);
    }
    if (L.latency) {
      body.appendChild(el('div', 'section', 'Typical latency'));
      body.appendChild(richText(el('p', 'latency'), L.latency));
      body.appendChild(el('p', 'fineprint', 'Order-of-magnitude figures for modern x86 servers, not measurements. Measure your own.'));
    }
    if (L.code && L.code.length) {
      body.appendChild(el('div', 'section', 'In PulseForge'));
      const ul = el('ul', 'code-refs');
      for (const c of L.code) {
        const li = el('li');
        li.appendChild(el('code', 'file', c.file));
        if (c.note) li.appendChild(richText(el('span', 'note'), ' — ' + c.note));
        ul.appendChild(li);
      }
      body.appendChild(ul);
    }
    if (L.tryIt && L.tryIt.length) {
      body.appendChild(el('div', 'section', 'Try it (WSL2)'));
      for (const cmd of L.tryIt) body.appendChild(el('pre', 'cmd', cmd));
    }
    if (L.interview) {
      const questions = Array.isArray(L.interview) ? L.interview : [L.interview];
      body.appendChild(el('div', 'section', questions.length > 1 ? 'Interview questions' : 'Interview question'));
      for (const iq of questions) {
        const d = el('details', 'interview');
        d.appendChild(richText(el('summary'), iq.q));
        d.appendChild(richText(el('p'), iq.a));
        body.appendChild(d);
      }
    }
    body.scrollTop = 0;
  }

  updateLive(rows) {
    if (!this.liveTable) return;
    this.liveTable.replaceChildren();
    for (const [k, v, cls] of rows) {
      const tr = el('tr');
      tr.appendChild(el('td', 'k', k));
      tr.appendChild(el('td', cls ? `v ${cls}` : 'v', v));
      this.liveTable.appendChild(tr);
    }
  }

  _headerTable(def, ctx) {
    const wrap = el('div', 'bytes');
    wrap.appendChild(el('div', 'section', `${def.name} — ${def.size} bytes${def.trailer ? ` + ${def.trailer}-byte trailer` : ''}`));
    const bar = el('div', 'bytebar');
    def.fields.forEach((f, i) => {
      const cell = el('div', `byte f${i % 6}${f.pad ? ' pad' : ''}`);
      cell.style.flexGrow = String(f.size);
      cell.title = `${f.name}: offset ${f.offset}, ${f.size} B`;
      cell.textContent = f.size >= 2 ? f.name : '';
      bar.appendChild(cell);
    });
    wrap.appendChild(bar);
    const t = el('table', 'fields');
    const head = el('tr');
    for (const h of ['off', 'size', 'field', 'this run']) head.appendChild(el('th', null, h));
    t.appendChild(head);
    for (const f of def.fields) {
      const tr = el('tr', f.pad ? 'pad' : null);
      tr.appendChild(el('td', 'num', f.offset));
      tr.appendChild(el('td', 'num', f.size));
      const name = el('td');
      name.appendChild(el('div', 'fname', f.name));
      if (f.note) name.appendChild(richText(el('div', 'fnote'), f.note));
      tr.appendChild(name);
      tr.appendChild(el('td', 'live', f.live ? String(f.live(ctx || {})) : ''));
      t.appendChild(tr);
    }
    wrap.appendChild(t);
    return wrap;
  }

  _layoutTable(def, lines) {
    const wrap = el('div', 'bytes');
    wrap.appendChild(el('div', 'section', `${def.id} — ${def.size} bytes, ${lines.length} cache line(s)`));
    const t = el('table', 'fields');
    const head = el('tr');
    for (const h of ['line', 'bytes', 'writers', '']) head.appendChild(el('th', null, h));
    t.appendChild(head);
    for (const l of lines) {
      const tr = el('tr');
      tr.appendChild(el('td', 'num', l.line));
      tr.appendChild(el('td', 'num', `${l.line * 64}-${l.line * 64 + 63}`));
      tr.appendChild(el('td', null, l.writers.length ? l.writers.join(' + ') : '(padding only)'));
      tr.appendChild(el('td', l.falseSharing ? 'bad' : 'ok', l.falseSharing ? 'FALSE SHARING' : 'ok'));
      t.appendChild(tr);
    }
    wrap.appendChild(t);
    return wrap;
  }

  _genericTable({ title, head, rows, note }) {
    const wrap = el('div', 'bytes');
    if (title) wrap.appendChild(el('div', 'section', title));
    const t = el('table', 'fields');
    const hr = el('tr');
    for (const h of head) hr.appendChild(el('th', null, h));
    t.appendChild(hr);
    for (const r of rows) {
      const tr = el('tr', r.cls || null);
      for (const c of r.cells || r) tr.appendChild(el('td', null, c));
      t.appendChild(tr);
    }
    wrap.appendChild(t);
    if (note) wrap.appendChild(el('p', 'fineprint', note));
    return wrap;
  }

  /* ---------------------------------------------------------- controls */
  /*
   * specs: [{ type: 'buttons', label?, items: [{ label, active, onClick }] }
   *         { type: 'toggle', label, value, onChange }
   *         { type: 'slider', label, min, max, step, value, fmt, onInput }
   *         { type: 'text', text }]
   */
  setControls(specs) {
    const root = $('controls');
    root.replaceChildren();
    root.classList.toggle('hidden', !specs.length);
    for (const s of specs) {
      const group = el('div', 'ctl');
      if (s.label && s.type !== 'toggle') group.appendChild(el('span', 'ctl-label', s.label));
      if (s.type === 'buttons') {
        for (const it of s.items) {
          const b = el('button', it.active ? 'chip active' : 'chip', it.label);
          b.addEventListener('click', it.onClick);
          group.appendChild(b);
        }
      } else if (s.type === 'toggle') {
        const b = el('button', s.value ? 'chip toggle active' : 'chip toggle', `${s.label}: ${s.value ? 'ON' : 'OFF'}`);
        b.addEventListener('click', () => s.onChange(!s.value));
        group.appendChild(b);
      } else if (s.type === 'slider') {
        const input = el('input');
        input.type = 'range';
        input.min = s.min; input.max = s.max; input.step = s.step || 1; input.value = s.value;
        const out = el('span', 'ctl-val', s.fmt ? s.fmt(s.value) : s.value);
        input.addEventListener('input', () => {
          const v = Number(input.value);
          out.textContent = s.fmt ? s.fmt(v) : v;
          s.onInput(v);
        });
        group.appendChild(input);
        group.appendChild(out);
      } else if (s.type === 'text') {
        group.appendChild(richText(el('span', 'ctl-text'), s.text));
      }
      root.appendChild(group);
    }
  }

  /* ------------------------------------------------------- live status */
  setSource({ source, detail }) {
    const pill = $('source-pill');
    pill.className = `pill ${source}`;
    pill.textContent = source === 'live' ? 'LIVE' : source === 'demo' ? 'DEMO DATA' : 'CONNECTING';
    pill.title = detail || '';
    $('source-detail').textContent =
      source === 'live' ? `streaming from ${detail}`
        : source === 'demo' ? 'simulated feed - start `receiver --telemetry` + `telemetry_bridge` for real data'
          : '';
  }

  updateStats(s) {
    const set = (id, v) => { const e = $(id); if (e) e.textContent = v; };
    set('s-received', s.received);
    set('s-processed', s.processed);
    set('s-gaps', s.gaps);
    set('s-recovered', s.recovered);
    set('s-queue_full', s.queue_full);
    set('s-queue_depth', s.queue_depth);
    set('s-avg', `${((s.avg_latency_ns || 0) / 1000).toFixed(1)} µs`);
    set('s-max', `${((s.max_latency_ns || 0) / 1000).toFixed(1)} µs`);
  }

  updateConfig(c) {
    const set = (id, v, on) => {
      const e = $(id);
      if (!e) return;
      e.textContent = v;
      e.className = on === undefined ? 'v' : on ? 'v on' : 'v off';
    };
    set('c-transport', c.transport === 'multicast' ? `multicast ${c.multicast_group}` : 'unicast', true);
    set('c-queue', `${c.queue_kind} x${c.workers}`);
    set('c-batch', c.batch > 1 ? `recvmmsg x${c.batch}` : 'off', c.batch > 1);
    set('c-busy', c.busy_poll_us >= 0 ? `${c.busy_poll_us} µs` : 'off', c.busy_poll_us >= 0);
    set('c-bpf', c.bpf_filter ? 'attached' : 'off', !!c.bpf_filter);
    set('c-recovery', c.recovery_enabled ? 'TCP on' : 'off', !!c.recovery_enabled);
  }

  /* Narration line above the controls (the network view's step-by-step). */
  caption(text) {
    const c = $('caption');
    c.textContent = text || '';
    c.classList.toggle('hidden', !text);
  }

  /* ----------------------------------------------------------- tooltip */
  tooltip(text, event) {
    const tip = $('tooltip');
    if (!text || !event) {
      tip.classList.add('hidden');
      return;
    }
    tip.textContent = text;
    tip.classList.remove('hidden');
    tip.style.left = `${event.clientX + 16}px`;
    tip.style.top = `${event.clientY + 14}px`;
  }

  /* ------------------------------------------------------------- bench */
  _renderBench() {
    const text = $('bench-input').value;
    const re = /(mutex|spsc|disruptor)\s+count=(\d+)\s+p50=\s*(\d+)ns\s+p99=\s*(\d+)ns\s+p99\.9=\s*(\d+)ns\s+max=\s*(\d+)ns/g;
    const rows = [];
    let m;
    let maxVal = 1;
    while ((m = re.exec(text)) !== null) {
      const row = { name: m[1], p50: +m[3], p99: +m[4], p999: +m[5], max: +m[6] };
      rows.push(row);
      maxVal = Math.max(maxVal, row.max);
    }
    const out = $('bench-bars');
    out.replaceChildren();
    if (!rows.length) {
      out.appendChild(el('p', 'fineprint', 'No queue_bench lines recognised - paste the full output of ./bin/queue_bench.'));
      return;
    }
    for (const row of rows) {
      for (const k of ['p50', 'p99', 'p999', 'max']) {
        const r = el('div', 'bar-row');
        r.appendChild(el('span', 'bar-label', `${row.name} ${k === 'p999' ? 'p99.9' : k}`));
        const track = el('div', 'bar-track');
        const fill = el('div', 'bar-fill');
        fill.style.width = `${Math.max(2, (Math.log10(row[k] + 1) / Math.log10(maxVal + 1)) * 100)}%`;
        track.appendChild(fill);
        r.appendChild(track);
        r.appendChild(el('span', 'bar-val', `${row[k].toLocaleString()} ns`));
        out.appendChild(r);
      }
    }
    out.appendChild(el('p', 'fineprint', 'Log scale - tails are where queues differ.'));
  }
}
