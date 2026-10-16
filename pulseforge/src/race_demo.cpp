/*
 * race_demo.cpp - a deliberate, undefined-behavior data race lab.
 *
 * Responsibility: N threads increment a plain (non-atomic) uint64_t
 * counter kItersPerThread times each, with no synchronization at all.
 * The expected total is N * kItersPerThread; the actual total is
 * usually lower, because "load, add one, store" is three separate
 * steps and two threads can interleave them and silently lose an
 * update (a lost-update race) - and under the C++ memory model this is
 * UNDEFINED BEHAVIOR, not just "a wrong number": the compiler is free
 * to do things like cache the value in a register per thread and never
 * observe the other thread's writes at all, which is exactly why this
 * cannot be fixed by "just adding a sleep" the way deadlock_demo's
 * timing works - it needs real synchronization (see the atomic
 * version below, or std::mutex).
 *
 * How to use this lab (see docs/debugging_labs.md):
 *   make tsan && ./bin/race_demo          # ThreadSanitizer reports the
 *                                          # exact two racing accesses,
 *                                          # with both backtraces
 *   valgrind --tool=helgrind ./bin/race_demo
 *   ./bin/race_demo                        # plain run: just prints a
 *                                          # wrong total, no crash - the
 *                                          # scariest kind of bug
 *
 * The fix demonstrated at the end (run_fixed_with_atomic) is the
 * minimal one: make the counter std::atomic<uint64_t> and use
 * fetch_add, the same idiom every shared counter in this project
 * (SharedStats, LatencyHistogram) already uses.
 */

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr int      kNumThreads       = 8;
constexpr uint64_t kItersPerThread   = 200000;

uint64_t g_racy_counter = 0; /* NOT atomic - this is the bug */

void increment_racy() {
    for (uint64_t i = 0; i < kItersPerThread; i++) {
        g_racy_counter++; /* read-modify-write with no synchronization */
    }
}

void run_racy() {
    std::vector<std::thread> threads;
    g_racy_counter = 0;
    for (int i = 0; i < kNumThreads; i++) {
        threads.emplace_back(increment_racy);
    }
    for (auto &t : threads) {
        t.join();
    }

    uint64_t expected = static_cast<uint64_t>(kNumThreads) * kItersPerThread;
    std::printf("racy counter:   got %" PRIu64 ", expected %" PRIu64
                " (lost %" PRIu64 " updates)\n",
                g_racy_counter, expected,
                expected > g_racy_counter ? expected - g_racy_counter : 0);
}

void run_fixed_with_atomic() {
    std::atomic<uint64_t> counter{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kNumThreads; i++) {
        threads.emplace_back([&counter] {
            for (uint64_t i = 0; i < kItersPerThread; i++) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }

    uint64_t expected = static_cast<uint64_t>(kNumThreads) * kItersPerThread;
    std::printf("atomic counter: got %" PRIu64 ", expected %" PRIu64
                " (fixed)\n",
                counter.load(), expected);
}
} // namespace

int main() {
    std::printf("race_demo: %d threads x %" PRIu64 " increments, no "
                "synchronization.\n",
                kNumThreads, kItersPerThread);
    run_racy();
    run_fixed_with_atomic();
    return 0;
}
