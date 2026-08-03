/*
 * worker.c - consumer threads.
 *
 * Responsibility: pop TickMessages from a queue and update shared
 * statistics (processed, latency total/max, queue-depth gauge).
 *
 * Thread-safety: safe with N workers thanks to the queue's internal
 * mutex; every counter we touch is atomic.
 *
 * Ownership: sees (does not own) the queue and stats the caller
 * created. Exits when pop() returns QUEUE_CLOSED (queue closed and
 * drained).
 */

#include "worker.h"

#include "time_util.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>

void *worker_main(void *arg) {
    WorkerConfig *cfg = (WorkerConfig *)arg;
    TickMessage msg;

    uint64_t processed = 0;
    uint64_t lat_total = 0;
    uint64_t lat_max   = 0;

    for (;;) {
        int rc = queue_mutex_pop(cfg->queue, &msg);
        if (rc == QUEUE_CLOSED) {
            break; /* closed and drained: time to go */
        }
        if (rc != QUEUE_OK) {
            continue; /* defensive; pop only returns OK or CLOSED */
        }

        /*
         * End-to-end latency: both the publisher and this worker read
         * CLOCK_MONOTONIC_RAW on the same machine, so the subtraction is
         * meaningful (it includes socket + queue + scheduling delay).
         */
        uint64_t lat = now_ns() - msg.send_timestamp_ns;
        processed++;
        lat_total += lat;
        if (lat > lat_max) {
            lat_max = lat;
        }

        if (cfg->slow_consumer_us > 0) {
            /* Fault injection: pretend the consumer is overloaded. */
            (void)sleep_ns(cfg->slow_consumer_us * 1000ULL);
        }

        /* Fold local totals into the shared atomics. Relaxed ordering is
         * fine for statistics: we only need atomicity, not ordering
         * between counters. */
        atomic_fetch_add_explicit(&cfg->stats->processed, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&cfg->stats->latency_total_ns, lat,
                                  memory_order_relaxed);

        /* Running maximum via a compare-exchange loop. */
        uint64_t cur = atomic_load_explicit(&cfg->stats->latency_max_ns,
                                            memory_order_relaxed);
        while (lat > cur &&
               !atomic_compare_exchange_weak_explicit(
                   &cfg->stats->latency_max_ns, &cur, lat,
                   memory_order_relaxed, memory_order_relaxed)) {
            /* cur was refreshed by the failed CAS; retry */
        }

        atomic_store_explicit(&cfg->stats->queue_depth,
                              queue_mutex_depth(cfg->queue),
                              memory_order_relaxed);
    }

    printf("  worker %d: processed %" PRIu64 "  avg latency %" PRIu64
           " ns  max latency %" PRIu64 " ns\n",
           cfg->worker_id, processed,
           processed ? lat_total / processed : 0, lat_max);
    return NULL;
}
