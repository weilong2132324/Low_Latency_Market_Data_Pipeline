/*
 * strategy.hpp - a deliberately simple order-book-imbalance taker.
 *
 * Responsibility: turn a book update into "send an order now" or "do
 * nothing". The point of the lab is the plumbing and its latency, not
 * alpha, so this is the textbook microstructure signal:
 *
 *   imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty)   at the top of book
 *
 * If far more shares are bid than offered, buyers are queueing and the
 * price tends to tick UP next (the thin ask gets eaten); so when
 * imbalance > +threshold we BUY by lifting the offer (IOC at the best
 * ask), and when imbalance < -threshold we SELL by hitting the bid.
 *
 * Why it loses money (and why that is a lesson): a taker pays the
 * spread on every trade and in this synthetic market the signal has no
 * real predictive power. Real taker strategies only work when the signal
 * beats spread + fees - and only if you get there before everyone else
 * who sees the same imbalance. That race is what tick-to-trade latency
 * is about.
 *
 * Integer-only arithmetic: the threshold test
 *     (bid - ask) / (bid + ask) > pct / 100
 * is rearranged to (bid - ask) * 100 > pct * (bid + ask) - no division,
 * no floating point, exactly reproducible.
 *
 * Thread-safety: none (trader thread).
 */

#ifndef PULSEFORGE_STRATEGY_HPP
#define PULSEFORGE_STRATEGY_HPP

#include "order_book.hpp"
#include "trading_types.hpp"

#include <cstdint>
#include <optional>

struct StrategyParams {
    int64_t  imbalance_pct = 60;          /* trigger when |imbalance| > 60% */
    Qty      order_qty     = 100;
    uint64_t min_top_qty   = 200;         /* ignore a nearly empty top of book */
    uint64_t cooldown_ns   = 1000000;     /* at most one signal per 1 ms */
    int64_t  max_position  = 1000;        /* strategy's own soft limit (risk has the hard one) */
};

struct Signal {
    Side    side;
    Price   price;
    Qty     qty;
    int64_t imbalance_pct; /* -100..100, for logging */
};

class ImbalanceStrategy {
public:
    explicit ImbalanceStrategy(const StrategyParams &p) : p_(p) {}

    std::optional<Signal> on_book_update(const OrderBook &book, int64_t position,
                                         bool order_in_flight, uint64_t now_ns) {
        if (order_in_flight || book.empty(Side::Buy) || book.empty(Side::Sell)) {
            return std::nullopt;
        }
        if (now_ns - last_signal_ns_ < p_.cooldown_ns) {
            return std::nullopt;
        }
        const int64_t bid = static_cast<int64_t>(book.best_qty(Side::Buy));
        const int64_t ask = static_cast<int64_t>(book.best_qty(Side::Sell));
        const int64_t total = bid + ask;
        if (static_cast<uint64_t>(total) < p_.min_top_qty) {
            return std::nullopt;
        }
        const int64_t diff = bid - ask;
        const int64_t q = static_cast<int64_t>(p_.order_qty);

        Signal s;
        s.qty           = p_.order_qty;
        s.imbalance_pct = diff * 100 / total;
        if (diff * 100 > p_.imbalance_pct * total && position + q <= p_.max_position) {
            s.side  = Side::Buy;
            s.price = book.best_price(Side::Sell); /* lift the offer */
        } else if (-diff * 100 > p_.imbalance_pct * total && position - q >= -p_.max_position) {
            s.side  = Side::Sell;
            s.price = book.best_price(Side::Buy); /* hit the bid */
        } else {
            return std::nullopt;
        }
        last_signal_ns_ = now_ns;
        return s;
    }

    const StrategyParams &params() const { return p_; }

private:
    StrategyParams p_;
    uint64_t       last_signal_ns_ = 0;
};

#endif /* PULSEFORGE_STRATEGY_HPP */
