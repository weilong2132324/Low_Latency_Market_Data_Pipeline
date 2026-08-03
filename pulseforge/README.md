# PulseForge

A small but serious **C learning codebase**: a miniature low-latency
market-data pipeline and debugging lab. Built for **educational purposes
only** - this is **not** production trading software.

- **Language:** C11
- **Platform:** Linux / POSIX (sockets, pthreads, mmap, fork, epoll)
- **Build:** `gcc` + `make` only, no external libraries
- **Warnings are errors:** `-Wall -Wextra -Wpedantic -Werror`

> Running on Windows? Use **WSL (Ubuntu)** - every API used here is
> available inside WSL2.

## Architecture

```
 UDP publisher ──UDP──> receiver ──> bounded queue ──> worker thread(s)
                                │        (mutex+condvar OR lock-free SPSC)
                                │
                                └──> shared memory (mmap + atomics) ──> monitor
```

**Phase 2 (current):** publisher + receiver over UDP on `127.0.0.1`,
a mutex + condition-variable bounded queue, worker threads, and a
separate monitor process that reads shared-memory statistics.

## Prerequisites

```sh
sudo apt update && sudo apt install -y build-essential
# optional, for later phases: valgrind
```

Verify:

```sh
gcc --version   # 12+ recommended
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
| `--queue mutex\|spsc` | queue backend (`mutex` now; `spsc` in Phase 4) |
| `--workers N` | consumer threads (default 1) |
| `--pin-cpu CPU` | pin the receiver thread to a CPU |
| `--slow-consumer-us US` | artificial worker delay per msg (fault injection) |
| `--shared-memory-name NAME` | shm object (default `/pulseforge_stats`) |

## Monitor options

```sh
./bin/monitor --shared-memory-name /pulseforge_stats
```

## Sanitizer builds

```sh
make asan   # AddressSanitizer build
make tsan   # ThreadSanitizer build (useful from Phase 3 on)
```

## What is next

- **Phase 3:** deadlock and data-race labs, ASan/TSan/Helgrind targets,
  debugging documentation.
- **Phase 4:** C11-atomic lock-free SPSC ring, memory-ordering notes,
  queue comparison scripts.
- **Phase 5:** Disruptor-style fan-out, epoll/eventfd/timerfd,
  CPU-affinity and `perf` exercises.
- **Phase 6 (on request):** huge pages, mlockall, eBPF, DPDK concepts.

## Warning

This is a **learning lab**, not production trading software. It
deliberately contains simplified design, synthetic data, and (in later
phases) opt-in demonstrations of bugs for debugging practice. Never use
it to handle real money or real market data.
