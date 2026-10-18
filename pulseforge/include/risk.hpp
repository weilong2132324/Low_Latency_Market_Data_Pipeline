/*
 * risk.hpp - pre-trade risk checks (the trader's last line of defence).
 *
 * Responsibility: every order passes through RiskGate::check() BEFORE it
 * is sent. Regulators require it (US SEC Rule 15c3-5, the "market access
 * rule"; EU MiFID II RTS 6), and history explains why: Knight Capital
 * lost $440M in 45 minutes in 2012 when a bad deploy sent millions of
 * unintended orders that no risk limit stopped.
 *
 * Checks, cheapest first so a rejected order exits early:
 *   kill switch     - a single flag ops (or the trader itself, e.g. when
 *                     the market data feed goes stale) can flip to stop
 *                     ALL new orders immediately
 *   max order qty   - fat-finger: 100 shares, not 100'000
 *   max notional    - price x qty cap per order
 *   price collar    - limit price must be within N basis points of a
 *                     reference price (mid). Catches a price typed as
 *                     1000.00 instead of 100.00 - and a stale/broken book
 *   max open orders - bounds how much can be in flight at once
 *   max position    - WORST-CASE position: current position plus every
 *                     open order on that side filling. Checking only the
 *                     current position lets 10 in-flight buys blow past
 *                     the limit together.
 *   order rate      - at most N orders in any rolling 1-second window
 *                     (runaway-loop protection; exchanges enforce
 *                     message-rate limits too and will disconnect you)
 *
 * Hot-path properties: no allocation (the rate-window ring is sized once
 * in the constructor), no locks, integer-only arithmetic, O(1) per check.
 *
 * Accounting contract (the trader calls these as OUCH events arrive):
 *   on_order_sent  -> open order +1, open qty on that side += qty
 *   on_fill        -> position moves, open qty -= filled
 *   on_cancelled   -> open qty -= cancelled (also used for rejects)
 *   on_order_closed-> open order -1 (order fully done)
 *
 * Thread-safety: none; owned by the trader's single thread.
 */

#ifndef PULSEFORGE_RISK_HPP
#define PULSEFORGE_RISK_HPP

#include "trading_types.hpp"

#include <cstdint>
#include <vector>

struct RiskLimits {
    Qty      max_order_qty      = 1000;
    int64_t  max_notional       = 250000LL * kPriceScale; /* $250k per order */
    int64_t  max_position       = 2000;                   /* shares, either way */
    uint32_t max_open_orders    = 4;
    uint32_t max_orders_per_sec = 200;
    int64_t  price_collar_bps   = 50; /* 0.50% */
};

enum class RiskVerdict : uint8_t {
    Pass,
    KillSwitch,
    MaxOrderQty,
    MaxNotional,
    NoReference,
    PriceCollar,
    MaxOpenOrders,
    MaxPosition,
    Throttled,
};

constexpr int kRiskVerdictCount = 9;

inline const char *to_string(RiskVerdict v) {
    switch (v) {
    case RiskVerdict::Pass:          return "pass";
    case RiskVerdict::KillSwitch:    return "kill switch";
    case RiskVerdict::MaxOrderQty:   return "max order qty";
    case RiskVerdict::MaxNotional:   return "max notional";
    case RiskVerdict::NoReference:   return "no reference price";
    case RiskVerdict::PriceCollar:   return "price collar";
    case RiskVerdict::MaxOpenOrders: return "max open orders";
    case RiskVerdict::MaxPosition:   return "max position";
    case RiskVerdict::Throttled:     return "rate throttle";
    }
    return "?";
}

class RiskGate {
public:
    explicit RiskGate(const RiskLimits &lim)
        : lim_(lim), sent_times_(lim.max_orders_per_sec > 0 ? lim.max_orders_per_sec : 1, 0) {}

    RiskVerdict check(Side side, Price price, Qty qty, Price reference, uint64_t now_ns) const {
        if (killed_) {
            return RiskVerdict::KillSwitch;
        }
        if (qty == 0 || qty > lim_.max_order_qty) {
            return RiskVerdict::MaxOrderQty;
        }
        if (price * static_cast<int64_t>(qty) > lim_.max_notional) {
            return RiskVerdict::MaxNotional;
        }
        if (reference <= 0) {
            return RiskVerdict::NoReference;
        }
        /* |price - ref| / ref > bps / 10000, rearranged to avoid division */
        const int64_t dev = price > reference ? price - reference : reference - price;
        if (dev * 10000 > lim_.price_collar_bps * reference) {
            return RiskVerdict::PriceCollar;
        }
        if (open_orders_ >= lim_.max_open_orders) {
            return RiskVerdict::MaxOpenOrders;
        }
        const int64_t q = static_cast<int64_t>(qty);
        if (side == Side::Buy ? position_ + open_buy_ + q > lim_.max_position
                              : position_ - open_sell_ - q < -lim_.max_position) {
            return RiskVerdict::MaxPosition;
        }
        /* Rolling window: the ring holds the send times of the last N
         * orders. If the oldest of them is less than 1 s old, N orders
         * already went out in the last second. */
        if (sent_count_ >= sent_times_.size() &&
            now_ns - sent_times_[next_slot_] < 1000000000ULL) {
            return RiskVerdict::Throttled;
        }
        return RiskVerdict::Pass;
    }

    void on_order_sent(Side side, Qty qty, uint64_t now_ns) {
        ++open_orders_;
        (side == Side::Buy ? open_buy_ : open_sell_) += qty;
        sent_times_[next_slot_] = now_ns;
        next_slot_ = (next_slot_ + 1) % sent_times_.size();
        ++sent_count_;
    }

    void on_fill(Side side, Qty qty) {
        const int64_t q = static_cast<int64_t>(qty);
        if (side == Side::Buy) {
            position_ += q;
            open_buy_ -= q;
        } else {
            position_ -= q;
            open_sell_ -= q;
        }
    }

    void on_cancelled(Side side, Qty qty) {
        (side == Side::Buy ? open_buy_ : open_sell_) -= static_cast<int64_t>(qty);
    }

    void on_order_closed() {
        if (open_orders_ > 0) {
            --open_orders_;
        }
    }

    void set_kill_switch(bool on) { killed_ = on; }
    bool killed() const { return killed_; }

    int64_t position() const { return position_; }
    int64_t open_buy_qty() const { return open_buy_; }
    int64_t open_sell_qty() const { return open_sell_; }
    uint32_t open_orders() const { return open_orders_; }
    const RiskLimits &limits() const { return lim_; }

private:
    RiskLimits            lim_;
    int64_t               position_    = 0;
    int64_t               open_buy_    = 0;
    int64_t               open_sell_   = 0;
    uint32_t              open_orders_ = 0;
    bool                  killed_      = false;
    std::vector<uint64_t> sent_times_; /* ring of the last N send times */
    size_t                next_slot_  = 0;
    uint64_t              sent_count_ = 0;
};

#endif /* PULSEFORGE_RISK_HPP */
