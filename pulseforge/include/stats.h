/*
 * stats.h - shared-memory statistics layout for the monitor process.
 *
 * Responsibility: defines the fixed layout of the metrics region that
 * the receiver/workers write and the monitor reads across processes.
 *
 * Thread-safety / process-safety: every counter is a C11 _Atomic
 * uint64_t so that any thread (and, on Linux/glibc, any process mapping
 * the same region) can update it without locks.
 *
 * CAVEAT: the C standard does NOT guarantee that atomics are lock-free,
 * nor that they work across processes in shared memory. On the
 * Linux x86-64/glibc targets this project is aimed at, _Atomic uint64_t
 * IS lock-free, so the layout is safe in practice. shared_stats.h
 * performs a runtime lock-free check at creation.
 *
 * False sharing: the receiver-written counters and the worker-written
 * counters are separated by padding so they land on different cache
 * lines; otherwise the two threads could bounce the same line between
 * cores on every update.
 */

#ifndef PULSEFORGE_STATS_H
#define PULSEFORGE_STATS_H

#include <stdatomic.h>
#include <stdint.h>

#define PULSEFORGE_SHM_DEFAULT_NAME "/pulseforge_stats"

typedef struct SharedStats {
    /* --- receiver-written group --- */
    _Atomic uint64_t received;      /* valid datagrams received          */
    _Atomic uint64_t invalid;       /* bad size / failed validation      */
    _Atomic uint64_t sequence_gaps; /* missing sequence numbers observed */
    _Atomic uint64_t reorders;      /* out-of-order (older) sequence seen */
    _Atomic uint64_t queue_full;    /* times push found the queue full   */
    uint64_t _pad1[3];              /* 5*8 + 3*8 = 64 bytes -> own line  */

    /* --- worker-written group --- */
    _Atomic uint64_t processed;         /* messages consumed              */
    _Atomic uint64_t latency_total_ns;  /* sum of per-message latency     */
    _Atomic uint64_t latency_max_ns;    /* largest latency seen           */
    _Atomic uint64_t queue_depth;       /* gauge; written by both sides   */
    uint64_t _pad2[4];                  /* 4*8 + 4*8 = 64 bytes -> own line */
} SharedStats;

/*
 * Reset every counter to zero. Called by the OWNER right after mapping
 * a fresh region. Relaxed ordering is fine: the region was just zeroed
 * and nothing has been published yet.
 */
static inline void shared_stats_reset(SharedStats *s) {
    atomic_store_explicit(&s->received, 0, memory_order_relaxed);
    atomic_store_explicit(&s->invalid, 0, memory_order_relaxed);
    atomic_store_explicit(&s->sequence_gaps, 0, memory_order_relaxed);
    atomic_store_explicit(&s->reorders, 0, memory_order_relaxed);
    atomic_store_explicit(&s->queue_full, 0, memory_order_relaxed);
    atomic_store_explicit(&s->processed, 0, memory_order_relaxed);
    atomic_store_explicit(&s->latency_total_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->latency_max_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->queue_depth, 0, memory_order_relaxed);
}

#endif /* PULSEFORGE_STATS_H */
