/*
 * monitor.cpp - shared-memory dashboard process.
 *
 * Responsibility: map the receiver's shared-stats region and redraw a
 * one-second dashboard so the whole pipeline is visible from a third
 * terminal.
 *
 * Ownership: guest only - SharedStatsRegion::open() maps the region,
 * RAII unmaps it on exit; never creates or unlinks the object. The
 * receiver owns it.
 *
 * Thread-safety: single-threaded; only reads atomics.
 */

#include "common.hpp"
#include "shared_stats.hpp"
#include "stats.hpp"
#include "time_util.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <unistd.h>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

struct Snapshot {
    uint64_t received  = 0;
    uint64_t processed = 0;
    uint64_t ts_ns     = 0;
};

void usage(const char *prog) {
    std::fprintf(stderr,
            "Usage: %s [options]\n"
            "  --shared-memory-name NAME  shm object (default %s)\n",
            prog, PULSEFORGE_SHM_DEFAULT_NAME);
}

int parse_args(int argc, char **argv, std::string &name) {
    name = PULSEFORGE_SHM_DEFAULT_NAME;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--shared-memory-name") == 0 &&
            i + 1 < argc) {
            name = argv[++i];
            if (name.empty() || name[0] != '/') {
                std::fprintf(stderr, "shm name must start with '/'\n");
                return -1;
            }
        } else {
            std::fprintf(stderr, "Unknown or malformed argument: %s\n",
                         argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

template <typename T>
T load(const std::atomic<T> &p) {
    return p.load(std::memory_order_relaxed);
}

void print_dashboard(SharedStats &s, Snapshot &prev, uint64_t now,
                     bool first, const char *name) {
    uint64_t received  = load(s.received);
    uint64_t invalid   = load(s.invalid);
    uint64_t gaps      = load(s.sequence_gaps);
    uint64_t reorders  = load(s.reorders);
    uint64_t qfull     = load(s.queue_full);
    uint64_t processed = load(s.processed);
    uint64_t qdepth    = load(s.queue_depth);
    uint64_t lat_total = load(s.latency_total_ns);
    uint64_t lat_max   = load(s.latency_max_ns);

    double recv_rate = 0.0, proc_rate = 0.0;
    if (!first && now > prev.ts_ns) {
        double dt = static_cast<double>(now - prev.ts_ns) / 1e9;
        if (dt > 0.0) {
            recv_rate = static_cast<double>(received - prev.received) / dt;
            proc_rate = static_cast<double>(processed - prev.processed) / dt;
        }
    }
    prev.received  = received;
    prev.processed = processed;
    prev.ts_ns     = now;

    double avg_us = processed
                        ? static_cast<double>(lat_total) /
                              static_cast<double>(processed) / 1000.0
                        : 0.0;
    double max_us = static_cast<double>(lat_max) / 1000.0;

    /* ANSI: home cursor + clear screen (works in a normal terminal). */
    std::printf("\033[H\033[2J");
    std::printf("PulseForge monitor  [shm %s]  (Ctrl-C to exit)\n\n", name);
    std::printf("  received      %12" PRIu64 "   (+%.0f/s)\n", received, recv_rate);
    std::printf("  invalid       %12" PRIu64 "\n", invalid);
    std::printf("  gaps          %12" PRIu64 "\n", gaps);
    std::printf("  reorders      %12" PRIu64 "\n", reorders);
    std::printf("  queue full    %12" PRIu64 "\n", qfull);
    std::printf("  queue depth   %12" PRIu64 "\n", qdepth);
    std::printf("  processed     %12" PRIu64 "   (+%.0f/s)\n", processed, proc_rate);
    std::printf("  avg latency   %12.1f us\n", avg_us);
    std::printf("  max latency   %12.1f us\n", max_us);
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::string shm_name;
        if (parse_args(argc, argv, shm_name) != 0) {
            return EXIT_USAGE;
        }

        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        if (sigaction(SIGINT, &sa, nullptr) != 0 ||
            sigaction(SIGTERM, &sa, nullptr) != 0) {
            std::perror("sigaction");
            return EXIT_SYSFAIL;
        }

        SharedStatsRegion region = SharedStatsRegion::open(shm_name);
        if (!region) {
            std::fprintf(stderr, "Cannot open shared stats '%s'.\n",
                         shm_name.c_str());
            std::fprintf(stderr,
                         "Start the receiver first - it creates the region.\n");
            return EXIT_CFG;
        }

        Snapshot prev;
        prev.ts_ns = now_ns();
        bool first = true;
        while (!g_stop.load(std::memory_order_relaxed)) {
            uint64_t ts = now_ns();
            print_dashboard(*region, prev, ts, first, shm_name.c_str());
            first = false;
            (void)sleep(1); /* interrupted by SIGINT/SIGTERM; loop re-checks */
        }

        std::printf("\nmonitor exiting\n");
        return EXIT_OK;
    } catch (const SystemError &e) {
        std::fprintf(stderr, "%s failed: %s\n", e.what(),
                     std::strerror(e.code()));
        return EXIT_SYSFAIL;
    }
}
