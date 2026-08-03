/*
 * monitor.c - shared-memory dashboard process.
 *
 * Responsibility: map the receiver's shared-stats region and redraw a
 * one-second dashboard so the whole pipeline is visible from a third
 * terminal.
 *
 * Ownership: guest only - shared_stats_open()/close() (map/unmap);
 * never creates or unlinks the object. The receiver owns it.
 *
 * Thread-safety: single-threaded; only reads atomics.
 */

#include "common.h"
#include "shared_stats.h"
#include "stats.h"
#include "time_util.h"

#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    uint64_t received;
    uint64_t processed;
    uint64_t ts_ns;
} Snapshot;

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --shared-memory-name NAME  shm object (default %s)\n",
            prog, PULSEFORGE_SHM_DEFAULT_NAME);
}

static int parse_args(int argc, char **argv, char *name, size_t name_sz) {
    snprintf(name, name_sz, "%s", PULSEFORGE_SHM_DEFAULT_NAME);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shared-memory-name") == 0 && i + 1 < argc) {
            (void)snprintf(name, name_sz, "%s", argv[++i]);
            if (name[0] != '/') {
                fprintf(stderr, "shm name must start with '/'\n");
                return -1;
            }
        } else {
            fprintf(stderr, "Unknown or malformed argument: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static inline uint64_t load(const _Atomic uint64_t *p) {
    return atomic_load_explicit(p, memory_order_relaxed);
}

static void print_dashboard(SharedStats *s, Snapshot *prev,
                            uint64_t now, int first, const char *name) {
    uint64_t received   = load(&s->received);
    uint64_t invalid    = load(&s->invalid);
    uint64_t gaps       = load(&s->sequence_gaps);
    uint64_t reorders   = load(&s->reorders);
    uint64_t qfull      = load(&s->queue_full);
    uint64_t processed  = load(&s->processed);
    uint64_t qdepth     = load(&s->queue_depth);
    uint64_t lat_total  = load(&s->latency_total_ns);
    uint64_t lat_max    = load(&s->latency_max_ns);

    double recv_rate = 0.0, proc_rate = 0.0;
    if (!first && now > prev->ts_ns) {
        double dt = (double)(now - prev->ts_ns) / 1e9;
        if (dt > 0.0) {
            recv_rate = (double)(received - prev->received) / dt;
            proc_rate = (double)(processed - prev->processed) / dt;
        }
    }
    prev->received  = received;
    prev->processed = processed;
    prev->ts_ns     = now;

    double avg_us = processed
                        ? (double)lat_total / (double)processed / 1000.0
                        : 0.0;
    double max_us = (double)lat_max / 1000.0;

    /* ANSI: home cursor + clear screen (works in a normal terminal). */
    printf("\033[H\033[2J");
    printf("PulseForge monitor  [shm %s]  (Ctrl-C to exit)\n\n", name);
    printf("  received      %12" PRIu64 "   (+%.0f/s)\n", received, recv_rate);
    printf("  invalid       %12" PRIu64 "\n", invalid);
    printf("  gaps          %12" PRIu64 "\n", gaps);
    printf("  reorders      %12" PRIu64 "\n", reorders);
    printf("  queue full    %12" PRIu64 "\n", qfull);
    printf("  queue depth   %12" PRIu64 "\n", qdepth);
    printf("  processed     %12" PRIu64 "   (+%.0f/s)\n", processed, proc_rate);
    printf("  avg latency   %12.1f us\n", avg_us);
    printf("  max latency   %12.1f us\n", max_us);
}

int main(int argc, char **argv) {
    char shm_name[64];
    SharedStats *s = NULL;
    struct sigaction sa;

    if (parse_args(argc, argv, shm_name, sizeof(shm_name)) != 0) {
        return EXIT_USAGE;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("sigaction");
        return EXIT_SYSFAIL;
    }

    s = shared_stats_open(shm_name);
    if (s == NULL) {
        fprintf(stderr, "Cannot open shared stats '%s'.\n", shm_name);
        fprintf(stderr, "Start the receiver first - it creates the region.\n");
        return EXIT_CFG;
    }

    Snapshot prev = {0, 0, now_ns()};
    int first = 1;
    while (!g_stop) {
        uint64_t ts = now_ns();
        print_dashboard(s, &prev, ts, first, shm_name);
        first = 0;
        (void)sleep(1); /* interrupted by SIGINT/SIGTERM; loop re-checks g_stop */
    }

    shared_stats_close(s);
    printf("\nmonitor exiting\n");
    return EXIT_OK;
}
