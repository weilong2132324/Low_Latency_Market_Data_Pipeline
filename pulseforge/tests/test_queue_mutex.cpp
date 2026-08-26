/*
 * test_queue_mutex.cpp - unit tests for the mutex + condvar bounded
 * queue.
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

#include "queue_mutex.hpp"
#include "stats.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static TickMessage make_msg(uint64_t seq) {
    TickMessage m{};
    m.sequence = seq;
    m.quantity = 1;
    m.flags    = TICK_FLAG_TRADE;
    return m;
}

static void wait_a_bit() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

static void test_fifo() {
    QueueMutex q(4);

    for (uint64_t s = 1; s <= 4; s++) {
        auto m = make_msg(s);
        CHECK(q.push(m) == QueueMutex::Status::Ok);
    }
    CHECK(q.depth() == 4);

    TickMessage m;
    for (uint64_t s = 1; s <= 4; s++) {
        CHECK(q.pop(m) == QueueMutex::Status::Ok);
        CHECK(m.sequence == s); /* FIFO order preserved */
    }
    CHECK(q.depth() == 0);
}

static void test_full_and_depth() {
    QueueMutex q(2);
    CHECK(q.capacity() == 2);

    auto m = make_msg(1);
    CHECK(q.push(m) == QueueMutex::Status::Ok);
    m.sequence = 2;
    CHECK(q.push(m) == QueueMutex::Status::Ok);
    CHECK(q.depth() == 2);

    m.sequence = 3;
    CHECK(q.try_push(m) == QueueMutex::Status::Full); /* full detected */
}

/* --- shutdown: a consumer blocked on an empty queue wakes on close --- */
static void test_close_wakes_blocked_consumer() {
    QueueMutex q(4);
    QueueMutex::Status result = QueueMutex::Status::Ok;

    std::thread t([&] {
        TickMessage m;
        result = q.pop(m);
    });
    wait_a_bit();              /* let the consumer block on empty */
    q.close();                 /* notify_all on not_empty -> wakes it */
    t.join();
    CHECK(result == QueueMutex::Status::Closed);
}

/* --- shutdown: a producer blocked on a full queue wakes on close --- */
static void test_close_wakes_blocked_producer() {
    QueueMutex q(1);
    auto m = make_msg(1);
    CHECK(q.push(m) == QueueMutex::Status::Ok); /* fill the queue */

    QueueMutex::Status result = QueueMutex::Status::Ok;
    std::thread t([&] {
        auto m2 = make_msg(2);
        result = q.push(m2);
    });
    wait_a_bit();              /* let the producer block on full */
    q.close();                 /* notify_all on not_full -> wakes it */
    t.join();
    CHECK(result == QueueMutex::Status::Closed);
}

/* --- shared-stat initialization --- */
static void test_shared_stats_init() {
    SharedStats s;
    shared_stats_reset(s);

    CHECK(s.received.load() == 0);
    CHECK(s.invalid.load() == 0);
    CHECK(s.sequence_gaps.load() == 0);
    CHECK(s.reorders.load() == 0);
    CHECK(s.queue_full.load() == 0);
    CHECK(s.processed.load() == 0);
    CHECK(s.queue_depth.load() == 0);
    CHECK(s.latency_total_ns.load() == 0);
    CHECK(s.latency_max_ns.load() == 0);
}

int main() {
    test_fifo();
    test_full_and_depth();
    test_close_wakes_blocked_consumer();
    test_close_wakes_blocked_producer();
    test_shared_stats_init();

    if (g_failures == 0) {
        std::printf("test_queue_mutex: ALL PASSED\n");
        return 0;
    }
    std::printf("test_queue_mutex: %d FAILURE(S)\n", g_failures);
    return 1;
}
