# PulseForge Study Guide

This guide treats the PulseForge repository as a course in the C++,
Linux, networking and market-structure knowledge an HFT engineering
role expects. Each section explains a concept, points to the exact file
where PulseForge implements it, gives you a command to run, and ends
with questions you should be able to answer out loud.

> Everything below was checked against the code in this repository. The
> latency numbers quoted come from real runs on the development machine
> (WSL2, kernel 6.6.87.2-microsoft-standard-WSL2, AMD Ryzen AI 9 365,
> 20 logical CPUs, 48 KiB L1d / 1 MiB L2 per core, 16 MiB L3). Your
> numbers will differ; the *shape* of the results is the lesson.

---

## Contents

- [0. How to use this guide](#0-how-to-use-this-guide)
- [1. The big picture](#1-the-big-picture)
- [2. Foundations used everywhere](#2-foundations-used-everywhere)
- [3. Concurrency: queues, atomics and the memory model](#3-concurrency-queues-atomics-and-the-memory-model)
- [4. Networking: from socket to kernel bypass](#4-networking-from-socket-to-kernel-bypass)
- [5. Telemetry and the 3D visualizer](#5-telemetry-and-the-3d-visualizer)
- [6. The trading loop](#6-the-trading-loop)
- [7. Hardware and OS reference](#7-hardware-and-os-reference)
- [8. Interview question bank](#8-interview-question-bank)
- [9. Exercises: extend PulseForge](#9-exercises-extend-pulseforge)
- [10. Appendix: file map, commands, glossary](#10-appendix-file-map-commands-glossary)

---

## 0. How to use this guide

### 0.1 The study loop

For every section:

1. **Read** the section here.
2. **Open the file** it points to and read the header comment first.
   Every PulseForge file starts with a block explaining its
   responsibility, ownership and thread-safety. Those comments are part
   of the course.
3. **Run** the command given. Watch the numbers.
4. **Break it on purpose.** Change one thing (remove a padding field,
   switch `memory_order_release` to `relaxed`, drop `MSG_WAITFORONE`),
   rebuild and see what happens. Most of the lessons here were learned
   by breaking something.
5. **Explain it out loud** using the questions at the end of the
   section, as if an interviewer had asked. If you can't explain it in
   two minutes without notes, reread.

### 0.2 Suggested order

| Week | Sections | Goal |
|---|---|---|
| 1 | 1, 2, 3.1–3.4 | Build, run, read the mutex queue and SPSC ring until you can rewrite them from memory |
| 2 | 3.5–3.10, 7.1–7.3 | Atomics, false sharing, shared memory, the debugging labs |
| 3 | 4, 7.4–7.6 | The socket path, multicast, batching, timestamps, kernel bypass |
| 4 | 6.1–6.9 | Market structure, wire protocols, the hash map and order book |
| 5 | 6.10–6.18 | Matching engine, exchange, trader, the latency report |
| 6 | 8, 9 | Interview drill; pick two exercises and build them |

### 0.3 Setup and the one-command smoke test

All C++ builds and runs inside WSL2/Linux:

```sh
cd pulseforge
make && make test                      # 9 programs + 13 test suites
bash scripts/run_trading.sh spin 10    # the whole trading loop, 10 s
```

If those three commands work, everything in this guide will work.

---

## 1. The big picture

### 1.1 What a trading system does

At its core an automated trading system is a loop:

```
   exchange publishes        you receive and         you decide         you send          exchange
   a market event      -->   rebuild the market -->  to trade     -->   an order    -->   matches it
   (a "tick")                (the order book)        (strategy+risk)    (order entry)     (fill)
        ^                                                                                    |
        +-------------------------- the fill changes the market -----------------------------+
```

**Tick-to-trade** is the time from the market event reaching you to
your order leaving you. In a race to trade on the same information
(someone else saw the same imbalance, the same stale quote), the faster
loop wins. Nearly every technique in this repository exists to shorten
some part of that loop, or to keep it predictable when the system is
under load (low *tail* latency).

### 1.2 How PulseForge maps onto a real setup

PulseForge grew in phases. Phases 0–5 built a market-data pipeline;
Phase 6 closed the loop with a mock exchange and a trader.

```
Phase 0-5: market-data pipeline (one machine, three processes)

 bin/publisher ──UDP unicast/multicast──> bin/receiver ──queue──> worker threads
   (synthetic ticks,      TickMessage       (validate, gap        (latency stats)
    pacing, loss,                            detect, recover)          │
    TCP snapshot server)                         │                      v
                                                 └──────shm──────> SharedStats ──> bin/monitor
                                                 └──shm event ring──> bin/telemetry_bridge ──WebSocket──> visualizer/

Phase 6: the trading loop (two processes)

 bin/exchange                                                   bin/trader
  synthetic participants ─┐                                      feed handler ─> replica OrderBook
  OUCH order gateway ─────┼─> MatchingEngine ──ITCH/MoldUDP64──>   (gap detect,     │
  snapshot (GLIMPSE-like) ┘   (OrderBook per     multicast          snapshot+replay) v
         ^                     symbol)                                        strategy ─> RiskGate
         └──────────────────────── OUCH over SoupBinTCP (TCP) ─────────────── order entry
```

| Real-world component | PulseForge | Files |
|---|---|---|
| Exchange matching engine | `MatchingEngine` + `OrderBook` | `include/matching_engine.hpp`, `include/order_book.hpp` |
| Exchange market-data feed (NASDAQ TotalView-ITCH) | ITCH 5.0 messages in MoldUDP64 over multicast | `include/itch.hpp`, `src/exchange.cpp` |
| Exchange order entry (NASDAQ OUCH) | OUCH-5.0-style messages over SoupBinTCP | `include/ouch.hpp`, `include/soupbin.hpp` |
| Snapshot/recovery service (NASDAQ GLIMPSE) | snapshot port 9101 | `src/exchange.cpp` (`send_snapshot`) |
| FIX engine (broker/drop-copy connections) | FIX 4.4 encoder and parser | `include/fix.hpp` |
| Feed handler | `SequenceTracker`, `apply_itch` | `include/feed_handler.hpp`, `src/trader.cpp` |
| Strategy | order-book imbalance taker | `include/strategy.hpp` |
| Pre-trade risk | `RiskGate` | `include/risk.hpp` |
| Latency measurement | `SO_TIMESTAMPING`, `LatencySampler` | `include/net_timestamp.hpp`, `include/latency_sampler.hpp` |
| Inter-thread queues (feed → book builder, risk, logger) | mutex queue, SPSC ring, Disruptor ring | `include/queue_mutex.hpp`, `include/spsc_ring.hpp`, `include/mpmc_fanout.hpp` |
| Monitoring and ops | shared-memory stats, monitor, telemetry bridge, visualizer | `include/stats.hpp`, `src/monitor.cpp`, `src/telemetry_bridge.cpp` |

### 1.3 Hot path vs cold path

A recurring idea in this codebase: **the hot path** is the code that
runs for every market event or order (receive, decode, book update,
strategy, send). Everything else (startup, logging, reporting,
reconnects, snapshots) is **the cold path**. Rules for the hot path,
which you will see enforced again and again:

- **No allocation.** `malloc`/`new` can take locks, can walk free lists,
  and can trigger a page fault when the heap grows. Pre-allocate.
  (`MemPool`, `OrderBook`'s node pool, `FlatIdMap`, `LatencySampler`'s
  pre-faulted vector, `FrameReader`'s fixed buffer.)
- **No locks shared with another busy thread.** A contended mutex can
  put you to sleep. Use single-writer designs and lock-free handoffs.
- **No syscalls you can avoid.** Each one costs a user→kernel transition
  (~100 ns or more with modern side-channel mitigations). Batch them
  (`recvmmsg`) or bypass the kernel.
- **No unbounded work.** Every loop has a bound; every buffer has a cap.
- **No exceptions thrown.** Errors are return values, or latched flags
  (`wire::Writer::ok()`). Exceptions are fine on the cold path
  (`CHECK()` during setup).
- **Predictable memory access.** Arrays over linked structures, adjacent
  data on the same cache line, hot data separated from cold.

### 1.4 Glossary of the first hour

| Term | Meaning |
|---|---|
| Bid / ask (offer) | Highest price someone will buy at / lowest price someone will sell at |
| Spread | ask − bid |
| Order book | All resting buy and sell orders, organized by price |
| L1 / L2 / L3 data | Top of book only / aggregated quantity per price level / every individual order ("market by order") |
| Tick | (1) a market-data event; (2) the minimum price increment (tick size) |
| Maker / taker | Resting order that provided liquidity / incoming order that removed it |
| Fill / execution | A trade against your order |
| IOC | Immediate-or-cancel: trade what you can now, cancel the rest |
| Sequence gap | A missing message number in a feed: your view of the market may now be wrong |
| Multicast | One UDP send delivered to every subscriber of a group address |
| Kernel bypass | Reading/writing NIC queues directly from user space, skipping the OS network stack |
| p50 / p99 / p99.9 | Median, 99th and 99.9th percentile latency; tails matter more than averages |

---

## 2. Foundations used everywhere

### 2.1 Build system and compiler flags

**File:** `Makefile`

```make
STD     := -std=c++17
WARN    := -Wall -Wextra -Wpedantic -Werror
POSIX   := -D_POSIX_C_SOURCE=200809L
CXXFLAGS_RELEASE := ... -O2 -g
CXXFLAGS_ASAN    := ... -O1 -g -fsanitize=address -fno-omit-frame-pointer
CXXFLAGS_TSAN    := ... -O1 -g -fsanitize=thread
```

- **`-Wall -Wextra -Wpedantic -Werror`**: every warning is a build
  failure. This caught real bugs while this project was written: a
  printf format mismatch (`%lu` vs `unsigned long long`), a member name
  clash (`signals_` used for both a `SignalFd` and a counter), and
  unused results. In industry, warnings-as-errors is standard in
  latency-critical repositories because a warning is often a latent bug.
- **`-D_POSIX_C_SOURCE=200809L`**: `-std=c++17` (strict ISO mode) hides
  POSIX APIs; this re-exposes them. g++ also predefines `_GNU_SOURCE`
  for C++, which is why Linux-only calls (`accept4`, `recvmmsg`,
  `epoll`) compile too.
- **`-O2 -g`**: optimize *and* keep debug symbols. You profile and debug
  the binary you ship. (`perf` and `gdb` both need symbols.)
- **Sanitizers**: `make asan` (AddressSanitizer: buffer overflows,
  use-after-free, leaks), `make tsan` (ThreadSanitizer: data races).
  Neither is free: ASan costs roughly 2× in speed and 3× in memory,
  TSan roughly 5–15×. You run them in CI and when chasing bugs, never in
  production. The whole trading phase was run under ASan + UBSan,
  including the gap-recovery path, with zero reports.
- **No external libraries.** Everything (SHA-1, Base64, WebSocket, JSON
  output, FIX, the hash map) is hand-written. That is a learning choice:
  you can read every line between the socket and the strategy.

Each program is compiled in a single `g++` invocation from all of its
`.cpp` files (see `EXCHANGE_SRCS`, `TRADER_SRCS`). That's simple, but it
means changing one file recompiles the whole program. A larger project
would compile each `.cpp` to an object file once and link.

**Try:** `make clean && make asan && ./bin/test_order_book`

### 2.2 Error handling and RAII

**File:** `include/common.hpp`

```cpp
#define CHECK(call, what)                                  \
    do {                                                   \
        if ((call) < 0) {                                  \
            throw SystemError((what), errno);              \
        }                                                  \
    } while (0)
```

- POSIX calls report failure as `-1` + `errno`. `CHECK()` converts that
  into a C++ exception carrying the errno. `main()` catches
  `SystemError` once, prints `what failed: strerror(errno)` and returns
  `EXIT_SYSFAIL`.
- **Why `do { ... } while (0)`?** It makes the macro a single statement,
  so `if (x) CHECK(...); else ...` parses correctly. A classic C/C++
  interview question.
- **RAII (Resource Acquisition Is Initialization):** a resource (fd,
  mapping, thread, lock) is owned by an object whose destructor releases
  it. When an exception propagates, stack unwinding runs destructors, so
  every exit path cleans up. There is no `goto cleanup` anywhere.

The `Socket` class is the template for every owner in the codebase:

```cpp
class Socket {
public:
    Socket(int domain, int type, int protocol);   // creates
    explicit Socket(int adopted_fd) noexcept;     // adopts (e.g. from accept())
    ~Socket() { close(); }
    Socket(const Socket &) = delete;              // no copies: two owners would double-close
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;              // moves transfer ownership
    Socket &operator=(Socket &&other) noexcept;
    ...
};
```

Concepts to be able to explain:

- **Rule of five / rule of zero.** If a class manages a resource, decide
  explicitly about the destructor, copy constructor, copy assignment,
  move constructor and move assignment. `Socket` deletes copies and
  implements moves. Classes that only hold RAII members (like `Exchange`)
  need none of them (rule of zero).
- **`explicit`** on the one-argument constructor stops an `int`
  silently converting into a `Socket` (which would then close that fd in
  its destructor!).
- **`noexcept` on moves**: `std::vector` only uses your move
  constructor during reallocation if it is `noexcept`; otherwise it
  copies (or here, fails to compile).
- **Self-move check** in move assignment (`if (this != &other)`).
- `Socket` is reused as a generic "close this fd" owner for epoll,
  timerfd and signalfd fds (`include/event_loop.hpp`).

**Exercise:** delete `noexcept` from `Socket`'s move constructor and put
a `std::vector<Socket>` somewhere. What changes?

### 2.3 Clocks and time

**Files:** `include/time_util.hpp`, `src/time_util.cpp`

Linux has several clocks, and using the wrong one is a real bug class:

| Function | Clock | Use for |
|---|---|---|
| `now_ns()` | `CLOCK_MONOTONIC_RAW` | Measuring elapsed time on one host. Never jumps, never slewed by NTP. |
| `now_ns_monotonic()` | `CLOCK_MONOTONIC` | Deadlines for `clock_nanosleep` (which does not accept `_RAW`) |
| `now_realtime_ns()` | `CLOCK_REALTIME` | Wall clock. Comparable with kernel packet timestamps and exchange timestamps; the clock PTP synchronises across machines. Can jump. |

**A real bug from this repo:** the publisher once computed pacing
deadlines from `now_ns()` (MONOTONIC_RAW) and slept with
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`. The two clocks
have different epochs. The deadlines landed in the past, every sleep
returned immediately, and `--rate` was silently ignored. The fix and the
explanation are in the `now_ns_monotonic()` comment.

**Why the trading loop uses `CLOCK_REALTIME`:** `SO_TIMESTAMPING`
kernel stamps are `CLOCK_REALTIME`, and ITCH/OUCH timestamps are
"nanoseconds since midnight" of wall-clock time. To subtract one from
another, every stamp must come from the same clock
(`include/trading_types.hpp`: `ns_since_midnight`, `mid_diff`).

**How fast is `clock_gettime`?** It doesn't enter the kernel. The
vDSO (a small shared library the kernel maps into every process) reads
the TSC (the CPU's timestamp counter) and converts it. That costs about
20–30 ns here; `bench/book_bench.cpp` measures it ("clock overhead per
sample ~30 ns"). Some firms read the TSC directly with `rdtsc` (a few
ns) and convert to nanoseconds later.

**Drift-free pacing** (`src/publisher.cpp`): instead of
`sleep(interval)` after each send (errors accumulate), compute absolute
deadlines `deadline += interval` and `sleep_until(deadline)`.

**Questions**
1. Why must elapsed-time measurements never use `CLOCK_REALTIME`?
2. Why can't you compare `CLOCK_MONOTONIC` timestamps from two different
   machines?
3. What keeps `CLOCK_REALTIME` on two machines in agreement, and how
   closely? (NTP: around a millisecond. PTP with hardware timestamping:
   tens of nanoseconds. See 7.6.)

### 2.4 Memory layout of a message

**File:** `include/message.hpp`

```cpp
struct TickMessage {
    uint64_t sequence;          // offset 0
    uint64_t send_timestamp_ns; // offset 8
    uint32_t symbol_id;         // offset 16
                                // offset 20: 4 bytes of PADDING
    int64_t  price;             // offset 24 (8-byte aligned)
    uint32_t quantity;          // offset 32
    uint32_t flags;             // offset 36
};                              // sizeof == 40
static_assert(sizeof(TickMessage) == 40, "...");
```

- **Alignment:** each type must sit at an address that is a multiple of
  its alignment (8 for `int64_t`). The compiler inserts padding to make
  that true. Reordering fields (largest first) usually removes padding.
- **`static_assert`** turns "someone added a field and broke the
  layout" into a compile error instead of a production incident.
- **The shortcut taken here:** Phases 0–5 send this struct's raw bytes
  as the wire format. That works only because both ends are the same
  program on the same little-endian machine. Real protocols never do
  this. Phase 6 (`include/wire.hpp`, section 6.3) shows the proper way,
  with an explanation of the three things that go wrong (endianness,
  padding, strict aliasing).

**Exercise:** reorder the fields to remove the padding. What is
`sizeof` now? Why might you *not* want to do that to a message that is
already deployed?

### 2.5 Fixed-point prices

**File:** `include/trading_types.hpp`

Prices are `int64_t` with 4 implied decimal places (`$100.25 ==
1'002'500`), the same scale ITCH and OUCH use on the wire. Never use
`double` for prices:

- `0.1 + 0.2 != 0.3` in binary floating point. Two prices that should
  compare equal might not, so a matching engine could fail to cross
  orders that should trade.
- Floating-point results can differ across compiler flags (`-ffast-math`,
  FMA contraction), and exchanges require deterministic, reproducible
  matching.
- Integer arithmetic is also exactly what the wire carries, so there's
  no conversion on the hot path.

Formatting to a decimal string (`format_price`) happens only on the
cold path (logs, reports).

---

## 3. Concurrency: queues, atomics and the memory model

The market-data pipeline (`bin/receiver`) has one receive thread
producing `TickMessage`s and N worker threads consuming them. The
producer → consumer handoff is implemented three ways behind one
interface, so you can compare them.

### 3.1 The mutex + condition variable queue

**Files:** `include/queue_mutex.hpp`, `src/queue_mutex.cpp`,
`tests/test_queue_mutex.cpp`

A bounded FIFO: a pre-allocated `std::vector<TickMessage>` used as a
circular buffer (`head_`, `tail_`, `count_`), one `std::mutex`, and two
condition variables: `not_empty_` (consumers wait on it) and
`not_full_` (producers wait on it).

```cpp
QueueMutex::Status QueueMutex::pop(TickMessage &msg) {
    std::unique_lock<std::mutex> lock(lock_);
    not_empty_.wait(lock, [this] { return count_ > 0 || closed_; });
    if (count_ == 0 && closed_) return Status::Closed;
    msg = buf_[head_];
    head_ = (head_ + 1) % capacity_;
    --count_;
    not_full_.notify_one();
    return Status::Ok;
}
```

Concepts:

- **Why `wait(lock, predicate)`:** condition variables can wake up
  without being notified (*spurious wakeups*), and another thread may
  have taken the item between the notify and your wakeup. The predicate
  overload re-checks in a loop. Writing `if (count_ == 0) cv.wait(lock);`
  is a classic bug.
- **`unique_lock` vs `lock_guard`:** `wait()` must unlock and re-lock
  the mutex, so it needs a `unique_lock`. Anywhere you only lock and
  unlock at scope exit, `lock_guard` is enough.
- **Backpressure:** when the queue is full, the producer waits (`push`)
  or is told `Full` (`try_push`) instead of growing memory or dropping
  silently. Unbounded queues turn an overload into an out-of-memory
  crash minutes later.
- **Close-then-drain shutdown:** `close()` sets `closed_` and
  `notify_all()`s *both* condition variables. Producers then fail with
  `Closed`; consumers keep popping until the queue is empty, then get
  `Closed`. Nothing in flight is lost, and no thread is left blocked
  forever.
- **The receiver uses `try_push` + a 10 µs backoff, not blocking
  `push`,** so that Ctrl-C is noticed even while the queue is full
  (`src/receiver.cpp`, `process_one`).
- **`depth()` takes the lock.** It's exact, but it costs a lock
  acquisition, so only call it occasionally.

Cost: an uncontended `std::mutex` lock/unlock is roughly 15–25 ns
(an atomic read-modify-write in user space). The expensive part is
contention: a waiting thread is put to sleep with the `futex` syscall,
and waking it takes a scheduler round trip of several microseconds.
That's the latency the lock-free queues remove.

### 3.2 The lock-free SPSC ring

**Files:** `include/spsc_ring.hpp`, `src/spsc_ring.cpp`,
`tests/test_spsc_ring.cpp`

Single producer, single consumer, and no locks. The only shared state
is two indices:

```cpp
alignas(64) std::atomic<size_t> head_{0};  // next slot to pop  (written by consumer)
char _pad1[64 - sizeof(std::atomic<size_t>)];
alignas(64) std::atomic<size_t> tail_{0};  // next slot to push (written by producer)
```

```cpp
Status SpscRing::try_push(const TickMessage &msg) {          // producer thread only
    const size_t tail = tail_.load(std::memory_order_relaxed);  // our own index
    const size_t head = head_.load(std::memory_order_acquire);  // the other side's
    if (tail - head >= buf_.size()) return Status::Full;
    buf_[tail & mask_] = msg;                                   // 1. write the data
    tail_.store(tail + 1, std::memory_order_release);           // 2. then publish it
    return Status::Ok;
}

bool SpscRing::try_pop(TickMessage &msg) {                   // consumer thread only
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);  // pairs with the release above
    if (head == tail) return false;
    msg = buf_[head & mask_];                                   // guaranteed fully written
    head_.store(head + 1, std::memory_order_release);           // slot free for the producer
    return true;
}
```

Everything in this function is a concept you'll be asked about:

1. **Acquire/release creates a happens-before edge.** The producer's
   `release` store of `tail_` guarantees that every write before it (the
   message bytes) is visible to any thread whose `acquire` load sees
   that new `tail_` value. With `relaxed` on either side, the compiler
   or CPU could make the `tail_` update visible before the message
   bytes, and the consumer would read a half-written message. On x86 the
   hardware rarely reorders stores this way (TSO), but the *compiler*
   can, and ARM hardware will. Code must be correct by the C++ memory
   model, not by the CPU you tested on.
2. **Why the thread's own index is loaded `relaxed`:** only this thread
   ever writes it, so there's nothing to synchronize with.
3. **Monotonic indices, masked on access:** `head_` and `tail_` only
   ever increase; the slot is `index & mask_`. "Full" is
   `tail - head == capacity` and "empty" is `head == tail`. You never
   need a separate count or a wasted slot, and unsigned wrap-around after
   2^64 operations is still correct.
4. **Power-of-two capacity** so the wrap is `& mask_` (1 cycle) instead
   of `% capacity` (a divide, ~20–40 cycles).
5. **Padding against false sharing:** the producer writes `tail_` on
   every push and the consumer writes `head_` on every pop. If they
   shared a 64-byte cache line, every write would invalidate the other
   core's copy and the line would bounce between cores (see 3.5), even
   though the two threads never touch the same variable.
6. **No blocking, so no sleeping:** the consumer adapter in
   `src/queue_backend.cpp` spins `kSpinsBeforeYield = 1000` times and
   then `std::this_thread::yield()`s. Lock-free queues trade idle CPU
   for wakeup latency. Real low-latency systems spin forever on an
   isolated core.

**Exercise:** change both `release` stores to `relaxed`, run
`make tsan && ./bin/test_spsc_ring`. Then read `docs/memory_ordering.md`.

**Further optimization you can explain (not implemented):** each side
caches the other side's index locally and only re-reads the atomic when
the cached value says "full" (producer) or "empty" (consumer). That
removes most cross-core cache traffic. Production SPSC queues
(e.g. Rigtorp's, or Folly's `ProducerConsumerQueue`) do this.

### 3.3 The Disruptor-style broadcast ring

**Files:** `include/mpmc_fanout.hpp`, `src/mpmc_fanout.cpp`,
`tests/test_mpmc_fanout.cpp`

The mutex queue and SPSC ring are **work-sharing**: each message goes to
exactly one consumer. The LMAX Disruptor pattern is **broadcast**: one
writer, many readers, and *every reader sees every message*, each at its
own pace. That is the realistic shape of a feed handler: the same tick
goes to the book builder, the risk engine and the logger.

- One `write_seq_` (written only by the producer), and one `read_seq_`
  cursor per reader, each written only by its own reader. This is the
  **single-writer principle**: every variable has exactly one writer, so
  no read-modify-write atomics (CAS, `fetch_add`) are needed anywhere.
- **Backpressure against the slowest reader:** the producer may not
  overwrite a slot until *all* readers have passed it.
  `min_read_seq()` scans every reader cursor on each push. That's
  O(readers); the real Disruptor caches the minimum and rescans only when
  the cached value says the ring is full (an exercise in section 9).
- **A C++17 trap documented in the constructor:** a default-constructed
  `std::atomic<size_t>` holds an indeterminate value in C++17 (zero
  initialization came in C++20), so the cursors are explicitly stored as
  0 in a loop.
- With `--queue disruptor --workers 3`, the receiver's `processed`
  count is 3 × received, because every worker processes every message.

### 3.4 Interfaces and the factory

**Files:** `include/queue_backend.hpp`, `src/queue_backend.cpp`

```cpp
class IQueueProducer { public: virtual PushStatus try_push(const TickMessage &) = 0; ... };
class IQueueConsumer { public: virtual PopStatus pop(TickMessage &) = 0; ... };
QueueBackend make_queue_backend(QueueKind kind, size_t capacity, size_t num_workers);
```

- **Factory pattern:** callers say *what* (`QueueKind::Spsc`), the
  factory decides *how* (which concrete adapter classes to build). The
  receiver's `--queue` flag is one factory call.
- **Adapter pattern:** `SpscProducer` adapts `SpscRing`'s API to
  `IQueueProducer`.
- **Ownership:** the concrete queue is held by `std::shared_ptr`, shared
  by the producer handle and every consumer handle; each handle is a
  `std::unique_ptr` with exactly one owner. The queue lives as long as
  any handle does.
- **Cost of virtual dispatch:** one indirect call per push/pop (a few ns
  when the branch predictor has learned the target). Acceptable here; on
  the hottest path you'd template on the queue type instead (static
  polymorphism). The same trade-off appears again in the matching
  engine (6.10).

### 3.5 Atomics for statistics, and false sharing

**Files:** `include/stats.hpp`, `src/worker.cpp`

```cpp
struct SharedStats {
    std::atomic<uint64_t> received, invalid, sequence_gaps, reorders, queue_full, recovered;
    uint64_t _pad1[2];                 // receiver-written group: exactly 64 bytes
    std::atomic<uint64_t> processed, latency_total_ns, latency_max_ns, queue_depth;
    uint64_t _pad2[4];                 // worker-written group: exactly 64 bytes
};
static_assert(sizeof(SharedStats) == 128, "...");
```

- **`memory_order_relaxed` is correct for independent counters.** You
  need each increment to be atomic (no lost updates), but no other
  memory depends on the counter's value, so no ordering is needed.
  Relaxed `fetch_add` is still a `lock xadd` on x86, about 5–20 ns
  uncontended, and much more when contended. That's why workers
  accumulate locally and you would publish in batches if it mattered.
- **Atomic maximum with compare-and-swap:**
  ```cpp
  uint64_t cur = stats.latency_max_ns.load(relaxed);
  while (lat > cur && !stats.latency_max_ns.compare_exchange_weak(cur, lat, relaxed, relaxed)) {}
  ```
  On failure, `compare_exchange_weak` writes the current value into
  `cur`, so the loop retries with fresh data. `_weak` may fail
  spuriously (on LL/SC architectures like ARM), which is fine inside a
  loop.
- **False sharing:** caches move data in 64-byte lines. Two threads
  writing *different* variables on the *same* line force that line to
  ping-pong between their cores (MESI invalidations), which costs tens of
  nanoseconds per write instead of ~1. The padding puts receiver-written
  and worker-written counters on separate lines.
- **A real flaw to find:** `queue_depth` is in the worker group, but the
  receiver *also* writes it after every push (`src/receiver.cpp`). So
  that one variable is written by both sides, and its line bounces
  anyway. The visualizer's cache-line view (machine view, level 3) draws
  exactly this. Fixing it is an exercise.

### 3.6 Shared memory between processes

**Files:** `include/shared_stats.hpp`, `src/monitor.cpp`

`bin/monitor` is a *separate process* that reads the receiver's
counters live:

1. Owner (receiver): `shm_unlink` (clear stale) → `shm_open(O_CREAT)` →
   `ftruncate(size)` → `mmap(MAP_SHARED)` → reset counters.
2. Guest (monitor, telemetry_bridge): `shm_open` (no create) → `mmap`.
3. Owner on shutdown: `shm_unlink` (the object disappears once the last
   mapping is gone). Guests never unlink.

The object lives in `/dev/shm`. Both processes see the same physical
pages, so a relaxed store in one is visible in the other without any
syscall. This is how real systems export hot-path metrics without
slowing the hot path.

Caveat (documented in `stats.hpp`): the C++ standard does not promise
that atomics work across processes. They do in practice when the
atomic is lock-free, so `create()` checks
`std::atomic<uint64_t>::is_always_lock_free`. A non-lock-free atomic
would be protected by a lock inside *one* process's address space, which
would not protect anything across processes.

### 3.7 Zero-allocation object pool

**Files:** `include/mem_pool.hpp`, `tests/test_mem_pool.cpp`

`MemPool<T, Capacity>` is a fixed array of slots, where each slot is a
`union` of `T`'s storage and a `next_free` pointer. The unused slots form
an **intrusive free list** (the list links are stored inside the free
slots themselves, so they cost no extra memory). `acquire()` pops the
head and `release()` pushes it back: O(1), no syscalls, no locks.

- Exhaustion returns `nullptr` rather than silently falling back to
  `new`, because the fallback would bring back the very latency spike
  the pool exists to remove.
- Not thread-safe on purpose: give each thread its own pool.
- The order book's node pool (6.9) is the same idea using 32-bit
  indices instead of pointers.

### 3.8 constexpr latency histogram

**Files:** `include/latency_histogram.hpp`, `tests/test_latency_histogram.cpp`

48 log-scale buckets (128 ns, 256 ns, ... doubling). The bucket bounds
are computed at **compile time**:

```cpp
constexpr std::array<uint64_t, 49> make_latency_bucket_bounds() { ... }
constexpr auto kLatencyBucketBounds = make_latency_bucket_bounds();
```

The table ends up in the binary's read-only data section. Nothing runs
at startup and nothing is recomputed per sample.

**Read the `queue_bench` output critically:**

```
mutex  count=200000  p50=1048576ns  p99=2097152ns  p99.9=2097152ns  max=1295042ns
spsc   count=200000  p50=262144ns   p99=524288ns   p99.9=524288ns   max=290571ns
```

1. **p99 is larger than max.** The percentile reports the *upper bound*
   of the bucket the sample fell in, and the true max sits inside that
   bucket. A log histogram trades precision for speed. Clamping
   percentiles to `max` is an easy exercise. This is why the trading
   loop uses `LatencySampler` (exact samples, sorted at the end) instead.
2. **These are queueing delays, not handoff costs.** `queue_bench`'s
   producer pushes as fast as it can, so the 4096-slot queue sits full
   and each message waits behind ~4000 others. The result measures
   throughput (spsc drains about 4× faster than mutex), not the ~100 ns
   handoff an idle SPSC ring achieves. When you read a benchmark, always
   ask "what is this actually measuring?"

### 3.9 Huge pages

**Files:** `include/hugepage_alloc.hpp`, `docs/hugepages.md`

Every memory access translates a virtual address through the **TLB**
(translation lookaside buffer), a small cache of page-table entries
(on the order of 64 L1 entries plus 1,500–3,000 L2 entries). With 4 KiB
pages, 2,000 entries cover only ~8 MiB, so a larger working set misses
the TLB and pays for a page-table walk (tens to hundreds of
nanoseconds). A 2 MiB huge page covers 512× more address space per
entry. `HugePageRegion` tries `mmap(MAP_HUGETLB)` and **falls back
honestly** (`used_huge_pages()` tells you which happened). Under WSL2
huge pages usually aren't reserved, so the fallback is the normal case.

### 3.10 Debugging labs: deadlock and data race

**Files:** `src/deadlock_demo.cpp`, `src/race_demo.cpp`,
`docs/debugging_labs.md`

- **Deadlock by lock-order inversion:** thread A locks `mu_a` then
  `mu_b`, thread B locks `mu_b` then `mu_a`. Attach `gdb -p <pid>`,
  `info threads`, `thread 2`, `bt`: both threads are blocked in
  `pthread_mutex_lock`. Fixes: a global lock order, or `std::lock(a, b)`
  / `std::scoped_lock(a, b)`, which acquire both without deadlock.
- **Data race:** 8 threads run `g_racy_counter++` on a plain
  `uint64_t`. The total comes out wrong (lost updates), and under the C++
  memory model a data race is **undefined behaviour**: the compiler may
  legally keep the counter in a register and never write it back. `make
  tsan && ./bin/race_demo` prints both racing stack traces. Fix:
  `std::atomic<uint64_t>::fetch_add`.
- Four conditions for deadlock (Coffman): mutual exclusion, hold and
  wait, no preemption, circular wait. Break any one of them.

### 3.11 Concurrency questions

1. Why does the SPSC ring need acquire/release while `SharedStats` only
   needs relaxed?
2. Draw what false sharing does to a cache line. How do you detect it?
   (`perf c2c` on Linux.)
3. Why is a spurious wakeup not a bug in the condition variable?
4. What does `compare_exchange_weak` do on failure? Why is "weak" fine
   in a loop?
5. Work-sharing vs broadcast queues: which fits "N strategies all need
   every tick"?
6. Why is `std::atomic<T>` across processes only safe when it is
   lock-free?
7. What is the ABA problem, and why do the rings in this repo not have
   it? (Indices only ever increase; there is no pointer being swapped.)

---

## 4. Networking: from socket to kernel bypass

### 4.1 The path of one UDP packet

Know this path cold, because every optimization below shortens some
part of it:

```
 NIC receives frame ─> DMA into RX ring in RAM ─> interrupt (IRQ) ─> NAPI poll (softirq)
   ─> driver builds skb ─> [RX timestamp: SO_TIMESTAMPING software] ─> IP ─> UDP
   ─> socket lookup ─> [BPF socket filter] ─> enqueue on socket receive buffer (SO_RCVBUF)
   ─> wake the waiting thread (scheduler) ─> recvfrom/recvmsg/recvmmsg copies to user space
```

Approximate costs on a tuned Linux box: interrupt + softirq + stack ~2–5
µs, the scheduler wakeup of a sleeping thread ~2–10 µs (much more in a
VM; we measured ~97 µs p50 under WSL2), the syscall + copy ~0.5–1 µs.
Kernel bypass (4.8) replaces nearly all of this with user-space polling
of the NIC's ring, ~1 µs or less end to end.

### 4.2 UDP sockets in the receiver

**File:** `src/receiver.cpp`

- `SO_REUSEADDR`: several receivers can bind the same port (needed for
  multicast fan-out on one host).
- `SO_RCVBUF = 4 MiB`: **the kernel socket buffer is the first queue.**
  Bursts that arrive while user space is busy wait here; once it fills,
  the kernel drops packets (`netstat -su` shows "receive buffer
  errors"). A bigger buffer absorbs bursts but cannot fix sustained
  overload. The kernel caps it at `net.core.rmem_max` (the trader prints
  the value it actually got: 425,984 bytes under WSL2's default).
- **Signals without `SA_RESTART`:** a Ctrl-C makes a blocked
  `recvfrom()` return `EINTR`, so the loop sees `g_stop` and shuts down
  cleanly. With `SA_RESTART`, the kernel would silently restart the
  syscall and the receiver would hang until the next packet arrived.
- **Gap and reorder detection:** sequence > last+1 means a gap; sequence
  < last means a reorder. UDP guarantees neither delivery nor order.
- Validation (`message_is_valid`) rejects malformed datagrams before
  they touch the queue. Never trust bytes from the network.

### 4.3 Multicast

**Files:** `include/multicast.hpp`, publisher/receiver `--transport multicast`

One `sendto()` to a group address (224.0.0.0/4) reaches every
subscriber. Switches and routers do the copying, so the sender's cost is
the same for 1 or 1,000 subscribers. That's why every exchange publishes
market data this way.

| Call | Side | What it does |
|---|---|---|
| `IP_ADD_MEMBERSHIP` | receiver | Join the group: the kernel sends an IGMP membership report, and the switch (with IGMP snooping) starts forwarding the group's traffic to your port |
| `IP_MULTICAST_TTL` | sender | Router-hop limit; 1 = this subnet only |
| `IP_MULTICAST_LOOP` | sender | Deliver a copy to receivers on the sending host (required for this one-machine lab) |

- `239.0.0.0/8` is administratively scoped (private) multicast. That's
  why PulseForge uses `239.255.0.1` and `239.255.0.2`.
- An IPv4 multicast group maps to an Ethernet MAC `01:00:5e` + the low
  23 bits of the group address, so the NIC can filter in hardware (the
  visualizer's Ethernet layer computes this live; see
  `visualizer/js/data/protocols.js`).
- The trader binds its socket to the **group address** rather than
  `INADDR_ANY`, so it only receives that group's datagrams and not every
  group sent to the same port (`src/trader.cpp`, `setup_md_socket`).
- Multicast is UDP: there's no retransmission. Exchanges solve recovery
  separately (6.12).

### 4.4 Batching syscalls: `recvmmsg` and the `MSG_WAITFORONE` bug

**File:** `src/receiver.cpp` (batch path, `--batch N`)

`recvmmsg()` receives up to N datagrams in one syscall, so the fixed
syscall cost is paid once per batch rather than once per packet.

**The bug this repository actually had:** without `MSG_WAITFORONE`, a
blocking `recvmmsg()` waits until *all N slots* are filled. At 400
msgs/s with `--batch 16`, every tick sat in the kernel for ~20 ms
waiting for its batch-mates, and average latency went from ~50 µs to
19 ms. The 3D visualizer exposed it. With `MSG_WAITFORONE`, the call
blocks only for the first datagram, then takes whatever else is already
queued and returns. **Batching must amortize cost, never add delay.**
The same tension appears in the exchange, which batches each loop
iteration's market data into one MoldUDP64 packet (6.11).

### 4.5 Busy polling

**File:** `src/receiver.cpp` (`--busy-poll US`)

`SO_BUSY_POLL` asks the kernel, when your thread calls `recv` on an
empty socket, to poll the NIC driver's queue directly for up to N
microseconds instead of sleeping until an interrupt. It needs a driver
with NAPI busy-poll support. On loopback or WSL2's virtual NIC it
usually does nothing measurable, so the code treats it as a soft failure
(warn and continue) rather than a fatal error. Related system-wide knobs:
`net.core.busy_poll` and `net.core.busy_read`.

The trader's `--spin` mode is the user-space version of the same idea:
never sleep, poll in a loop (6.16).

### 4.6 Classic BPF socket filter

**Files:** `include/bpf_filter.hpp`, `src/bpf_filter.cpp` (`--bpf-filter`)

A 4-instruction classic-BPF program, attached with `SO_ATTACH_FILTER`,
runs inside the kernel for each datagram before it's queued to the
socket:

```
ld  [32]          ; A = 32-bit word at payload offset 32 (TickMessage::quantity)
jeq #0, drop, ok
drop: ret #0      ; keep 0 bytes   -> drop
ok:   ret #0xffff ; keep the whole datagram
```

That is "kernel bypass in miniature": the decision moves from user space
into the kernel, so invalid packets are never copied out. It's the same
mechanism `tcpdump` filters use. The modern descendants are eBPF socket
filters and **XDP** (eBPF that runs in the driver, before an skb is even
allocated). Note the filter reads the struct's in-memory layout, which
works here only because sender and receiver share it (2.4).

### 4.7 CPU pinning and topology

**Files:** `src/cpu_util.cpp`, `src/hw_topology.cpp`, `include/cpu_list.hpp`

- `pin_to_cpu()` calls `sched_setaffinity` on the calling thread. A
  pinned thread keeps its warm L1/L2 caches and is never migrated
  mid-burst. `--pin-cpu` (receiver) and `--cpu` (exchange, trader) use it.
- `bin/hw_topology` (and `make topology`) reads the same sysfs files
  `lscpu`/`lstopo` use: `/sys/devices/system/cpu/cpuN/topology`,
  `.../cache/indexK`, NUMA nodes, NICs. `parse_cpu_list` parses the kernel's
  `0-3,8,10-11` list syntax and has its own unit test.
- **Placement rules for a real box:** isolate cores from the scheduler
  (`isolcpus=`, `nohz_full=`, `rcu_nocbs=` boot parameters), pin one hot
  thread per isolated core, don't put two hot threads on hyperthread
  siblings (they share L1/L2 and execution units), keep the thread on the
  same NUMA node as the NIC's PCIe slot (cross-socket memory access costs
  roughly +50–100 ns), and steer the NIC's interrupts away from the hot
  cores (`/proc/irq/*/smp_affinity`).

### 4.8 TCP vs UDP: the gap-recovery channel

**Files:** `include/snapshot_protocol.hpp`, `src/snapshot_server.cpp`,
`src/snapshot_client.cpp`, receiver `--snapshot-host/--snapshot-port`

The live feed stays on UDP (lowest latency, loss is detected), but
*repairing* a gap needs reliable, ordered request/response: exactly what
TCP provides. The publisher keeps a ring of the last 8,192 *generated*
messages (generated, not sent: the ring models the exchange, which has
everything, while only the wire loses packets). On a gap the receiver:

```cpp
pending_recoveries.push_back(std::async(std::launch::async,
    request_snapshot_recovery, host, port, gap_start, count));
...
if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { msgs = fut.get(); ... }
```

- **`std::async` + `std::future`:** the slow blocking TCP round trip runs
  on another thread; the receive loop polls with a zero timeout and never
  blocks. `std::launch::async` forces a real thread (the default policy
  may defer the work until `get()`).
- **Single-writer preserved:** only the receive thread pushes recovered
  messages into the queue, so the SPSC/Disruptor single-producer rule
  still holds.
- **Thundering herd:** an unbounded "one thread per gap" design falls
  apart under heavy loss. Thread creation slows the receive loop, which
  causes more loss, which spawns more threads.
  `kMaxPendingRecoveries = 8` caps it.
- **`MSG_NOSIGNAL`:** writing to a TCP socket whose peer has closed
  raises `SIGPIPE`, whose default action kills the process. This
  actually killed `telemetry_bridge` whenever a browser tab closed, until
  every `send()` got `MSG_NOSIGNAL` (`src/ws_server.cpp`,
  `src/snapshot_server.cpp`, and all Phase 6 sends).
- **`recv_full` / `send_full` loops:** TCP is a byte stream, and one
  `recv` may return part of a struct (6.5 builds a proper framing layer).

The trading loop uses the production approach instead: a **snapshot**
of the whole book plus incremental replay (6.12).

### 4.9 Kernel bypass on real hardware

**File:** `docs/kernel_bypass_next_steps.md`

What this lab can't run under WSL2, and you should be able to discuss:

| Technology | Idea | Typical |
|---|---|---|
| Solarflare/AMD **OpenOnload** | Intercepts normal socket calls (LD_PRELOAD) and runs the TCP/UDP stack in user space against the NIC | No code changes; ~1–2 µs |
| **ef_vi** / **TCPDirect** | Raw access to the NIC's RX/TX rings from user space | Sub-microsecond receive |
| **DPDK** | Poll-mode drivers; the NIC is unbound from the kernel and owned by your process; huge pages for packet buffers | Throughput-oriented, very low latency |
| **XDP / AF_XDP** | eBPF in the driver; AF_XDP sockets get frames with zero copy | Kernel-native middle ground |
| **RDMA / InfiniBand** | NIC writes straight into remote memory | Inter-server messaging |
| **FPGA NICs** | Parsing, book building and even order generation in hardware | Tick-to-trade in the hundreds of nanoseconds |

The common ingredients: poll instead of interrupts, no syscalls, no
copies (the NIC DMAs into memory your process reads directly), and
pinned, isolated cores.

### 4.10 Networking questions

1. Walk through what happens between a packet arriving at the NIC and
   `recvfrom()` returning.
2. Why do exchanges use multicast UDP for market data but TCP for order
   entry?
3. What does `IP_ADD_MEMBERSHIP` cause on the wire? (IGMP.)
4. Why can batching increase latency, and how does `MSG_WAITFORONE`
   prevent it?
5. What is `SIGPIPE` and how do you avoid it?
6. Why does a full `SO_RCVBUF` cause loss even when your queue is empty?
7. What does kernel bypass bypass, and what do you give up? (The kernel's
   TCP/IP stack, firewalling, `tcpdump` visibility, and sharing the NIC.)

---

## 5. Telemetry and the 3D visualizer

### 5.1 The event ring in shared memory

**File:** `include/shared_events.hpp`

A second shared-memory region holds a ring of 16,384 small `Event`
records (type, sequence, timestamp, latency, worker id). Writers reserve
a slot with `write_index.fetch_add(1, relaxed)`, which can't collide
because each caller gets a unique index. The documented *racy by design*
gap: the reader may see `write_index` advance before the writer has
finished filling the slot. That's acceptable for visualization; a
trading-grade version would add a per-slot sequence number written with
`release` after the fields (a seqlock), and the reader would check it.

`SharedConfig` uses the textbook **one-shot publish**: write every plain
field, then `ready.store(1, release)`. Readers check
`ready.load(acquire)` first. That's the same happens-before pattern as
the SPSC ring, applied once.

### 5.2 The telemetry bridge and a hand-rolled WebSocket

**Files:** `src/telemetry_bridge.cpp`, `src/ws_server.cpp`,
`include/ws_server.hpp`, `include/sha1.hpp`

- A guest process that polls the stats and event ring every 50 ms and
  broadcasts JSON to browsers.
- **WebSocket handshake (RFC 6455):** the browser sends an HTTP
  `Upgrade` request with `Sec-WebSocket-Key`; the server answers `101
  Switching Protocols` with `Sec-WebSocket-Accept =
  base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`. That's why
  this repo contains a SHA-1 and a Base64 encoder.
- **Framing:** byte 0 = `0x81` (FIN + text opcode); byte 1 = length
  (<126), or 126 followed by a 16-bit length, or 127 followed by a 64-bit
  length. Server-to-client frames are not masked; client-to-server frames
  are.
- **Two real bugs fixed here:** (1) `SIGPIPE` on a closed tab (4.8);
  (2) the bridge advanced its event cursor even with no browser
  connected, so events were "consumed" by nobody. Now it advances only
  when `client_count() > 0`.

### 5.3 The visualizer's architecture

**Files:** `visualizer/` (see `visualizer/README.md`)

A static Three.js app (no build step) organized like a well-structured
UI codebase:

| Layer | Files | Rule |
|---|---|---|
| Data | `js/data/lessons.js`, `protocols.js`, `layouts.js`, `hardware.js` | Teaching content and byte layouts; no rendering |
| Views | `js/views/*View.js` | One screen each; never touch the DOM |
| Engine | `js/engine/Engine.js`, `factory.js`, `tween.js`, `Navigator.js` | Renderer, bloom, camera flights, picking, labels |
| UI | `js/ui/Hud.js` | All 2D UI; writes with `textContent` only (no HTML injection) |
| Feed | `js/telemetry/FeedHub.js` | WebSocket client, with an automatic DEMO fallback |

Lessons from building it, which carry over to any production frontend:

- One failing static ES-module import blanked the whole page. The fix was
  a dynamic `import()` for the optional bloom effect plus a classic (non-
  module) error-banner script that reports load failures.
- Bare module specifiers (`import 'three'`) need an **import map** in the
  browser.
- Text drawn inside a WebGL scene with bloom becomes unreadable, so the
  labels are real HTML elements positioned by `CSS2DRenderer`.
- Pages must be served over `http://` (`python3 -m http.server`), because
  browsers block ES modules on `file://`.

---

## 6. The trading loop

Phase 6 is a complete, if small, trading stack: an exchange with a real
matching engine and real (NASDAQ-shaped) protocols, and a trader that
rebuilds the book from the feed, decides, checks risk, sends orders, and
measures every microsecond in between.

```sh
bash scripts/run_trading.sh            # both sides block in epoll_wait
bash scripts/run_trading.sh spin       # both busy-poll, pinned to CPUs 1 and 2
bash scripts/run_trading.sh gaps       # simulated packet loss -> gap -> re-snapshot
./bin/exchange --help ; ./bin/trader --help
```

### 6.1 Market microstructure primer

**The limit order book.** Each resting order is (side, price, quantity,
arrival time). Bids are sorted from the highest price down, asks from
the lowest up:

```
            ASKS (sell orders)
   100.05 |  300            <- level 3
   100.03 |  200 100        <- level 2: two orders, FIFO left to right
   100.02 |  400            <- best ask (the "offer")
   -------+---------------  spread = 100.02 - 100.00 = 2 ticks
   100.00 |  500 200        <- best bid
    99.99 |  100
            BIDS (buy orders)
```

- **Price-time priority (FIFO):** better price first; at the same price,
  earlier arrival first. Your position in the queue at a price level
  is valuable, which is why speed matters even for passive orders.
- **Marketable order:** a buy priced at or above the best ask (or a sell
  at or below the best bid). It trades immediately as the **taker**
  against resting **makers**, at the makers' prices.
- **Limit vs market:** a limit order has a worst acceptable price. A
  market order has none (PulseForge only supports limit orders: a
  "market" order is just a limit far through the book, and most risk
  systems forbid true market orders).
- **Time in force:** Day (rest until cancelled or end of day) vs IOC
  (immediate-or-cancel: never rests). Others you should know: FOK
  (fill-or-kill, all or nothing), GTC (good-till-cancelled), and
  post-only (rejected if it would take liquidity).
- **Maker/taker fees:** exchanges typically pay makers a rebate and
  charge takers a fee. The OUCH execution report's liquidity flag
  (`'A'` added / `'R'` removed) tells you which you were, so it's a
  direct PnL input.
- **Tick size and band:** prices must be on the grid (1 cent here) and
  inside an allowed band (PulseForge: $50–$150, standing in for the
  limit-up/limit-down collars real venues apply).
- **Self-trade (wash trade):** trading with yourself is market
  manipulation in most jurisdictions. Venues offer self-match prevention
  (6.10).
- **Adverse selection:** when your passive order gets filled, it's often
  because someone better informed wanted the other side. Taker strategies
  pay the spread instead. Either way, being slow means you trade on stale
  information.
- **Data depth:** L1 = best bid/offer. L2 = total quantity per price
  level. L3 / market-by-order = every order. ITCH is L3, and you build
  L1/L2 yourself from it.

### 6.2 Vocabulary types

**File:** `include/trading_types.hpp`

| Type | Representation | Why |
|---|---|---|
| `Price` | `int64_t`, 4 implied decimals | exact, deterministic, same as the wire (2.5) |
| `Qty` | `uint32_t` | ITCH/OUCH share fields are 32-bit |
| `OrderId` | `uint64_t` | the exchange's order reference number |
| `Side` | `enum class Side : char { Buy = 'B', Sell = 'S' }` | the enum value *is* the wire byte, so encoding is a cast |
| `TimeInForce` | `'0'` Day, `'3'` IOC | FIX tag 59 / OUCH 5.0 values |
| `Symbol` | `std::array<char, 8>`, space padded | the exact wire form: memcpy, 8-byte compare, no `std::string` |

Also in this file: `ns_since_midnight()` and `mid_diff()` (the
midnight-wrap-tolerant difference, signed so clock skew shows as a
negative number instead of a huge positive one), and the `overloaded`
helper used with `std::visit` (6.12).

### 6.3 Binary serialization done properly

**File:** `include/wire.hpp`

Why not `memcpy` a struct onto the wire, the way Phases 0–5 did?

1. **Endianness:** x86 and ARM are little-endian; network protocols are
   big-endian ("network byte order"). The value `0x01020304` is sent as
   bytes `01 02 03 04`.
2. **Padding and alignment:** the compiler inserts invisible bytes, and
   `__attribute__((packed))` removes them at the cost of unaligned
   accesses and non-portable code.
3. **Strict aliasing:** casting a `uint8_t*` buffer to `MyStruct*` and
   reading through it is undefined behaviour in C++.

The portable way is shifts:

```cpp
inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
```

g++ `-O2` recognises this pattern and emits one `MOVBE` (or `MOV` +
`BSWAP`) instruction, so the readable version costs nothing. Check it
yourself with `objdump -d` or godbolt.org.

**Bounds-checked cursors without exceptions.** `wire::Writer` and
`wire::Reader` never touch memory outside the buffer. On overflow they
latch `ok() == false` and every later call becomes a no-op, so you
check once at the end:

```cpp
wire::Reader r(p, n);
m.order_ref = r.u64(); m.shares = r.u32(); ...
return r.ok() ? Message(m) : std::nullopt;
```

That's the same idea as `std::ostream`'s failbit, and it keeps
exceptions off the hot path. Also here: 48-bit integers (ITCH
timestamps: a day is 8.64×10¹³ ns, which needs 47 bits), and "alpha"
fields (fixed-width, left-justified, space-padded ASCII).

### 6.4 ITCH market data in MoldUDP64 packets

**File:** `include/itch.hpp`

ITCH is an **order-by-order** feed. The exchange never tells you "best
bid is 100.00 x 700". It tells you every event (order 7 added, order 7
executed 100 shares, order 7 deleted) and you rebuild the book by
replaying them in sequence. These layouts are byte-for-byte NASDAQ
TotalView-ITCH 5.0:

**Add Order `'A'` (36 bytes)**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Message type `'A'` |
| 1 | 2 | Stock locate (small integer standing in for the symbol) |
| 3 | 2 | Tracking number |
| 5 | 6 | Timestamp, ns since midnight |
| 11 | 8 | Order reference number |
| 19 | 1 | Buy/sell indicator `'B'`/`'S'` |
| 20 | 4 | Shares |
| 24 | 8 | Stock symbol, space padded |
| 32 | 4 | Price, 4 implied decimals |

**Order Executed `'E'` (31):** common header (11) + order ref (8) +
executed shares (4) + match number (8).
**Order Cancel `'X'` (23):** header + order ref + cancelled shares (a
partial reduction).
**Order Delete `'D'` (19):** header + order ref (the whole order is gone).
**System Event `'S'` (12)** marks start and end of day; **Stock Directory
`'R'` (39)** maps locate codes to symbols; **End of Snapshot `'G'` (21)**
is from GLIMPSE (6.12).

Why the locate code: the hot path indexes an array by a 16-bit integer
instead of comparing 8-byte strings.

**MoldUDP64 framing** (one UDP datagram):

```
 0        10            18     20
 | session | sequence #  | count | len | message | len | message | ...
   10 B      8 B (first     2 B    2 B             2 B
             message's seq)
```

- The k-th message in a packet has sequence `header.sequence + k`.
- **Packing several messages per datagram** amortizes the per-packet
  costs (syscall, interrupt, 28 bytes of UDP/IP headers). The exchange
  averaged 3.5 messages per packet in the runs above.
- **`count == 0` is a heartbeat** carrying the *next* expected sequence,
  so a receiver that lost the last packet before a quiet period still
  detects the gap.
- **`kMaxPacketLen = 1400`:** stay under a 1500-byte Ethernet MTU minus
  IP and UDP headers. An IP-fragmented datagram is lost if *any* fragment
  is lost, which multiplies the loss rate.
- `PacketBuilder` owns an in-object 1400-byte buffer: building a packet
  never allocates.
- `itch::decode` checks the type **and the exact length** and returns
  `std::optional<std::variant<...>>`: a truncated message is rejected,
  never half-applied to the book.

### 6.5 TCP framing: SoupBinTCP and the FrameReader

**File:** `include/soupbin.hpp`

TCP delivers a **byte stream**, not messages. One 44-byte `send()` can
arrive as `recv()`s of 20 + 24 bytes, and two sends can arrive glued
together. Code that assumes one `recv()` equals one message works on
loopback in testing and breaks under load. Every TCP protocol needs
framing; SoupBinTCP uses a length prefix:

```
| packet length (u16, counts type + payload) | packet type (1 char) | payload |
```

Types: client→server `'L'` login, `'U'` unsequenced data (orders), `'R'`
heartbeat, `'O'` logout; server→client `'A'` login accepted, `'J'` login
rejected, `'S'` sequenced data (execution reports), `'H'` heartbeat,
`'Z'` end of session.

`FrameReader` is the receive side:

```cpp
ssize_t n = recv(fd, r.write_ptr(), r.write_space(), 0);   // straight into its buffer: no extra copy
r.commit(n);
while (r.next(type, payload, len) == FrameReader::Status::Frame) handle(type, payload, len);
```

- Partial frame → `NeedMore` (the bytes stay buffered).
- `length == 0` or `> kMaxFrame (1024)` → `Error`, and the caller drops
  the connection. This bounds what a buggy or hostile peer can make you
  buffer or wait for.
- When fully drained, both indices reset to 0 (no `memmove`). When the
  tail space can no longer hold a maximum-size frame, the unparsed
  remainder slides to the front.
- Fixed 64 KiB in-object buffer: no allocation per connection after
  construction.
- `tests/test_wire_protocols.cpp` feeds two frames *one byte at a time*,
  then glued into one read, then with hostile lengths.

### 6.6 OUCH binary order entry

**File:** `include/ouch.hpp`

Modelled on NASDAQ OUCH 5.0 (field set trimmed; fixed-width, big-endian,
inside SoupBinTCP frames).

**Enter Order `'O'` (41 bytes)**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `'O'` |
| 1 | 4 | UserRefNum (client's order number, strictly increasing per session) |
| 5 | 1 | Side |
| 6 | 4 | Quantity |
| 10 | 8 | Symbol |
| 18 | 8 | Price (signed, 4 implied decimals) |
| 26 | 1 | Time in force |
| 27 | 14 | ClOrdID (free-form client label) |

Responses (in `'S'` frames): **Accepted `'A'` (58)**, which carries the
exchange's **order reference number, the same id the public ITCH feed
uses for this order**, so a firm can recognize its own orders in market
data. Also **Executed `'E'` (34)** with price, liquidity flag and match
number; **Canceled `'C'` (18)** with the decrement and a reason (`'U'`
user, `'I'` IOC remainder, `'Q'` self-match, `'S'` supervisory); and
**Rejected `'J'` (28)**.

- **Why UserRefNum instead of a string token (OUCH 4 → 5):** "is this
  greater than the last one?" catches duplicates and replays in O(1) with
  no lookup, and an integer key is cheap to index on both sides. The
  exchange enforces it (`handle_enter`: `m.user_ref <= s.last_user_ref` →
  reject `'N'`).
- **Cancel semantics:** `CancelOrder{user_ref, qty}` carries the order's
  *new intended size*; 0 means cancel completely. Cancels can only
  shrink an order. A size reduction keeps time priority; an increase
  would lose it, so it isn't allowed.

### 6.7 FIX

**File:** `include/fix.hpp`, `bench/protocol_bench.cpp`

FIX is text `tag=value` fields separated by SOH (0x01, shown as `|`).
A real message produced by the encoder:

```
8=FIX.4.4|9=112|35=D|49=PFTRADER|56=PFX|34=1999999|52=20261017-14:30:00.123|
11=PF123456|55=PFRG|54=1|38=107|40=2|44=100.63|59=3|10=125|
```

| Tag | Name | Here |
|---|---|---|
| 8 | BeginString | always first |
| 9 | BodyLength | bytes from after `9=...|` up to and including the SOH before `10=` |
| 35 | MsgType | `D` = NewOrderSingle (`8` = ExecutionReport, `F` = cancel, `A` = logon, `0` = heartbeat) |
| 49 / 56 | Sender / TargetCompID | session identities |
| 34 | MsgSeqNum | session sequence number (gap → ResendRequest `2`) |
| 52 | SendingTime | UTC |
| 11 | ClOrdID | client order id |
| 55 / 54 / 38 / 40 / 44 / 59 | Symbol / Side (1 buy, 2 sell) / OrderQty / OrdType (2 limit) / Price / TimeInForce | the order |
| 10 | CheckSum | sum of all preceding bytes mod 256, 3 digits, always last |

Implementation lessons:

- **Build backwards.** BodyLength is only known after the body is
  written, so the encoder writes the body into `buf + 24`, then writes
  `8=FIX.4.4|9=<len>|` right-to-left so it ends exactly where the body
  begins, and returns a `string_view` that starts inside the buffer. No
  second pass, no `memmove`.
- **`std::to_chars` / `std::from_chars`:** no locale, no allocation, no
  exceptions, unlike `sprintf`, `std::stoi` or `stringstream`.
- **A string-literal trap:** `"8=FIX.4.4\x01" "9="` is two literals on
  purpose. Written as `"\x019="`, the compiler would read the hex escape
  `\x019` as one character.
- **Cache formatted timestamps.** Formatting SendingTime per message is
  expensive, so real engines reuse the `YYYYMMDD-HH:MM:SS` prefix and
  only rewrite the milliseconds.
- **Zero-copy parse:** `fix::Message::parse` records each field as a
  `(tag, string_view)` into the caller's buffer, validates that 8 is
  first, 9 is second and 10 is last, and checks BodyLength and the
  checksum. `find()` is a linear scan, which beats a hash map for ~15
  fields.

**Measured** (`make bench-protocol`, 2M iterations):

| Format | Bytes | Encode | Decode |
|---|---|---|---|
| OUCH-style binary + frame | 44 | 4.5 ns | 25.3 ns |
| FIX 4.4 tag=value | 135 | 188.3 ns | 382.3 ns |

FIX costs 3.1× the bytes, 42× more to encode and 15× more to decode,
even with both codecs allocation-free. The gap comes from the format:
fixed offsets vs scanning for delimiters, fixed widths vs ASCII digit
conversion, no checksum pass (TCP already checksums the data). That's
why the latency-critical path to a venue uses its native binary protocol
(OUCH, CME iLink3 SBE, Eurex ETI), while FIX remains the universal
language for brokers, drop copies and slower venues.

### 6.8 FlatIdMap: an open-addressing hash map

**File:** `include/flat_id_map.hpp`, `tests/test_flat_id_map.cpp`

"Find order 123456789" runs on every execute, cancel and delete, so it's
the hottest lookup in a book.

**Why not `std::unordered_map`:** it's node-based. Each insert calls the
allocator, and each lookup follows a pointer from the bucket array to a
separately allocated node, which is often a cache miss. It also
**rehashes** (allocates a bigger table and moves every element) as it
grows, causing a multi-millisecond stall at an unpredictable moment.

**FlatIdMap's design:**

- One flat `std::vector<Slot{key, value}>`, sized once in the
  constructor. Nothing allocates after that.
- **Open addressing with linear probing:** on a collision, try the next
  slot. Neighbouring slots share cache lines, so a 3-slot probe usually
  costs one cache miss.
- **Power-of-two capacity + Fibonacci hashing:**
  `slot = (key * 0x9E3779B97F4A7C15) >> (64 - log2(capacity))`. The
  constant is 2⁶⁴/φ. Multiplying scrambles sequential ids (exchanges
  issue 1, 2, 3, ...) across the table, and the shift keeps the
  well-mixed high bits. No modulo.
- **Load factor ≤ 50%:** capacity is the next power of two ≥ 2 ×
  max_entries. Linear probing degrades sharply above ~70% full, while at
  50% the expected successful probe length is about 1.5. This spends
  memory to buy predictable latency.
- **Key 0 means "empty",** which is free since order ids start at 1.
- **Backward-shift deletion instead of tombstones.** With tombstones,
  deleted slots are only marked; under constant add/cancel churn they
  pile up and every probe gets longer until you rebuild (a stall).
  Backward shift repairs the cluster on each erase:

```
 erase key K at slot i (now a hole). Walk forward j = i+1, i+2, ... until an empty slot.
 For the entry at j with home slot h: if the hole i lies on its probe path
 (dist(h, j) >= dist(i, j), measured cyclically), move it into the hole: i = j.
 Finally mark slot i empty.

 example, 8 slots, home in brackets:   [A:2][B:2][C:3][D:5]  at slots 2,3,4,5
 erase A at 2 -> hole 2. j=3: B home 2, dist(2,3)=1 >= dist(2,3)=1 -> move B to 2, hole=3
                         j=4: C home 3, dist(3,4)=1 >= dist(3,4)=1 -> move C to 3, hole=4
                         j=5: D home 5, dist(5,5)=0 <  dist(4,5)=1 -> D stays (hole is before its home)
                         j=6: empty -> stop; slot 4 marked empty
```

- **AoS layout (`{key, value}` together):** on a hit, the key and value
  are on the same cache line. SoA (separate key and value arrays) fits
  more keys per line, and wins when misses dominate.
- **Fixed capacity:** `insert()` returns false when full rather than
  growing.

**Testing a hash map:** `test_flat_id_map` runs 200,000 random
insert/erase/find operations against `std::unordered_map` as an oracle,
including a tiny 16-slot table that is always nearly full and wraps
around. That's where a wrong shift condition would make keys silently
unreachable.

**Measured** (`make bench-book`, 200k sequential ids, random lookup order):

| | insert | find | erase |
|---|---|---|---|
| FlatIdMap | 8.3 ns | 8.4 ns | 12.0 ns |
| std::unordered_map (pre-reserved) | 16.8 ns | 12.5 ns | 58.2 ns |

### 6.9 The order book

**Files:** `include/order_book.hpp`, `src/order_book.cpp`,
`tests/test_order_book.cpp`, `bench/book_bench.cpp`

The textbook design is `std::map<Price, std::list<Order>>` per side plus
`std::unordered_map<OrderId, iterator>`. It's correct and readable, but
every add allocates up to three nodes (list, map, hash) scattered across
the heap. PulseForge's book uses five ideas instead:

**1. Price ladder.** Prices live on a tick grid inside a band, so
`level = (price - min_price) / tick` is a dense integer and each side is
a `std::vector<Level>` indexed by it. Finding a level is an array index,
not a tree walk.

```cpp
struct Level { uint64_t qty; uint32_t head; uint32_t tail; uint32_t count; };  // 24 bytes
```

$50.00–$150.00 at 1 cent = 10,001 levels × 24 B ≈ 240 KB per side.

**2. Occupancy bitmap + count-leading/trailing-zeros.** One bit per
level says whether it's non-empty. When the best level empties, the next
best is found 64 levels per instruction:

```cpp
// lowest set bit at or above idx  (next ask)
uint64_t word = bits[w] & (~0ULL << b);
if (word) return w * 64 + __builtin_ctzll(word);     // TZCNT: index of lowest set bit
// highest set bit at or below idx (next bid)
uint64_t word = bits[w] & mask_up_to(b);
if (word) return w * 64 + 63 - __builtin_clzll(word); // LZCNT
```

Worked example: asks at levels 63, 64 and 191 (bits in words 0, 1 and 2).
Removing level 63 → `find_next(64)` → word 1 has bit 0 set → 64.
Removing 64 → `find_next(65)` → word 1 masked above bit 1 is empty, so
move to word 2 → `ctz` = 63 → 2×64 + 63 = 191. That's three word reads
instead of 128 level checks. (C++20 spells these `std::countr_zero` and
`std::countl_zero`; `__builtin_ctzll(0)` is undefined, hence the
`word != 0` check.) `test_bitmap_scans` tests exactly these word
boundaries.

**3. Cached best level.** `best_` per side is updated on every add
(higher index is better for bids, lower for asks) and on every level
removal (via the bitmap scan). `best_price()` and `best_qty()`, which the
strategy calls on every update, are loads, not searches.

**4. Order pool + intrusive doubly-linked FIFO.** All orders live in
one pre-sized `std::vector<BookOrder>` (40 bytes each). A free list
threads through unused nodes (the `MemPool` idea), with **32-bit indices
instead of pointers**: half the size, and still valid if the vector is
ever copied. Orders at a price form a FIFO through `prev`/`next` indices
stored *inside* the order (intrusive: no separate list nodes):

- add → append at the tail (newest = lowest time priority);
- match → take from the head;
- cancel from the middle → O(1) unlink, which is why `prev` is stored as
  well as `next`.

**5. Id index:** `FlatIdMap<uint32_t>` maps order id → pool index.

| Operation | Map book | PulseForge book |
|---|---|---|
| add at existing price | O(log L) tree walk + 2 allocations | O(1), no allocation |
| add at new price | O(log L) + 3 allocations | O(1) + set one bit |
| cancel by id | hash lookup + list erase + maybe map erase | hash lookup + O(1) unlink |
| best price | O(1) (`begin()`) | O(1) (cached) |
| best level empties | O(1) amortized (`begin()` again) | bitmap scan, 64 levels per instruction |
| memory | proportional to orders, scattered | fixed up front, contiguous |

**Measured** (`make bench-book`, 1M ops: 50% add, 30% cancel, 12% execute, 8% top-of-book):

```
throughput, mean ns/op:  MapBook 106.7   OrderBook 29.4   (3.6x)
  impl       op          count     p50     p99    p99.9
  MapBook    add        424786     121     311      551
  OrderBook  add        424786      60     231      431
  MapBook    cancel     374788     180     621      902
  OrderBook  cancel     374788      80     241      431
  MapBook    execute    120535     121     521      842
  OrderBook  execute    120535      70     240      411
```

(Each per-op sample includes ~30 ns of clock overhead; the throughput
line has none.) Note that `top` is identical for both books: `begin()` of
a `std::map` is also O(1). The wins come from allocation and pointer
chasing, and they show up most in the tails.

**One class, two roles.** The same `OrderBook` is the exchange's
authoritative book and the trader's replica. The replica can't drift
because of a different data structure, and every bug fix applies to
both.

**Trade-offs to explain:** memory for empty levels; prices outside the
band are rejected (`BadPrice`) rather than handled; and if prices move
far outside the band you'd recentre the ladder, which real books do with
a ring-buffer ladder around the current price.

**Testing:** `test_randomized_against_reference` runs 200,000 random
add/reduce/remove operations against a deliberately naive
`std::map + std::list` reference book and compares best price, best
quantity, front order id and order count after *every* step.

### 6.10 The matching engine

**Files:** `include/matching_engine.hpp`, `src/matching_engine.cpp`,
`tests/test_matching_engine.cpp`

```cpp
void MatchingEngine::submit(uint16_t locate, const OrderRequest &req) {
    validate: qty in (0, max], price on tick and inside the band, id not duplicate -> else on_rejected
    on_accepted(req)                                     // OUCH sends Accepted before any fill
    remaining = req.qty
    while (remaining > 0 && (maker = book.front(opposite side)) && crosses(maker->price)) {
        if (same owner && owner != 0)  -> self-match prevention (below)
        q = min(remaining, maker->qty)
        on_fill(Fill{price = maker->price, q, maker..., taker...})   // trade at the MAKER's price
        book.reduce_front(contra, q)                    // O(1): no id lookup
        remaining -= q
    }
    if (remaining > 0)  IOC -> on_taker_cancelled(remaining)   |   Day -> book.add(...) -> on_rested
}
```

**Worked example** (from `test_walk_the_book_then_rest`): asks 100.00×100
(id 1), 100.01×100 (id 2), 100.05×100 (id 3). A Day buy for 500 @ 100.01
produces:

```
Accepted(4) -> Fill(maker 1, 100 @ 100.00) -> Fill(maker 2, 100 @ 100.01) -> Rested(4, 300 @ 100.01)
```

The order paid 100.00 for its first 100 shares: **price improvement goes
to the aggressor**, because trades happen at the resting price. Order 3
at 100.05 is beyond the limit and untouched. The best bid is now 100.01
(the remainder), and the best ask is 100.05.

**Self-match prevention.** If the incoming order's owner matches the
front maker's owner:
- `CancelResting` (cancel oldest): remove the resting order (reason
  SelfMatch) and keep matching against the next one;
- `CancelIncoming` (cancel newest): cancel the rest of the incoming
  order and stop.
Owner 0 (the exchange's synthetic flow) is exempt, since those orders
represent many different participants.

**Cancel:** only the owner may cancel; `new_qty >= current` is
`NoChange`; reducing keeps queue priority; 0 removes the order.

**Events, not side effects.** The engine knows nothing about sockets,
ITCH or OUCH. It reports to an `EngineListener` (the Observer pattern):

| Engine event | Public market data (ITCH) | Private report (OUCH) |
|---|---|---|
| `on_accepted` | – | `'A'` Accepted to the owner |
| `on_rejected` | – | `'J'` Rejected |
| `on_fill` | `'E'` Order Executed, referencing the **maker's** order | `'E'` to the maker (liquidity `'A'`) and to the taker (`'R'`) |
| `on_rested` | `'A'` Add Order | – |
| `on_resting_cancelled` | `'X'` Order Cancel (partial) or `'D'` Order Delete (full) | `'C'` Canceled |
| `on_taker_cancelled` | – (the taker was never displayed) | `'C'` Canceled (IOC remainder, self-match) |

This separation is what makes the engine testable without a network:
`tests/test_matching_engine.cpp` records events into a vector and asserts
the exact sequence.

Design points you should be able to defend:

- **Single-threaded by design.** Determinism (the same input sequence
  always produces the same trades) is a regulatory and operational
  requirement: you must be able to replay an incident. One thread with a
  warm cache beats locks. Exchanges scale by partitioning symbols across
  engines, not by sharing a book between threads.
- **Virtual dispatch:** one indirect call per event. The exchange's
  listener is `final`, so calls through the concrete type can be
  devirtualized. The zero-cost alternative is a template parameter
  (`MatchingEngine<Listener>`, static polymorphism, a cousin of CRTP).
- **Re-entrancy:** a listener must not call back into the engine from
  inside a callback, because the book is mid-update. This is a real rule
  in event-driven systems.
- **Copy before mutate:** `const BookOrder before = *maker;` before
  `remove()`, because the pointer is invalid afterwards. Pointer and
  iterator invalidation is the bug behind many crashes.

### 6.11 The exchange process

**File:** `src/exchange.cpp`, `include/event_loop.hpp`

One thread, one `epoll` loop, several kinds of file descriptor:

```cpp
ep_.add(order_listen_, EPOLLIN, kTagOrderListen);   // OUCH sessions
ep_.add(snap_listen_,  EPOLLIN, kTagSnapshotListen); // snapshot requests
ep_.add(timer_,        EPOLLIN, kTagTimer);          // timerfd, every 1 ms
ep_.add(signals_,      EPOLLIN, kTagSignal);         // signalfd: SIGINT/SIGTERM as data
while (!stop_) {
    n = ep_.wait(events, 64, spin ? 0 : -1);
    dispatch each event by tag
    reap_dead_sessions();
    flush_dirty_sessions();   // private OUCH reports first...
    md_.flush();              // ...then the public ITCH packet
}
```

**Linux event-loop primitives** (`include/event_loop.hpp`):

- **epoll:** "which of these fds are ready?" costs O(ready) per wakeup,
  vs O(all fds) for `select`/`poll`. Level-triggered (the default) keeps
  reporting an fd while data remains. Edge-triggered (`EPOLLET`) reports
  only transitions and *requires* draining to `EAGAIN` every time.
- **timerfd:** a timer you `read()`; the value is the number of
  expirations since the last read, so a late loop knows it missed ticks.
- **signalfd:** signals become readable data in the loop instead of an
  asynchronous handler interrupting your code at an arbitrary
  instruction (with all the async-signal-safety rules that implies). The
  signals must be blocked with `sigprocmask` first, before any thread
  starts, since threads inherit the mask.
- **Tags, not pointers, in `epoll_event.data`:** if a session is closed
  while an event for it is already sitting in the current batch, looking
  up an id fails safely, whereas a pointer would dangle. That's a classic
  use-after-free in event loops.

**Sessions** (`struct Session`): a socket, a `FrameReader`, an output
byte queue, login state, `last_user_ref`, heartbeat times, and a
`FlatIdMap<OrderHandle>` from UserRefNum to (locate, order id) for
cancels.

- **Non-blocking writes:** `send(MSG_DONTWAIT)`. On `EAGAIN` (the
  kernel's send buffer is full, i.e. a slow client), the exchange arms
  `EPOLLOUT` and finishes later. One slow client never blocks the loop.
- **Dirty list:** callbacks only *append* to a session's buffer; each
  session is flushed once at the end of the loop iteration. Many reports
  go out in one `send()`, with fewer syscalls.
- **Private before public:** session flushes run before the market-data
  flush, so a firm hears about its own fill no later than everyone else
  hears about the trade.
- **`TCP_NODELAY`** on every order socket. Nagle's algorithm delays small
  writes to coalesce them, and combined with delayed ACKs that can add up
  to ~40 ms. For small urgent messages, Nagle is pure latency.
- **Cancel-on-disconnect:** when a session drops, all its resting orders
  are cancelled, because a firm must not be left with live orders it can
  no longer manage. Collect the handles first, *then* cancel: the cancel
  callbacks erase from the same map, and modifying a container while
  iterating it is a classic bug.
- **Heartbeats and timeouts:** server heartbeat after 1 s of idle output;
  disconnect a client that is silent for 15 s. MoldUDP64 heartbeats on
  the feed when it's idle.
- **Bind to 127.0.0.1 by default** (`--bind`): an unauthenticated order
  port should never listen on every interface by accident.

**Synthetic flow** (`synth_step`): per symbol, a fair price does a slow
random walk. Each step either adds a passive Day order 1–8 ticks from
fair, cancels (sometimes only halving) a random synthetic order, or
sends an aggressive IOC through fair that generates trades. Details
worth noticing:

- `Xorshift64`: three shifts and three xors per number. `std::mt19937_64`
  carries 2.5 KB of state, which is overkill here. (Never use either for
  anything security-related.)
- **Swap-and-pop** removes an element from an unordered vector in O(1).
- **Lazy deletion:** ids of synthetic orders that were filled stay in
  the `live` vector until a cancel attempt finds them gone. Cheaper than
  tracking every fill.

**The snapshot service** (`send_snapshot`): `md_.flush()` first, so
"everything before sequence N is in the book" is exactly true; then a
Stock Directory per symbol, every resting order as an Add Order *in
priority order* (`for_each_order`: best price first, FIFO within a
level), then End-of-Snapshot carrying N, then end of session. Because the
engine is single-threaded, the snapshot is consistent by construction:
no other event can happen in the middle of it.

**Measured inside the exchange:** client order decoded → engine done →
reports queued: p50 **1.7 µs** (busy-polling), 3.0 µs (blocking).

### 6.12 The feed handler: snapshot, replay, gaps

**Files:** `include/feed_handler.hpp`, `src/trader.cpp`
(`setup_md_socket`, `snapshot`, `handle_packet`, `resync`)

**Startup order is the whole trick:**

```
1. join the multicast group  -> the kernel starts queueing live packets in SO_RCVBUF
2. fetch the snapshot (TCP)  -> the book as of sequence S
3. drain the socket          -> skip messages < S (already in the snapshot), apply >= S
```

If you snapshot first and subscribe second, the messages published in
between are lost forever. Here the kernel socket buffer itself is the
buffer for the overlap. The trader prints `duplicates N`: messages from
packets that straddled S.

**`SequenceTracker`** classifies each message's sequence number:
`== expected` → Apply (and advance); `< expected` → Duplicate;
`> expected` → **Gap**. A heartbeat whose next-sequence is beyond
`expected` is also a gap.

**`apply_itch` with `std::visit`:**

```cpp
return std::visit(overloaded{
    [&](const itch::AddOrder &m)      { ... book.add(...) ... },
    [&](const itch::OrderExecuted &m) { ... book.reduce(m.order_ref, m.executed_shares) ... },
    [&](const itch::OrderCancel &m)   { ... book.reduce(...) ... },
    [&](const itch::OrderDelete &m)   { ... book.remove(...) ... },
    [](const itch::SystemEvent &)     { return ApplyResult::Ignored; },
    ...
}, msg);
```

The compiler checks that *every* alternative is handled. Add a message
type to the variant and this stops compiling until you handle it, which
a `switch` on a type byte would never enforce. (On the very hottest path,
some firms still prefer a `switch` on the raw type byte, because it
avoids decoding into a variant at all.)

**`Inconsistent`:** an execute, cancel or delete for an order id we
don't have, or for more shares than it has left, means the replica is
out of sync. That's treated exactly like a gap.

**On a gap or inconsistency (`resync`):**
1. mark the book **stale** (the strategy is never consulted on a stale
   book);
2. **pull open orders** (`cancel_all`), because you cannot manage quotes
   in a market you can't see;
3. re-snapshot, then resume incremental processing at the new S.

Observed with `run_trading.sh gaps` (the exchange drops every 2000th packet):

```
[trader] GAP: sequence gap (expected seq 7535) -> book STALE, pulling orders, re-snapshotting
[trader] snapshot: 299 PFRG orders, resume incremental at seq 7545  (bid 99.8400 x 1600 / ask 99.8500 x 400)
...
market data  packets 9144  messages 31431  duplicates 3  gaps 5  inconsistencies 0  snapshots 6
```

Five gaps, five re-snapshots, and zero inconsistencies afterwards,
meaning the rebuilt book matched every later message. The strongest
correctness test in the repository is `test_replica_matches_exchange`
(`tests/test_trader_logic.cpp`). It sends 50,000 random operations
through the *real* `MatchingEngine`, encodes every event to ITCH bytes,
decodes and applies them to a replica, and asserts that the replica
equals the exchange book: same orders, same sizes, same **priority
order**. A second replica joins halfway through from a snapshot and
must converge too.

**Production details to know (not implemented here):**
- **Retransmission instead of snapshot:** MoldUDP64 has a request server
  (send session, sequence, count; get the missing packets back by unicast
  UDP). It's faster than a snapshot for small gaps.
- **A/B line arbitration:** exchanges publish every packet on two
  independent multicast feeds over different network paths. The handler
  takes whichever copy of sequence N arrives first and drops the other as
  a `Duplicate`.
- **Evaluate the strategy per packet, not per message:** the trader runs
  the strategy once after *all* messages in a packet are applied, so it
  never acts on an intermediate state (for example, between an execution
  and the delete that follows it in the same packet).

### 6.13 The strategy

**File:** `include/strategy.hpp`

Order-book imbalance at the top of book:

```
imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty)     in [-1, +1]
> +60%  -> buyers queueing, thin offer likely to be lifted -> BUY IOC at the best ask
< -60%  -> SELL IOC at the best bid
```

- **Integer-only test:** `(bid - ask) * 100 > pct * (bid + ask)`. No
  division, no floating point, exactly reproducible.
- **Guards:** no signal while an order is in flight, a cooldown (1 ms
  default), a minimum top-of-book size, and a soft position limit.
- **Why it loses money,** which is the most valuable lesson here. In the
  first run it ended +2000 shares and **−$1,990**:
  1. a taker pays half the spread on every trade, plus fees in real life;
  2. in this synthetic market the signal has no real predictive power;
  3. **your own trades feed your signal:** lifting the offer thins the ask,
     which raises the imbalance, which triggers another buy, until the
     position limit stops it. Strategies must be tested against the
     market impact of their own orders.
  Real taker strategies only work when the signal beats spread plus fees,
  *and* only if you're among the first to act on it. That race is what
  tick-to-trade latency is about.

### 6.14 Pre-trade risk

**File:** `include/risk.hpp`, `tests/test_trader_logic.cpp` (`test_risk_gate`)

Every order passes `RiskGate::check()` *before* it's sent. This is
legally required (US SEC Rule 15c3-5, the market access rule; EU MiFID II
RTS 6), and Knight Capital's 2012 incident (about $440M lost in 45
minutes after a bad deployment sent unintended orders) is the standard
illustration of why.

| Check | Catches |
|---|---|
| Kill switch | anything at all: ops, or the system itself, can stop all new orders instantly |
| Max order qty | fat finger: 100,000 instead of 100 |
| Max notional | price × qty cap per order |
| Price collar (bps from mid) | a price typed wrong by 10×, or a broken or stale book |
| Max open orders | runaway order counts in flight |
| **Worst-case position** | `position + open buys + this order <= max` (and the mirror for sells). Checking only the current position lets 10 in-flight buys exceed the limit together. |
| Rolling-window rate | at most N orders in any 1 s window: runaway loops, and exchange message-rate limits that would disconnect you |

**The rolling window in O(1):** a ring buffer holds the send times of the
last N orders. If the oldest of them is less than 1 s old, N orders went
out in the last second, so refuse. No allocation and no scan.

Hot-path properties: no allocation, no locks, integer arithmetic only,
and checks ordered cheapest first. The accounting contract
(`on_order_sent`, `on_fill`, `on_cancelled`, `on_order_closed`) is driven
by the OUCH responses. In the spin run, the rate throttle blocked 25
signals ("risk blocks rate throttle=25").

### 6.15 The order-entry client

**File:** `src/trader.cpp`

- **Connect, enable TX timestamps, log in:** TX timestamps are enabled
  right after `connect()` and *before* the first byte, so the timestamp
  id space starts at the login's first byte (6.16). The login is a
  blocking request with a 2 s `SO_RCVTIMEO`, and only then does the
  socket go non-blocking.
- **Encode in place:** the OUCH message is encoded directly at
  `frame + 3`, then the 3-byte SoupBin header is written in front. No
  intermediate buffer.
- **`oe_send()`:** every byte to the order socket goes through one
  function so `stream_bytes_` stays exact. If `send()` is partial or
  returns `EAGAIN`, the rest goes to `oe_pending_` and `EPOLLOUT` is
  armed. Later sends append to the pending buffer, preserving order on
  the stream.
- **Order lifecycle** (`LiveOrder` in a `FlatIdMap` keyed by UserRefNum):

```
 sent ──Accepted──> live ──Executed (leaves -= q)──> ... ──leaves == 0──> closed
   └──Rejected──> closed                └──Canceled (leaves -= q)──> ... ──> closed
```

  Closing an order updates the risk gate and erases it. A report for an
  unknown UserRefNum is counted, never trusted.
- **Position and PnL:** `cash -= price × qty` on buys and `+=` on sells.
  Mark-to-market PnL = `cash + position × mid`.
- **Wind-down:** cancel anything open, pump the loop for up to 200 ms to
  collect acknowledgements, log out, report.

### 6.16 Kernel timestamps and the tick-to-trade report

**Files:** `include/net_timestamp.hpp`, `src/net_timestamp.cpp`,
`src/trader.cpp` (`report`)

**The problem:** a timestamp taken after `recvmsg()` returns includes the
interrupt, softirq, socket wakeup, scheduling and syscall time, which
hides exactly the delays you're trying to measure.

**`SO_TIMESTAMPING`** asks the kernel to stamp packets at fixed points
in the stack and return the stamps as ancillary data:

| Flag | Stamp taken | Delivered |
|---|---|---|
| `SOF_TIMESTAMPING_RX_SOFTWARE` | when the packet enters the network stack, just after the driver | with the packet, as a `SCM_TIMESTAMPING` cmsg on `recvmsg` |
| `SOF_TIMESTAMPING_TX_SOFTWARE` | when the driver hands the packet to the device | later, on the socket's **error queue** (`recvmsg(MSG_ERRQUEUE)`), signalled by `EPOLLERR` |
| `..._RX/TX_HARDWARE` + `RAW_HARDWARE` | by the NIC's own clock at the PHY | same API, in `ts[2]` instead of `ts[0]`; needs a capable NIC (`ethtool -T eth0`) and the `SIOCSHWTSTAMP` ioctl |
| `SOF_TIMESTAMPING_SOFTWARE` | — | *reporting* flag: generation and reporting are separate bits |
| `OPT_ID` | — | tag each TX stamp with an id; for TCP the id is the **byte offset** of the last byte of that `send()` |
| `OPT_TSONLY` | — | don't loop the packet payload back with the stamp |

Ancillary data primer: `recvmsg()` fills a control buffer with typed
records (`cmsghdr{len, level, type}` followed by data), which you walk
with `CMSG_FIRSTHDR`/`CMSG_NXTHDR`. `CMSG_DATA()` may be unaligned for the
struct inside, so the code `memcpy`s out instead of casting.

**Matching a TX stamp to an order:** if 1,000 bytes have been sent and
the next order frame is 44 bytes, its stamp arrives with id 1043. The
trader keeps a 256-entry ring of `{expected id, kernel RX time of the
trigger, send start}`, separate from `LiveOrder` because the stamp can
arrive after the order has already completed. One subtlety: for TCP the
kernel only accepts `OPT_ID` on a connected socket.

**Every stage of the loop is stamped on `CLOCK_REALTIME`:**

```
exchange event (ITCH ts) -> kernel RX (SO_TIMESTAMPING) -> user space -> book updated
  -> strategy + risk + encode -> send() start -> driver TX stamp -> send() returned
  -> exchange accepted (OUCH ts) -> ack back in user space
```

**Measured: blocking vs busy-poll** (p50, µs):

| Stage | Both sides block in `epoll_wait` | Both busy-poll, pinned |
|---|---|---|
| exchange event → kernel RX | 16.1 | 12.5 |
| kernel RX → user space (wakeup + recvmsg) | **96.7** | **10.6** |
| user space → book updated (decode + apply) | 2.4 | 1.3 |
| book updated → `send()` (strategy + risk + encode) | 4.8 | 1.4 |
| `send()` syscall | 100.7 | 25.6 |
| **TICK-TO-TRADE: kernel RX → `send()` returned** | **214.7** | **39.6** |
| `send()` start → driver TX stamp | 20.2 | 11.3 |
| kernel RX → driver TX (software wire-to-wire) | 132.5 | 25.3 |
| `send()` start → ack in user space (round trip) | – | 46.0 |

How to read each row. This is the kind of analysis interviews probe.

1. **Wakeup dominates when blocking.** A sleeping thread must be woken by
   the scheduler: an inter-processor interrupt, a context switch and
   cache refill. In a VM (WSL2 runs on Hyper-V) that's much slower than on
   bare metal. Busy-polling removes it, at the cost of a core at 100%.
   That's why HFT systems spin on isolated cores.
2. **The user-space work is tiny:** decode + book update ~1.3 µs per
   packet, and strategy + risk + encode ~1.4 µs. The application code is
   not the bottleneck. The kernel and the scheduler are, which is the
   argument for kernel bypass.
3. **`send()` is slow here because of loopback.** On `lo`, the receiving
   side's entire TCP processing (softirq) runs inline *inside the
   sender's `send()` call*, including waking the exchange. Hence
   `send()` p50 of 100 µs when the exchange is asleep, and 25 µs when it
   is spinning. On a real NIC, `send()` returns once the packet is queued
   to the device (a few µs, or ~1 µs or less with kernel bypass).
4. **That's also why wire-to-wire < tick-to-trade:** the TX stamp is
   taken at the loopback driver *during* `send()`, before `send()`
   finishes the receive-side work. And it's why the exchange can accept
   an order before the trader's `send()` returns (so that leg is measured
   from `send()` start).
5. **"Exchange event → kernel RX" includes the exchange's batching:**
   events are timestamped when generated but sent at the end of the loop
   iteration, packed ~3.5 per packet. Batching trades a few microseconds
   for fewer packets.
6. **Look at the tails.** p99 and p99.9 run 10–100× the median (page
   faults, interrupts, other processes, the hypervisor). In production
   you chase the tails: isolated cores, `mlock`, no allocations, huge
   pages, and interrupts steered off the hot cores.

**Avoiding page faults in the measurement code itself.**
`LatencySampler` does `resize(capacity); clear();` in its constructor.
`reserve()` alone only reserves *virtual* memory, so the first write to
each 4 KiB page would take a page fault (~1 µs) in the middle of the hot
path. Writing every element once faults all the pages in up front, and
`clear()` keeps the capacity. `--mlock` (`mlockall(MCL_CURRENT |
MCL_FUTURE)`) also pins every page in RAM so nothing can be swapped out.

**What production adds:** hardware timestamps at the NIC; PTP (IEEE 1588)
to synchronize NIC clocks across machines to tens of nanoseconds; and
passive network taps or switch timestamping (e.g. an Arista 7130
appliance) that capture every packet with a precise time, so tick-to-trade
is measured *on the wire*, outside the system being measured.

### 6.17 How the trading code is tested

| Test suite | Technique |
|---|---|
| `test_wire_protocols` | exact byte layouts and lengths, round trips, truncated and corrupted inputs, FIX BodyLength and CheckSum verified independently of the parser |
| `test_flat_id_map` | randomized differential test against `std::unordered_map` |
| `test_order_book` | unit tests + randomized differential test against a naive `std::map` + `std::list` book, compared after every step |
| `test_matching_engine` | recording listener: assert the exact event sequence for price/time priority, walking the book, IOC, cancels, self-match prevention, rejects |
| `test_trader_logic` | sequence tracker, `apply_itch`, risk gate, strategy, and the **replica-equals-exchange property test** through real ITCH bytes, including a late joiner via snapshot |
| sanitizer run | everything above plus a live exchange + trader session with simulated gaps, under ASan + UBSan |

The ideas generalize: compare a fast implementation against an obviously
correct slow one (**differential testing**), assert *properties* over
random inputs (property-based testing), test protocol code with adversarial
bytes (fuzzing in miniature), and run everything under sanitizers.

### 6.18 How the benchmarks are built

**Files:** `bench/book_bench.cpp`, `bench/protocol_bench.cpp`, `bench/queue_bench.cpp`

- **Same input for every contestant:** a deterministic, seeded operation
  stream.
- **Warm-up pass** (code paged in, branch predictors trained), then a
  **throughput pass with no per-op clock reads**, then a **latency pass**
  with a clock read pair per op. Report the clock overhead (~30 ns) so
  readers can subtract it.
- **Keep the optimizer honest:**
  `__asm__ __volatile__("" : : "g"(&v) : "memory")` tells the compiler
  the value is used and memory may have changed. Without it, a benchmark
  loop whose result is unused can be deleted entirely, and you measure
  0 ns. Vary inputs per iteration so work can't be hoisted out of the
  loop.
- **Report distributions,** not just means.
- **State what you measured.** For example, `queue_bench` measures
  saturated queueing delay, not handoff latency (3.8).

---

## 7. Hardware and OS reference

Numbers are rough orders of magnitude for a modern x86 server at 3–4
GHz. Measure your own (section 9 has exercises for it). The goal is to
know which things cost 1 ns, 100 ns, 10 µs and 1 ms.

### 7.1 The latency ladder

| Operation | Approx. |
|---|---|
| Register op / L1 cache hit | ~1 ns (4–5 cycles) |
| Branch mispredict | ~5 ns (15–20 cycles) |
| L2 hit | ~3–5 ns |
| L3 hit | ~10–20 ns |
| Uncontended atomic RMW (`lock xadd`, CAS) | ~5–20 ns |
| Cache line transfer between cores (contended atomic, false sharing) | ~40–100 ns |
| DRAM access (cache miss) | ~60–100 ns (+50–100 ns on the remote NUMA node) |
| `clock_gettime` via vDSO | ~20–30 ns (measured: ~30 ns) |
| Syscall round trip (no real work) | ~100–200 ns (more with mitigations) |
| Minor page fault | ~0.5–1 µs |
| Context switch / waking a sleeping thread | ~2–10 µs (measured ~97 µs p50 in WSL2) |
| Kernel UDP receive path, NIC → `recvfrom` | ~5–15 µs |
| Kernel-bypass receive (ef_vi, DPDK) | ~1 µs or less |
| Light in fibre | ~5 µs per km (≈ 2/3 c); microwave links are faster in air |
| Major page fault (disk) | ~1–10 ms |

### 7.2 Caches and cache lines

- Data moves between memory and caches in **64-byte lines**. Touching
  one byte loads 64.
- **Spatial locality:** arrays beat linked structures, because the
  hardware prefetcher detects sequential access and loads ahead. That's
  why the price ladder is an array and the order pool is contiguous.
- **Temporal locality:** keep the hot working set small enough to stay
  in L1/L2.
- **Coherence (MESI):** a line is Modified, Exclusive, Shared or
  Invalid in each core's cache. A write requires exclusive ownership,
  which invalidates other copies. That's the mechanism behind false
  sharing (3.5) and behind why contended atomics are slow.
- `alignas(64)` and explicit padding put independently written data on
  separate lines. C++17 also offers
  `std::hardware_destructive_interference_size` (where supported).

### 7.3 Virtual memory, TLB, page faults

- Every address is virtual and translated through page tables; the TLB
  caches translations (3.9).
- `malloc`/`new` of a large block maps virtual memory only. Physical
  pages arrive on the **first write** (a minor page fault). Pre-touch
  memory at startup (`LatencySampler`), and `mlockall` so nothing is
  swapped out (6.16).
- `fork` + copy-on-write, `mmap`, and `MAP_SHARED` vs `MAP_PRIVATE` are
  all on the same page-table machinery (3.6).

### 7.4 CPUs, SMT, NUMA, frequency

- **SMT/hyperthreading:** two logical CPUs share one physical core's
  execution units and L1/L2, so never put two hot threads on siblings
  (`/sys/devices/system/cpu/cpuN/topology/thread_siblings_list`).
- **NUMA:** in multi-socket servers, each socket has local memory and
  local PCIe lanes. Keep the thread, its memory and the NIC on the same
  node (`numactl --cpunodebind --membind`, and
  `/sys/class/net/eth0/device/numa_node`).
- **Frequency and C-states:** a core waking from a deep sleep state can
  take tens of microseconds. Latency servers pin a fixed frequency,
  disable deep C-states (`intel_idle.max_cstate=0`, `processor.max_cstate=1`
  or the BIOS equivalent), and busy-poll, which also keeps the core awake.
- **Isolation:** `isolcpus=`, `nohz_full=` (no scheduler tick on that
  core), `rcu_nocbs=`, and IRQ affinity, so the only thing running on a
  hot core is your hot thread.

### 7.5 Interrupts, softirqs and NAPI

The NIC raises an interrupt; the driver schedules NAPI polling in a
softirq; NAPI pulls many packets per interrupt, and under load it keeps
polling with interrupts disabled. Interrupt coalescing (`ethtool -C`)
trades latency for CPU: fewer interrupts, each one later. RSS spreads
flows across RX queues and cores; flow steering pins your feed's flow to
the queue whose IRQ is on the right core. `SO_BUSY_POLL` (4.5) lets your
thread do the NAPI polling itself.

### 7.6 Time synchronization

- **NTP:** synchronization to around a millisecond over the network.
  Fine for logs, useless for measuring microseconds across machines.
- **PTP (IEEE 1588):** a grandmaster clock (often GPS-disciplined)
  exchanges timestamped messages with clients. With **hardware
  timestamping** in NICs and PTP-aware switches (boundary or transparent
  clocks), clocks agree within tens of nanoseconds. Linux tools:
  `ptp4l` (syncs the NIC's PTP hardware clock) and `phc2sys` (syncs the
  system clock to it).
- **Regulation:** MiFID II RTS 25 requires HFT timestamps traceable to
  UTC within 100 µs, at 1 µs granularity or better.
- **Why it matters here:** "exchange event → kernel RX" subtracts an
  exchange timestamp from a local one. On one host the clocks are
  identical. Across hosts, that number is only as good as your clock
  synchronization.

### 7.7 Profiling tools you should be able to use

| Tool | For |
|---|---|
| `perf stat -e cycles,instructions,cache-misses,branch-misses ./bin/book_bench` | Hardware counters: IPC, miss rates |
| `perf record -g` + `perf report` | Where time goes (sampling profiler) |
| `perf c2c` | Finding false sharing |
| `perf trace` / `strace -c` | Syscalls and their cost |
| `bpftrace`, `bcc` | Tracing kernel paths (e.g. socket wakeups) |
| `ltrace`, `valgrind --tool=callgrind` | Library calls; instruction-level profiles |
| `gdb -p`, `pstack` | Live hung processes (`deadlock_demo`) |
| ASan, UBSan, TSan | Memory errors, undefined behaviour, data races |
| `ethtool -S/-T/-C/-l`, `netstat -su`, `ss -tmi` | NIC counters, timestamp capability, coalescing, queues; UDP drops; TCP internals |
| godbolt.org, `objdump -d` | What the compiler actually generated (check `get_u32` → `movbe`) |

---

## 8. Interview question bank

The answers are short pointers, not scripts: say them in your own words,
then go deeper with the referenced section.

### C++ language

1. **What is RAII and why does it matter for latency-sensitive code?**
   Resources tied to object lifetime, so every path (including
   exceptions) cleans up. Deterministic destruction, no GC pauses. (2.2)
2. **Rule of three/five/zero?** (2.2: `Socket` deletes copies and
   implements noexcept moves; `Exchange` follows rule of zero.)
3. **Why `noexcept` on move constructors?** `std::vector` only moves
   during reallocation if the move can't throw. (2.2)
4. **`std::variant` vs inheritance for a message type?** The variant is
   a closed set, stored inline without heap allocation, and `std::visit`
   checks exhaustiveness at compile time. Inheritance is open but needs
   pointers, usually heap allocation and virtual calls. (6.4, 6.12)
5. **What does `constexpr` buy you?** Work done at compile time and baked
   into the binary. (3.8)
6. **Virtual dispatch cost, and how to avoid it?** An indirect call
   (~1–3 ns when predicted, much more on a miss) and blocked inlining.
   Avoid it with templates/CRTP, `final` for devirtualization, or
   `std::variant`. (3.4, 6.10)
7. **What is undefined behaviour? Give three examples from this repo.**
   A data race (`race_demo`), strict-aliasing violations (why `wire.hpp`
   uses shifts), `__builtin_ctzll(0)`. (3.10, 6.3, 6.9)
8. **Why `string_view`, and what's the danger?** Zero-copy views; it
   dangles if the buffer dies (`fix::Message`, `FrameReader` payload
   lifetimes). (6.5, 6.7)
9. **`std::from_chars` vs `atoi`/`stoi`?** No locale, no exceptions, no
   allocation, and it reports where parsing stopped. (6.7)
10. **Iterator and pointer invalidation:** why does the engine copy
    `*maker` before `remove()`? (6.10)

### Concurrency

11. **Implement a lock-free SPSC queue on a whiteboard.** Explain each
    memory order. (3.2)
12. **acquire / release / relaxed / seq_cst: when to use each?** (3.2,
    3.5, `docs/memory_ordering.md`)
13. **What is false sharing, and how do you find and fix it?** (3.5,
    7.2, `perf c2c`)
14. **Spurious wakeups and the predicate wait.** (3.1)
15. **How do you shut down a producer/consumer pipeline cleanly?**
    Close, then drain, then join. (3.1)
16. **Disruptor vs a queue: when is broadcast the right shape?** (3.3)
17. **Deadlock: the four conditions and two fixes.** (3.10)
18. **Why are mutexes slow under contention?** The futex syscall plus
    sleep and wakeup through the scheduler. (3.1, 7.1)
19. **Spin vs block: trade-offs?** Latency vs CPU usage; spinning only
    makes sense on a dedicated core. (6.16: 97 µs → 11 µs)
20. **Why are the matching engine and order book single-threaded?**
    Determinism, no locks, cache warmth. Scale by partitioning. (6.10)

### Networking and Linux

21. **Trace a packet from the NIC to your code.** (4.1)
22. **TCP vs UDP: which for market data, which for orders, and why?**
    (4.8, 6.4, 6.5)
23. **How does multicast work, end to end?** (4.3)
24. **What is Nagle's algorithm and why disable it?** (6.11)
25. **TCP is a byte stream: what does that mean for your parser?** (6.5)
26. **How do you detect and recover from a market-data gap?** Sequence
    numbers and heartbeats; stale book, pull orders; snapshot + replay or
    retransmission; A/B arbitration. (6.12)
27. **What is kernel bypass? Name three implementations and the
    trade-offs.** (4.9)
28. **How would you measure tick-to-trade?** Hardware timestamps, PTP,
    wire capture; software: `SO_TIMESTAMPING`. (6.16)
29. **epoll vs select/poll; level- vs edge-triggered?** (6.11)
30. **What is SIGPIPE and how do you avoid it?** (4.8)
31. **What would you tune on a box to cut latency?** Isolation, pinning,
    IRQ affinity, C-states, huge pages, `SO_RCVBUF`, busy polling,
    coalescing, NUMA locality. (4.7, 7.4, 7.5)

### Data structures and algorithms

32. **Design a limit order book. What are the complexities?** (6.9)
33. **How do you find the next best price quickly when a level empties?**
    The bitmap plus ctz/clz. (6.9)
34. **Open addressing vs chaining; why linear probing; how does deletion
    work?** (6.8)
35. **Why are sequential order ids a problem for a naive hash, and how
    does Fibonacci hashing help?** (6.8)
36. **Why use indices instead of pointers in a pool?** (6.9)
37. **How do you remove from an unordered vector in O(1)?** Swap-and-pop.
    (6.11)
38. **How do you implement a rolling-window rate limit without
    allocation?** A ring of timestamps. (6.14)

### Trading and market structure

39. **Explain price-time priority. At what price does an aggressive order
    trade?** (6.1, 6.10)
40. **Maker vs taker; why does the liquidity flag matter?** (6.1, 6.6)
41. **What is self-match prevention, and what are the policies?** (6.10)
42. **IOC vs Day vs FOK.** (6.1)
43. **L1 vs L2 vs L3 data. How do you build L2 from ITCH?** (6.1, 6.4,
    6.9: `for_each_level`)
44. **Why do pre-trade risk checks exist? Name five.** (6.14)
45. **What's the difference between checking current position and
    worst-case position?** (6.14)
46. **Why might an imbalance strategy lose money even when the signal is
    "right"?** (6.13)
47. **Binary protocol vs FIX: what are the costs, and why does FIX
    survive?** (6.7)
48. **What is cancel-on-disconnect and why do exchanges offer it?**
    (6.11)

### SRE and operations

49. **A trading process is hung. What do you do?** `gdb -p`, `info
    threads`, `bt`; check for lock-order inversion; look at TSan and
    helgrind results from CI. (3.10)
50. **p99.9 latency doubled after a deploy, but p50 didn't move. Where do
    you look?** Allocation on a new path, a lock, page faults, a log line
    on the hot path, a changed thread placement, interrupts, GC in a
    sidecar. Use `perf` to compare. (1.3, 7.7)
51. **How do you export metrics from a hot path without slowing it?**
    Relaxed atomics in shared memory, read by a separate process. (3.5,
    3.6)
52. **Why does warnings-as-errors matter? Why sanitizers in CI?** (2.1)
53. **What should a kill switch do, and who should be able to pull it?**
    (6.14)

---

## 9. Exercises: extend PulseForge

Ordered roughly by difficulty. Each one teaches something specific;
write a test for it before you write the code.

**Warm-up**

1. **Clamp histogram percentiles to `max`** (`latency_histogram.hpp`),
   so `p99 > max` can never be printed. Add a test.
2. **Fix the `queue_depth` false sharing** (`stats.hpp`): give the
   receiver its own depth gauge on its own line. Measure with `perf c2c`
   before and after.
3. **Measure the latency ladder on your machine:** write a tiny program
   timing an L1 hit, a DRAM miss (pointer chasing through a shuffled
   array), an uncontended atomic, a contended atomic (two threads), a
   syscall (`getppid`) and `clock_gettime`. Fill in your own 7.1 table.
4. **Add `perf stat` output** for `book_bench` (cache misses per op for
   each book). Explain the difference.

**Core**

5. **ITCH Order Replace `'U'`** (original ref, new ref, shares, price):
   the order loses its time priority. Implement it in the book (remove +
   add), the engine (a `replace()` API), the exchange (OUCH Replace), and
   `apply_itch`. Extend `test_replica_matches_exchange`.
6. **Passive market-making strategy:** quote Day orders on both sides,
   cancel/replace when the market moves, and track queue position from
   ITCH using your own order refs from OUCH Accepted. Compare its PnL to
   the taker's.
7. **Spin cached cursors in `SpscRing`:** each side caches the other's
   index. Benchmark it with `queue_bench`, and explain the reduction in
   cross-core traffic.
8. **Cache the slowest reader in `MpmcFanout`** and only rescan when the
   cache says full.
9. **A seqlock for the telemetry event ring** (5.1) so readers never see
   torn events. Prove it with TSan.
10. **CRTP or template listener:** make `MatchingEngine<Listener>` and
    measure the difference in `test_matching_engine` throughput.

**Advanced**

11. **MoldUDP64 retransmission server:** the exchange keeps the last N
    packets; the trader requests missing ranges over UDP before falling
    back to a full snapshot.
12. **A/B feed arbitration:** publish the feed on two groups, have the
    trader subscribe to both, and take the first copy of each sequence.
    Simulate loss on each line independently and show that gaps only
    happen when both lose the same packet.
13. **FIX session layer:** Logon (`A`), Heartbeat (`0`), TestRequest
    (`1`), ResendRequest (`2`), SequenceReset (`4`) and Logout (`5`) on a
    second order-entry port; translate to the engine. Compare its latency
    to OUCH with the trader.
14. **Multi-threaded trader:** a feed thread that builds the book and
    publishes top-of-book updates through the SPSC ring to a strategy
    thread on another pinned core. Measure what the handoff costs vs the
    single-threaded version.
15. **Symbol partitioning in the exchange:** one engine thread per symbol
    group, with an order router in front. Determinism must be preserved
    per symbol.
16. **Hardware timestamps:** on a Linux machine with a capable NIC
    (`ethtool -T` shows `hardware-receive`), add the `SIOCSHWTSTAMP`
    ioctl and read `ts[2]`. Compare with software stamps.
17. **Kernel bypass port:** replace the trader's UDP receive with AF_XDP
    or DPDK on bare-metal Linux, keeping `handle_packet` unchanged.

---

## 10. Appendix: file map, commands, glossary

### 10.1 Every file and what it teaches

**Core and utilities**

| File | Purpose | Concepts |
|---|---|---|
| `include/common.hpp` | `CHECK`, `SystemError`, `Socket`, exit codes | RAII, rule of five, `explicit`, macros |
| `include/time_util.hpp`, `src/time_util.cpp` | three clocks, sleeps | clock domains, drift-free pacing, vDSO |
| `include/message.hpp` | `TickMessage` | layout, padding, `static_assert` |
| `include/cpu_util.hpp`, `src/cpu_util.cpp` | pinning | `sched_setaffinity` |
| `include/cpu_list.hpp`, `src/hw_topology.cpp` | sysfs topology → JSON | SMT, caches, NUMA, NICs |

**Phase 1–2 pipeline**

| File | Purpose | Concepts |
|---|---|---|
| `src/publisher.cpp` | synthetic ticks, pacing, loss, multicast | absolute deadlines, `sendto` |
| `src/receiver.cpp` | receive, validate, gap detect, enqueue, recover | `SO_RCVBUF`, `EINTR`, `recvmmsg`, `std::async` |
| `include/queue_mutex.hpp`, `src/queue_mutex.cpp` | blocking bounded queue | mutex, condvars, backpressure, shutdown |
| `include/spsc_ring.hpp`, `src/spsc_ring.cpp` | lock-free SPSC | acquire/release, padding, power-of-two masks |
| `include/mpmc_fanout.hpp`, `src/mpmc_fanout.cpp` | broadcast ring | Disruptor, single writer, slowest-reader backpressure |
| `include/queue_backend.hpp`, `src/queue_backend.cpp` | interface + factory | factory/adapter patterns, ownership |
| `include/worker.hpp`, `src/worker.cpp` | consumers | relaxed atomics, CAS max |
| `include/stats.hpp`, `include/shared_stats.hpp`, `src/monitor.cpp` | shm metrics | false sharing, `shm_open`/`mmap`, owner/guest |
| `include/mem_pool.hpp` | object pool | intrusive free list, zero allocation |
| `include/latency_histogram.hpp` | log histogram | `constexpr` tables, percentile estimation |
| `include/hugepage_alloc.hpp` | huge-page mapping | TLB, honest fallback |
| `include/multicast.hpp` | group join / TTL / loop | IGMP, multicast addressing |
| `include/bpf_filter.hpp`, `src/bpf_filter.cpp` | in-kernel filter | classic BPF |
| `include/snapshot_*.hpp`, `src/snapshot_*.cpp` | TCP gap repair | TCP vs UDP, `MSG_NOSIGNAL`, framing by size |
| `src/deadlock_demo.cpp`, `src/race_demo.cpp` | broken on purpose | lock ordering, data races, gdb/TSan |
| `bench/queue_bench.cpp` | queue comparison | reading benchmark results critically |

**Phase 3–4 telemetry and visualizer**

| File | Purpose | Concepts |
|---|---|---|
| `include/shared_events.hpp` | event ring + config in shm | `fetch_add` slots, one-shot release publish |
| `include/ws_server.hpp`, `src/ws_server.cpp`, `include/sha1.hpp` | WebSocket server | RFC 6455 handshake and framing |
| `src/telemetry_bridge.cpp` | shm → JSON → browser | guest process, polling |
| `visualizer/` | 3D drill-down course | layered frontend architecture |

**Phase 6 trading loop**

| File | Purpose | Concepts |
|---|---|---|
| `include/trading_types.hpp` | prices, sides, symbols, time | fixed point, `overloaded` |
| `include/wire.hpp` | big-endian codec | endianness, aliasing, latched errors |
| `include/itch.hpp` | ITCH 5.0 + MoldUDP64 | order-by-order feeds, sequence numbers, MTU |
| `include/soupbin.hpp` | SoupBinTCP framing | byte-stream framing, bounded buffers |
| `include/ouch.hpp` | OUCH-style order entry | fixed-offset binary, UserRefNum |
| `include/fix.hpp` | FIX 4.4 | tag=value, BodyLength, CheckSum, zero-copy parse |
| `include/flat_id_map.hpp` | id → index map | open addressing, Fibonacci hash, backward shift |
| `include/order_book.hpp`, `src/order_book.cpp` | L3 book | ladder, bitmap + ctz/clz, intrusive FIFO, pool |
| `include/matching_engine.hpp`, `src/matching_engine.cpp` | matching | price-time priority, STP, Observer |
| `include/event_loop.hpp` | epoll/timerfd/signalfd RAII | single-threaded event loops |
| `src/exchange.cpp` | the venue | sessions, non-blocking I/O, batching, snapshots, synthetic flow |
| `include/feed_handler.hpp` | sequence + book replay | gaps, `std::visit`, consistency |
| `include/strategy.hpp` | imbalance taker | microstructure signal, integer math |
| `include/risk.hpp` | pre-trade risk | worst-case position, rolling rate limit |
| `include/net_timestamp.hpp`, `src/net_timestamp.cpp` | `SO_TIMESTAMPING` | cmsg, error queue, `OPT_ID` |
| `include/latency_sampler.hpp` | exact percentiles | pre-faulted storage |
| `src/trader.cpp` | the client | snapshot+replay, order lifecycle, latency report |
| `bench/book_bench.cpp`, `bench/protocol_bench.cpp` | benchmarks | methodology, `do_not_optimize` |
| `tests/test_*.cpp` (13 suites) | tests | differential and property testing |
| `scripts/run_trading.sh` | run the loop | blocking / spin / gaps modes |

### 10.2 Command cheat sheet

```sh
make && make test                        # build everything, run 13 suites
make asan | make tsan | make debug       # alternate builds (make clean between)
bash scripts/run_basic.sh                # Phase 1 pipeline end to end
./bin/queue_bench --count 200000 --workers 2
bash scripts/run_trading.sh [blocking|spin|gaps] [seconds]
./bin/exchange --help ; ./bin/trader --help
./bin/trader --duration 30 --verbose --spin --cpu 2
make bench-book ; make bench-protocol
make topology && cd visualizer && python3 -m http.server 8080
./bin/deadlock_demo   # then: gdb -p <pid>
make tsan && ./bin/race_demo
```

### 10.3 Glossary

| Term | Meaning |
|---|---|
| A/B lines | Two redundant copies of a multicast feed over independent paths |
| Acquire / release | Memory orders that create a happens-before edge between threads |
| Adverse selection | Getting filled mostly when the market is about to move against you |
| Backpressure | A full consumer slowing or refusing the producer instead of dropping or growing |
| Busy-poll / spin | Checking for work in a loop instead of sleeping |
| cmsg | Ancillary ("control") data attached to `recvmsg` results |
| Colocation | Running your servers in the exchange's data centre |
| Differential testing | Comparing an implementation against a simpler trusted one on the same inputs |
| Disruptor | Single-writer ring where each reader keeps its own cursor (LMAX) |
| Drop copy | A copy of all your execution reports sent to a risk/back-office system |
| False sharing | Different variables on the same cache line written by different cores |
| FIFO priority | Earlier orders at a price trade first |
| GLIMPSE | NASDAQ's snapshot service for book recovery |
| Heartbeat | A message sent when idle to prove liveness (and, in MoldUDP64, to carry the next sequence) |
| IOC / FOK / Day / GTC | Time-in-force variants (6.1) |
| ITCH | NASDAQ's order-by-order market-data protocol |
| Kill switch | A control that blocks all new orders immediately |
| Locate code | A small integer standing in for a symbol in ITCH |
| Maker / taker | Liquidity provider (resting) / remover (aggressor) |
| Match number | Unique id of a trade, shared by both sides' reports |
| MoldUDP64 | Sequenced UDP packet framing used under ITCH |
| NAPI | Linux's interrupt-mitigating, polling receive mechanism |
| OUCH | NASDAQ's binary order-entry protocol |
| PTP | Precision Time Protocol: nanosecond-level clock synchronization |
| Price-time priority | Matching order: best price first, then earliest |
| RAII | Resource lifetime tied to object lifetime |
| Replica book | Your reconstruction of the exchange's book from its feed |
| SoupBinTCP | NASDAQ's length-prefixed TCP session protocol |
| STP | Self-trade (self-match) prevention |
| Tick-to-trade | Time from a market event reaching you to your order leaving you |
| TLB | Cache of virtual→physical page translations |
| UserRefNum | OUCH 5.0 client order number, strictly increasing per session |
| vDSO | Kernel code mapped into user space so `clock_gettime` needs no syscall |
| Wire-to-wire | Packet in at the NIC to packet out at the NIC |
| XDP | eBPF programs running in the NIC driver before the network stack |
