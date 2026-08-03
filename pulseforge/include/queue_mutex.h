/*
 * queue_mutex.h - bounded FIFO queue using a mutex + two condition
 * variables.
 *
 * Responsibility: safe message handoff between the receiver (producer)
 * and worker thread(s) (consumers), with backpressure and clean
 * shutdown.
 *
 * Thread-safety: fully thread-safe (safe for many producers / many
 * consumers, i.e. MPSC or MPMC). All public calls are guarded by an
 * internal pthread_mutex_t; push/pop may block.
 *
 * Ownership:
 *   - queue_mutex_create() returns a heap-allocated queue the caller
 *     owns.
 *   - The owner must queue_mutex_close() it (this wakes every blocked
 *     push/pop), then queue_mutex_destroy() it AFTER joining all
 *     threads that could have been blocked on it.
 *
 * Why `while` loops, not `if`, around every cond_wait:
 *   - Spurious wakeups are allowed by POSIX: a thread can wake from
 *     pthread_cond_wait() with no signal having been sent. Re-checking
 *     the predicate in a while loop is the only correct pattern.
 *   - The predicate can also change between the signal and the woken
 *     thread re-acquiring the mutex (another thread may drain/fill the
 *     queue first).
 */

#ifndef PULSEFORGE_QUEUE_MUTEX_H
#define PULSEFORGE_QUEUE_MUTEX_H

#include <stddef.h>
#include <stdint.h>

#include "message.h"

/* Opaque type; the struct lives in queue_mutex.c. */
typedef struct QueueMutex QueueMutex;

enum {
    QUEUE_OK     = 0,  /* success */
    QUEUE_FULL   = 1,  /* try_push only: capacity reached */
    QUEUE_CLOSED = -1  /* queue is closed */
};

/* Create a queue holding up to `capacity` messages. Returns NULL on OOM. */
QueueMutex *queue_mutex_create(size_t capacity);

/* Destroy (free) a queue. Caller must have joined all blocked threads
 * after queue_mutex_close(). */
void queue_mutex_destroy(QueueMutex *q);

/* Mark the queue closed and wake every blocked push/pop. After this:
 *   - push/try_push fail with QUEUE_CLOSED,
 *   - pop keeps draining the remaining items, then returns QUEUE_CLOSED
 *     once the queue is empty.
 * This is how shutdown wakes blocked producers AND consumers. */
void queue_mutex_close(QueueMutex *q);

/* Blocking push; returns QUEUE_OK or QUEUE_CLOSED. Waits while full
 * (this is backpressure: a full queue makes the producer wait instead
 * of growing memory or dropping messages). */
int queue_mutex_push(QueueMutex *q, const TickMessage *msg);

/* Non-blocking push; returns QUEUE_OK, QUEUE_FULL, or QUEUE_CLOSED. */
int queue_mutex_try_push(QueueMutex *q, const TickMessage *msg);

/* Blocking pop; returns QUEUE_OK, or QUEUE_CLOSED when closed & empty.
 * On success *msg receives the dequeued message. */
int queue_mutex_pop(QueueMutex *q, TickMessage *msg);

/* Number of messages currently queued. */
size_t queue_mutex_depth(QueueMutex *q);

/* Maximum number of messages the queue can hold. */
size_t queue_mutex_capacity(const QueueMutex *q);

#endif /* PULSEFORGE_QUEUE_MUTEX_H */
