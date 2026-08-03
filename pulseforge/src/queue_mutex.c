/*
 * queue_mutex.c - implementation of the mutex + condvar bounded queue.
 *
 * Responsibility: bounded FIFO with blocking push/pop, backpressure,
 * and cooperative shutdown.
 *
 * Thread-safety: all public functions are safe from any thread; guarded
 * by an internal mutex. push/pop may block on their condvar.
 *
 * Why NOT busy-spin: condition variables put the thread to sleep and
 * let the OS schedule it only when there is work - they burn ~zero CPU
 * while waiting. Busy-spinning a full/empty queue would waste a core
 * for no latency benefit at this scale.
 */

#include "queue_mutex.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct QueueMutex {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty; /* signaled when a message is pushed     */
    pthread_cond_t  not_full;  /* signaled when a message is popped     */
    TickMessage    *buf;       /* preallocated fixed-size storage       */
    size_t          capacity;
    size_t          head;      /* index of next pop  (FIFO)             */
    size_t          tail;      /* index of next push                    */
    size_t          count;     /* messages currently queued             */
    int             closed;
};

QueueMutex *queue_mutex_create(size_t capacity) {
    QueueMutex *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        perror("calloc(QueueMutex)");
        return NULL;
    }
    q->buf = malloc(capacity * sizeof(*q->buf));
    if (q->buf == NULL) {
        perror("malloc(queue buffer)");
        free(q);
        return NULL;
    }
    q->capacity = capacity;

    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        fprintf(stderr, "pthread_mutex_init failed\n");
        free(q->buf);
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        fprintf(stderr, "pthread_cond_init(not_empty) failed\n");
        pthread_mutex_destroy(&q->lock);
        free(q->buf);
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        fprintf(stderr, "pthread_cond_init(not_full) failed\n");
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->lock);
        free(q->buf);
        free(q);
        return NULL;
    }
    return q;
}

void queue_mutex_destroy(QueueMutex *q) {
    if (q == NULL) {
        return;
    }
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    free(q);
}

void queue_mutex_close(QueueMutex *q) {
    if (q == NULL) {
        return;
    }
    if (pthread_mutex_lock(&q->lock) != 0) {
        return;
    }
    q->closed = 1;
    /* Wake everyone: producers blocked on a full queue and consumers
     * blocked on an empty one. They re-check their predicates and see
     * `closed`, then return QUEUE_CLOSED. */
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

int queue_mutex_push(QueueMutex *q, const TickMessage *msg) {
    if (pthread_mutex_lock(&q->lock) != 0) {
        return QUEUE_CLOSED;
    }
    while (q->count == q->capacity && !q->closed) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        return QUEUE_CLOSED;
    }
    q->buf[q->tail] = *msg;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return QUEUE_OK;
}

int queue_mutex_try_push(QueueMutex *q, const TickMessage *msg) {
    if (pthread_mutex_lock(&q->lock) != 0) {
        return QUEUE_CLOSED;
    }
    int res;
    if (q->closed) {
        res = QUEUE_CLOSED;
    } else if (q->count == q->capacity) {
        res = QUEUE_FULL;
    } else {
        q->buf[q->tail] = *msg;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        res = QUEUE_OK;
    }
    if (res == QUEUE_OK) {
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->lock);
    return res;
}

int queue_mutex_pop(QueueMutex *q, TickMessage *msg) {
    if (pthread_mutex_lock(&q->lock) != 0) {
        return QUEUE_CLOSED;
    }
    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->count == 0 && q->closed) {
        pthread_mutex_unlock(&q->lock);
        return QUEUE_CLOSED;
    }
    *msg = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return QUEUE_OK;
}

size_t queue_mutex_depth(QueueMutex *q) {
    size_t d = 0;
    if (pthread_mutex_lock(&q->lock) != 0) {
        return 0;
    }
    d = q->count;
    pthread_mutex_unlock(&q->lock);
    return d;
}

size_t queue_mutex_capacity(const QueueMutex *q) {
    return q->capacity;
}
