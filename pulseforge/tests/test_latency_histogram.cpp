/*
 * test_latency_histogram.cpp - unit tests for the log-scale latency
 * histogram and its percentile estimation.
 */

#include "latency_histogram.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_bucket_bounds_are_geometric() {
    CHECK(kLatencyBucketBounds[0] == 0);
    CHECK(kLatencyBucketBounds[1] == kLatencyHistogramMinNs);
    CHECK(kLatencyBucketBounds[2] == kLatencyHistogramMinNs * 2);
    CHECK(kLatencyBucketBounds[3] == kLatencyHistogramMinNs * 4);
}

static void test_reset_is_all_zero() {
    LatencyHistogram h;
    latency_histogram_reset(h);
    auto p = latency_histogram_percentiles(h);
    CHECK(p.count == 0);
    CHECK(p.max_ns == 0);
}

static void test_single_sample() {
    LatencyHistogram h;
    latency_histogram_reset(h);
    latency_histogram_record(h, 1000);

    auto p = latency_histogram_percentiles(h);
    CHECK(p.count == 1);
    CHECK(p.max_ns == 1000);
    /* with one sample, every percentile falls in the same bucket */
    CHECK(p.p50 == p.p99);
    CHECK(p.p99 == p.p999);
}

static void test_percentiles_track_distribution() {
    LatencyHistogram h;
    latency_histogram_reset(h);

    /* 990 fast samples (~1us) + 10 slow outliers (~10ms) - p50/p99
     * should land in the fast bucket, p999 should catch an outlier. */
    for (int i = 0; i < 990; i++) {
        latency_histogram_record(h, 1000);
    }
    for (int i = 0; i < 10; i++) {
        latency_histogram_record(h, 10'000'000);
    }

    auto p = latency_histogram_percentiles(h);
    CHECK(p.count == 1000);
    CHECK(p.max_ns == 10'000'000);
    CHECK(p.p50 < 2000);          /* firmly in the fast cluster */
    CHECK(p.p99 < 2000);          /* 99th percentile still fast: 10/1000 = 1% outliers */
    CHECK(p.p999 >= 2000);        /* p99.9 must see an outlier */
}

static void test_overflow_bucket() {
    LatencyHistogram h;
    latency_histogram_reset(h);
    uint64_t huge = kLatencyBucketBounds[kLatencyHistogramBuckets] + 1;
    latency_histogram_record(h, huge);
    CHECK(h.overflow.load() == 1);
    auto p = latency_histogram_percentiles(h);
    CHECK(p.max_ns == huge);
    CHECK(p.p999 == huge); /* unmet target falls back to true max */
}

static void test_merge_combines_counts() {
    LatencyHistogram a, b;
    latency_histogram_reset(a);
    latency_histogram_reset(b);
    latency_histogram_record(a, 500);
    latency_histogram_record(b, 5000);

    latency_histogram_merge(a, b);
    auto p = latency_histogram_percentiles(a);
    CHECK(p.count == 2);
    CHECK(p.max_ns == 5000);
}

int main() {
    test_bucket_bounds_are_geometric();
    test_reset_is_all_zero();
    test_single_sample();
    test_percentiles_track_distribution();
    test_overflow_bucket();
    test_merge_combines_counts();

    if (g_failures == 0) {
        std::printf("test_latency_histogram: ALL PASSED\n");
        return 0;
    }
    std::printf("test_latency_histogram: %d FAILURE(S)\n", g_failures);
    return 1;
}
