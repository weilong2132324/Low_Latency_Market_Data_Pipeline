/*
 * main.js - entry point. Builds the shared `app` object every view
 * receives, and pushes the first view.
 *
 *   app.engine   renderer, camera, picking          (engine/Engine.js)
 *   app.nav      view stack + breadcrumb            (engine/Navigator.js)
 *   app.hud      all 2D UI                          (ui/Hud.js)
 *   app.feed     live WebSocket or DEMO data        (telemetry/FeedHub.js)
 *   app.topo     raw hardware topology              (data/hardware.js)
 *   app.hw       topology summarised per core
 *   app.open     shortcuts to drill into a machine or a link
 *
 * window.__pf exposes `app` for poking at it from the browser console.
 */
import { Engine } from './engine/Engine.js';
import { Navigator } from './engine/Navigator.js';
import { Hud } from './ui/Hud.js';
import { FeedHub } from './telemetry/FeedHub.js';
import { loadTopology, summarize } from './data/hardware.js';
import { OverviewView } from './views/OverviewView.js';
import { MachineView } from './views/MachineView.js';
import { NetworkStackView } from './views/NetworkStackView.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  /* Labels are drawn into canvases; wait for the webfont so they don't
   * render in a fallback font (but never block startup on it). */
  await Promise.race([document.fonts.ready, sleep(1500)]);

  const hud = new Hud();
  const engine = new Engine(document.getElementById('scene'));
  const { topo, real, reason } = await loadTopology();
  if (!real) console.info('[PulseForge] using the generic hardware model:', reason, '- run `make topology`');

  const params = new URLSearchParams(location.search);
  const wsUrl = params.get('ws') || document.getElementById('ws-url').value;
  document.getElementById('ws-url').value = wsUrl;

  const app = { engine, hud, topo, topoReal: real, hw: summarize(topo) };
  app.feed = new FeedHub(wsUrl);
  app.nav = new Navigator(app);
  app.open = {
    machine: (role) => app.nav.push(new MachineView(app, role)),
    link: (id) => app.nav.push(new NetworkStackView(app, id)),
  };

  hud.onConnect = (url) => app.feed.connect(url);
  app.feed.on('source', (s) => hud.setSource(s));
  app.feed.on('stats', (s) => hud.updateStats(s));
  app.feed.on('config', (c) => hud.updateConfig(c));
  hud.setSource({ source: app.feed.source, detail: wsUrl });

  engine.onHover = (pick, event) => hud.tooltip(pick && pick.tip, event);
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') app.nav.back();
  });

  app.nav.push(new OverviewView(app));

  /* Deep links for sharing a view: ?view=machine&role=B&level=3 or ?view=link&id=feed */
  const view = params.get('view');
  if (view === 'machine') {
    app.open.machine(params.get('role') || 'B');
    const level = Number(params.get('level') || 0);
    if (level) app.nav.current.setLevel(level);
  } else if (view === 'link') {
    app.open.link(params.get('id') || 'feed');
  }

  window.__pf = app;
}

main().catch((e) => {
  console.error(e);
  if (window.showFatalError) window.showFatalError(e.stack || e.message);
});
