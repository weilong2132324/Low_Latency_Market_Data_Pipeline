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
make            # bin/publisher, receiver, monitor, exchange, trader, labs, telemetry_bridge
make test       # build + run all 13 unit test suites
make bin/queue_bench
make debug | make asan | make tsan   # alternate builds
make clean
```

**Trading loop: exchange + trader (order book, matching, order entry, tick-to-trade):**
```sh
bash scripts/run_trading.sh            # 15 s, both sides block in epoll_wait
bash scripts/run_trading.sh spin       # both sides busy-poll, pinned to CPUs 1 and 2
bash scripts/run_trading.sh gaps       # exchange drops packets; watch gap -> re-snapshot
# or by hand, two terminals:
./bin/exchange
./bin/trader --duration 20 --verbose
make bench-book       # std::map order book vs the ladder book
make bench-protocol   # OUCH binary vs FIX text encode/decode
```

**Study guide:** [`pulseforge/docs/STUDY_GUIDE.md`](pulseforge/docs/STUDY_GUIDE.md) -
the whole codebase as a course, file by file and concept by concept.

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

**Telemetry + 3D visualizer** (see `pulseforge/visualizer/README.md`):
```sh
make topology   # export your real CPU/cache/NIC layout for the machine view
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

`pulseforge/visualizer/` - a static Three.js app (no build step) that is
also a course: every click drills one layer deeper and opens a lesson
(what it is, why HFT cares, typical latency, where PulseForge implements
it, a command to try, interview questions with answers).

- **Overview**: three machines (exchange gateway, trading receiver,
  monitoring host) with live ticks flowing over the multicast feed, into
  the queue's ring buffer and out to individual workers.
- **Open a machine** (click it): chassis &rarr; motherboard (CPU socket,
  DRAM, PCIe + NIC, NUMA node) &rarr; CPU die (your real cores and
  L1/L2/L3 from `make topology`, with suggested thread placement)
  &rarr; a single 64-byte cache line showing a real PulseForge struct
  coloured by writer thread. False sharing is drawn as a line bouncing
  between cores; a toggle deletes SharedStats' padding to show why it's
  there.
- **Open a cable** (click it): its protocol stack. A packet descends the
  sender's stack gaining real headers (UDP 8 B, IP 20 B, Ethernet 14 B +
  FCS), crosses the wire and a switch, and climbs the receiver's stack
  shedding them. Click any layer for its byte layout filled with this
  run's values (multicast group, derived multicast MAC, TTL, ports). A
  **kernel bypass** toggle shows exactly which layers DPDK/ef_vi skip.
- **Live or demo**: streams from `telemetry_bridge` when it's running,
  otherwise runs on a clearly-labelled DEMO feed, and flips to LIVE
  automatically when the backend appears.

```sh
make && make topology
./bin/receiver --telemetry --port 9000
./bin/telemetry_bridge
./bin/publisher --port 9000 --rate 2000 --count 50000 --drop-every 17
# serve the folder - must be http://, not file:// (ES modules)
cd visualizer && python3 -m http.server 8080
# open http://localhost:8080/
```

The visualizer has already paid for itself once: it exposed a real bug
where `recvmmsg()` without `MSG_WAITFORONE` waited for a full batch,
pushing average latency from ~50 us to 19 ms at low message rates (see
`src/receiver.cpp` and the socket-layer lesson).

`pulseforge/visualizer/README.md` maps the visualizer's own code.

## Trading loop (Phase 6)

Two programs close the loop from market data to an order and back:

```
 bin/exchange (one thread, one epoll loop)            bin/trader (one thread)
 ┌──────────────────────────────────────┐   ITCH/MoldUDP64   ┌──────────────────────────┐
 │ synthetic flow ─┐                    │ ─── multicast ───▶ │ feed handler (gaps, seq)  │
 │ OUCH gateway ───┼─▶ matching engine  │  239.255.0.2:9200  │  └▶ replica OrderBook     │
 │  (TCP :9100)    │   price-time FIFO  │                    │      └▶ imbalance strategy│
 │ snapshot        │   OrderBook/symbol │ ◀── OUCH/SoupBin ── │          └▶ risk gate     │
 │  (TCP :9101) ◀──┘                    │      TCP :9100     │              └▶ order out │
 └──────────────────────────────────────┘                    └──────────────────────────┘
```

- **Order book** (`include/order_book.hpp`): price ladder array +
  occupancy bitmap scanned with `clz`/`ctz` + pooled intrusive FIFO per
  level + open-addressing `FlatIdMap`. No allocation after construction.
- **Matching engine** (`include/matching_engine.hpp`): price-time
  priority, trades at the maker's price, IOC vs Day, self-match
  prevention, venue-side validation; emits events to an `EngineListener`.
- **Protocols**: ITCH 5.0 message layouts in MoldUDP64 packets
  (`itch.hpp`), SoupBinTCP framing (`soupbin.hpp`), OUCH-5.0-style order
  entry (`ouch.hpp`), a FIX 4.4 encoder/zero-copy parser (`fix.hpp`), all
  on explicit big-endian helpers (`wire.hpp`).
- **Trader**: subscribe → snapshot → replay (`feed_handler.hpp`); on a
  sequence gap the book goes stale, open orders are pulled and the book
  is rebuilt from a fresh snapshot. Pre-trade `RiskGate` (`risk.hpp`):
  kill switch, fat-finger qty, notional, price collar, worst-case
  position, open orders, rolling-window order rate.
- **Kernel timestamps** (`net_timestamp.hpp`): `SO_TIMESTAMPING` software
  RX stamps on market data and TX stamps (via the socket error queue) on
  orders, so the report splits tick-to-trade into stages.

Example report from WSL2, `run_trading.sh spin` (numbers vary by machine):

```
latency in ns                                    count      p50      p90      p99
  exchange event -> kernel RX                       9004    12491    19891    70373
  kernel RX -> user space (wakeup + recvmsg)        9137    10569    18233   180915
  user space -> book updated (decode+apply)         9137     1327     2373     6010
  book updated -> send() (strategy+risk+enc)         780     1422     4892    10076
  TICK-TO-TRADE: kernel RX -> send() returned        780    39613    82661   484753
  send() start -> ack in user space (RTT)            780    46003   102472   523502
```

In blocking mode the "kernel RX -> user space" row is ~97 us p50 (the
scheduler wakeup); busy-polling cuts it to ~11 us. Benchmarks from the
same machine: the ladder book is ~3.6x the throughput of a `std::map`
book; FIX is 3.1x the bytes, ~40x slower to encode and ~15x slower to
decode than the binary order.

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

Phases 0-6 are built: core concurrency primitives,
multicast/kernel-adjacent networking, the telemetry bridge, the 3D
visualizer, and the trading loop (order book, matching engine, ITCH /
OUCH / FIX, risk, SO_TIMESTAMPING tick-to-trade). See `docs/` for the
write-ups behind each, starting with `docs/STUDY_GUIDE.md`.

## Warning

This is a **learning lab**, not production trading software. It
deliberately contains simplified design, synthetic data, and (in later
phases) opt-in demonstrations of bugs for debugging practice. Never use
it to handle real money or real market data.
