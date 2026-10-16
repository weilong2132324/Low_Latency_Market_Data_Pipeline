/*
 * queue_bench.cpp - latency comparison across queue backends.
 *
 * Responsibility: drive the SAME synthetic workload (one producer
 * thread pushing kCount messages back-to-back, --workers consumer
 * thread(s) draining them) through each QueueKind via the
 * make_queue_backend() factory, recording end-to-end latency
 * (now_ns() - send_timestamp_ns) into a LatencyHistogram per backend,
 * and printing p50/p99/p99.9/max side by side.
 *
 * This is the "latency reduction" evidence for the mutex -> spsc /
 * disruptor story: the visualizer's comparison panel (Phase 4) replays
 * this program's output.
 *
 * Not a rigorous microbenchmark (no CPU pinning here, no isolation from
 * the rest of the system, JIT-free C++ so nothing to warm up but the
 * cache/branch predictor) - good enough to see the shape of the
 * difference, not to publish a number.
 */

#include "latency_histogram.hpp"
#include "message.hpp"
#include "queue_backend.hpp"
#include "time_util.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchConfig {
    uint64_t count   = 200000;
    int      workers = 1;
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [--count N] [--workers N]\n"
        "  Runs mutex, spsc (workers forced to 1), and disruptor back to\n"
        "  back with the same message count and prints latency percentiles.\n",
        prog);
}

int parse_args(int argc, char **argv, BenchConfig &cfg) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--count" && i + 1 < argc) {
            cfg.count = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--workers" && i + 1 < argc) {
            cfg.workers = std::atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    if (cfg.count == 0 || cfg.workers <= 0) {
        usage(argv[0]);
        return -1;
    }
    return 0;
}

TickMessage make_msg(uint64_t seq) {
    TickMessage m{};
    m.sequence          = seq;
    m.send_timestamp_ns = now_ns();
    m.symbol_id         = 1;
    m.price             = 10000;
    m.quantity          = 1;
    m.flags             = TICK_FLAG_TRADE;
    return m;
}

/* Runs one backend's workload, merging the result into `merged` (an
 * out-param, not a return value: LatencyHistogram holds std::atomic
 * members, which are neither copyable nor movable). */
void run_one(QueueKind kind, uint64_t count, int workers,
             LatencyHistogram &merged) {
    QueueBackend backend =
        make_queue_backend(kind, 4096, static_cast<size_t>(workers));

    std::vector<LatencyHistogram> per_worker(static_cast<size_t>(workers));
    for (auto &h : per_worker) {
        latency_histogram_reset(h);
    }

    std::vector<std::thread> consumers;
    for (int w = 0; w < workers; w++) {
        consumers.emplace_back([&, w] {
            TickMessage msg;
            IQueueConsumer *c = backend.consumers[static_cast<size_t>(w)].get();
            for (;;) {
                if (c->pop(msg) == PopStatus::Closed) {
                    break;
                }
                uint64_t lat = now_ns() - msg.send_timestamp_ns;
                latency_histogram_record(per_worker[static_cast<size_t>(w)], lat);
            }
        });
    }

    std::thread producer([&] {
        for (uint64_t s = 1; s <= count; s++) {
            TickMessage msg = make_msg(s);
            for (;;) {
                auto rc = backend.producer->try_push(msg);
                if (rc == PushStatus::Ok) {
                    break;
                }
                if (rc == PushStatus::Closed) {
                    return;
                }
                std::this_thread::yield();
            }
        }
        backend.producer->close();
    });

    producer.join();
    for (auto &t : consumers) {
        t.join();
    }

    latency_histogram_reset(merged);
    for (auto &h : per_worker) {
        latency_histogram_merge(merged, h);
    }
}

void print_row(const char *label, const LatencyPercentiles &p) {
    std::printf("  %-10s  count=%-8" PRIu64 "  p50=%6" PRIu64 "ns  p99=%7"
                PRIu64 "ns  p99.9=%8" PRIu64 "ns  max=%9" PRIu64 "ns\n",
                label, p.count, p.p50, p.p99, p.p999, p.max_ns);
}

} // namespace

int main(int argc, char **argv) {
    BenchConfig cfg;
    if (parse_args(argc, argv, cfg) != 0) {
        return 2;
    }

    std::printf("queue_bench: count=%" PRIu64 " workers=%d\n", cfg.count,
                cfg.workers);

    LatencyHistogram mutex_hist;
    run_one(QueueKind::Mutex, cfg.count, cfg.workers, mutex_hist);
    print_row("mutex", latency_histogram_percentiles(mutex_hist));

    /* spsc is single-consumer by construction; force workers=1 here
     * regardless of --workers so the comparison stays meaningful rather
     * than failing. */
    LatencyHistogram spsc_hist;
    run_one(QueueKind::Spsc, cfg.count, 1, spsc_hist);
    print_row("spsc", latency_histogram_percentiles(spsc_hist));

    LatencyHistogram disruptor_hist;
    run_one(QueueKind::Disruptor, cfg.count, cfg.workers, disruptor_hist);
    print_row("disruptor", latency_histogram_percentiles(disruptor_hist));

    std::printf("\nNote: disruptor's count is workers x %" PRIu64
                " because every worker independently sees every message "
                "(broadcast), not a shard of it - see mpmc_fanout.hpp.\n",
                cfg.count);
    return 0;
}
