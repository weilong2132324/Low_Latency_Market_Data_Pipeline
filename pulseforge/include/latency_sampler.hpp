/*
 * latency_sampler.hpp - exact latency percentiles from raw samples.
 *
 * Responsibility: latency_histogram.hpp buckets samples into powers of
 * two - cheap and shareable across threads, but p50 can only ever read
 * "2048 ns" or "4096 ns". For the tick-to-trade report we want exact
 * numbers, so this keeps every raw sample and sorts them at the end.
 *
 * Zero allocation on the hot path: the constructor reserve()s the full
 * capacity. std::vector guarantees push_back does NOT reallocate while
 * size() < capacity(), so record() is a bounds check + a store. When it
 * is full, new samples are counted as dropped rather than growing the
 * vector (growth = allocation + a copy of everything = a latency spike
 * in the very code being measured).
 *
 * summarize() copies and sorts: O(n log n), cold path only (end of run
 * or a periodic report from a non-critical moment).
 *
 * Thread-safety: none; one sampler per thread.
 */

#ifndef PULSEFORGE_LATENCY_SAMPLER_HPP
#define PULSEFORGE_LATENCY_SAMPLER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

class LatencySampler {
public:
    struct Summary {
        size_t   count   = 0;
        size_t   dropped = 0;
        uint64_t min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
        double   mean = 0.0;
    };

    /* resize() then clear(): reserve() alone only reserves VIRTUAL
     * memory - the first write to each 4 KiB page would still take a
     * page fault (~1 us) in the middle of the hot path. Writing every
     * element once here faults all pages in up front; clear() keeps the
     * capacity (and the now-resident pages). */
    explicit LatencySampler(size_t capacity = 1u << 20) {
        samples_.resize(capacity);
        samples_.clear();
    }

    void record(uint64_t ns) {
        if (samples_.size() < samples_.capacity()) {
            samples_.push_back(ns);
        } else {
            ++dropped_;
        }
    }

    size_t count() const { return samples_.size(); }

    Summary summarize() const {
        Summary s;
        s.count   = samples_.size();
        s.dropped = dropped_;
        if (samples_.empty()) {
            return s;
        }
        std::vector<uint64_t> v(samples_);
        std::sort(v.begin(), v.end());
        auto pct = [&v](double p) {
            size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
            return v[idx];
        };
        s.min  = v.front();
        s.max  = v.back();
        s.p50  = pct(0.50);
        s.p90  = pct(0.90);
        s.p99  = pct(0.99);
        s.p999 = pct(0.999);
        long double sum = 0;
        for (uint64_t x : v) {
            sum += x;
        }
        s.mean = static_cast<double>(sum / static_cast<long double>(v.size()));
        return s;
    }

    void clear() {
        samples_.clear(); /* keeps capacity */
        dropped_ = 0;
    }

private:
    std::vector<uint64_t> samples_;
    size_t                dropped_ = 0;
};

#endif /* PULSEFORGE_LATENCY_SAMPLER_HPP */
