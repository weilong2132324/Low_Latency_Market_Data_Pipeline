/*
 * worker.hpp - worker thread API.
 *
 * Responsibility: consume TickMessages from a queue and fold them into
 * the shared statistics (processed count, latency total/max, and the
 * queue-depth gauge).
 *
 * Ownership: the caller owns the QueueMutex and SharedStats objects and
 * must keep them alive for the worker's lifetime. The worker never
 * frees them; it only holds borrowed pointers.
 *
 * Thread-safety: each worker runs independently. Many workers may share
 * one queue (the mutex queue is MPSC-safe). All shared counters are
 * atomic.
 */

#ifndef PULSEFORGE_WORKER_HPP
#define PULSEFORGE_WORKER_HPP

#include <cstdint>

#include "queue_mutex.hpp"
#include "stats.hpp"

struct WorkerConfig {
    QueueMutex *queue;             /* borrowed: caller-owned queue   */
    SharedStats *stats;            /* borrowed: caller-owned stats   */
    uint64_t slow_consumer_us = 0; /* artificial delay per message   */
    int worker_id = 0;             /* 0-based, for logging           */
};

/* Thread entry point. Runs until the queue is closed and drained.
 * Takes its config by value so each std::thread owns a private copy;
 * safe to pass directly to std::thread(worker_run, cfg). */
void worker_run(WorkerConfig cfg);

#endif /* PULSEFORGE_WORKER_HPP */
