# Memory ordering in this codebase

Three different atomics-usage patterns live side by side on purpose -
comparing them is the point.

## `relaxed` everywhere: `SharedStats` / `LatencyHistogram`

`received`, `processed`, histogram buckets, etc. are independent
counters. Nothing else in the program depends on the *order* in which
two different counters change relative to each other - only that each
individual `fetch_add` is atomic (no lost updates). `memory_order_relaxed`
gives exactly that, as cheaply as possible (no fence instructions, no
compiler barrier). This is the same reasoning `stats.hpp`'s top comment
already gives; `latency_histogram.hpp` follows it for the same reason.

## `acquire`/`release` handoff: `spsc_ring.hpp`, `mpmc_fanout.hpp`

Here, correctness depends on order, not just atomicity: when a consumer
sees the producer's updated cursor, it must also see the message bytes
the producer wrote *before* publishing that cursor.

```
Producer:                          Consumer:
buf_[tail] = msg;                  size_t t = tail_.load(acquire);
tail_.store(tail+1, release);  ->  if (t != head_) { read buf_[head] }
```

`release` on the store and `acquire` on the matching load create a
*happens-before* edge: the compiler and CPU are forbidden from
reordering the write to `buf_[]` after the `release` store, and
forbidden from letting the consumer's read of `buf_[]` be reordered
before its `acquire` load. Without this (e.g. if both sides used
`relaxed`), the consumer could observe the new cursor value but read
stale or half-written message bytes - undefined behavior, and the
textbook case `race_demo.cpp` demonstrates with a simpler example.

The same reasoning applies symmetrically to the producer's `head_`
read: it must see the consumer's `release` store before reusing that
slot, or it could overwrite a message the consumer hasn't actually
finished reading yet.

## Why not just use `seq_cst` everywhere?

`memory_order_seq_cst` (the default for `<atomic>` if you omit the
order) is always correct but is also the most expensive: it forces a
total global order across *all* seq_cst operations on *all* threads,
which on x86 means extra fence instructions the weaker orderings don't
need. The exercise in this codebase is picking the weakest order that
is still provably correct for each specific handoff - `relaxed` for
independent counters, `acquire`/`release` for a producer/consumer
publish, and reaching for `seq_cst` only where you cannot convince
yourself a weaker order is safe.

## `MpmcFanout`'s one extra wrinkle

The writer refuses to overwrite a slot the *slowest* reader hasn't
consumed yet, which means it has to read every reader's cursor
(`min_read_seq()`) with `acquire` before deciding whether there is room.
This is strictly more work than the SPSC ring's single `head_` read -
the price of letting N independent readers see the same tape instead of
competing for messages. A production Disruptor caches that minimum and
only rescans when stale; this project's version rescans every push
(documented, and fine at this project's message rates).
