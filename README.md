# PulseForge

A small but serious **C++ learning codebase**: a miniature low-latency
market-data pipeline and debugging lab. Built for **educational purposes
only** - this is **not** production trading software.

- **Language:** C++17 (`std::atomic`, `std::thread`, `std::mutex`,
  `std::condition_variable`, RAII)
- **Platform:** Linux / POSIX (sockets, pthreads, mmap, fork, epoll)
- **Build:** `g++` + `make` only, no external libraries
- **Warnings are errors:** `-Wall -Wextra -Wpedantic -Werror`

## Quick start (all commands)

Everything here runs inside WSL2/Linux (`cd pulseforge` first). Each
block is independent - run just the ones you want.

**Build:**
```sh
make            # bin/publisher, receiver, monitor, deadlock_demo, race_demo, telemetry_bridge
make test       # build + run all unit tests
make bin/queue_bench
make debug | make asan | make tsan   # alternate builds
make clean
```

**Basic pipeline (3 terminals):**
```sh
./bin/receiver --port 9000 --queue mutex --workers 1
./bin/monitor --shared-memory-name /pulseforge_stats
./bin/publisher --port 9000 --rate 10000 --count 10000 --drop-every 7
# or, one-shot: bash scripts/run_basic.sh
```

**Queue backend comparison bench:**
```sh
./bin/queue_bench --count 200000 --workers 2
```

**Multicast (one publisher, many receivers):**
```sh
./bin/receiver  --port 9200 --transport multicast --group 239.255.0.5 --shared-memory-name /pf_a &
./bin/receiver  --port 9200 --transport multicast --group 239.255.0.5 --shared-memory-name /pf_b &
./bin/publisher --port 9200 --transport multicast --group 239.255.0.5 --rate 5000 --count 5000
```

**TCP gap recovery:**
```sh
./bin/receiver  --snapshot-host 127.0.0.1 --snapshot-port 9501 --port 9500 --shared-memory-name /pf_r
./bin/publisher --snapshot-port 9501 --port 9500 --rate 500 --count 500 --drop-every 23
```

**Telemetry + 3D visualizer:**
```sh
# shows networking options (multicast, batch, busy-poll, bpf-filter,
# TCP recovery) live in the visualizer's Networking panel - drop any
# of these flags to see that row fall back to "off":
./bin/receiver --telemetry --port 9000 --transport multicast --group 239.255.0.5 \
               --batch 16 --busy-poll 50 --bpf-filter \
               --snapshot-host 127.0.0.1 --snapshot-port 9501
./bin/telemetry_bridge
./bin/publisher --port 9000 --transport multicast --group 239.255.0.5 \
                --snapshot-port 9501 --rate 2000 --count 50000 --drop-every 17
# serve the visualizer - do NOT just double-click/open index.html:
# browsers block the ES module imports it needs on file:// pages
cd visualizer && python3 -m http.server 8080
# then open http://localhost:8080/ in a browser
```

**Debugging labs:**
```sh
./bin/deadlock_demo
./bin/race_demo
make tsan && ./bin/race_demo       # catch the race with ThreadSanitizer
make helgrind                       # prints the valgrind invocation
```

## Architecture

![PulseForge architecture diagram](image/image.png)

**Phase 1 (current)** is the full pipeline above, built from four
moving parts:

1. **UDP publisher** (`bin/publisher`) - synthesizes fixed-size
   `TickMessage`s (a 40-byte struct: sequence, send timestamp, symbol,
   price, quantity, flags) and sends them over UDP on `127.0.0.1`. It
   paces to a target rate (best effort), can deliberately *skip* every
   Nth sequence (`--drop-every`) to simulate packet loss, and can send
   in bursts (`--burst`) within each pacing step.

2. **UDP receiver** (`bin/receiver`) - binds the loopback socket
   (`SO_REUSEADDR` + a 4 MiB `SO_RCVBUF`; the kernel socket buffer acts
   as the *first* queue ahead of the bounded one). It validates each
   datagram (exact 40-byte size, then `message_is_valid()`: nonzero
   quantity, known flag bits), detects **sequence gaps** and **reorders**
   against the last-seen sequence, then enqueues the tick into a bounded
   queue with backpressure.

3. **Queue backend + workers** - the receiver's single producer handle
   and N worker threads' consumer handles come from one factory,
   `make_queue_backend()` (`include/queue_backend.hpp`), which hides
   which of three concrete queues is actually running behind `--queue`:
   a mutex+condvar FIFO (`queue_mutex.hpp`), a lock-free SPSC ring
   (`spsc_ring.hpp`), or a Disruptor-style broadcast ring
   (`mpmc_fanout.hpp`) - see "Queue backends" below. Workers fold each
   message into the shared statistics (processed count, latency
   total/max, queue-depth gauge) and may be throttled with
   `--slow-consumer-us` as fault injection. Shutdown is a clean
   "close the queue -> drain remaining items -> join workers" sequence
   regardless of which backend is active.

4. **Shared-memory monitor** (`bin/monitor`) - a *separate process* that
   opens the stats region (`shm_open` + `mmap MAP_SHARED`) and renders a
   live dashboard once per second. The receiver is the **owner**
   (creates and unlinks the object); the monitor is a guest
   (open/close only, never unlinks).

Cross-cutting Phase 1 concerns:

- Every counter that crosses threads or processes is a C++17
  `std::atomic<uint64_t>`. Receiver-written and worker-written counters
  are padded onto separate cache lines to avoid **false sharing**, and a
  runtime check verifies the atomics are truly lock-free on this
  platform.
- Signal handling deliberately omits `SA_RESTART` so a blocking
  `recvfrom()` is interrupted (returns `EINTR`), letting the receiver
  observe `Ctrl-C` and shut down cleanly instead of spinning or hanging.
- All sockets, threads, and shared-memory resources are owned by RAII
  wrappers (`Socket`, `SharedStatsRegion`, `std::thread`, `QueueMutex`)
  from `common.hpp`/`shared_stats.hpp`. `CHECK()` throws a `SystemError`
  on syscall failure; a top-level `catch` in `main()` reports it and
  returns an exit code. Stack unwinding runs the destructors, so every
  exit path cleans up - no `goto cleanup` needed.

## Prerequisites

```sh
sudo apt update && sudo apt install -y build-essential
# optional, for later phases: valgrind
```

Verify:

```sh
g++ --version   # 12+ recommended (C++17 support)
make --version
```

## Build

```sh
make          # release build -> bin/publisher, bin/receiver, bin/monitor
make debug    # -O0 -g3 with assertions
make test     # build and run the unit tests
make clean
```

> Tip: if you switch between `make`, `make asan`, `make tsan`, run
> `make clean` first so stale objects are not reused.

## Run (three terminals)

Terminal A - receiver (mutex queue, one worker):

```sh
./bin/receiver --port 9000 --queue mutex --workers 1
```

Terminal B - monitor (separate process, refreshes every second):

```sh
./bin/monitor --shared-memory-name /pulseforge_stats
```

Terminal C - publisher (10,000 ticks, dropping every 7th):

```sh
./bin/publisher --port 9000 --rate 10000 --count 10000 --drop-every 7
```

Stop the receiver and monitor with `Ctrl-C`.

There is also a one-shot script that runs all three:

```sh
bash scripts/run_basic.sh
```

### Expected output (from a real run)

Receiver:

```
PulseForge receiver (queue=mutex, workers=1, shm=/pulseforge_stats)
  listening on 127.0.0.1:9000  (Ctrl-C to stop)

Closing queue; draining 1 worker(s)...
  worker 0: processed 8572  avg latency NNN ns  max latency NNN ns
Receiver summary:
  received    8572
  invalid     0
  gaps        1428
  reorders    0
  queue full  0
  processed   8572
  avg latency X.X us   max latency Y.Y us
  runtime     X.XXX s
```

Monitor (one dashboard frame):

```
PulseForge monitor  [shm /pulseforge_stats]  (Ctrl-C to exit)

  received        8572   (+8572/s)
  invalid            0
  gaps            1428
  reorders           0
  queue full         0
  queue depth        0
  processed       8572   (+8572/s)
  avg latency      X.X us
  max latency     Y.Y us
```

(`10000 / 7` sequences are intentionally dropped, so gaps = 1428.
Latency/timing numbers vary per run.)

## Publisher options

```sh
./bin/publisher --port 9000 --rate 100000 --count 1000000 \
                --symbol 7 --drop-every 0 --burst 16
```

| Option | Meaning |
|---|---|
| `--port PORT` | UDP destination port |
| `--rate MPS` | average messages/second (best effort) |
| `--count N` | sequence numbers to generate |
| `--symbol ID` | synthetic symbol id in each tick |
| `--drop-every N` | skip every Nth sequence (simulates loss; `0` = off) |
| `--burst SIZE` | datagrams sent per pacing step |

## Receiver options

```sh
./bin/receiver --port 9000 --queue mutex --workers 2 \
               --pin-cpu 2 --slow-consumer-us 200 \
               --shared-memory-name /pulseforge_stats
```

| Option | Meaning |
|---|---|
| `--port PORT` | UDP port to bind |
| `--queue mutex\|spsc\|disruptor` | queue backend (see "Queue backends" below) |
| `--workers N` | consumer threads (default 1) |
| `--pin-cpu CPU` | pin the receiver thread to a CPU |
| `--slow-consumer-us US` | artificial worker delay per msg (fault injection) |
| `--shared-memory-name NAME` | shm object (default `/pulseforge_stats`) |

## Monitor options

```sh
./bin/monitor --shared-memory-name /pulseforge_stats
```

## Queue backends (Phase 2)

Three interchangeable backends sit behind one factory
(`include/queue_backend.hpp` / `make_queue_backend()`), picked with
`--queue`:

| Kind | Shape | Concurrency | Notes |
|---|---|---|---|
| `mutex` | work-sharing | MPSC-safe, any `--workers` | `std::mutex` + 2 condvars (`queue_mutex.hpp`); blocking pop. |
| `spsc` | work-sharing | **single consumer only** (`--workers 1`) | Lock-free ring (`spsc_ring.hpp`); consumer busy-spins + yields instead of blocking. |
| `disruptor` | **broadcast** | any `--workers`, each sees every message | Single-writer multi-reader ring (`mpmc_fanout.hpp`); `processed` in the summary is `workers x received`, not a shard. |

`bin/queue_bench` drives the same synthetic workload through all three
and prints p50/p99/p99.9/max latency side by side:

```sh
make bin/queue_bench
./bin/queue_bench --count 200000 --workers 2
```

## Multicast + latency-shaving (Phase 2)

Both publisher and receiver support `--transport unicast|multicast`.
One publisher, many independent receiver *processes*, no per-subscriber
send cost on the publisher:

```sh
./bin/receiver  --port 9200 --transport multicast --group 239.255.0.5 --shared-memory-name /pf_a &
./bin/receiver  --port 9200 --transport multicast --group 239.255.0.5 --shared-memory-name /pf_b &
./bin/publisher --port 9200 --transport multicast --group 239.255.0.5 --rate 5000 --count 5000
```

Latency-shaving flags on the receiver (each documented in
`docs/kernel_bypass_next_steps.md` for what it buys on real hardware
vs. this dev environment):

| Flag | What it does |
|---|---|
| `--batch N` | `recvmmsg()`: N datagrams per syscall instead of 1 |
| `--busy-poll US` | `SO_BUSY_POLL` (soft-fails cleanly where unsupported) |
| `--bpf-filter` | classic-BPF filter drops invalid datagrams in-kernel |

## TCP gap recovery (Phase 2)

The live tick feed stays UDP (fast, lossy); repairing a detected
sequence gap uses a separate TCP channel - this is the "why TCP vs UDP"
story made concrete. The publisher can run a small TCP server
remembering recently-generated messages; on a gap, the receiver issues
an async (`std::async`/`std::future`) recovery request without blocking
its own receive loop, and only the receive-loop thread ever pushes into
the queue (preserving the SPSC/Disruptor single-writer invariant):

```sh
./bin/receiver  --snapshot-host 127.0.0.1 --snapshot-port 9501 --port 9500 --shared-memory-name /pf_r
./bin/publisher --snapshot-port 9501 --port 9500 --rate 500 --count 500 --drop-every 23
```

The receiver's summary prints a `recovered` line; `monitor` shows it
live. See `include/snapshot_protocol.hpp` for the wire format and
`src/receiver.cpp`'s `kMaxPendingRecoveries` comment for a real lesson
in why an unbounded "one recovery thread per gap" is a thundering-herd
bug waiting to happen under heavy loss.

## Telemetry + visualizer feed (Phase 3)

The receiver can record every pipeline event (received, gap, recovered,
processed, invalid, queue-full) into a second shared-memory ring;
`telemetry_bridge` opens it (guest-only, like `monitor`) and streams it
as JSON over a hand-rolled WebSocket server - no third-party WebSocket
or JSON library, matching this project's "no external deps" rule
(`include/ws_server.hpp`, `include/sha1.hpp`).

```sh
./bin/receiver --telemetry --port 9000 --shared-memory-name /pf_stats --events-shm-name /pf_events
./bin/telemetry_bridge --shared-memory-name /pf_stats --events-shm-name /pf_events --ws-port 9700
./bin/publisher --port 9000 --rate 2000 --count 20000 --drop-every 13
```

Then serve and open `visualizer/index.html` (Phase 4, below - don't
open it as a bare `file://` URL), or connect any WebSocket client to
`ws://127.0.0.1:9700` directly - each frame is
`{"stats": {...}, "events": [...]}`.

## 3D visualizer (Phase 4)

`pulseforge/visualizer/index.html` - a single static file (Three.js
pinned via CDN, no build step, no React) that connects to
`telemetry_bridge`'s WebSocket feed and renders the live pipeline:

- **Packets flowing**: a small glowing sphere spawns per event and
  travels along the real pipeline path (publisher &rarr; network &rarr;
  receiver &rarr; queue &rarr; workers &rarr; telemetry), colored by
  event type (white = normal, red = gap, blue = recovered via the TCP
  channel, yellow = queue-full). A "time dilation" slider controls
  playback speed since real latencies are microseconds - too fast to
  watch directly.
- **Hot vs. cold path**: receiver/queue/workers render in warm orange
  (the latency-critical path); telemetry/recovery render in cool blue
  (observability, off the critical path) - the literal hot/cold-path
  requirement, rendered as color.
- **Click-to-inspect, with a deep-dive layer**: click any node for a
  live side panel - current stats, a one-paragraph description, the
  source file it maps to, and (for network/receiver/queue/recovery) the
  *actual* live config this run chose. Click an individual worker
  sphere or queue slot cube for THAT worker's own processed/latency
  numbers or that slot's fill state - not just the pool-wide aggregate.
- **Networking panel**: a dedicated panel (bottom-left on connect, or
  the reference flags before connecting) showing which transport
  (unicast/multicast + group), `recvmmsg` batch size, `SO_BUSY_POLL`,
  BPF filter, and TCP recovery are actually active on the connected
  receiver - read live from a `SharedConfig` block the receiver
  publishes once at startup, not guessed or hardcoded.
- **Latency-reduction view**: paste `./bin/queue_bench`'s real output
  into the bottom-left panel to render its p50/p99/p99.9/max bars per
  backend - actual measured numbers, not illustrative ones.

```sh
# three terminals + a browser:
./bin/receiver --telemetry --port 9000
./bin/telemetry_bridge
./bin/publisher --port 9000 --rate 2000 --count 50000 --drop-every 17
# serve the visualizer folder (do NOT open index.html directly as a
# file:// URL - browsers block the ES module imports it needs there):
cd visualizer && python3 -m http.server 8080
# then open http://localhost:8080/ in a browser
```

If the page loads but shows a red error banner instead of the scene,
read the banner - it names the exact problem (usually this file://
issue) and gives the fix.

## Debugging labs (Phase 1)

Two small, deliberately-broken standalone binaries for practicing with
a debugger/sanitizer instead of reading about one:

```sh
./bin/deadlock_demo   # two threads lock two mutexes in opposite order and hang
./bin/race_demo       # 8 threads increment a non-atomic counter with no sync
```

See `docs/debugging_labs.md` for the gdb/helgrind/TSan walkthroughs.

## Sanitizer builds

```sh
make asan      # AddressSanitizer build
make tsan      # ThreadSanitizer build - run bin/race_demo under it
make helgrind  # prints the valgrind --tool=helgrind invocations
```

## Project status

All planned phases (0-5) are built: core concurrency primitives,
multicast/kernel-adjacent networking, the telemetry bridge, and the 3D
visualizer. See `docs/` for the write-ups behind each (memory ordering,
debugging labs, kernel-bypass limits in WSL2, huge pages).

## Warning

This is a **learning lab**, not production trading software. It
deliberately contains simplified design, synthetic data, and (in later
phases) opt-in demonstrations of bugs for debugging practice. Never use
it to handle real money or real market data.
