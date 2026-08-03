/*
 * worker.h - worker thread API.
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
 *
 * NOTE: include/worker.h is a small addition to the originally listed
 * tree - it is the natural home for the worker entry point that
 * receiver.c and worker.c share.
 */

#ifndef PULSEFORGE_WORKER_H
#define PULSEFORGE_WORKER_H

#include <pthread.h>
#include <stdint.h>

#include "queue_mutex.h"
#include "stats.h"

typedef struct WorkerConfig {
    QueueMutex  *queue;             /* borrowed: caller-owned queue   */
    SharedStats *stats;             /* borrowed: caller-owned stats   */
    uint64_t     slow_consumer_us;  /* artificial delay per message   */
    int          worker_id;         /* 0-based, for logging           */
} WorkerConfig;

/* Thread entry point. Runs until the queue is closed and drained.
 * May be passed directly to pthread_create. Returns NULL. */
void *worker_main(void *arg);

#endif /* PULSEFORGE_WORKER_H */
