/*
 * test_mpmc_fanout.cpp - unit tests for the Disruptor-style broadcast
 * ring (one writer, N independent readers who each see every message).
 */

#include "mpmc_fanout.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

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

static void test_every_reader_sees_every_message() {
    MpmcFanout f(4, 3); /* capacity 4, 3 independent readers */

    for (uint64_t s = 1; s <= 4; s++) {
        auto m = make_msg(s);
        CHECK(f.try_push(m) == MpmcFanout::Status::Ok);
    }

    for (size_t reader = 0; reader < 3; reader++) {
        TickMessage m;
        for (uint64_t s = 1; s <= 4; s++) {
            CHECK(f.try_pop(reader, m));
            CHECK(m.sequence == s); /* broadcast: full tape, every reader */
        }
        CHECK(!f.try_pop(reader, m)); /* each reader drains independently */
    }
}

static void test_backpressure_from_slowest_reader() {
    MpmcFanout f(2, 2);
    auto m = make_msg(1);
    CHECK(f.try_push(m) == MpmcFanout::Status::Ok);
    m.sequence = 2;
    CHECK(f.try_push(m) == MpmcFanout::Status::Ok);

    /* Reader 0 drains, reader 1 does not -> writer must stay blocked by
     * reader 1, the slowest one. */
    TickMessage out;
    CHECK(f.try_pop(0, out));
    CHECK(f.try_pop(0, out));

    m.sequence = 3;
    CHECK(f.try_push(m) == MpmcFanout::Status::Full);

    CHECK(f.try_pop(1, out)); /* now reader 1 catches up */
    CHECK(f.try_pop(1, out));
    CHECK(f.try_push(m) == MpmcFanout::Status::Ok); /* both readers clear now */
}

static void test_close_then_drain_per_reader() {
    MpmcFanout f(4, 2);
    auto m = make_msg(1);
    CHECK(f.try_push(m) == MpmcFanout::Status::Ok);
    f.close();

    CHECK(f.try_push(m) == MpmcFanout::Status::Closed);
    CHECK(!f.closed_and_empty(0));
    CHECK(!f.closed_and_empty(1));

    TickMessage out;
    CHECK(f.try_pop(0, out));
    CHECK(f.closed_and_empty(0));
    CHECK(!f.closed_and_empty(1)); /* reader 1 hasn't drained yet */
    CHECK(f.try_pop(1, out));
    CHECK(f.closed_and_empty(1));
}

/* Real writer + N reader threads at volume, broadcast semantics. */
static void test_threaded_broadcast() {
    constexpr uint64_t kCount       = 100000;
    constexpr size_t   kNumReaders  = 3;
    MpmcFanout f(1024, kNumReaders);

    std::thread writer([&] {
        for (uint64_t s = 1; s <= kCount; s++) {
            auto m = make_msg(s);
            while (f.try_push(m) == MpmcFanout::Status::Full) {
                std::this_thread::yield();
            }
        }
        f.close();
    });

    std::vector<std::thread> readers;
    std::vector<uint64_t>    received(kNumReaders, 0);
    std::vector<bool>        order_ok(kNumReaders, true);

    for (size_t r = 0; r < kNumReaders; r++) {
        readers.emplace_back([&, r] {
            uint64_t expected = 1;
            TickMessage m;
            for (;;) {
                if (f.try_pop(r, m)) {
                    if (m.sequence != expected) {
                        order_ok[r] = false;
                    }
                    ++expected;
                    ++received[r];
                } else if (f.closed_and_empty(r)) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    writer.join();
    for (auto &t : readers) {
        t.join();
    }

    for (size_t r = 0; r < kNumReaders; r++) {
        CHECK(order_ok[r]);
        CHECK(received[r] == kCount); /* every reader got the full tape */
    }
}

int main() {
    test_every_reader_sees_every_message();
    test_backpressure_from_slowest_reader();
    test_close_then_drain_per_reader();
    test_threaded_broadcast();

    if (g_failures == 0) {
        std::printf("test_mpmc_fanout: ALL PASSED\n");
        return 0;
    }
    std::printf("test_mpmc_fanout: %d FAILURE(S)\n", g_failures);
    return 1;
}
