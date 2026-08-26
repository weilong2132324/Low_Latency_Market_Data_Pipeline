/*
 * test_message.cpp - unit tests for TickMessage validation and layout.
 *
 * Responsibility: verify message_is_valid() and the fixed-size
 * invariant. No third-party framework; plain assertions + a counter.
 */

#include "message.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_size() {
    CHECK(sizeof(TickMessage) == TICK_MESSAGE_EXPECTED_SIZE);
    CHECK(sizeof(TickMessage) == 40);
}

static void test_valid() {
    TickMessage m{};
    m.sequence  = 1;
    m.symbol_id = 7;
    m.price     = 10000;
    m.quantity  = 100;
    m.flags     = TICK_FLAG_TRADE;
    CHECK(message_is_valid(m));

    m.flags = TICK_FLAG_QUOTE | TICK_FLAG_LAST;
    CHECK(message_is_valid(m));
}

static void test_invalid() {
    TickMessage m{};
    m.quantity = 100;
    m.flags    = TICK_FLAG_TRADE;

    /* A zero-initialized message is the C++ equivalent of the old
     * "NULL message" case: zero quantity -> invalid. */
    TickMessage zero{};
    CHECK(!message_is_valid(zero));

    m.quantity = 0; /* zero quantity is meaningless */
    CHECK(!message_is_valid(m));
    m.quantity = 100;

    m.flags = TICK_FLAG_TRADE | (1u << 31); /* unknown/reserved bit */
    CHECK(!message_is_valid(m));
}

int main() {
    test_size();
    test_valid();
    test_invalid();

    if (g_failures == 0) {
        std::printf("test_message: ALL PASSED\n");
        return 0;
    }
    std::printf("test_message: %d FAILURE(S)\n", g_failures);
    return 1;
}
