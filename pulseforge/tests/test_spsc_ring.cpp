/*
 * test_spsc_ring.cpp - unit tests for the lock-free SPSC ring.
 *
 * Honest limits: single-producer/single-consumer threading is exercised
 * with one real background thread, but these are still timing-dependent
 * unit tests, not proof of memory-ordering correctness - that needs
 * ThreadSanitizer (make tsan) and sustained stress runs.
 */

#include "spsc_ring.hpp"

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

static void test_capacity_rounds_up_to_pow2() {
    SpscRing r(5);
    CHECK(r.capacity() == 8);
}

static void test_fifo_order() {
    SpscRing r(4);
    for (uint64_t s = 1; s <= 4; s++) {
        auto m = make_msg(s);
        CHECK(r.try_push(m) == SpscRing::Status::Ok);
    }
    CHECK(r.depth() == 4);

    TickMessage m;
    for (uint64_t s = 1; s <= 4; s++) {
        CHECK(r.try_pop(m));
        CHECK(m.sequence == s);
    }
    CHECK(r.depth() == 0);
}

static void test_full_detected() {
    SpscRing r(2); /* rounds up to 2 already (pow2) */
    auto m = make_msg(1);
    CHECK(r.try_push(m) == SpscRing::Status::Ok);
    m.sequence = 2;
    CHECK(r.try_push(m) == SpscRing::Status::Ok);
    m.sequence = 3;
    CHECK(r.try_push(m) == SpscRing::Status::Full);
}

static void test_empty_pop_fails() {
    SpscRing r(4);
    TickMessage m;
    CHECK(!r.try_pop(m));
}

static void test_close_then_drain() {
    SpscRing r(4);
    auto m = make_msg(1);
    CHECK(r.try_push(m) == SpscRing::Status::Ok);
    r.close();

    /* Closed but not yet drained: pushing fails, draining still works. */
    CHECK(r.try_push(m) == SpscRing::Status::Closed);
    CHECK(!r.closed_and_empty());

    TickMessage out;
    CHECK(r.try_pop(out));
    CHECK(out.sequence == 1);
    CHECK(r.closed_and_empty());
}

/* Real producer/consumer threads, high volume, to shake out obvious
 * ordering bugs (not a substitute for TSan). */
static void test_threaded_throughput() {
    constexpr uint64_t kCount = 200000;
    SpscRing r(1024);

    std::thread producer([&] {
        for (uint64_t s = 1; s <= kCount; s++) {
            auto m = make_msg(s);
            while (r.try_push(m) == SpscRing::Status::Full) {
                std::this_thread::yield();
            }
        }
        r.close();
    });

    uint64_t expected = 1;
    uint64_t received = 0;
    bool     order_ok = true;
    std::thread consumer([&] {
        TickMessage m;
        for (;;) {
            if (r.try_pop(m)) {
                if (m.sequence != expected) {
                    order_ok = false;
                }
                ++expected;
                ++received;
            } else if (r.closed_and_empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK(order_ok);
    CHECK(received == kCount);
}

int main() {
    test_capacity_rounds_up_to_pow2();
    test_fifo_order();
    test_full_detected();
    test_empty_pop_fails();
    test_close_then_drain();
    test_threaded_throughput();

    if (g_failures == 0) {
        std::printf("test_spsc_ring: ALL PASSED\n");
        return 0;
    }
    std::printf("test_spsc_ring: %d FAILURE(S)\n", g_failures);
    return 1;
}
