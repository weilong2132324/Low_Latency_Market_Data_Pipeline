/*
 * matching_engine.hpp - price-time priority matching over OrderBooks.
 *
 * Responsibility: the heart of an exchange. Given an incoming order,
 * trade it against the opposite side of the book, best price first and,
 * at the same price, oldest order first (PRICE-TIME PRIORITY, a.k.a.
 * FIFO - used by NASDAQ, CME futures and most equity venues). Whatever
 * is left either rests in the book (Day) or is cancelled (IOC).
 *
 * Rules implemented (each is a one-line answer to "how does matching
 * work?" in an interview):
 *   - An incoming BUY at limit L trades with asks priced <= L, cheapest
 *     first; an incoming SELL trades with bids >= L, highest first.
 *   - The trade happens at the RESTING order's price. If the best ask is
 *     100.00 and you send a buy limit 100.05, you pay 100.00 (price
 *     improvement goes to the aggressor).
 *   - The resting order is the "maker" (added liquidity), the incoming
 *     one the "taker" (removed liquidity).
 *   - Self-match prevention (STP): an order may not trade against a
 *     resting order from the same owner (that would be a wash trade -
 *     illegal market manipulation in most jurisdictions). Policy
 *     CancelResting removes the old order and keeps matching;
 *     CancelIncoming stops and cancels what is left of the new order.
 *     Owner 0 (the exchange's own synthetic flow) is exempt.
 *   - Validation (venue-side risk): price on the tick grid and inside
 *     the book's band (a "price collar" / limit-up-limit-down analog),
 *     0 < qty <= max_order_qty.
 *
 * Output is a stream of events delivered SYNCHRONOUSLY to an
 * EngineListener (the Observer pattern). The engine does not know about
 * sockets, ITCH or OUCH; the exchange's listener turns events into
 * public market data and private execution reports. This separation is
 * what makes the engine unit-testable without a network
 * (tests/test_matching_engine.cpp records events into a vector).
 *
 * Virtual dispatch cost: one indirect call per event (~1-2 ns when the
 * branch predictor has seen it). The exchange's listener is marked
 * `final`, which lets the compiler devirtualise calls made through the
 * concrete type. If it mattered, the zero-cost alternative is a
 * template parameter (static polymorphism / CRTP): MatchingEngine<L>.
 *
 * Re-entrancy: a listener must NOT call back into the engine from inside
 * a callback (the book is mid-update). Queue the work instead.
 *
 * Thread-safety: none. Real matching engines are single-threaded per
 * symbol partition: determinism (same input sequence -> same trades) is
 * a regulatory requirement, and one core with a warm cache beats locks.
 */

#ifndef PULSEFORGE_MATCHING_ENGINE_HPP
#define PULSEFORGE_MATCHING_ENGINE_HPP

#include "order_book.hpp"
#include "trading_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct OrderRequest {
    OrderId     id       = 0; /* assigned by the exchange before submit() */
    Side        side     = Side::Buy;
    Price       price    = 0;
    Qty         qty      = 0;
    TimeInForce tif      = TimeInForce::Day;
    uint32_t    owner    = 0; /* session id; 0 = exchange synthetic flow */
    uint32_t    user_ref = 0; /* the client's order number               */
};

enum class RejectReason : uint8_t { BadPrice, BadQty, DuplicateId, BookFull, UnknownSymbol };

enum class CancelReason : uint8_t { UserRequested, ImmediateOrCancel, SelfMatch, Supervisory };

enum class SelfMatchPolicy : uint8_t { CancelResting, CancelIncoming };

struct Fill {
    uint64_t match_number; /* unique per trade, shared by both sides */
    Price    price;        /* the maker's price */
    Qty      qty;
    /* maker = the resting order */
    OrderId  maker_id;
    uint32_t maker_owner;
    uint32_t maker_user_ref;
    Side     maker_side;
    Qty      maker_remaining;
    /* taker = the incoming order */
    OrderId  taker_id;
    uint32_t taker_owner;
    uint32_t taker_user_ref;
    Qty      taker_remaining;
};

class EngineListener {
public:
    virtual ~EngineListener() = default;
    /* Validation passed; sent before any fills (OUCH sends Accepted first). */
    virtual void on_accepted(uint16_t locate, const OrderRequest &req) = 0;
    virtual void on_rejected(uint16_t locate, const OrderRequest &req, RejectReason why) = 0;
    virtual void on_fill(uint16_t locate, const Fill &fill) = 0;
    /* The remainder of an incoming order now rests in the book (public: ITCH Add). */
    virtual void on_rested(uint16_t locate, const BookOrder &order) = 0;
    /* A resting order lost `cancelled` shares. `before` is its state
     * before the cancel; it is gone from the book iff cancelled == before.qty. */
    virtual void on_resting_cancelled(uint16_t locate, const BookOrder &before,
                                      Qty cancelled, CancelReason why) = 0;
    /* Part of an incoming order that never rested was cancelled (IOC
     * remainder, self-match, book full). Private only - never displayed. */
    virtual void on_taker_cancelled(uint16_t locate, const OrderRequest &req,
                                    Qty cancelled, CancelReason why) = 0;
};

class MatchingEngine {
public:
    enum class CancelResult : uint8_t { Ok, NoChange, UnknownOrder, UnknownSymbol, NotOwner };

    explicit MatchingEngine(EngineListener &listener,
                            SelfMatchPolicy stp = SelfMatchPolicy::CancelResting,
                            Qty max_order_qty = 1000000);

    /* Lists a symbol and returns its stock locate (1-based, like ITCH). */
    uint16_t add_symbol(const Symbol &symbol, const BookConfig &cfg);

    /* Returns the locate for a symbol, or 0 if it is not listed. */
    uint16_t find_symbol(const Symbol &symbol) const;

    void submit(uint16_t locate, const OrderRequest &req);

    /* Reduce a resting order to new_qty shares (0 = cancel entirely).
     * owner must match the order's owner (sessions cannot cancel each
     * other's orders). new_qty >= current qty is a no-op. */
    CancelResult cancel(uint16_t locate, OrderId id, Qty new_qty, uint32_t owner,
                        CancelReason why = CancelReason::UserRequested);

    const OrderBook &book(uint16_t locate) const { return *books_[locate - 1]; }
    const Symbol &symbol(uint16_t locate) const { return symbols_[locate - 1]; }
    size_t symbol_count() const { return books_.size(); }
    uint64_t trade_count() const { return next_match_ - 1; }

private:
    bool valid_locate(uint16_t locate) const {
        return locate >= 1 && locate <= books_.size();
    }

    EngineListener                          &listener_;
    SelfMatchPolicy                          stp_;
    Qty                                      max_order_qty_;
    std::vector<std::unique_ptr<OrderBook>>  books_;
    std::vector<Symbol>                      symbols_;
    uint64_t                                 next_match_ = 1;
};

#endif /* PULSEFORGE_MATCHING_ENGINE_HPP */
