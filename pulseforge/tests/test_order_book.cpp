/*
 * test_order_book.cpp - unit tests for the ladder/bitmap/pool OrderBook,
 * plus a randomized comparison against a deliberately naive reference
 * book built from std::map + std::list (obviously correct, slow).
 */

#include "order_book.hpp"

#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
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

static Price px(double dollars) { return static_cast<Price>(dollars * kPriceScale + 0.5); }

static void test_bitmap_scans() {
    std::vector<uint64_t> bits(3, 0); /* 192 levels */
    EXPECT(bitmap_find_next(bits, 0) == -1);
    EXPECT(bitmap_find_prev(bits, 191) == -1);
    bits[0] |= 1ULL << 63; /* level 63  */
    bits[1] |= 1ULL << 0;  /* level 64  */
    bits[2] |= 1ULL << 63; /* level 191 */
    EXPECT(bitmap_find_next(bits, 0) == 63);
    EXPECT(bitmap_find_next(bits, 63) == 63);
    EXPECT(bitmap_find_next(bits, 64) == 64);
    EXPECT(bitmap_find_next(bits, 65) == 191);
    EXPECT(bitmap_find_next(bits, 192) == -1);
    EXPECT(bitmap_find_prev(bits, 191) == 191);
    EXPECT(bitmap_find_prev(bits, 190) == 64);
    EXPECT(bitmap_find_prev(bits, 64) == 64);
    EXPECT(bitmap_find_prev(bits, 63) == 63);
    EXPECT(bitmap_find_prev(bits, 62) == -1);
    EXPECT(bitmap_find_prev(bits, -1) == -1);
}

static void test_best_prices_and_fifo() {
    OrderBook b{BookConfig{}};
    EXPECT(b.empty(Side::Buy) && b.empty(Side::Sell) && b.front(Side::Buy) == nullptr);

    EXPECT(b.add(1, Side::Buy, px(99.98), 100) == OrderBook::AddResult::Ok);
    EXPECT(b.add(2, Side::Buy, px(99.99), 200) == OrderBook::AddResult::Ok);
    EXPECT(b.add(3, Side::Buy, px(99.99), 300) == OrderBook::AddResult::Ok);
    EXPECT(b.add(4, Side::Sell, px(100.01), 400) == OrderBook::AddResult::Ok);
    EXPECT(b.add(5, Side::Sell, px(100.02), 500) == OrderBook::AddResult::Ok);

    EXPECT(b.best_price(Side::Buy) == px(99.99));
    EXPECT(b.best_qty(Side::Buy) == 500 && b.best_count(Side::Buy) == 2);
    EXPECT(b.best_price(Side::Sell) == px(100.01));
    EXPECT(b.front(Side::Buy)->id == 2); /* time priority: 2 arrived before 3 */
    EXPECT(b.order_count() == 5);

    /* partial reduce keeps priority */
    EXPECT(b.reduce(2, 50));
    EXPECT(b.front(Side::Buy)->id == 2 && b.front(Side::Buy)->qty == 150);
    EXPECT(b.best_qty(Side::Buy) == 450);

    /* reduce_front to zero pops it; 3 becomes the front */
    b.reduce_front(Side::Buy, 150);
    EXPECT(b.front(Side::Buy)->id == 3 && b.best_qty(Side::Buy) == 300);
    EXPECT(b.find(2) == nullptr);

    /* removing the last order at the best level moves best down */
    EXPECT(b.remove(3));
    EXPECT(b.best_price(Side::Buy) == px(99.98));
    EXPECT(b.remove(1));
    EXPECT(b.empty(Side::Buy));

    EXPECT(!b.reduce(4, 401)); /* over-reduce = inconsistency, refused */
    EXPECT(!b.remove(12345));
}

static void test_unlink_from_middle() {
    OrderBook b{BookConfig{}};
    for (OrderId id = 1; id <= 5; id++) {
        b.add(id, Side::Sell, px(100.00), 100);
    }
    EXPECT(b.remove(3)); /* middle of the FIFO */
    std::vector<OrderId> order;
    b.for_each_order([&](const BookOrder &o) { order.push_back(o.id); });
    EXPECT((order == std::vector<OrderId>{1, 2, 4, 5}));
    EXPECT(b.remove(1) && b.remove(5)); /* head and tail */
    order.clear();
    b.for_each_order([&](const BookOrder &o) { order.push_back(o.id); });
    EXPECT((order == std::vector<OrderId>{2, 4}));
    EXPECT(b.level_at(Side::Sell, px(100.00)).qty == 200);
    EXPECT(b.level_at(Side::Sell, px(100.00)).orders == 2);
}

static void test_validation_and_capacity() {
    BookConfig cfg;
    cfg.max_orders = 3;
    OrderBook b{cfg};
    EXPECT(b.add(1, Side::Buy, px(100.005), 100) == OrderBook::AddResult::BadPrice); /* off tick */
    EXPECT(b.add(1, Side::Buy, px(49.99), 100) == OrderBook::AddResult::BadPrice);   /* below band */
    EXPECT(b.add(1, Side::Buy, px(150.01), 100) == OrderBook::AddResult::BadPrice);  /* above band */
    EXPECT(b.add(1, Side::Buy, px(100), 0) == OrderBook::AddResult::BadQty);
    EXPECT(b.add(1, Side::Buy, px(50), 100) == OrderBook::AddResult::Ok);   /* band edges are valid */
    EXPECT(b.add(2, Side::Sell, px(150), 100) == OrderBook::AddResult::Ok);
    EXPECT(b.add(2, Side::Sell, px(149), 100) == OrderBook::AddResult::DuplicateId);
    EXPECT(b.add(3, Side::Sell, px(149), 100) == OrderBook::AddResult::Ok);
    EXPECT(b.add(4, Side::Sell, px(148), 100) == OrderBook::AddResult::Full); /* pool exhausted */
    EXPECT(b.remove(3));
    EXPECT(b.add(4, Side::Sell, px(148), 100) == OrderBook::AddResult::Ok); /* slot recycled */
}

static void test_depth_iteration_across_words() {
    OrderBook b{BookConfig{}};
    /* asks spread over several 64-level bitmap words */
    const double asks[] = {100.00, 100.70, 101.50, 103.10};
    OrderId id = 1;
    for (double a : asks) {
        b.add(id++, Side::Sell, px(a), 100);
    }
    std::vector<Price> seen;
    b.for_each_level(Side::Sell, 10, [&](const LevelView &l) { seen.push_back(l.price); });
    EXPECT(seen.size() == 4 && seen[0] == px(100.00) && seen[3] == px(103.10));
    seen.clear();
    b.for_each_level(Side::Sell, 2, [&](const LevelView &l) { seen.push_back(l.price); });
    EXPECT(seen.size() == 2 && seen[1] == px(100.70));

    /* remove best repeatedly: best must jump across words via the bitmap */
    EXPECT(b.remove(1) && b.best_price(Side::Sell) == px(100.70));
    EXPECT(b.remove(2) && b.best_price(Side::Sell) == px(101.50));
    EXPECT(b.remove(3) && b.best_price(Side::Sell) == px(103.10));
    EXPECT(b.remove(4) && b.empty(Side::Sell));

    b.add(10, Side::Buy, px(60), 1);
    b.add(11, Side::Buy, px(140), 1);
    EXPECT(b.remove(11) && b.best_price(Side::Buy) == px(60));
    b.clear();
    EXPECT(b.order_count() == 0 && b.empty(Side::Buy) && b.find(10) == nullptr);
    EXPECT(b.add(10, Side::Buy, px(60), 1) == OrderBook::AddResult::Ok); /* reusable after clear */
}

/* ---- randomized comparison with a naive reference book -------------- */

struct RefBook {
    struct O {
        OrderId id;
        Qty     qty;
    };
    std::map<Price, std::list<O>> bids, asks;
    std::map<OrderId, std::pair<Side, Price>> where;

    std::map<Price, std::list<O>> &side(Side s) { return s == Side::Buy ? bids : asks; }

    void add(OrderId id, Side s, Price p, Qty q) {
        side(s)[p].push_back({id, q});
        where[id] = {s, p};
    }
    bool reduce(OrderId id, Qty by) {
        auto it = where.find(id);
        if (it == where.end()) return false;
        auto &lvl = side(it->second.first)[it->second.second];
        for (auto o = lvl.begin(); o != lvl.end(); ++o) {
            if (o->id == id) {
                if (by > o->qty) return false;
                o->qty -= by;
                if (o->qty == 0) {
                    lvl.erase(o);
                    if (lvl.empty()) side(it->second.first).erase(it->second.second);
                    where.erase(it);
                }
                return true;
            }
        }
        return false;
    }
    bool remove(OrderId id) {
        auto it = where.find(id);
        if (it == where.end()) return false;
        auto &lvl = side(it->second.first)[it->second.second];
        Qty q = 0;
        for (auto &o : lvl) {
            if (o.id == id) q = o.qty;
        }
        return reduce(id, q);
    }
};

static void test_randomized_against_reference() {
    BookConfig cfg;
    cfg.max_orders = 5000;
    OrderBook b{cfg};
    RefBook ref;
    std::mt19937_64 rng(7);
    std::vector<OrderId> live;
    OrderId next = 1;
    for (int i = 0; i < 200000; i++) {
        const unsigned op = static_cast<unsigned>(rng() % 10);
        if (op < 5 && live.size() < 4000) {
            Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            /* clustered around $100 so levels share orders */
            Price p = 100 * kPriceScale + (static_cast<Price>(rng() % 41) - 20) * kCent;
            Qty q = static_cast<Qty>(1 + rng() % 500);
            EXPECT(b.add(next, s, p, q) == OrderBook::AddResult::Ok);
            ref.add(next, s, p, q);
            live.push_back(next++);
        } else if (op < 8 && !live.empty()) {
            size_t k = static_cast<size_t>(rng() % live.size());
            OrderId id = live[k];
            const BookOrder *o = b.find(id);
            EXPECT(o != nullptr);
            if (o == nullptr) continue;
            Qty by = static_cast<Qty>(1 + rng() % o->qty);
            bool gone = by == o->qty;
            EXPECT(b.reduce(id, by) == ref.reduce(id, by));
            if (gone) {
                live[k] = live.back();
                live.pop_back();
            }
        } else if (!live.empty()) {
            size_t k = static_cast<size_t>(rng() % live.size());
            EXPECT(b.remove(live[k]) && ref.remove(live[k]));
            live[k] = live.back();
            live.pop_back();
        }
        /* compare top of book every step */
        EXPECT(b.empty(Side::Buy) == ref.bids.empty());
        EXPECT(b.empty(Side::Sell) == ref.asks.empty());
        if (!ref.bids.empty()) {
            auto best = ref.bids.rbegin();
            uint64_t q = 0;
            for (auto &o : best->second) q += o.qty;
            EXPECT(b.best_price(Side::Buy) == best->first && b.best_qty(Side::Buy) == q);
            EXPECT(b.front(Side::Buy)->id == best->second.front().id);
        }
        if (!ref.asks.empty()) {
            auto best = ref.asks.begin();
            uint64_t q = 0;
            for (auto &o : best->second) q += o.qty;
            EXPECT(b.best_price(Side::Sell) == best->first && b.best_qty(Side::Sell) == q);
            EXPECT(b.front(Side::Sell)->id == best->second.front().id);
        }
        EXPECT(b.order_count() == ref.where.size());
        if (g_failures > 20) {
            return; /* do not drown the output */
        }
    }
}

int main() {
    test_bitmap_scans();
    test_best_prices_and_fifo();
    test_unlink_from_middle();
    test_validation_and_capacity();
    test_depth_iteration_across_words();
    test_randomized_against_reference();
    if (g_failures == 0) {
        std::printf("test_order_book: ALL PASSED\n");
        return 0;
    }
    std::printf("test_order_book: %d FAILURE(S)\n", g_failures);
    return 1;
}
