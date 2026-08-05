/*
 * receiver.c - UDP tick receiver for the PulseForge lab.
 *
 * Responsibility (Phase 1): bind a UDP socket, receive fixed-size
 * TickMessages, validate them, detect sequence gaps, enqueue them into a
 * bounded mutex+condvar queue, and let worker threads fold them into
 * shared-memory statistics that a separate monitor process displays.
 *
 * Ownership:
 *   - Owns the UDP socket fd (closed in cleanup).
 *   - Owns the shared-stats region (creates AND unlinks it).
 *   - Owns the QueueMutex and its worker threads (closes the queue to
 *     wake blocked workers, joins them, then destroys the queue).
 *   - The workers see (do not own) the queue and stats.
 *
 * Thread-safety: the receive loop is the only producer; N worker
 * threads are the consumers; every counter that crosses threads or
 * processes is atomic.
 */

#include "common.h"
#include "cpu_util.h"
#include "message.h"
#include "queue_mutex.h"
#include "shared_stats.h"
#include "time_util.h"
#include "worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT       9000
#define DEFAULT_WORKERS    1
#define QUEUE_CAPACITY     4096
#define DEFAULT_SHM_NAME   "/pulseforge_stats"

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef enum { QUEUE_KIND_MUTEX, QUEUE_KIND_SPSC } QueueKind;

typedef struct {
    int         port;
    int         workers;
    int         pin_cpu;         /* -1 = don't pin */
    uint64_t    slow_consumer_us;
    QueueKind   queue_kind;
    char        shm_name[64];
} ReceiverConfig;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT            UDP port to bind (default %d)\n"
        "  --queue mutex|spsc     queue backend (default mutex; spsc = Phase 4)\n"
        "  --workers N            consumer threads (default %d)\n"
        "  --pin-cpu CPU          pin receiver thread to a CPU (default off)\n"
        "  --slow-consumer-us US  artificial worker delay per msg (default 0)\n"
        "  --shared-memory-name N shared stats object (default %s)\n",
        prog, DEFAULT_PORT, DEFAULT_WORKERS, DEFAULT_SHM_NAME);
}

static int parse_args(int argc, char **argv, ReceiverConfig *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--port") == 0 && val) {
            cfg->port = atoi(val); i++;
        } else if (strcmp(arg, "--queue") == 0 && val) {
            if (strcmp(val, "mutex") == 0) {
                cfg->queue_kind = QUEUE_KIND_MUTEX;
            } else if (strcmp(val, "spsc") == 0) {
                fprintf(stderr,
                        "--queue spsc is not implemented yet (arrives in "
                        "Phase 4); use --queue mutex\n");
                return -1;
            } else {
                fprintf(stderr, "unknown queue kind: %s\n", val);
                return -1;
            }
            i++;
        } else if (strcmp(arg, "--workers") == 0 && val) {
            cfg->workers = atoi(val); i++;
        } else if (strcmp(arg, "--pin-cpu") == 0 && val) {
            cfg->pin_cpu = atoi(val); i++;
        } else if (strcmp(arg, "--slow-consumer-us") == 0 && val) {
            cfg->slow_consumer_us = (uint64_t)strtoull(val, NULL, 10); i++;
        } else if (strcmp(arg, "--shared-memory-name") == 0 && val) {
            (void)snprintf(cfg->shm_name, sizeof(cfg->shm_name), "%s", val);
            if (cfg->shm_name[0] != '/') {
                fprintf(stderr, "shared-memory name must start with '/'\n");
                return -1;
            }
            i++;
        } else {
            fprintf(stderr, "Unknown or malformed argument: %s\n", arg);
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg->port <= 0 || cfg->port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", cfg->port);
        return -1;
    }
    if (cfg->workers <= 0) {
        fprintf(stderr, "--workers must be > 0\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    ReceiverConfig cfg = {
        .port            = DEFAULT_PORT,
        .workers         = DEFAULT_WORKERS,
        .pin_cpu         = -1,
        .slow_consumer_us = 0,
        .queue_kind      = QUEUE_KIND_MUTEX,
    };
    (void)snprintf(cfg.shm_name, sizeof(cfg.shm_name), "%s", DEFAULT_SHM_NAME);

    int rc = EXIT_OK;
    int fd = -1;
    QueueMutex *q = NULL;
    SharedStats *stats = NULL;
    pthread_t *threads = NULL;
    WorkerConfig *wcfgs = NULL;
    struct sockaddr_in addr;
    struct sigaction sa;

    if (parse_args(argc, argv, &cfg) != 0) {
        return EXIT_USAGE;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd, "socket");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)cfg.port);
    int one = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)),
          "setsockopt(SO_REUSEADDR)");
    /*
     * Grow the UDP socket receive buffer. The kernel buffer is the FIRST
     * queue: bursts that arrive while our bounded queue is full are held
     * here, and only overflow once it is exhausted. A bigger buffer
     * absorbs spikes but never removes loss under sustained overload.
     */
    int rcvbuf = 4 * 1024 * 1024;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)),
          "setsockopt(SO_RCVBUF)");
    CHECK(bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)),
          "bind");

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    /*
     * Deliberately NO SA_RESTART: we want blocking recvfrom() to be
     * interrupted (returning EINTR) so the loop can observe g_stop and
     * shut down cleanly instead of spinning or hanging.
     */
    sa.sa_flags = 0;
    CHECK(sigaction(SIGINT, &sa, NULL), "sigaction(SIGINT)");
    CHECK(sigaction(SIGTERM, &sa, NULL), "sigaction(SIGTERM)");

    stats = shared_stats_create(cfg.shm_name, NULL);
    if (stats == NULL) {
        fprintf(stderr, "failed to create shared stats '%s'\n", cfg.shm_name);
        rc = EXIT_SYSFAIL;
        goto cleanup;
    }

    q = queue_mutex_create(QUEUE_CAPACITY);
    if (q == NULL) {
        fprintf(stderr, "queue_mutex_create failed\n");
        rc = EXIT_SYSFAIL;
        goto cleanup;
    }

    threads = calloc((size_t)cfg.workers, sizeof(*threads));
    wcfgs   = calloc((size_t)cfg.workers, sizeof(*wcfgs));
    if (threads == NULL || wcfgs == NULL) {
        fprintf(stderr, "calloc(workers) failed\n");
        rc = EXIT_SYSFAIL;
        goto cleanup;
    }

    if (cfg.pin_cpu >= 0 && pin_to_cpu(cfg.pin_cpu) != 0) {
        rc = EXIT_SYSFAIL;
        goto cleanup;
    }

    printf("PulseForge receiver (queue=%s, workers=%d, shm=%s)\n",
           cfg.queue_kind == QUEUE_KIND_MUTEX ? "mutex" : "spsc",
           cfg.workers, cfg.shm_name);
    if (cfg.pin_cpu >= 0) {
        printf("  pinned to CPU %d\n", cfg.pin_cpu);
    }
    if (cfg.slow_consumer_us > 0) {
        printf("  slow-consumer delay %" PRIu64 " us/msg\n",
               cfg.slow_consumer_us);
    }
    printf("  listening on 127.0.0.1:%d  (Ctrl-C to stop)\n", cfg.port);

    /* Start the workers. If any pthread_create fails, close + join the
     * ones already running, then bail out through cleanup. */
    int nstarted = 0;
    for (int i = 0; i < cfg.workers; i++) {
        wcfgs[i].queue           = q;
        wcfgs[i].stats           = stats;
        wcfgs[i].slow_consumer_us = cfg.slow_consumer_us;
        wcfgs[i].worker_id       = i;
        if (pthread_create(&threads[i], NULL, worker_main, &wcfgs[i]) != 0) {
            fprintf(stderr, "pthread_create(%d) failed\n", i);
            rc = EXIT_SYSFAIL;
            break;
        }
        nstarted++;
    }
    if (rc != EXIT_OK) {
        queue_mutex_close(q);
        for (int i = 0; i < nstarted; i++) {
            (void)pthread_join(threads[i], NULL);
        }
        goto cleanup;
    }

    uint64_t received = 0, invalid = 0, gaps = 0, reorders = 0;
    uint64_t queue_full = 0;
    uint64_t last_seq   = 0;
    int      have_last  = 0;
    uint64_t start_ns   = now_ns();

    for (;;) {
        TickMessage msg;
        ssize_t n = recvfrom(fd, &msg, sizeof(msg), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop) {
                    break;
                }
                continue; /* spurious interrupt; keep receiving */
            }
            perror("recvfrom");
            rc = EXIT_SYSFAIL;
            break; /* stop cleanly (close + drain) rather than leak */
        }

        if (n != (ssize_t)sizeof(msg)) {
            invalid++;
            atomic_fetch_add_explicit(&stats->invalid, 1, memory_order_relaxed);
            continue;
        }
        if (!message_is_valid(&msg)) {
            invalid++;
            atomic_fetch_add_explicit(&stats->invalid, 1, memory_order_relaxed);
            continue;
        }

        received++;
        atomic_fetch_add_explicit(&stats->received, 1, memory_order_relaxed);

        /* Sequence-gap detection (first message establishes baseline). */
        if (have_last) {
            if (msg.sequence > last_seq + 1) {
                uint64_t g = msg.sequence - (last_seq + 1);
                gaps += g;
                atomic_fetch_add_explicit(&stats->sequence_gaps, g,
                                          memory_order_relaxed);
            } else if (msg.sequence < last_seq) {
                reorders++;
                atomic_fetch_add_explicit(&stats->reorders, 1,
                                          memory_order_relaxed);
            }
        }
        last_seq  = msg.sequence;
        have_last = 1;

        /*
         * Enqueue with backpressure. We prefer try_push + a tiny backoff
         * over blocking in queue_mutex_push() so that Ctrl-C (g_stop) is
         * observed quickly even when the queue is full; the queue itself
         * still provides the classic blocking push/pop + close semantics
         * (proven by the unit tests).
         */
        int full_reported = 0;
        for (;;) {
            int prc = queue_mutex_try_push(q, &msg);
            if (prc == QUEUE_OK) {
                break;
            }
            if (prc == QUEUE_CLOSED) {
                g_stop = 1;
                break;
            }
            /* QUEUE_FULL: count one queue-full event per message, then
             * wait briefly and retry. */
            if (!full_reported) {
                full_reported = 1;
                queue_full++;
                atomic_fetch_add_explicit(&stats->queue_full, 1,
                                          memory_order_relaxed);
            }
            if (g_stop) {
                break;
            }
            (void)sleep_ns(10000); /* 10 us backoff */
        }
        atomic_store_explicit(&stats->queue_depth,
                              queue_mutex_depth(q), memory_order_relaxed);
        if (g_stop) {
            break;
        }
    }

    printf("\nClosing queue; draining %d worker(s)...\n", cfg.workers);
    queue_mutex_close(q);
    for (int i = 0; i < cfg.workers; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "pthread_join(%d) failed\n", i);
        }
    }

    uint64_t processed  = atomic_load_explicit(&stats->processed,
                                               memory_order_relaxed);
    uint64_t lat_total  = atomic_load_explicit(&stats->latency_total_ns,
                                               memory_order_relaxed);
    uint64_t lat_max    = atomic_load_explicit(&stats->latency_max_ns,
                                               memory_order_relaxed);
    uint64_t end_ns     = now_ns();
    double   elapsed_s  = (double)(end_ns - start_ns) / 1e9;

    printf("Receiver summary:\n");
    printf("  received    %" PRIu64 "\n", received);
    printf("  invalid     %" PRIu64 "\n", invalid);
    printf("  gaps        %" PRIu64 "\n", gaps);
    printf("  reorders    %" PRIu64 "\n", reorders);
    printf("  queue full  %" PRIu64 "\n", queue_full);
    printf("  processed   %" PRIu64 "\n", processed);
    printf("  avg latency %.1f us   max latency %.1f us\n",
           processed ? (double)lat_total / (double)processed / 1000.0 : 0.0,
           (double)lat_max / 1000.0);
    printf("  runtime     %.3f s\n", elapsed_s);

cleanup:
    if (q != NULL) {
        queue_mutex_destroy(q);
    }
    if (stats != NULL) {
        shared_stats_close(stats);   /* unmap */
        shared_stats_unlink(cfg.shm_name); /* owner removes the object */
    }
    free(threads);
    free(wcfgs);
    if (fd >= 0) {
        close(fd);
    }
    return rc;
}
