/*
 * feed_handler.hpp - turning an ITCH stream into a correct order book.
 *
 * Responsibility: the two pieces of a feed handler that have nothing to
 * do with sockets, so they can be unit tested:
 *   1. SequenceTracker - decides, per message sequence number, whether
 *      to apply it, skip it as a duplicate, or declare a GAP.
 *   2. apply_itch()    - applies one decoded ITCH message to a replica
 *      OrderBook, and reports inconsistencies.
 *
 * Why gaps are the whole game: an order-by-order feed is a log of deltas.
 * If you miss "order 7 deleted", order 7 stays in your book forever and
 * your best bid is a price nobody is bidding. There is no self-healing;
 * once a gap happens the book must be considered WRONG until rebuilt.
 * So a feed handler must:
 *   - detect the gap (sequence numbers; heartbeats carry the next
 *     expected sequence so even a gap right before a quiet period shows)
 *   - mark the book STALE and stop trading on it (the trader also pulls
 *     its open orders - you cannot manage quotes you cannot see)
 *   - recover. Two standard ways:
 *       a. retransmission: ask the exchange to resend the missing range
 *          (MoldUDP64 has a request server for this)
 *       b. snapshot + replay: fetch a full copy of the book as of
 *          sequence S (NASDAQ GLIMPSE), then apply live messages >= S
 *     This lab implements (b): it also solves joining late in the day.
 *     The trader subscribes to the multicast feed FIRST (so the kernel
 *     socket buffer starts queueing live packets), then pulls the
 *     snapshot, then drains the queue, discarding anything < S.
 *
 * A/B line arbitration (production detail, not implemented): exchanges
 * publish every packet on two independent multicast feeds ("A" and "B"
 * lines, different switches/paths). The handler takes whichever copy of
 * sequence N arrives first and drops the other as a duplicate - the
 * Duplicate verdict below is exactly what that relies on.
 *
 * Thread-safety: none (feed thread only).
 */

#ifndef PULSEFORGE_FEED_HANDLER_HPP
#define PULSEFORGE_FEED_HANDLER_HPP

#include "itch.hpp"
#include "order_book.hpp"
#include "trading_types.hpp"

#include <cstdint>
#include <variant>

class SequenceTracker {
public:
    enum class Verdict : uint8_t { Apply, Duplicate, Gap };

    void reset(uint64_t next_expected) { expected_ = next_expected; }

    /* One message with sequence `seq`. On Apply the caller applies the
     * message and the tracker advances. */
    Verdict on_message(uint64_t seq) {
        if (seq == expected_) {
            ++expected_;
            return Verdict::Apply;
        }
        return seq < expected_ ? Verdict::Duplicate : Verdict::Gap;
    }

    /* A MoldUDP64 heartbeat says "the next message will be next_seq". */
    Verdict on_heartbeat(uint64_t next_seq) const {
        return next_seq > expected_ ? Verdict::Gap : Verdict::Apply;
    }

    uint64_t expected() const { return expected_; }

private:
    uint64_t expected_ = 1;
};

enum class ApplyResult : uint8_t {
    Applied,      /* book changed            */
    Ignored,      /* other symbol / not a book message */
    Inconsistent, /* references an order we do not have, or bad qty: we are out of sync */
};

/*
 * Apply one message to the replica book for `locate`. Uses std::visit
 * with the overloaded{} idiom (trading_types.hpp): the compiler checks
 * that every alternative of itch::Message is handled - add a new message
 * type to the variant and this stops compiling until you handle it.
 * (A switch on a type byte gives no such guarantee.)
 */
inline ApplyResult apply_itch(OrderBook &book, uint16_t locate, const itch::Message &msg) {
    return std::visit(
        overloaded{
            [&](const itch::AddOrder &m) {
                if (m.locate != locate) return ApplyResult::Ignored;
                return book.add(m.order_ref, m.side, static_cast<Price>(m.price), m.shares) ==
                               OrderBook::AddResult::Ok
                           ? ApplyResult::Applied
                           : ApplyResult::Inconsistent;
            },
            [&](const itch::OrderExecuted &m) {
                if (m.locate != locate) return ApplyResult::Ignored;
                return book.reduce(m.order_ref, m.executed_shares) ? ApplyResult::Applied
                                                                   : ApplyResult::Inconsistent;
            },
            [&](const itch::OrderCancel &m) {
                if (m.locate != locate) return ApplyResult::Ignored;
                return book.reduce(m.order_ref, m.cancelled_shares) ? ApplyResult::Applied
                                                                    : ApplyResult::Inconsistent;
            },
            [&](const itch::OrderDelete &m) {
                if (m.locate != locate) return ApplyResult::Ignored;
                return book.remove(m.order_ref) ? ApplyResult::Applied
                                                : ApplyResult::Inconsistent;
            },
            [](const itch::SystemEvent &) { return ApplyResult::Ignored; },
            [](const itch::StockDirectory &) { return ApplyResult::Ignored; },
            [](const itch::EndOfSnapshot &) { return ApplyResult::Ignored; },
        },
        msg);
}

/* ITCH timestamp of a message (0 for EndOfSnapshot, which has none). */
inline uint64_t itch_timestamp(const itch::Message &msg) {
    return std::visit(overloaded{
                          [](const itch::EndOfSnapshot &) -> uint64_t { return 0; },
                          [](const auto &m) -> uint64_t { return m.timestamp; },
                      },
                      msg);
}

#endif /* PULSEFORGE_FEED_HANDLER_HPP */
