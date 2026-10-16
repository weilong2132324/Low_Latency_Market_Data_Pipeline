/*
 * latency_histogram.hpp - log-scale latency histogram with percentile
 * estimation, in the same "plain atomic struct" style as stats.hpp (so
 * it can later live in shared memory, mmap-compatible, no constructor
 * to run on someone else's mapping).
 *
 * Responsibility: Phase 1's SharedStats only ever tracked avg/max
 * latency - useless for tail-latency work, which is what actually
 * matters in a market-data pipeline (p99.9 is the number that pages
 * someone). This gives every bench/worker a cheap way to record a full
 * distribution and estimate p50/p99/p99.9 from it.
 *
 * constexpr / compile-time computation: the bucket boundaries are a
 * geometric sequence (128ns, 256ns, 512ns, ... ~16ms) computed ONCE at
 * COMPILE time by kLatencyBucketBounds below, not recomputed with
 * pow()/shifts at every record() call or at process startup. This is
 * the concrete "runtime work moved to compile time" optimization from
 * the SRE prep list - the values are simply baked into the binary's
 * rodata.
 *
 * Thread-safety: every bucket is std::atomic<uint64_t> with relaxed
 * fetch_add - safe for many threads to record() concurrently into one
 * histogram, at the cost of cache-line contention on whichever bucket
 * is hot. For the lowest-contention option, give each worker thread its
 * own LatencyHistogram and merge_into() them into one for reporting
 * (see bench/queue_bench.cpp).
 *
 * Accuracy: this is a *lossy* approximation, same honesty as
 * QueueMutex::depth() being racy by design - percentile estimates
 * return a bucket boundary, not the exact sample, with the usual
 * log-histogram error bound (within one bucket width of the true
 * value, i.e. roughly +/-50% of the bucket's own size at that scale).
 */

#ifndef PULSEFORGE_LATENCY_HISTOGRAM_HPP
#define PULSEFORGE_LATENCY_HISTOGRAM_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

constexpr size_t   kLatencyHistogramBuckets = 48;
constexpr uint64_t kLatencyHistogramMinNs   = 128;

/* bounds[0] == 0; bounds[i] for i >= 1 is 128 * 2^(i-1) ns. Anything
 * >= bounds[kLatencyHistogramBuckets] falls into the overflow counter. */
constexpr std::array<uint64_t, kLatencyHistogramBuckets + 1>
make_latency_bucket_bounds() {
    std::array<uint64_t, kLatencyHistogramBuckets + 1> b{};
    b[0] = 0;
    uint64_t v = kLatencyHistogramMinNs;
    for (size_t i = 1; i <= kLatencyHistogramBuckets; i++) {
        b[i] = v;
        v *= 2;
    }
    return b;
}

constexpr auto kLatencyBucketBounds = make_latency_bucket_bounds();

struct LatencyHistogram {
    std::atomic<uint64_t> buckets[kLatencyHistogramBuckets];
    std::atomic<uint64_t> overflow; /* samples >= the top bucket bound */
    std::atomic<uint64_t> count;    /* total samples recorded          */
    std::atomic<uint64_t> max_ns;
};

inline void latency_histogram_reset(LatencyHistogram &h) {
    using std::memory_order_relaxed;
    for (auto &b : h.buckets) {
        b.store(0, memory_order_relaxed);
    }
    h.overflow.store(0, memory_order_relaxed);
    h.count.store(0, memory_order_relaxed);
    h.max_ns.store(0, memory_order_relaxed);
}

/* Linear scan over 48 buckets - a binary search would be O(log 48)
 * instead of O(48), but 48 atomic-free comparisons on a local array is
 * already a handful of nanoseconds; not worth the extra code for this
 * bucket count. Flagged here precisely because "is this the hot path
 * and does it matter" is the judgment call the SRE role requires. */
inline size_t latency_bucket_index(uint64_t latency_ns) {
    for (size_t i = 0; i < kLatencyHistogramBuckets; i++) {
        if (latency_ns < kLatencyBucketBounds[i + 1]) {
            return i;
        }
    }
    return kLatencyHistogramBuckets; /* overflow */
}

inline void latency_histogram_record(LatencyHistogram &h, uint64_t latency_ns) {
    using std::memory_order_relaxed;
    size_t idx = latency_bucket_index(latency_ns);
    if (idx == kLatencyHistogramBuckets) {
        h.overflow.fetch_add(1, memory_order_relaxed);
    } else {
        h.buckets[idx].fetch_add(1, memory_order_relaxed);
    }
    h.count.fetch_add(1, memory_order_relaxed);

    uint64_t cur = h.max_ns.load(memory_order_relaxed);
    while (latency_ns > cur &&
           !h.max_ns.compare_exchange_weak(cur, latency_ns, memory_order_relaxed,
                                            memory_order_relaxed)) {
        /* cur refreshed by the failed CAS; retry */
    }
}

/* Adds `from`'s counts into `into` (e.g. merging N per-worker
 * histograms before reporting). Not atomic as a whole operation - call
 * this only once all contributing threads are done recording. */
inline void latency_histogram_merge(LatencyHistogram &into,
                                     const LatencyHistogram &from) {
    using std::memory_order_relaxed;
    for (size_t i = 0; i < kLatencyHistogramBuckets; i++) {
        into.buckets[i].fetch_add(from.buckets[i].load(memory_order_relaxed),
                                   memory_order_relaxed);
    }
    into.overflow.fetch_add(from.overflow.load(memory_order_relaxed),
                             memory_order_relaxed);
    into.count.fetch_add(from.count.load(memory_order_relaxed),
                          memory_order_relaxed);
    uint64_t from_max = from.max_ns.load(memory_order_relaxed);
    uint64_t cur = into.max_ns.load(memory_order_relaxed);
    while (from_max > cur &&
           !into.max_ns.compare_exchange_weak(cur, from_max, memory_order_relaxed,
                                               memory_order_relaxed)) {
    }
}

struct LatencyPercentiles {
    uint64_t p50;
    uint64_t p99;
    uint64_t p999;
    uint64_t max_ns;
    uint64_t count;
};

/* Estimates percentiles by walking cumulative counts bucket-by-bucket
 * and reporting the bucket's upper bound once the running total crosses
 * each target fraction - the standard log-histogram percentile
 * approximation. */
inline LatencyPercentiles latency_histogram_percentiles(const LatencyHistogram &h) {
    using std::memory_order_relaxed;
    LatencyPercentiles out{};
    out.max_ns = h.max_ns.load(memory_order_relaxed);
    uint64_t total = h.count.load(memory_order_relaxed);
    out.count = total;
    if (total == 0) {
        return out;
    }

    const uint64_t p50_target  = (total * 50 + 99) / 100;
    const uint64_t p99_target  = (total * 99 + 99) / 100;
    const uint64_t p999_target = (total * 999 + 999) / 1000;

    uint64_t running = 0;
    bool have50 = false, have99 = false, have999 = false;
    for (size_t i = 0; i < kLatencyHistogramBuckets; i++) {
        running += h.buckets[i].load(memory_order_relaxed);
        uint64_t bound = kLatencyBucketBounds[i + 1];
        if (!have50 && running >= p50_target) {
            out.p50 = bound;
            have50  = true;
        }
        if (!have99 && running >= p99_target) {
            out.p99 = bound;
            have99  = true;
        }
        if (!have999 && running >= p999_target) {
            out.p999 = bound;
            have999  = true;
        }
    }
    /* Anything still unmet falls in the overflow bucket; report the
     * true max as the best available estimate for it. */
    if (!have50) {
        out.p50 = out.max_ns;
    }
    if (!have99) {
        out.p99 = out.max_ns;
    }
    if (!have999) {
        out.p999 = out.max_ns;
    }
    return out;
}

#endif /* PULSEFORGE_LATENCY_HISTOGRAM_HPP */
