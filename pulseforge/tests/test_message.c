/*
 * test_message.c - unit tests for TickMessage validation and layout.
 *
 * Responsibility: verify message_is_valid() and the fixed-size
 * invariant. No third-party framework; plain assertions + a counter.
 */

#include "message.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                    #cond);                                             \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void test_size(void) {
    CHECK(sizeof(TickMessage) == TICK_MESSAGE_EXPECTED_SIZE);
    CHECK(sizeof(TickMessage) == 40);
}

static void test_valid(void) {
    TickMessage m;
    memset(&m, 0, sizeof(m));
    m.sequence  = 1;
    m.symbol_id = 7;
    m.price     = 10000;
    m.quantity  = 100;
    m.flags     = TICK_FLAG_TRADE;
    CHECK(message_is_valid(&m));

    m.flags = TICK_FLAG_QUOTE | TICK_FLAG_LAST;
    CHECK(message_is_valid(&m));
}

static void test_invalid(void) {
    TickMessage m;
    memset(&m, 0, sizeof(m));
    m.quantity = 100;
    m.flags    = TICK_FLAG_TRADE;

    CHECK(!message_is_valid(NULL));

    m.quantity = 0; /* zero quantity is meaningless */
    CHECK(!message_is_valid(&m));
    m.quantity = 100;

    m.flags = TICK_FLAG_TRADE | (1u << 31); /* unknown/reserved bit */
    CHECK(!message_is_valid(&m));
}

int main(void) {
    test_size();
    test_valid();
    test_invalid();

    if (g_failures == 0) {
        printf("test_message: ALL PASSED\n");
        return 0;
    }
    printf("test_message: %d FAILURE(S)\n", g_failures);
    return 1;
}
