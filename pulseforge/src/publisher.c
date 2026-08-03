/*
 * publisher.c - UDP tick publisher for the PulseForge lab.
 *
 * Responsibility: generate synthetic fixed-size TickMessage datagrams,
 * pace them at a configured rate, and send them to 127.0.0.1:PORT.
 *
 * Thread-safety: single-threaded; no shared state.
 */

#include "common.h"
#include "message.h"
#include "time_util.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Defaults. */
#define DEFAULT_PORT       9000
#define DEFAULT_RATE       100000   /* average messages/second */
#define DEFAULT_COUNT      1000000  /* sequence numbers to generate */
#define DEFAULT_SYMBOL     1
#define DEFAULT_DROP_EVERY 0        /* 0 == no artificial drops */
#define DEFAULT_BURST      1

typedef struct {
    int      port;
    uint64_t rate;       /* average messages per second  */
    uint64_t count;      /* sequence numbers to generate */
    uint32_t symbol_id;  /* synthetic symbol id          */
    uint64_t drop_every; /* skip every Nth sequence (0 = off) */
    uint64_t burst;      /* datagrams sent per pacing step */
} PublisherConfig;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT      UDP destination port (default %d)\n"
        "  --rate MPS       average messages/second (default %d)\n"
        "  --count N        sequence numbers to generate (default %d)\n"
        "  --symbol ID      synthetic symbol id (default %d)\n"
        "  --drop-every N   skip every Nth sequence to simulate loss (default off)\n"
        "  --burst SIZE     datagrams sent per pacing step (default %d)\n",
        prog, DEFAULT_PORT, DEFAULT_RATE, DEFAULT_COUNT, DEFAULT_SYMBOL, DEFAULT_BURST);
}

static int parse_args(int argc, char **argv, PublisherConfig *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--port") == 0 && val) {
            cfg->port = atoi(val); i++;
        } else if (strcmp(arg, "--rate") == 0 && val) {
            cfg->rate = (uint64_t)strtoull(val, NULL, 10); i++;
        } else if (strcmp(arg, "--count") == 0 && val) {
            cfg->count = (uint64_t)strtoull(val, NULL, 10); i++;
        } else if (strcmp(arg, "--symbol") == 0 && val) {
            cfg->symbol_id = (uint32_t)strtoul(val, NULL, 10); i++;
        } else if (strcmp(arg, "--drop-every") == 0 && val) {
            cfg->drop_every = (uint64_t)strtoull(val, NULL, 10); i++;
        } else if (strcmp(arg, "--burst") == 0 && val) {
            cfg->burst = (uint64_t)strtoull(val, NULL, 10); i++;
        } else {
            fprintf(stderr, "Unknown or malformed argument: %s\n", arg);
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg->rate == 0 || cfg->count == 0 || cfg->burst == 0) {
        fprintf(stderr, "--rate, --count, and --burst must be > 0\n");
        return -1;
    }
    if (cfg->port <= 0 || cfg->port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", cfg->port);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    PublisherConfig cfg = {
        .port       = DEFAULT_PORT,
        .rate       = DEFAULT_RATE,
        .count      = DEFAULT_COUNT,
        .symbol_id  = DEFAULT_SYMBOL,
        .drop_every = DEFAULT_DROP_EVERY,
        .burst      = DEFAULT_BURST,
    };
    int rc = EXIT_OK;
    int fd = -1;
    struct sockaddr_in dst;

    if (parse_args(argc, argv, &cfg) != 0) {
        return EXIT_USAGE;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd, "socket");

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)cfg.port);
    if (inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed for 127.0.0.1\n");
        rc = EXIT_CFG;
        goto cleanup;
    }

    printf("PulseForge publisher\n");
    printf("  destination 127.0.0.1:%d  rate %" PRIu64 "/s  count %" PRIu64
           "  symbol %u  drop-every %" PRIu64 "  burst %" PRIu64 "\n",
           cfg.port, cfg.rate, cfg.count, cfg.symbol_id, cfg.drop_every, cfg.burst);

    /*
     * Pacing: sleep once per burst, using an absolute deadline so errors
     * do not accumulate (drift-free). NOTE: sleep-based pacing is only
     * accurate to roughly the scheduler's timer resolution (typically
     * tens of microseconds) - good enough for a demo, not for a real
     * low-latency system, which would use busy-poll or a hardware timer.
     */
    uint64_t interval_ns = cfg.burst * 1000000000ULL / cfg.rate;
    uint64_t deadline_ns = now_ns();
    uint64_t start_ns    = now_ns();

    uint64_t sent = 0, dropped = 0, send_errors = 0;

    for (uint64_t seq = 1; seq <= cfg.count; seq++) {
        if (cfg.drop_every > 0 && (seq % cfg.drop_every == 0)) {
            /* Simulated loss: generate the sequence number, do not send it. */
            dropped++;
        } else {
            TickMessage msg;
            memset(&msg, 0, sizeof(msg));
            msg.sequence          = seq;
            msg.send_timestamp_ns = now_ns();
            msg.symbol_id         = cfg.symbol_id;
            msg.price             = 10000 + (int64_t)(seq % 100);
            msg.quantity          = (uint32_t)(100 + (seq % 50));
            msg.flags             = TICK_FLAG_TRADE;
            if (seq == cfg.count) {
                msg.flags |= TICK_FLAG_LAST; /* mark the final message */
            }

            ssize_t n = sendto(fd, &msg, sizeof(msg), 0,
                               (const struct sockaddr *)&dst,
                               (socklen_t)sizeof(dst));
            if (n != (ssize_t)sizeof(msg)) {
                if (n < 0) {
                    perror("sendto");
                }
                send_errors++;
            } else {
                sent++;
            }
        }

        if (seq % cfg.burst == 0) {
            deadline_ns += interval_ns;
            (void)sleep_until_ns(deadline_ns);
        }
    }

    uint64_t end_ns    = now_ns();
    double   elapsed_s = (double)(end_ns - start_ns) / 1e9;

    printf("Publish complete: generated %" PRIu64 "  sent %" PRIu64
           "  dropped %" PRIu64 "  send errors %" PRIu64 "\n",
           cfg.count, sent, dropped, send_errors);
    printf("  elapsed %.3f s  average %.0f msgs/s\n",
           elapsed_s,
           elapsed_s > 0.0 ? (double)sent / elapsed_s : 0.0);

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    return rc;
}
