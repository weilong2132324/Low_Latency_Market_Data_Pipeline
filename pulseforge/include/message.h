/*
 * message.h - fixed-size TickMessage and validation.
 *
 * Responsibility: defines the single local message type used across the
 * publisher, receiver, queues, and workers.
 *
 * Thread-safety: a TickMessage is owned by the thread moving it; do not
 * share one message object across threads concurrently.
 */

#ifndef PULSEFORGE_MESSAGE_H
#define PULSEFORGE_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Known flag bits. Unknown/reserved bits must be zero (validated). */
enum {
    TICK_FLAG_TRADE = 1u << 0, /* this tick is a trade            */
    TICK_FLAG_QUOTE = 1u << 1, /* this tick is a quote update     */
    TICK_FLAG_LAST  = 1u << 2, /* final message of a publish run */
};

typedef struct TickMessage {
    uint64_t sequence;          /* monotonic per-run sequence number      */
    uint64_t send_timestamp_ns; /* CLOCK_MONOTONIC_RAW at send time       */
    uint32_t symbol_id;         /* synthetic symbol identifier            */
    int64_t  price;             /* price in scaled integer units          */
    uint32_t quantity;          /* quantity; must be > 0                  */
    uint32_t flags;             /* TICK_FLAG_* bitmask                    */
} TickMessage;

/*
 * Layout / alignment walk-through (64-bit target):
 *   offset 0  : sequence          (uint64_t, 8-aligned)
 *   offset 8  : send_timestamp_ns (uint64_t, 8-aligned)
 *   offset 16 : symbol_id         (uint32_t, 4-aligned)
 *   offset 20 : 4 bytes PADDING   (int64_t needs 8-alignment)
 *   offset 24 : price             (int64_t, 8-aligned)
 *   offset 32 : quantity          (uint32_t, 4-aligned)
 *   offset 36 : flags             (uint32_t, 4-aligned)
 *   offset 40 : end; struct alignment = 8 => sizeof == 40
 *
 * Compilers insert padding to satisfy each member's alignment; the C
 * standard does not guarantee field offsets, so never assume this layout
 * for a wire format. This lab deliberately uses the in-memory struct as
 * the local "wire" format for simplicity.
 *
 * ENDIANNESS: on this lab everything runs on one little-endian machine,
 * so we ignore byte order. If a big-endian machine (or a different
 * architecture) joined, the same bytes would be interpreted differently.
 * Real protocols use explicit byte-order conversions (htons/ntohs for
 * 16-bit, htonl/ntohl for 32-bit, or hand-rolled 64-bit helpers) and a
 * versioned, padded wire schema. That is deliberately out of scope here.
 */
#define TICK_MESSAGE_EXPECTED_SIZE 40

_Static_assert(sizeof(TickMessage) == TICK_MESSAGE_EXPECTED_SIZE,
               "TickMessage layout changed; update the alignment notes above");

/*
 * Validate a TickMessage for the invariants this lab cares about.
 * Returns true if the message is well-formed.
 */
static inline bool message_is_valid(const TickMessage *msg) {
    if (msg == NULL) {
        return false;
    }
    if (msg->quantity == 0) {
        return false; /* a zero-quantity tick is meaningless here */
    }
    /* Every flag bit must be one we know about. */
    if (msg->flags & ~(TICK_FLAG_TRADE | TICK_FLAG_QUOTE | TICK_FLAG_LAST)) {
        return false;
    }
    return true;
}

#endif /* PULSEFORGE_MESSAGE_H */
