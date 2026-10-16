# Debugging labs

Two deliberately-broken binaries (`src/deadlock_demo.cpp`,
`src/race_demo.cpp`) to practice the tools, not just read about them.

## `deadlock_demo` - lock-order inversion

```sh
./bin/deadlock_demo &
```

It hangs on purpose. Diagnose it live:

```sh
gdb -p $(pgrep deadlock_demo)
(gdb) info threads            # both worker threads BLOCKED
(gdb) thread 2
(gdb) bt                      # frame shows pthread_mutex_lock(mu_b)
(gdb) thread 3
(gdb) bt                      # frame shows pthread_mutex_lock(mu_a)
```

Thread 2 holds `mu_a` and wants `mu_b`; thread 3 holds `mu_b` and wants
`mu_a` - a textbook deadlock, visible directly in the two backtraces.

Or catch it before it even hangs, with Helgrind's lock-order analysis:

```sh
timeout 5 valgrind --tool=helgrind ./bin/deadlock_demo
```

**Fix** (left as an exercise): always acquire shared mutexes in one
globally agreed order, or use `std::lock(mu_a, mu_b)` to acquire both
atomically without hand-rolling an ordering convention.

## `race_demo` - unsynchronized counter

```sh
make tsan
./bin/race_demo
```

A plain `-O2` run may or may not print a visibly wrong total - lost
updates are timing-dependent, and on a lightly-loaded multi-core box the
race can go unnoticed for many runs in a row. That is itself the lesson:
**undefined behavior that "usually looks fine" is still a bug** that
will eventually show up under different load, a different CPU, or a
different compiler. ThreadSanitizer finds it unconditionally, every
run, because it instruments every memory access rather than hoping for
an unlucky interleaving:

```
WARNING: ThreadSanitizer: data race
  Read of size 8 ... in increment_racy src/race_demo.cpp:46
  Previous write of size 8 ... in increment_racy src/race_demo.cpp:46
  Location is global 'g_racy_counter'
```

`valgrind --tool=helgrind ./bin/race_demo` reports the same race from a
different detection strategy (lock-set analysis instead of shadow
memory) - useful to compare the two tools' output format on the exact
same bug.

**Fix** (shown in the same binary, `run_fixed_with_atomic`): make the
counter `std::atomic<uint64_t>` and use `fetch_add` - the same idiom
every shared counter in this project (`SharedStats`, `LatencyHistogram`)
already uses.
