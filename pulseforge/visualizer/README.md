# PulseForge visualizer

A drill-down 3D view of the pipeline. Every click goes one layer deeper,
and every layer has a lesson panel: what it is, why HFT cares, typical
latency, where PulseForge implements it, a command to try, and an
interview question.

```
Overview ─┬─ click a machine ─► Machine view
          │                      0 chassis ─► 1 motherboard ─► 2 CPU die ─► 3 cache line
          └─ click a cable ───► Network stack view
                                 app ─► socket ─► UDP/TCP ─► IP ─► Ethernet ─► driver ─► NIC ─► PHY ─► wire
```

## Run it

```sh
make && make topology                 # build + export YOUR machine's CPU/cache/NIC layout
cd visualizer && python3 -m http.server 8080
# open http://localhost:8080  (must be http://, not file://)
```

Without a backend it runs on a clearly-labelled **DEMO** feed. Start
`./bin/receiver --telemetry` and `./bin/telemetry_bridge` and it flips to
**LIVE** on its own.

Deep links: `?view=machine&role=B&level=3`, `?view=link&id=feed`,
`?ws=ws://host:9700`.

## How the code is organised

Read in this order:

| File | What it teaches |
|---|---|
| `js/data/lessons.js` | All teaching text. Readable on its own, top to bottom. |
| `js/data/protocols.js` | Byte-exact Ethernet/IPv4/UDP/TCP/WebSocket headers; multicast MAC mapping; serialization delay. |
| `js/data/layouts.js` | PulseForge's C++ structs mapped onto 64-byte cache lines; false-sharing detection. |
| `js/data/hardware.js` | Loads `data/hw_topology.json` (from `src/hw_topology.cpp`); suggested thread placement. |
| `js/views/View.js` | The contract every screen implements. |
| `js/views/OverviewView.js` | Three machines, live packets driven by real events. |
| `js/views/MachineView.js` | The layer-peeling hardware view. |
| `js/views/NetworkStackView.js` | Encapsulation animation, kernel-bypass mode. |
| `js/engine/*` | Renderer, bloom, camera flights, picking, tweens, 3D building blocks. |
| `js/telemetry/FeedHub.js` | WebSocket client + the DEMO feed. |
| `js/ui/Hud.js` | All 2D UI. Writes data with `textContent` only. |

Data, views, engine and UI are separate on purpose: lesson content never
contains rendering code, and views never touch the DOM.

## Extending it

- **New lesson**: add an entry to `LESSONS` in `js/data/lessons.js`, then
  `app.hud.showLesson('your-id')` from any view.
- **New drill-down screen**: subclass `View`, build into `this.scene`,
  register clickable meshes with `this.addPickable(mesh, { kind, tip })`,
  and push it with `app.nav.push(new YourView(app))`.
- **C++ struct changed?** Update its entry in `js/data/layouts.js` (the
  `static_assert`s in the headers guard the C++ side).
