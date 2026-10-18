/*
 * order_book.hpp - a cache-friendly, allocation-free limit order book.
 *
 * Responsibility: hold every resting order for ONE symbol, in price-time
 * priority, and answer "what is the best bid/ask and how much is there"
 * in O(1). The same class serves two roles:
 *   - inside the exchange: the authoritative book the matching engine
 *     trades against (matching_engine.hpp);
 *   - inside the trader: a replica rebuilt from the ITCH feed
 *     (feed_handler.hpp). Same code on both sides, so the replica cannot
 *     drift from the exchange because of a different data structure.
 *
 * The textbook book is std::map<Price, std::list<Order>> per side plus
 * std::unordered_map<OrderId, iterator>. It is correct and simple, and
 * every operation hits the allocator and chases pointers across the heap
 * (map nodes, list nodes, hash nodes). bench/book_bench.cpp races that
 * design against this one. The ideas here:
 *
 * 1. PRICE LADDER (array indexed by price). Prices live on a tick grid
 *    inside a band, so level = (price - min_price) / tick is a dense
 *    integer. Each side is a std::vector<Level>; finding a price level
 *    is an array index, not a tree walk. Cost: memory for empty levels
 *    (10'001 levels x 24 B x 2 sides ~ 480 KB for $50-$150 at 1c) - fine,
 *    and it fits in L2 cache.
 *
 * 2. OCCUPANCY BITMAP + CLZ/CTZ. Which levels are non-empty is one bit
 *    per level in a std::vector<uint64_t>. When the best level empties
 *    we must find the next one; instead of walking levels one by one we
 *    scan 64 levels per instruction: __builtin_ctzll (count trailing
 *    zeros) finds the lowest set bit = next ask, __builtin_clzll (count
 *    leading zeros) the highest = next bid. Each is a single TZCNT/LZCNT
 *    instruction on x86. (C++20 spells them std::countr_zero/countl_zero.)
 *
 * 3. CACHED BEST LEVEL. best_[side] is kept up to date on every add and
 *    remove, so best_bid()/best_ask() - called on every market data
 *    message by the strategy - is a load, not a search.
 *
 * 4. ORDER POOL + INTRUSIVE FIFO. All BookOrder nodes live in one
 *    pre-sized std::vector; a free list threads through unused nodes
 *    (the mem_pool.hpp idea, with 32-bit indices instead of pointers -
 *    half the size, and they stay valid if the vector is copied). Orders
 *    at one price form a doubly-linked FIFO via prev/next indices stored
 *    IN the order ("intrusive": no separate list nodes). Time priority
 *    = position in that FIFO: append at the tail, match from the head,
 *    unlink from the middle on cancel in O(1).
 *
 * 5. ID INDEX. FlatIdMap<uint32_t> maps order id -> pool index for
 *    cancel/execute/delete-by-id (flat_id_map.hpp).
 *
 * After construction, no operation allocates. Capacity limits (band,
 * max_orders) are explicit and return errors instead of growing.
 *
 * Thread-safety: none. A book is owned by exactly one thread (the
 * exchange's matching thread, or the trader's feed thread) - the same
 * single-writer principle as the Disruptor ring in mpmc_fanout.hpp.
 */

#ifndef PULSEFORGE_ORDER_BOOK_HPP
#define PULSEFORGE_ORDER_BOOK_HPP

#include "flat_id_map.hpp"
#include "trading_types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

struct BookConfig {
    Price    min_price  = 50 * kPriceScale;  /* inclusive */
    Price    max_price  = 150 * kPriceScale; /* inclusive */
    Price    tick_size  = kCent;
    uint32_t max_orders = 1u << 16;
};

/* One resting order (40 bytes). owner/user_ref are the exchange's
 * bookkeeping (which session, which client order number); the trader's
 * replica leaves them 0. */
struct BookOrder {
    OrderId  id;
    Price    price;
    Qty      qty;      /* remaining (open) shares */
    uint32_t owner;
    uint32_t user_ref;
    uint32_t prev;     /* pool index of the order ahead in the FIFO */
    uint32_t next;     /* pool index of the order behind            */
    Side     side;
};

struct LevelView {
    Price    price;
    uint64_t qty;
    uint32_t orders;
};

class OrderBook {
public:
    static constexpr uint32_t kNil = 0xFFFFFFFFu;

    enum class AddResult : uint8_t { Ok, BadPrice, BadQty, DuplicateId, Full };

    explicit OrderBook(const BookConfig &cfg);

    AddResult add(OrderId id, Side side, Price price, Qty qty,
                  uint32_t owner = 0, uint32_t user_ref = 0);

    /* Remove `by` shares from the order (execute or partial cancel);
     * removes it entirely when it reaches 0. false if the id is unknown
     * or by > the order's open qty (an inconsistency the caller must
     * handle - for a replica book it means "we are out of sync"). */
    bool reduce(OrderId id, Qty by);

    /* Remove the order entirely. false if the id is unknown. */
    bool remove(OrderId id);

    /* Oldest order at the best price on `side`, or nullptr if that side
     * is empty. The pointer is invalidated by any mutating call. */
    const BookOrder *front(Side side) const;

    /* Matching-engine fast path: reduce the front order of `side` by
     * `by` shares (no id lookup). Precondition: front(side) != nullptr
     * and by <= front(side)->qty. */
    void reduce_front(Side side, Qty by);

    const BookOrder *find(OrderId id) const;

    bool  empty(Side side) const { return ladder(side).best < 0; }
    /* Precondition: !empty(side). */
    Price best_price(Side side) const { return price_of(ladder(side).best); }
    /* 0 if the side is empty. */
    uint64_t best_qty(Side side) const;
    uint32_t best_count(Side side) const;

    LevelView level_at(Side side, Price price) const;

    /* f(const LevelView&) for up to max_levels price levels, best first. */
    template <typename F>
    void for_each_level(Side side, size_t max_levels, F &&f) const;

    /* f(const BookOrder&) for every order, each side best price first
     * and FIFO order within a price: exactly the order a snapshot must
     * replay them in so the receiver gets the same time priority. */
    template <typename F>
    void for_each_order(F &&f) const;

    void clear();

    bool valid_price(Price p) const;
    size_t order_count() const { return live_; }
    const BookConfig &config() const { return cfg_; }
    size_t level_count() const { return levels_per_side_; }

private:
    struct Level {
        uint64_t qty;   /* total open shares at this price */
        uint32_t head;  /* oldest order (matches first)    */
        uint32_t tail;  /* newest order                    */
        uint32_t count; /* number of orders                */
    };

    struct Ladder {
        std::vector<Level>    levels;
        std::vector<uint64_t> bits; /* 1 bit per level: non-empty? */
        int32_t               best = -1;
    };

    Ladder &ladder(Side s) { return s == Side::Buy ? bids_ : asks_; }
    const Ladder &ladder(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    int32_t index_of(Price p) const {
        return static_cast<int32_t>((p - cfg_.min_price) / cfg_.tick_size);
    }
    Price price_of(int32_t idx) const {
        return cfg_.min_price + static_cast<Price>(idx) * cfg_.tick_size;
    }

    /* Next non-empty level strictly worse than `idx` (lower for bids,
     * higher for asks), or -1. */
    int32_t next_worse(Side side, int32_t idx) const;

    void unlink_and_free(uint32_t node);

    BookConfig             cfg_;
    size_t                 levels_per_side_;
    Ladder                 bids_;
    Ladder                 asks_;
    std::vector<BookOrder> nodes_;
    uint32_t               free_head_ = kNil;
    size_t                 live_      = 0;
    FlatIdMap<uint32_t>    ids_;
};

/* ---- bitmap scans (exposed for tests and the bench) ------------------ */

/* Highest set bit index <= idx, or -1. */
int32_t bitmap_find_prev(const std::vector<uint64_t> &bits, int32_t idx);
/* Lowest set bit index >= idx, or -1. */
int32_t bitmap_find_next(const std::vector<uint64_t> &bits, int32_t idx);

template <typename F>
void OrderBook::for_each_level(Side side, size_t max_levels, F &&f) const {
    const Ladder &l = ladder(side);
    int32_t idx = l.best;
    for (size_t n = 0; idx >= 0 && n < max_levels; n++) {
        const Level &lv = l.levels[static_cast<size_t>(idx)];
        f(LevelView{price_of(idx), lv.qty, lv.count});
        idx = next_worse(side, idx);
    }
}

template <typename F>
void OrderBook::for_each_order(F &&f) const {
    for (Side side : {Side::Buy, Side::Sell}) {
        const Ladder &l = ladder(side);
        for (int32_t idx = l.best; idx >= 0; idx = next_worse(side, idx)) {
            for (uint32_t n = l.levels[static_cast<size_t>(idx)].head; n != kNil;
                 n = nodes_[n].next) {
                f(nodes_[n]);
            }
        }
    }
}

#endif /* PULSEFORGE_ORDER_BOOK_HPP */
