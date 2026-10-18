/*
 * flat_id_map.hpp - open-addressing hash map from a 64-bit id to a value.
 *
 * Responsibility: "find order 123456789" happens on every cancel,
 * execute and delete - it is the hottest lookup in an order book. This
 * is the map the book uses for it.
 *
 * Why not std::unordered_map?
 *   - it is node-based: every insert calls the allocator (malloc on the
 *     hot path = unbounded latency), and every lookup chases a pointer
 *     from the bucket array to a separately allocated node = a likely
 *     cache miss per lookup.
 *   - it rehashes (allocates a bigger table and moves everything) when
 *     it grows - a multi-millisecond stall at an unpredictable moment.
 *   This map is ONE flat array of {key, value} slots, sized once in the
 *   constructor. No allocation after construction, ever; a lookup is a
 *   hash, then a short linear walk over adjacent memory.
 *
 * Design choices (each is an interview question):
 *   - Open addressing + linear probing: on a collision, try the next
 *     slot. Neighbouring slots share cache lines, so a probe sequence of
 *     length 3 usually costs one cache miss, not three.
 *   - Power-of-two capacity + Fibonacci hashing: slot =
 *     (key * 2^64/phi) >> (64 - log2(capacity)). Multiplying by the
 *     golden-ratio constant scrambles the bits so SEQUENTIAL ids (which
 *     exchanges hand out: 1, 2, 3, ...) spread evenly, and the shift
 *     keeps the well-mixed high bits. No modulo (a divide is ~25-40
 *     cycles; a shift is 1).
 *   - Load factor <= 50%: capacity is the next power of two >= 2 x
 *     max_entries. Linear probing degrades sharply above ~70% full
 *     (clusters merge); at 50% the expected successful probe length is
 *     ~1.5. Memory is traded for predictable latency.
 *   - Backward-shift deletion instead of tombstones: when a key is
 *     erased, later keys in its cluster are slid back into the hole.
 *     Tombstones ("deleted" markers) are simpler but accumulate under
 *     the add/cancel churn of an order book, making every probe longer
 *     until you rebuild the table - which is a stall.
 *   - Key 0 is the "empty slot" sentinel, so 0 cannot be used as a key.
 *     Exchange order ids start at 1, so this costs nothing.
 *   - Fixed capacity: insert() returns false when full instead of
 *     growing. Size it for the worst case up front (the order book
 *     sizes it from BookConfig::max_orders).
 *
 * Thread-safety: none (single-owner, like the book that holds it).
 */

#ifndef PULSEFORGE_FLAT_ID_MAP_HPP
#define PULSEFORGE_FLAT_ID_MAP_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

template <typename V>
class FlatIdMap {
public:
    static constexpr uint64_t kEmpty = 0;

    explicit FlatIdMap(size_t max_entries) : max_entries_(max_entries) {
        size_t cap = 16;
        unsigned bits = 4;
        while (cap < max_entries * 2) {
            cap <<= 1;
            ++bits;
        }
        slots_.assign(cap, Slot{kEmpty, V{}});
        mask_  = cap - 1;
        shift_ = 64 - bits;
    }

    /* false if key == 0, the key already exists, or the map is full. */
    bool insert(uint64_t key, const V &value) {
        if (key == kEmpty || size_ >= max_entries_) {
            return false;
        }
        size_t i = home(key);
        while (slots_[i].key != kEmpty) {
            if (slots_[i].key == key) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        slots_[i].key = key;
        slots_[i].val = value;
        ++size_;
        return true;
    }

    V *find(uint64_t key) {
        if (key == kEmpty) {
            return nullptr;
        }
        size_t i = home(key);
        while (slots_[i].key != kEmpty) {
            if (slots_[i].key == key) {
                return &slots_[i].val;
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    const V *find(uint64_t key) const {
        return const_cast<FlatIdMap *>(this)->find(key);
    }

    bool erase(uint64_t key) {
        if (key == kEmpty) {
            return false;
        }
        size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmpty) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        /*
         * Backward shift. `i` is now a hole. Walk forward through the rest
         * of the cluster; any entry whose home slot is at or "before" the
         * hole (cyclically) would become unreachable if we left the hole
         * there, so move it into the hole - which opens a new hole where
         * it was. Stop at the first empty slot: the cluster ends there.
         * "dist(a, b)" = how far b is past a, wrapping around the table.
         */
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmpty) {
                break;
            }
            size_t h = home(slots_[j].key);
            if (((j - h) & mask_) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = kEmpty;
        --size_;
        return true;
    }

    void clear() {
        for (auto &s : slots_) {
            s.key = kEmpty;
        }
        size_ = 0;
    }

    /* Visit every (key, value). Do not insert/erase inside f - collect
     * the keys first and modify afterwards. */
    template <typename F>
    void for_each(F &&f) const {
        for (const auto &s : slots_) {
            if (s.key != kEmpty) {
                f(s.key, s.val);
            }
        }
    }

    size_t size() const { return size_; }
    size_t max_entries() const { return max_entries_; }
    size_t slot_count() const { return slots_.size(); }

private:
    /* AoS layout {key, value}: a hit reads the key and the value from
     * the same cache line. (SoA - separate key and value arrays - packs
     * more keys per line and wins when misses dominate; here hits do.) */
    struct Slot {
        uint64_t key;
        V        val;
    };

    size_t home(uint64_t key) const {
        return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    std::vector<Slot> slots_;
    size_t            mask_        = 0;
    unsigned          shift_       = 0;
    size_t            size_        = 0;
    size_t            max_entries_ = 0;
};

#endif /* PULSEFORGE_FLAT_ID_MAP_HPP */
