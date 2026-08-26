/*
 * worker.cpp - consumer threads.
 *
 * Responsibility: pop TickMessages from a queue and update shared
 * statistics (processed, latency total/max, queue-depth gauge).
 *
 * Thread-safety: safe with N workers thanks to the queue's internal
 * mutex; every counter we touch is std::atomic.
 *
 * Ownership: sees (does not own) the queue and stats the caller
 * created. Exits when pop() returns Status::Closed (queue closed and
 * drained).
 */

#include "worker.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "time_util.hpp"

void worker_run(WorkerConfig cfg) {
    TickMessage msg;

    uint64_t processed = 0;
    uint64_t lat_total = 0;
    uint64_t lat_max   = 0;

    using std::memory_order_relaxed;

    for (;;) {
        auto rc = cfg.queue->pop(msg);
        if (rc == QueueMutex::Status::Closed) {
            break; /* closed and drained: time to go */
        }
        if (rc != QueueMutex::Status::Ok) {
            continue; /* defensive; pop only returns Ok or Closed */
        }

        /*
         * End-to-end latency: both the publisher and this worker read
         * CLOCK_MONOTONIC_RAW on the same machine, so the subtraction is
         * meaningful (it includes socket + queue + scheduling delay).
         */
        uint64_t lat = now_ns() - msg.send_timestamp_ns;
        ++processed;
        lat_total += lat;
        if (lat > lat_max) {
            lat_max = lat;
        }

        if (cfg.slow_consumer_us > 0) {
            /* Fault injection: pretend the consumer is overloaded. */
            (void)sleep_ns(cfg.slow_consumer_us * 1000ULL);
        }

        /* Fold local totals into the shared atomics. Relaxed ordering is
         * fine for statistics: we only need atomicity, not ordering
         * between counters. */
        cfg.stats->processed.fetch_add(1, memory_order_relaxed);
        cfg.stats->latency_total_ns.fetch_add(lat, memory_order_relaxed);

        /* Running maximum via a compare-exchange loop. */
        uint64_t cur = cfg.stats->latency_max_ns.load(memory_order_relaxed);
        while (lat > cur &&
               !cfg.stats->latency_max_ns.compare_exchange_weak(
                   cur, lat, memory_order_relaxed, memory_order_relaxed)) {
            /* cur was refreshed by the failed CAS; retry */
        }

        cfg.stats->queue_depth.store(cfg.queue->depth(), memory_order_relaxed);
    }

    std::printf("  worker %d: processed %" PRIu64 "  avg latency %" PRIu64
                " ns  max latency %" PRIu64 " ns\n",
                cfg.worker_id, processed,
                processed ? lat_total / processed : 0, lat_max);
}
