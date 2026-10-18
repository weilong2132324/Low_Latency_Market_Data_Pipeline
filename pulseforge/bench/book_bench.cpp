/*
 * book_bench.cpp - why the order book is built the way it is.
 *
 * Race 1: the same deterministic stream of 1M book operations (add,
 * cancel by id, execute at best, read top of book) through
 *   (a) MapBook    - the textbook design: std::map<Price, std::list<Order>>
 *                    per side + std::unordered_map<OrderId, iterator>
 *   (b) OrderBook  - order_book.hpp: price ladder array + occupancy
 *                    bitmap + pooled intrusive FIFO + FlatIdMap
 *
 * Race 2: FlatIdMap vs std::unordered_map for the id index alone.
 *
 * Per-op latency is measured with now_ns() around each operation, so
 * every sample includes the cost of one clock read (printed as "clock
 * overhead"); the mean ns/op comes from timing the whole run, no
 * per-op clock reads. Look at the TAILS (p99, p99.9) as much as the
 * mean: the allocator and cache misses show up there.
 *
 * Run: make bench-book
 */

#include "flat_id_map.hpp"
#include "latency_sampler.hpp"
#include "order_book.hpp"
#include "time_util.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

template <class T>
inline void do_not_optimize(const T &v) {
    __asm__ __volatile__("" : : "g"(&v) : "memory");
}

enum class OpType : uint8_t { Add, Cancel, Execute, Top };

struct Op {
    OpType  type;
    Side    side;
    OrderId id;
    Price   price;
    Qty     qty;
};

std::vector<Op> make_ops(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<OrderId> live;
    OrderId next = 1;
    Price mid = 100 * kPriceScale;
    for (size_t i = 0; i < n; i++) {
        if (rng() % 100 < 3) {
            mid += (rng() & 1) ? kCent : -kCent;
        }
        const unsigned r = static_cast<unsigned>(rng() % 100);
        Op op{};
        op.side = (rng() & 1) ? Side::Buy : Side::Sell;
        if ((r < 50 && live.size() < 50000) || live.size() < 100) {
            op.type  = OpType::Add;
            op.id    = next++;
            Price k  = static_cast<Price>(1 + rng() % 20);
            op.price = op.side == Side::Buy ? mid - k * kCent : mid + k * kCent;
            op.qty   = static_cast<Qty>(100 * (1 + rng() % 5));
            live.push_back(op.id);
        } else if (r < 80) {
            op.type = OpType::Cancel;
            size_t k = static_cast<size_t>(rng() % live.size());
            op.id = live[k];
            live[k] = live.back();
            live.pop_back();
        } else if (r < 92) {
            op.type = OpType::Execute;
            op.qty  = static_cast<Qty>(100 * (1 + rng() % 3));
        } else {
            op.type = OpType::Top;
        }
        ops.push_back(op);
    }
    return ops;
}

/* The textbook book. Correct and readable; every add allocates a list
 * node, a map node (for a new price) and a hash node. */
class MapBook {
public:
    struct Order {
        OrderId id;
        Qty     qty;
    };
    using Level = std::list<Order>;

    void add(OrderId id, Side s, Price p, Qty q) {
        if (s == Side::Buy) {
            Level &l = bids_[p];
            l.push_back({id, q});
            index_[id] = Loc{s, p, std::prev(l.end())};
        } else {
            Level &l = asks_[p];
            l.push_back({id, q});
            index_[id] = Loc{s, p, std::prev(l.end())};
        }
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) {
            return false;
        }
        const Loc loc = it->second;
        index_.erase(it);
        if (loc.side == Side::Buy) {
            erase_from(bids_, loc);
        } else {
            erase_from(asks_, loc);
        }
        return true;
    }

    void execute_best(Side s, Qty q) {
        if (s == Side::Buy) {
            execute_in(bids_, q);
        } else {
            execute_in(asks_, q);
        }
    }

    Price best(Side s) const {
        if (s == Side::Buy) {
            return bids_.empty() ? 0 : bids_.begin()->first;
        }
        return asks_.empty() ? 0 : asks_.begin()->first;
    }

private:
    struct Loc {
        Side            side;
        Price           price;
        Level::iterator it;
    };

    template <class M>
    void erase_from(M &m, const Loc &loc) {
        auto lvl = m.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) {
            m.erase(lvl);
        }
    }

    template <class M>
    void execute_in(M &m, Qty q) {
        while (q > 0 && !m.empty()) {
            auto lvl = m.begin();
            Order &o = lvl->second.front();
            Qty f = q < o.qty ? q : o.qty;
            o.qty -= f;
            q -= f;
            if (o.qty == 0) {
                index_.erase(o.id);
                lvl->second.pop_front();
                if (lvl->second.empty()) {
                    m.erase(lvl);
                }
            }
        }
    }

    std::map<Price, Level, std::greater<Price>> bids_; /* highest first */
    std::map<Price, Level>                      asks_; /* lowest first  */
    std::unordered_map<OrderId, Loc>            index_;
};

/* Adapter giving OrderBook the same 4-call interface. */
class LadderBook {
public:
    LadderBook() : b_(cfg()) {}
    static BookConfig cfg() {
        BookConfig c;
        c.max_orders = 1u << 17;
        return c;
    }
    void add(OrderId id, Side s, Price p, Qty q) { (void)b_.add(id, s, p, q); }
    bool cancel(OrderId id) { return b_.remove(id); }
    void execute_best(Side s, Qty q) {
        while (q > 0) {
            const BookOrder *f = b_.front(s);
            if (f == nullptr) {
                return;
            }
            Qty x = q < f->qty ? q : f->qty;
            b_.reduce_front(s, x);
            q -= x;
        }
    }
    Price best(Side s) const { return b_.empty(s) ? 0 : b_.best_price(s); }

private:
    OrderBook b_;
};

template <class Book>
void apply(Book &b, const Op &op) {
    switch (op.type) {
    case OpType::Add:
        b.add(op.id, op.side, op.price, op.qty);
        break;
    case OpType::Cancel:
        do_not_optimize(b.cancel(op.id));
        break;
    case OpType::Execute:
        b.execute_best(op.side, op.qty);
        break;
    case OpType::Top: {
        Price p = b.best(op.side);
        do_not_optimize(p);
        break;
    }
    }
}

struct Result {
    double                  mean_ns;
    LatencySampler::Summary add, cancel, execute, top;
};

template <class Book>
Result run_book(const std::vector<Op> &ops) {
    Result r{};
    {   /* warm-up pass: page in code and data, train branch predictors */
        auto b = std::make_unique<Book>();
        for (const Op &op : ops) apply(*b, op);
    }
    {   /* throughput pass: no per-op clock reads */
        auto b = std::make_unique<Book>();
        uint64_t t0 = now_ns();
        for (const Op &op : ops) apply(*b, op);
        r.mean_ns = static_cast<double>(now_ns() - t0) / static_cast<double>(ops.size());
    }
    {   /* latency pass: one clock read pair per op */
        auto b = std::make_unique<Book>();
        LatencySampler s_add(ops.size()), s_cancel(ops.size()), s_exec(ops.size()), s_top(ops.size());
        for (const Op &op : ops) {
            uint64_t t0 = now_ns();
            apply(*b, op);
            uint64_t dt = now_ns() - t0;
            switch (op.type) {
            case OpType::Add:     s_add.record(dt); break;
            case OpType::Cancel:  s_cancel.record(dt); break;
            case OpType::Execute: s_exec.record(dt); break;
            case OpType::Top:     s_top.record(dt); break;
            }
        }
        r.add     = s_add.summarize();
        r.cancel  = s_cancel.summarize();
        r.execute = s_exec.summarize();
        r.top     = s_top.summarize();
    }
    return r;
}

void print_row(const char *impl, const char *op, const LatencySampler::Summary &s) {
    std::printf("  %-10s %-8s %8zu %7" PRIu64 " %7" PRIu64 " %8" PRIu64 " %9" PRIu64 "\n", impl, op,
                s.count, s.p50, s.p99, s.p999, s.max);
}

void id_map_race() {
    constexpr size_t N = 200000;
    std::mt19937_64 rng(5);
    std::vector<uint64_t> keys(N);
    for (size_t i = 0; i < N; i++) {
        keys[i] = i + 1; /* exchange-style sequential ids */
    }
    std::vector<uint64_t> probe(keys);
    std::shuffle(probe.begin(), probe.end(), rng);

    auto time_it = [](auto &&fn) {
        uint64_t t0 = now_ns();
        fn();
        return static_cast<double>(now_ns() - t0);
    };

    FlatIdMap<uint32_t> fm(N);
    std::unordered_map<uint64_t, uint32_t> um;
    um.reserve(N); /* even pre-reserved, every insert allocates a node */

    double fi = time_it([&] { for (uint64_t k : keys) fm.insert(k, static_cast<uint32_t>(k)); });
    double ui = time_it([&] { for (uint64_t k : keys) um.emplace(k, static_cast<uint32_t>(k)); });
    uint64_t sink = 0;
    double ff = time_it([&] { for (uint64_t k : probe) sink += *fm.find(k); });
    double uf = time_it([&] { for (uint64_t k : probe) sink += um.find(k)->second; });
    double fe = time_it([&] { for (uint64_t k : probe) fm.erase(k); });
    double ue = time_it([&] { for (uint64_t k : probe) um.erase(k); });
    do_not_optimize(sink);

    const double n = static_cast<double>(N);
    std::printf("\nid index (%zu sequential ids, random lookup order), mean ns/op\n", N);
    std::printf("  %-22s %8s %8s %8s\n", "", "insert", "find", "erase");
    std::printf("  %-22s %8.1f %8.1f %8.1f\n", "FlatIdMap", fi / n, ff / n, fe / n);
    std::printf("  %-22s %8.1f %8.1f %8.1f\n", "std::unordered_map", ui / n, uf / n, ue / n);
}

} // namespace

int main() {
    constexpr size_t kOps = 1000000;
    std::vector<Op> ops = make_ops(kOps, 2026);

    uint64_t overhead = 0;
    {
        LatencySampler s(100000);
        for (int i = 0; i < 100000; i++) {
            uint64_t t0 = now_ns();
            s.record(now_ns() - t0);
        }
        overhead = s.summarize().p50;
    }

    std::printf("PulseForge book_bench: %zu operations (50%% add, 30%% cancel, 12%% execute, 8%% top)\n",
                kOps);
    std::printf("clock overhead per sample ~%" PRIu64 " ns (included in every latency below)\n\n",
                overhead);

    Result m = run_book<MapBook>(ops);
    Result l = run_book<LadderBook>(ops);

    std::printf("throughput, mean ns/op:  MapBook %.1f   OrderBook %.1f   (%.1fx)\n\n", m.mean_ns,
                l.mean_ns, m.mean_ns / l.mean_ns);
    std::printf("  %-10s %-8s %8s %7s %7s %8s %9s\n", "impl", "op", "count", "p50", "p99", "p99.9",
                "max");
    print_row("MapBook", "add", m.add);
    print_row("OrderBook", "add", l.add);
    print_row("MapBook", "cancel", m.cancel);
    print_row("OrderBook", "cancel", l.cancel);
    print_row("MapBook", "execute", m.execute);
    print_row("OrderBook", "execute", l.execute);
    print_row("MapBook", "top", m.top);
    print_row("OrderBook", "top", l.top);

    id_map_race();
    return 0;
}
