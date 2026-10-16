/*
 * test_mem_pool.cpp - unit tests for the fixed-capacity free-list pool.
 */

#include "mem_pool.hpp"

#include <cstdio>
#include <cstdint>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

struct OrderBookEvent {
    uint64_t symbol_id;
    int64_t  price;
    uint32_t quantity;
};

static void test_acquire_release_reuse() {
    MemPool<OrderBookEvent, 4> pool;
    CHECK(pool.capacity() == 4);
    CHECK(pool.available() == 4);

    OrderBookEvent *a = pool.acquire();
    CHECK(a != nullptr);
    CHECK(pool.available() == 3);
    a->symbol_id = 42;

    pool.release(a);
    CHECK(pool.available() == 4);

    /* Reacquiring must hand back the exact same slot (LIFO free list) -
     * this is what "zero allocation" means in practice: no new memory
     * is ever touched after the pool is constructed. */
    OrderBookEvent *b = pool.acquire();
    CHECK(b == a);
}

static void test_exhaustion_returns_null() {
    MemPool<OrderBookEvent, 2> pool;
    OrderBookEvent *a = pool.acquire();
    OrderBookEvent *b = pool.acquire();
    CHECK(a != nullptr && b != nullptr);
    CHECK(pool.available() == 0);

    OrderBookEvent *c = pool.acquire();
    CHECK(c == nullptr); /* exhausted: no silent fallback allocation */

    pool.release(a);
    CHECK(pool.available() == 1);
    OrderBookEvent *d = pool.acquire();
    CHECK(d == a);
}

static void test_all_slots_distinct() {
    MemPool<OrderBookEvent, 8> pool;
    OrderBookEvent *slots[8];
    for (auto &s : slots) {
        s = pool.acquire();
        CHECK(s != nullptr);
    }
    for (size_t i = 0; i < 8; i++) {
        for (size_t j = i + 1; j < 8; j++) {
            CHECK(slots[i] != slots[j]);
        }
    }
}

int main() {
    test_acquire_release_reuse();
    test_exhaustion_returns_null();
    test_all_slots_distinct();

    if (g_failures == 0) {
        std::printf("test_mem_pool: ALL PASSED\n");
        return 0;
    }
    std::printf("test_mem_pool: %d FAILURE(S)\n", g_failures);
    return 1;
}
