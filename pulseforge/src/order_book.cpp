/*
 * order_book.cpp - implementation of the ladder + bitmap + pool book.
 * See order_book.hpp for the design rationale.
 */

#include "order_book.hpp"

#include <stdexcept>

/* ---- bitmap scans ----------------------------------------------------- */

int32_t bitmap_find_prev(const std::vector<uint64_t> &bits, int32_t idx) {
    if (idx < 0) {
        return -1;
    }
    int32_t w = idx >> 6;
    const int b = idx & 63;
    /* keep bits 0..b of the first word: mask = 2^(b+1) - 1, careful at b == 63 */
    uint64_t mask = (b == 63) ? ~0ULL : ((1ULL << (b + 1)) - 1);
    uint64_t word = bits[static_cast<size_t>(w)] & mask;
    for (;;) {
        if (word != 0) {
            /* highest set bit = 63 - (number of leading zero bits) */
            return (w << 6) + (63 - __builtin_clzll(word));
        }
        if (--w < 0) {
            return -1;
        }
        word = bits[static_cast<size_t>(w)];
    }
}

int32_t bitmap_find_next(const std::vector<uint64_t> &bits, int32_t idx) {
    if (idx < 0) {
        idx = 0;
    }
    size_t w = static_cast<size_t>(idx) >> 6;
    if (w >= bits.size()) {
        return -1;
    }
    const int b = idx & 63;
    uint64_t word = bits[w] & (~0ULL << b); /* drop bits below b */
    for (;;) {
        if (word != 0) {
            /* __builtin_ctzll is undefined for 0 - hence the check above */
            return static_cast<int32_t>((w << 6) + static_cast<size_t>(__builtin_ctzll(word)));
        }
        if (++w >= bits.size()) {
            return -1;
        }
        word = bits[w];
    }
}

/* ---- OrderBook -------------------------------------------------------- */

OrderBook::OrderBook(const BookConfig &cfg) : cfg_(cfg), ids_(cfg.max_orders) {
    if (cfg.tick_size <= 0 || cfg.max_price < cfg.min_price ||
        (cfg.max_price - cfg.min_price) % cfg.tick_size != 0 || cfg.max_orders == 0 ||
        cfg.max_orders >= kNil) {
        throw std::invalid_argument("OrderBook: invalid BookConfig");
    }
    levels_per_side_ = static_cast<size_t>((cfg.max_price - cfg.min_price) / cfg.tick_size) + 1;
    const size_t words = (levels_per_side_ + 63) / 64;
    for (Ladder *l : {&bids_, &asks_}) {
        l->levels.assign(levels_per_side_, Level{0, kNil, kNil, 0});
        l->bits.assign(words, 0);
        l->best = -1;
    }
    nodes_.resize(cfg.max_orders);
    clear();
}

void OrderBook::clear() {
    for (Ladder *l : {&bids_, &asks_}) {
        /* Only touch levels that are non-empty (bitmap) - clearing 20k
         * levels one by one would be wasteful on a resync. */
        for (int32_t i = bitmap_find_next(l->bits, 0); i >= 0;
             i = bitmap_find_next(l->bits, i + 1)) {
            l->levels[static_cast<size_t>(i)] = Level{0, kNil, kNil, 0};
        }
        for (auto &w : l->bits) {
            w = 0;
        }
        l->best = -1;
    }
    /* Rebuild the free list: node i -> i+1 -> ... -> kNil. */
    for (uint32_t i = 0; i < nodes_.size(); i++) {
        nodes_[i].next = (i + 1 < nodes_.size()) ? i + 1 : kNil;
    }
    free_head_ = nodes_.empty() ? kNil : 0;
    live_      = 0;
    ids_.clear();
}

bool OrderBook::valid_price(Price p) const {
    return p >= cfg_.min_price && p <= cfg_.max_price &&
           (p - cfg_.min_price) % cfg_.tick_size == 0;
}

OrderBook::AddResult OrderBook::add(OrderId id, Side side, Price price, Qty qty,
                                    uint32_t owner, uint32_t user_ref) {
    if (!valid_price(price)) {
        return AddResult::BadPrice;
    }
    if (qty == 0) {
        return AddResult::BadQty;
    }
    if (free_head_ == kNil) {
        return AddResult::Full;
    }
    const uint32_t n = free_head_;
    if (!ids_.insert(id, n)) {
        /* insert fails on a duplicate or id 0; the map is sized like the
         * pool, so it cannot be "full" while a node is free */
        return AddResult::DuplicateId;
    }
    free_head_ = nodes_[n].next;

    BookOrder &o = nodes_[n];
    o.id       = id;
    o.price    = price;
    o.qty      = qty;
    o.owner    = owner;
    o.user_ref = user_ref;
    o.side     = side;
    o.next     = kNil;

    Ladder &l = ladder(side);
    const int32_t idx = index_of(price);
    Level &lv = l.levels[static_cast<size_t>(idx)];
    /* Append at the TAIL: newest order has the lowest time priority. */
    o.prev = lv.tail;
    if (lv.tail != kNil) {
        nodes_[lv.tail].next = n;
    } else {
        lv.head = n;
        l.bits[static_cast<size_t>(idx) >> 6] |= 1ULL << (idx & 63);
    }
    lv.tail = n;
    lv.qty += qty;
    lv.count++;

    /* Keep the cached best level current: higher is better for bids,
     * lower is better for asks. */
    if (l.best < 0 || (side == Side::Buy ? idx > l.best : idx < l.best)) {
        l.best = idx;
    }
    ++live_;
    return AddResult::Ok;
}

int32_t OrderBook::next_worse(Side side, int32_t idx) const {
    const Ladder &l = ladder(side);
    return side == Side::Buy ? bitmap_find_prev(l.bits, idx - 1)
                             : bitmap_find_next(l.bits, idx + 1);
}

void OrderBook::unlink_and_free(uint32_t n) {
    BookOrder &o = nodes_[n];
    Ladder &l = ladder(o.side);
    const int32_t idx = index_of(o.price);
    Level &lv = l.levels[static_cast<size_t>(idx)];

    /* O(1) unlink from the middle of the FIFO - the point of storing
     * prev as well as next. */
    if (o.prev != kNil) {
        nodes_[o.prev].next = o.next;
    } else {
        lv.head = o.next;
    }
    if (o.next != kNil) {
        nodes_[o.next].prev = o.prev;
    } else {
        lv.tail = o.prev;
    }
    lv.qty -= o.qty;
    lv.count--;

    if (lv.count == 0) {
        lv.head = lv.tail = kNil;
        lv.qty  = 0;
        l.bits[static_cast<size_t>(idx) >> 6] &= ~(1ULL << (idx & 63));
        if (idx == l.best) {
            l.best = next_worse(o.side, idx); /* the bitmap scan */
        }
    }

    ids_.erase(o.id);
    o.next     = free_head_;
    free_head_ = n;
    --live_;
}

bool OrderBook::reduce(OrderId id, Qty by) {
    const uint32_t *n = ids_.find(id);
    if (n == nullptr) {
        return false;
    }
    BookOrder &o = nodes_[*n];
    if (by > o.qty) {
        return false;
    }
    if (by == o.qty) {
        unlink_and_free(*n);
        return true;
    }
    o.qty -= by;
    ladder(o.side).levels[static_cast<size_t>(index_of(o.price))].qty -= by;
    return true;
}

bool OrderBook::remove(OrderId id) {
    const uint32_t *n = ids_.find(id);
    if (n == nullptr) {
        return false;
    }
    unlink_and_free(*n);
    return true;
}

const BookOrder *OrderBook::front(Side side) const {
    const Ladder &l = ladder(side);
    if (l.best < 0) {
        return nullptr;
    }
    return &nodes_[l.levels[static_cast<size_t>(l.best)].head];
}

void OrderBook::reduce_front(Side side, Qty by) {
    Ladder &l = ladder(side);
    Level &lv = l.levels[static_cast<size_t>(l.best)];
    const uint32_t n = lv.head;
    BookOrder &o = nodes_[n];
    if (by >= o.qty) {
        unlink_and_free(n);
    } else {
        o.qty -= by;
        lv.qty -= by;
    }
}

const BookOrder *OrderBook::find(OrderId id) const {
    const uint32_t *n = ids_.find(id);
    return n == nullptr ? nullptr : &nodes_[*n];
}

uint64_t OrderBook::best_qty(Side side) const {
    const Ladder &l = ladder(side);
    return l.best < 0 ? 0 : l.levels[static_cast<size_t>(l.best)].qty;
}

uint32_t OrderBook::best_count(Side side) const {
    const Ladder &l = ladder(side);
    return l.best < 0 ? 0 : l.levels[static_cast<size_t>(l.best)].count;
}

LevelView OrderBook::level_at(Side side, Price price) const {
    LevelView v{price, 0, 0};
    if (!valid_price(price)) {
        return v;
    }
    const Level &lv = ladder(side).levels[static_cast<size_t>(index_of(price))];
    v.qty    = lv.qty;
    v.orders = lv.count;
    return v;
}
