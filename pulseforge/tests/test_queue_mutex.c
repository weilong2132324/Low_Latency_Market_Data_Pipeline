/*
 * test_queue_mutex.c - unit tests for the mutex + condvar bounded queue.
 *
 * Responsibility: verify FIFO ordering, capacity limits, the queue-full
 * path, shutdown wake-ups (blocked producer AND blocked consumer), and
 * shared-stat initialization.
 *
 * Honest limits of these tests: they exercise single-threaded behavior
 * and one blocking wait. They CANNOT prove concurrency correctness -
 * races and deadlocks are timing-dependent and need TSan/Helgrind and
 * stress runs, not unit tests.
 */

#include "queue_mutex.h"
#include "stats.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                    #cond);                                             \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static TickMessage make_msg(uint64_t seq) {
    TickMessage m;
    memset(&m, 0, sizeof(m));
    m.sequence = seq;
    m.quantity = 1;
    m.flags    = TICK_FLAG_TRADE;
    return m;
}

static void wait_a_bit(void) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000L}; /* 50 ms */
    (void)nanosleep(&ts, NULL);
}

static void test_fifo(void) {
    QueueMutex *q = queue_mutex_create(4);
    CHECK(q != NULL);

    for (uint64_t s = 1; s <= 4; s++) {
        TickMessage m = make_msg(s);
        CHECK(queue_mutex_push(q, &m) == QUEUE_OK);
    }
    CHECK(queue_mutex_depth(q) == 4);

    TickMessage m;
    for (uint64_t s = 1; s <= 4; s++) {
        CHECK(queue_mutex_pop(q, &m) == QUEUE_OK);
        CHECK(m.sequence == s); /* FIFO order preserved */
    }
    CHECK(queue_mutex_depth(q) == 0);

    queue_mutex_destroy(q);
}

static void test_full_and_depth(void) {
    QueueMutex *q = queue_mutex_create(2);
    CHECK(queue_mutex_capacity(q) == 2);

    TickMessage m = make_msg(1);
    CHECK(queue_mutex_push(q, &m) == QUEUE_OK);
    m.sequence = 2;
    CHECK(queue_mutex_push(q, &m) == QUEUE_OK);
    CHECK(queue_mutex_depth(q) == 2);

    m.sequence = 3;
    CHECK(queue_mutex_try_push(q, &m) == QUEUE_FULL); /* full detected */

    queue_mutex_destroy(q);
}

/* --- shutdown: a consumer blocked on an empty queue wakes on close --- */
typedef struct {
    QueueMutex *q;
    int         result;
} PopArgs;

static void *pop_runner(void *arg) {
    PopArgs *a = (PopArgs *)arg;
    TickMessage m;
    a->result = queue_mutex_pop(a->q, &m);
    return NULL;
}

static void test_close_wakes_blocked_consumer(void) {
    QueueMutex *q = queue_mutex_create(4);
    PopArgs args = {q, -99};
    pthread_t t;

    CHECK(pthread_create(&t, NULL, pop_runner, &args) == 0);
    wait_a_bit();                 /* let the consumer block on empty */
    queue_mutex_close(q);         /* broadcast not_empty -> wakes it  */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(args.result == QUEUE_CLOSED);

    queue_mutex_destroy(q);
}

/* --- shutdown: a producer blocked on a full queue wakes on close --- */
typedef struct {
    QueueMutex *q;
    TickMessage m;
    int         result;
} PushArgs;

static void *push_runner(void *arg) {
    PushArgs *a = (PushArgs *)arg;
    a->result = queue_mutex_push(a->q, &a->m);
    return NULL;
}

static void test_close_wakes_blocked_producer(void) {
    QueueMutex *q = queue_mutex_create(1);
    TickMessage m = make_msg(1);
    CHECK(queue_mutex_push(q, &m) == QUEUE_OK); /* fill the queue */

    PushArgs args = {q, make_msg(2), -99};
    pthread_t t;

    CHECK(pthread_create(&t, NULL, push_runner, &args) == 0);
    wait_a_bit();                 /* let the producer block on full */
    queue_mutex_close(q);         /* broadcast not_full -> wakes it */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(args.result == QUEUE_CLOSED);

    queue_mutex_destroy(q);
}

/* --- shared-stat initialization --- */
static void test_shared_stats_init(void) {
    SharedStats s;
    shared_stats_reset(&s);

    CHECK(atomic_load(&s.received) == 0);
    CHECK(atomic_load(&s.invalid) == 0);
    CHECK(atomic_load(&s.sequence_gaps) == 0);
    CHECK(atomic_load(&s.reorders) == 0);
    CHECK(atomic_load(&s.queue_full) == 0);
    CHECK(atomic_load(&s.processed) == 0);
    CHECK(atomic_load(&s.queue_depth) == 0);
    CHECK(atomic_load(&s.latency_total_ns) == 0);
    CHECK(atomic_load(&s.latency_max_ns) == 0);
}

int main(void) {
    test_fifo();
    test_full_and_depth();
    test_close_wakes_blocked_consumer();
    test_close_wakes_blocked_producer();
    test_shared_stats_init();

    if (g_failures == 0) {
        printf("test_queue_mutex: ALL PASSED\n");
        return 0;
    }
    printf("test_queue_mutex: %d FAILURE(S)\n", g_failures);
    return 1;
}
