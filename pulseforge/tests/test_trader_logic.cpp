/*
 * test_trader_logic.cpp - the trader's socket-free logic:
 *   - SequenceTracker (gap / duplicate detection)
 *   - apply_itch (replica book maintenance)
 *   - the end-to-end market data contract: random flow through the REAL
 *     MatchingEngine, every event encoded to ITCH bytes, decoded and
 *     applied to a replica -> the replica must equal the exchange book.
 *     A second replica joins late from a snapshot and must converge too.
 *   - RiskGate checks
 *   - ImbalanceStrategy signals
 */

#include "feed_handler.hpp"
#include "itch.hpp"
#include "matching_engine.hpp"
#include "risk.hpp"
#include "strategy.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static Price px(double d) { return static_cast<Price>(d * kPriceScale + 0.5); }

static void test_sequence_tracker() {
    SequenceTracker t;
    t.reset(10);
    using V = SequenceTracker::Verdict;
    EXPECT(t.on_message(10) == V::Apply && t.expected() == 11);
    EXPECT(t.on_message(10) == V::Duplicate); /* the B-line copy of 10 */
    EXPECT(t.on_message(11) == V::Apply);
    EXPECT(t.on_message(13) == V::Gap && t.expected() == 12); /* 12 is missing */
    EXPECT(t.on_heartbeat(12) == V::Apply);
    EXPECT(t.on_heartbeat(14) == V::Gap); /* heartbeat reveals a lost packet */
}

static void test_apply_itch() {
    OrderBook b{BookConfig{}};
    const uint16_t loc = 1;
    itch::AddOrder a{loc, 1, 100, Side::Buy, 300, make_symbol("PFRG"), static_cast<uint32_t>(px(99.5))};
    EXPECT(apply_itch(b, loc, a) == ApplyResult::Applied);
    EXPECT(b.best_price(Side::Buy) == px(99.5) && b.best_qty(Side::Buy) == 300);

    itch::AddOrder other = a;
    other.locate    = 2;
    other.order_ref = 101;
    EXPECT(apply_itch(b, loc, other) == ApplyResult::Ignored); /* another symbol */

    EXPECT(apply_itch(b, loc, itch::OrderExecuted{loc, 2, 100, 100, 1}) == ApplyResult::Applied);
    EXPECT(b.best_qty(Side::Buy) == 200);
    EXPECT(apply_itch(b, loc, itch::OrderCancel{loc, 3, 100, 50}) == ApplyResult::Applied);
    EXPECT(b.best_qty(Side::Buy) == 150);
    /* executing more than is left, or an unknown id, means we are out of sync */
    EXPECT(apply_itch(b, loc, itch::OrderExecuted{loc, 4, 100, 999, 2}) == ApplyResult::Inconsistent);
    EXPECT(apply_itch(b, loc, itch::OrderDelete{loc, 4, 555}) == ApplyResult::Inconsistent);
    EXPECT(apply_itch(b, loc, itch::OrderDelete{loc, 5, 100}) == ApplyResult::Applied);
    EXPECT(b.empty(Side::Buy));
    EXPECT(apply_itch(b, loc, itch::SystemEvent{0, 6, 'C'}) == ApplyResult::Ignored);
    EXPECT(itch_timestamp(itch::Message{itch::OrderDelete{loc, 77, 1}}) == 77);
}

/* Listener that plays the exchange's market-data role, into memory. */
class ItchTap final : public EngineListener {
public:
    std::vector<std::vector<uint8_t>> wire; /* one encoded message each */
    Symbol sym = make_symbol("PFRG");

    void on_accepted(uint16_t, const OrderRequest &) override {}
    void on_rejected(uint16_t, const OrderRequest &, RejectReason) override {}
    void on_fill(uint16_t loc, const Fill &f) override {
        push(itch::OrderExecuted{loc, ts_++, f.maker_id, f.qty, f.match_number});
    }
    void on_rested(uint16_t loc, const BookOrder &o) override {
        push(itch::AddOrder{loc, ts_++, o.id, o.side, o.qty, sym, static_cast<uint32_t>(o.price)});
    }
    void on_resting_cancelled(uint16_t loc, const BookOrder &o, Qty c, CancelReason) override {
        if (c == o.qty) {
            push(itch::OrderDelete{loc, ts_++, o.id});
        } else {
            push(itch::OrderCancel{loc, ts_++, o.id, c});
        }
    }
    void on_taker_cancelled(uint16_t, const OrderRequest &, Qty, CancelReason) override {}

private:
    template <typename M>
    void push(const M &m) {
        uint8_t buf[itch::kMaxMessageLen];
        size_t n = itch::encode(m, buf, sizeof(buf));
        wire.emplace_back(buf, buf + n);
    }
    uint64_t ts_ = 1;
};

static bool books_equal(const OrderBook &x, const OrderBook &y) {
    if (x.order_count() != y.order_count()) return false;
    std::vector<BookOrder> ox, oy;
    x.for_each_order([&](const BookOrder &o) { ox.push_back(o); });
    y.for_each_order([&](const BookOrder &o) { oy.push_back(o); });
    if (ox.size() != oy.size()) return false;
    for (size_t i = 0; i < ox.size(); i++) {
        /* same orders, same prices, same sizes, same PRIORITY ORDER */
        if (ox[i].id != oy[i].id || ox[i].price != oy[i].price || ox[i].qty != oy[i].qty ||
            ox[i].side != oy[i].side) {
            return false;
        }
    }
    return true;
}

static void test_replica_matches_exchange() {
    ItchTap tap;
    MatchingEngine eng(tap);
    const uint16_t loc = eng.add_symbol(tap.sym, BookConfig{});
    OrderBook replica{BookConfig{}};
    OrderBook late{BookConfig{}};
    bool late_joined = false;
    size_t applied = 0;
    std::mt19937_64 rng(11);
    std::vector<OrderId> live;
    OrderId next = 1;

    auto drain = [&](bool to_late) {
        for (const auto &m : tap.wire) {
            auto msg = itch::decode(m.data(), m.size());
            EXPECT(msg.has_value());
            if (!msg) continue;
            EXPECT(apply_itch(replica, loc, *msg) == ApplyResult::Applied);
            if (to_late) {
                EXPECT(apply_itch(late, loc, *msg) == ApplyResult::Applied);
            }
            ++applied;
        }
        tap.wire.clear();
    };

    for (int i = 0; i < 50000; i++) {
        const unsigned op = static_cast<unsigned>(rng() % 10);
        if (op < 6 || live.empty()) {
            OrderRequest r;
            r.id    = next++;
            r.side  = (rng() & 1) ? Side::Buy : Side::Sell;
            r.price = 100 * kPriceScale + (static_cast<Price>(rng() % 21) - 10) * kCent;
            r.qty   = static_cast<Qty>(100 * (1 + rng() % 5));
            r.tif   = (rng() % 4 == 0) ? TimeInForce::IOC : TimeInForce::Day;
            eng.submit(loc, r);
            if (eng.book(loc).find(r.id)) live.push_back(r.id);
        } else {
            size_t k = static_cast<size_t>(rng() % live.size());
            const BookOrder *o = eng.book(loc).find(live[k]);
            if (o != nullptr && o->qty >= 200 && (rng() & 1)) {
                eng.cancel(loc, o->id, o->qty / 2, 0);
            } else {
                if (o != nullptr) eng.cancel(loc, o->id, 0, 0);
                live[k] = live.back();
                live.pop_back();
            }
        }
        drain(late_joined);

        if (i == 20000) {
            /* a late joiner: snapshot the exchange book exactly as the
             * snapshot service does (priority order), then go incremental */
            eng.book(loc).for_each_order([&](const BookOrder &o) {
                EXPECT(late.add(o.id, o.side, o.price, o.qty) == OrderBook::AddResult::Ok);
            });
            late_joined = true;
        }
        if (i % 5000 == 0) {
            EXPECT(books_equal(eng.book(loc), replica));
        }
    }
    std::printf("  replica test: %zu ITCH messages applied, %" PRIu64 " trades, %zu resting\n",
                applied, eng.trade_count(), eng.book(loc).order_count());
    EXPECT(applied > 25000 && eng.trade_count() > 1000); /* the flow really exercised the book */
    EXPECT(books_equal(eng.book(loc), replica));
    EXPECT(books_equal(eng.book(loc), late));
}

static void test_risk_gate() {
    RiskLimits lim;
    lim.max_order_qty      = 500;
    lim.max_position       = 300;
    lim.max_open_orders    = 2;
    lim.max_orders_per_sec = 3;
    lim.price_collar_bps   = 50;
    RiskGate r(lim);
    const Price ref = px(100);
    const uint64_t t0 = 1000000000ULL;

    EXPECT(r.check(Side::Buy, ref, 100, ref, t0) == RiskVerdict::Pass);
    EXPECT(r.check(Side::Buy, ref, 600, ref, t0) == RiskVerdict::MaxOrderQty);
    EXPECT(r.check(Side::Buy, ref, 100, 0, t0) == RiskVerdict::NoReference);
    EXPECT(r.check(Side::Buy, px(100.51), 100, ref, t0) == RiskVerdict::PriceCollar); /* 51 bps */
    EXPECT(r.check(Side::Buy, px(100.50), 100, ref, t0) == RiskVerdict::Pass);        /* 50 bps */
    EXPECT(r.check(Side::Buy, ref, 400, ref, t0) == RiskVerdict::MaxPosition);

    /* worst-case position: two open buys of 150 use up the 300 limit */
    r.on_order_sent(Side::Buy, 150, t0);
    EXPECT(r.check(Side::Buy, ref, 150, ref, t0) == RiskVerdict::Pass);
    r.on_order_sent(Side::Buy, 150, t0);
    EXPECT(r.check(Side::Sell, ref, 100, ref, t0) == RiskVerdict::MaxOpenOrders);
    r.on_fill(Side::Buy, 150);
    r.on_order_closed();
    EXPECT(r.position() == 150 && r.open_buy_qty() == 150 && r.open_orders() == 1);
    EXPECT(r.check(Side::Buy, ref, 100, ref, t0) == RiskVerdict::MaxPosition);
    EXPECT(r.check(Side::Sell, ref, 100, ref, t0) == RiskVerdict::Pass);
    r.on_cancelled(Side::Buy, 150);
    r.on_order_closed();
    EXPECT(r.open_buy_qty() == 0 && r.open_orders() == 0);

    /* rate throttle: 3 sent within one second -> 4th refused until the window slides */
    r.on_order_sent(Side::Sell, 1, t0 + 100);
    r.on_order_closed();
    EXPECT(r.check(Side::Sell, ref, 1, ref, t0 + 200) == RiskVerdict::Throttled);
    EXPECT(r.check(Side::Sell, ref, 1, ref, t0 + 1000000000ULL) == RiskVerdict::Pass);

    r.set_kill_switch(true);
    EXPECT(r.check(Side::Sell, ref, 1, ref, t0 + 5000000000ULL) == RiskVerdict::KillSwitch);
}

static void test_strategy() {
    StrategyParams p;
    p.imbalance_pct = 60;
    p.order_qty     = 100;
    p.min_top_qty   = 200;
    p.cooldown_ns   = 1000;
    p.max_position  = 100;
    ImbalanceStrategy s(p);
    OrderBook b{BookConfig{}};
    b.add(1, Side::Buy, px(99.99), 900);
    b.add(2, Side::Sell, px(100.01), 100);
    /* imbalance = (900-100)/1000 = 80% > 60%: buy at the ask */
    auto sig = s.on_book_update(b, 0, false, 10000);
    EXPECT(sig && sig->side == Side::Buy && sig->price == px(100.01) && sig->imbalance_pct == 80);
    EXPECT(!s.on_book_update(b, 0, false, 10500));  /* cooldown */
    EXPECT(!s.on_book_update(b, 0, true, 20000));   /* order already in flight */
    EXPECT(!s.on_book_update(b, 100, false, 30000)); /* would exceed max position */

    OrderBook b2{BookConfig{}};
    b2.add(1, Side::Buy, px(99.99), 100);
    b2.add(2, Side::Sell, px(100.01), 900);
    auto sell = s.on_book_update(b2, 0, false, 40000);
    EXPECT(sell && sell->side == Side::Sell && sell->price == px(99.99));

    OrderBook b3{BookConfig{}};
    b3.add(1, Side::Buy, px(99.99), 500);
    b3.add(2, Side::Sell, px(100.01), 400); /* balanced: no signal */
    EXPECT(!s.on_book_update(b3, 0, false, 50000));
}

int main() {
    test_sequence_tracker();
    test_apply_itch();
    test_replica_matches_exchange();
    test_risk_gate();
    test_strategy();
    if (g_failures == 0) {
        std::printf("test_trader_logic: ALL PASSED\n");
        return 0;
    }
    std::printf("test_trader_logic: %d FAILURE(S)\n", g_failures);
    return 1;
}
