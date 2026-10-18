/*
 * test_matching_engine.cpp - matching rules, checked through the event
 * stream the engine emits. A recording listener turns every callback
 * into a small Event, so each test can assert "exactly these events, in
 * this order" - the same contract the exchange's ITCH/OUCH output
 * depends on.
 */

#include "matching_engine.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

enum class Ev { Accepted, Rejected, Fill, Rested, RestingCancelled, TakerCancelled };

struct Event {
    explicit Event(Ev t) : type(t) {}
    Ev           type;
    OrderId      id     = 0; /* order the event is about (maker for fills) */
    OrderId      taker  = 0;
    Price        price  = 0;
    Qty          qty    = 0;
    Qty          remain = 0;
    RejectReason reject = RejectReason::BadPrice;
    CancelReason cancel = CancelReason::UserRequested;
};

class Recorder final : public EngineListener {
public:
    std::vector<Event> ev;

    void on_accepted(uint16_t, const OrderRequest &r) override {
        Event e{Ev::Accepted};
        e.id = r.id;
        ev.push_back(e);
    }
    void on_rejected(uint16_t, const OrderRequest &r, RejectReason why) override {
        Event e{Ev::Rejected};
        e.id     = r.id;
        e.reject = why;
        ev.push_back(e);
    }
    void on_fill(uint16_t, const Fill &f) override {
        Event e{Ev::Fill};
        e.id     = f.maker_id;
        e.taker  = f.taker_id;
        e.price  = f.price;
        e.qty    = f.qty;
        e.remain = f.maker_remaining;
        ev.push_back(e);
    }
    void on_rested(uint16_t, const BookOrder &o) override {
        Event e{Ev::Rested};
        e.id    = o.id;
        e.price = o.price;
        e.qty   = o.qty;
        ev.push_back(e);
    }
    void on_resting_cancelled(uint16_t, const BookOrder &o, Qty c, CancelReason why) override {
        Event e{Ev::RestingCancelled};
        e.id     = o.id;
        e.qty    = c;
        e.remain = o.qty - c;
        e.cancel = why;
        ev.push_back(e);
    }
    void on_taker_cancelled(uint16_t, const OrderRequest &r, Qty c, CancelReason why) override {
        Event e{Ev::TakerCancelled};
        e.id     = r.id;
        e.qty    = c;
        e.cancel = why;
        ev.push_back(e);
    }
};

static Price px(double d) { return static_cast<Price>(d * kPriceScale + 0.5); }

static OrderRequest order(OrderId id, Side s, double price, Qty q,
                          TimeInForce tif = TimeInForce::Day, uint32_t owner = 0) {
    OrderRequest r;
    r.id    = id;
    r.side  = s;
    r.price = px(price);
    r.qty   = q;
    r.tif   = tif;
    r.owner = owner;
    return r;
}

struct Fixture {
    Recorder       rec;
    MatchingEngine eng;
    uint16_t       loc;
    explicit Fixture(SelfMatchPolicy stp = SelfMatchPolicy::CancelResting)
        : eng(rec, stp), loc(eng.add_symbol(make_symbol("PFRG"), BookConfig{})) {}
    const OrderBook &book() const { return eng.book(loc); }
};

static void test_rest_and_cross_at_maker_price() {
    Fixture f;
    f.eng.submit(f.loc, order(1, Side::Sell, 100.00, 300));
    EXPECT(f.rec.ev.size() == 2 && f.rec.ev[0].type == Ev::Accepted && f.rec.ev[1].type == Ev::Rested);
    f.rec.ev.clear();

    /* buy limit 100.05 crosses the 100.00 offer and trades AT 100.00 */
    f.eng.submit(f.loc, order(2, Side::Buy, 100.05, 100));
    EXPECT(f.rec.ev.size() == 2);
    EXPECT(f.rec.ev[0].type == Ev::Accepted && f.rec.ev[0].id == 2);
    EXPECT(f.rec.ev[1].type == Ev::Fill && f.rec.ev[1].id == 1 && f.rec.ev[1].taker == 2);
    EXPECT(f.rec.ev[1].price == px(100.00) && f.rec.ev[1].qty == 100 && f.rec.ev[1].remain == 200);
    EXPECT(f.book().best_qty(Side::Sell) == 200 && f.book().empty(Side::Buy));
    EXPECT(f.eng.trade_count() == 1);
}

static void test_price_then_time_priority() {
    Fixture f;
    f.eng.submit(f.loc, order(1, Side::Sell, 100.02, 100)); /* worse price, earliest */
    f.eng.submit(f.loc, order(2, Side::Sell, 100.01, 100)); /* best price, first in time */
    f.eng.submit(f.loc, order(3, Side::Sell, 100.01, 100)); /* best price, second in time */
    f.rec.ev.clear();
    f.eng.submit(f.loc, order(4, Side::Buy, 100.02, 250));
    std::vector<OrderId> makers;
    for (const Event &e : f.rec.ev) {
        if (e.type == Ev::Fill) makers.push_back(e.id);
    }
    EXPECT((makers == std::vector<OrderId>{2, 3, 1})); /* price first, then time */
    EXPECT(f.book().best_qty(Side::Sell) == 50 && f.book().front(Side::Sell)->id == 1);
}

static void test_walk_the_book_then_rest() {
    Fixture f;
    f.eng.submit(f.loc, order(1, Side::Sell, 100.00, 100));
    f.eng.submit(f.loc, order(2, Side::Sell, 100.01, 100));
    f.eng.submit(f.loc, order(3, Side::Sell, 100.05, 100));
    f.rec.ev.clear();
    f.eng.submit(f.loc, order(4, Side::Buy, 100.01, 500));
    /* takes both levels <= 100.01, then rests 300 at 100.01 */
    EXPECT(f.rec.ev.size() == 4);
    EXPECT(f.rec.ev[1].type == Ev::Fill && f.rec.ev[1].price == px(100.00));
    EXPECT(f.rec.ev[2].type == Ev::Fill && f.rec.ev[2].price == px(100.01));
    EXPECT(f.rec.ev[3].type == Ev::Rested && f.rec.ev[3].qty == 300 && f.rec.ev[3].price == px(100.01));
    EXPECT(f.book().best_price(Side::Buy) == px(100.01) && f.book().best_price(Side::Sell) == px(100.05));
}

static void test_ioc_never_rests() {
    Fixture f;
    f.eng.submit(f.loc, order(1, Side::Sell, 100.00, 100));
    f.rec.ev.clear();
    f.eng.submit(f.loc, order(2, Side::Buy, 100.00, 250, TimeInForce::IOC));
    EXPECT(f.rec.ev.size() == 3);
    EXPECT(f.rec.ev[1].type == Ev::Fill && f.rec.ev[1].qty == 100);
    EXPECT(f.rec.ev[2].type == Ev::TakerCancelled && f.rec.ev[2].qty == 150 &&
           f.rec.ev[2].cancel == CancelReason::ImmediateOrCancel);
    EXPECT(f.book().empty(Side::Buy) && f.book().find(2) == nullptr);

    f.rec.ev.clear();
    f.eng.submit(f.loc, order(3, Side::Buy, 99.00, 100, TimeInForce::IOC)); /* nothing to hit */
    EXPECT(f.rec.ev.size() == 2 && f.rec.ev[1].type == Ev::TakerCancelled && f.rec.ev[1].qty == 100);
}

static void test_cancels() {
    Fixture f;
    f.eng.submit(f.loc, order(1, Side::Buy, 99.00, 500, TimeInForce::Day, 7));
    f.eng.submit(f.loc, order(2, Side::Buy, 99.00, 100, TimeInForce::Day, 7));
    f.rec.ev.clear();

    /* reduce 1 to 200: keeps its place at the front of the queue */
    EXPECT(f.eng.cancel(f.loc, 1, 200, 7) == MatchingEngine::CancelResult::Ok);
    EXPECT(f.rec.ev.size() == 1 && f.rec.ev[0].qty == 300 && f.rec.ev[0].remain == 200);
    EXPECT(f.book().front(Side::Buy)->id == 1 && f.book().front(Side::Buy)->qty == 200);

    EXPECT(f.eng.cancel(f.loc, 1, 500, 7) == MatchingEngine::CancelResult::NoChange); /* cannot grow */
    EXPECT(f.eng.cancel(f.loc, 1, 0, 8) == MatchingEngine::CancelResult::NotOwner);
    EXPECT(f.eng.cancel(f.loc, 99, 0, 7) == MatchingEngine::CancelResult::UnknownOrder);
    EXPECT(f.eng.cancel(9, 1, 0, 7) == MatchingEngine::CancelResult::UnknownSymbol);

    f.rec.ev.clear();
    EXPECT(f.eng.cancel(f.loc, 1, 0, 7) == MatchingEngine::CancelResult::Ok);
    EXPECT(f.rec.ev.size() == 1 && f.rec.ev[0].remain == 0 && f.rec.ev[0].qty == 200);
    EXPECT(f.book().front(Side::Buy)->id == 2);
}

static void test_self_match_prevention() {
    {
        Fixture f(SelfMatchPolicy::CancelResting);
        f.eng.submit(f.loc, order(1, Side::Sell, 100.00, 100, TimeInForce::Day, 5)); /* ours  */
        f.eng.submit(f.loc, order(2, Side::Sell, 100.01, 100, TimeInForce::Day, 6)); /* other */
        f.rec.ev.clear();
        f.eng.submit(f.loc, order(3, Side::Buy, 100.01, 100, TimeInForce::IOC, 5));
        /* our resting sell is cancelled, then we trade with owner 6 */
        EXPECT(f.rec.ev.size() == 3);
        EXPECT(f.rec.ev[1].type == Ev::RestingCancelled && f.rec.ev[1].id == 1 &&
               f.rec.ev[1].cancel == CancelReason::SelfMatch);
        EXPECT(f.rec.ev[2].type == Ev::Fill && f.rec.ev[2].id == 2);
    }
    {
        Fixture f(SelfMatchPolicy::CancelIncoming);
        f.eng.submit(f.loc, order(1, Side::Sell, 100.00, 100, TimeInForce::Day, 5));
        f.rec.ev.clear();
        f.eng.submit(f.loc, order(3, Side::Buy, 100.00, 100, TimeInForce::Day, 5));
        EXPECT(f.rec.ev.size() == 2 && f.rec.ev[1].type == Ev::TakerCancelled &&
               f.rec.ev[1].cancel == CancelReason::SelfMatch);
        EXPECT(f.book().find(1) != nullptr && f.book().find(3) == nullptr);
    }
    {
        Fixture f; /* owner 0 = synthetic flow: exempt from STP */
        f.eng.submit(f.loc, order(1, Side::Sell, 100.00, 100));
        f.rec.ev.clear();
        f.eng.submit(f.loc, order(2, Side::Buy, 100.00, 100));
        EXPECT(f.rec.ev.size() == 2 && f.rec.ev[1].type == Ev::Fill);
    }
}

static void test_rejects() {
    Fixture f;
    f.eng.submit(f.loc, order(1, Side::Buy, 100.005, 100)); /* off tick */
    f.eng.submit(f.loc, order(2, Side::Buy, 10.00, 100));   /* outside band */
    f.eng.submit(f.loc, order(3, Side::Buy, 100.00, 0));    /* zero qty */
    f.eng.submit(9, order(4, Side::Buy, 100.00, 100));      /* unknown symbol */
    EXPECT(f.rec.ev.size() == 4);
    EXPECT(f.rec.ev[0].type == Ev::Rejected && f.rec.ev[0].reject == RejectReason::BadPrice);
    EXPECT(f.rec.ev[1].reject == RejectReason::BadPrice);
    EXPECT(f.rec.ev[2].reject == RejectReason::BadQty);
    EXPECT(f.rec.ev[3].reject == RejectReason::UnknownSymbol);
    EXPECT(f.book().order_count() == 0);
}

int main() {
    test_rest_and_cross_at_maker_price();
    test_price_then_time_priority();
    test_walk_the_book_then_rest();
    test_ioc_never_rests();
    test_cancels();
    test_self_match_prevention();
    test_rejects();
    if (g_failures == 0) {
        std::printf("test_matching_engine: ALL PASSED\n");
        return 0;
    }
    std::printf("test_matching_engine: %d FAILURE(S)\n", g_failures);
    return 1;
}
