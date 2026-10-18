/*
 * test_flat_id_map.cpp - FlatIdMap against std::unordered_map as an
 * oracle. Backward-shift deletion is the subtle part: a wrong shift
 * condition makes keys silently unreachable, which only shows up under
 * heavy insert/erase churn in a nearly full, wrapped-around table - so
 * that is what the randomized test generates.
 */

#include "flat_id_map.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <unordered_map>
#include <vector>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void test_basics() {
    FlatIdMap<int> m(100);
    EXPECT(m.slot_count() == 256); /* next pow2 >= 2 x 100 */
    EXPECT(m.insert(5, 50));
    EXPECT(!m.insert(5, 51)); /* duplicate */
    EXPECT(!m.insert(0, 1));  /* 0 is the empty sentinel */
    EXPECT(m.find(5) != nullptr && *m.find(5) == 50);
    EXPECT(m.find(6) == nullptr);
    *m.find(5) = 55;
    EXPECT(*m.find(5) == 55);
    EXPECT(m.erase(5));
    EXPECT(!m.erase(5));
    EXPECT(m.find(5) == nullptr && m.size() == 0);
}

static void test_capacity_limit() {
    FlatIdMap<int> m(4);
    for (uint64_t k = 1; k <= 4; k++) {
        EXPECT(m.insert(k, static_cast<int>(k)));
    }
    EXPECT(!m.insert(99, 1)); /* full: refuses instead of rehashing */
    EXPECT(m.erase(2));
    EXPECT(m.insert(99, 1));
}

static void test_for_each_and_clear() {
    FlatIdMap<int> m(16);
    uint64_t sum = 0;
    for (uint64_t k = 1; k <= 10; k++) {
        m.insert(k, 1);
    }
    m.for_each([&](uint64_t k, int v) { sum += k * static_cast<uint64_t>(v); });
    EXPECT(sum == 55);
    m.clear();
    EXPECT(m.size() == 0 && m.find(3) == nullptr);
}

static void test_randomized_against_oracle(size_t max_entries, uint64_t key_space, int ops,
                                           unsigned seed) {
    FlatIdMap<uint64_t> m(max_entries);
    std::unordered_map<uint64_t, uint64_t> ref;
    std::mt19937_64 rng(seed);
    for (int i = 0; i < ops; i++) {
        const uint64_t k = 1 + rng() % key_space;
        const unsigned op = static_cast<unsigned>(rng() % 3);
        if (op == 0) {
            const bool expect_ok = ref.count(k) == 0 && ref.size() < max_entries;
            const bool ok = m.insert(k, k * 7);
            EXPECT(ok == expect_ok);
            if (ok) {
                ref[k] = k * 7;
            }
        } else if (op == 1) {
            EXPECT(m.erase(k) == (ref.erase(k) == 1));
        } else {
            const uint64_t *v = m.find(k);
            auto it = ref.find(k);
            EXPECT((v == nullptr) == (it == ref.end()));
            if (v && it != ref.end()) {
                EXPECT(*v == it->second);
            }
        }
        if (i % 997 == 0) {
            EXPECT(m.size() == ref.size());
            for (const auto &kv : ref) {
                const uint64_t *v = m.find(kv.first);
                EXPECT(v != nullptr && *v == kv.second);
            }
        }
    }
}

int main() {
    test_basics();
    test_capacity_limit();
    test_for_each_and_clear();
    /* tiny table, keys that wrap around it, constantly full: worst case for backward shift */
    test_randomized_against_oracle(8, 40, 200000, 1);
    test_randomized_against_oracle(64, 200, 200000, 2);
    /* order-book-like: sequential-ish ids, big table */
    test_randomized_against_oracle(10000, 30000, 300000, 3);

    if (g_failures == 0) {
        std::printf("test_flat_id_map: ALL PASSED\n");
        return 0;
    }
    std::printf("test_flat_id_map: %d FAILURE(S)\n", g_failures);
    return 1;
}
