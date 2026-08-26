/*
 * stats.hpp - shared-memory statistics layout for the monitor process.
 *
 * Responsibility: defines the fixed layout of the metrics region that
 * the receiver/workers write and the monitor reads across processes.
 *
 * Thread-safety / process-safety: every counter is a std::atomic
 * uint64_t so that any thread (and, on Linux/glibc, any process mapping
 * the same region) can update it without locks.
 *
 * CAVEAT: the C++ standard does NOT guarantee that atomics are
 * lock-free, nor that they work across processes in shared memory. On
 * the Linux x86-64/glibc targets this project is aimed at,
 * std::atomic<uint64_t> IS lock-free and layout-compatible with the C
 * _Atomic form, so the region is safe in practice. shared_stats.hpp
 * performs a runtime lock-free check at creation.
 *
 * False sharing: the receiver-written counters and the worker-written
 * counters are separated by padding so they land on different cache
 * lines; otherwise the two threads could bounce the same line between
 * cores on every update.
 */

#ifndef PULSEFORGE_STATS_HPP
#define PULSEFORGE_STATS_HPP

#include <atomic>
#include <cstdint>

#define PULSEFORGE_SHM_DEFAULT_NAME "/pulseforge_stats"

struct SharedStats {
    /* --- receiver-written group --- */
    std::atomic<uint64_t> received;      /* valid datagrams received          */
    std::atomic<uint64_t> invalid;       /* bad size / failed validation      */
    std::atomic<uint64_t> sequence_gaps; /* missing sequence numbers observed */
    std::atomic<uint64_t> reorders;      /* out-of-order (older) sequence seen */
    std::atomic<uint64_t> queue_full;    /* times push found the queue full   */
    uint64_t _pad1[3];                   /* 5*8 + 3*8 = 64 bytes -> own line  */

    /* --- worker-written group --- */
    std::atomic<uint64_t> processed;        /* messages consumed              */
    std::atomic<uint64_t> latency_total_ns; /* sum of per-message latency     */
    std::atomic<uint64_t> latency_max_ns;   /* largest latency seen           */
    std::atomic<uint64_t> queue_depth;      /* gauge; written by both sides   */
    uint64_t _pad2[4];                      /* 4*8 + 4*8 = 64 bytes -> own line */
};

/* The two 64-byte groups must not collide on one cache line. */
static_assert(sizeof(SharedStats) == 128,
              "SharedStats layout changed; update the padding above");

/*
 * Reset every counter to zero. Called by the OWNER right after mapping
 * a fresh region (ftruncate + mmap already zeroed the bytes). Relaxed
 * ordering is fine: nothing has been published yet.
 */
inline void shared_stats_reset(SharedStats &s) {
    using std::memory_order_relaxed;
    s.received.store(0, memory_order_relaxed);
    s.invalid.store(0, memory_order_relaxed);
    s.sequence_gaps.store(0, memory_order_relaxed);
    s.reorders.store(0, memory_order_relaxed);
    s.queue_full.store(0, memory_order_relaxed);
    s.processed.store(0, memory_order_relaxed);
    s.latency_total_ns.store(0, memory_order_relaxed);
    s.latency_max_ns.store(0, memory_order_relaxed);
    s.queue_depth.store(0, memory_order_relaxed);
}

#endif /* PULSEFORGE_STATS_HPP */
