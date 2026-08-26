/*
 * receiver.cpp - UDP tick receiver for the PulseForge lab.
 *
 * Responsibility (Phase 1): bind a UDP socket, receive fixed-size
 * TickMessages, validate them, detect sequence gaps, enqueue them into a
 * bounded mutex+condvar queue, and let worker threads fold them into
 * shared-memory statistics that a separate monitor process displays.
 *
 * Ownership (all RAII - no manual cleanup labels):
 *   - Socket owns the UDP socket fd.
 *   - SharedStatsRegion owns the shared-stats region (creates it, and
 *     unlink() removes the object on shutdown).
 *   - QueueMutex + std::thread own the queue and worker threads (close
 *     the queue to wake blocked workers, join them, then the queue's
 *     destructor frees its storage).
 *   - The workers see (do not own) the queue and stats.
 *
 * Thread-safety: the receive loop is the only producer; N worker
 * threads are the consumers; every counter that crosses threads or
 * processes is std::atomic.
 */

#include "common.hpp"
#include "cpu_util.hpp"
#include "message.hpp"
#include "queue_mutex.hpp"
#include "shared_stats.hpp"
#include "time_util.hpp"
#include "worker.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr int         kDefaultPort       = 9000;
constexpr int         kDefaultWorkers    = 1;
constexpr size_t      kQueueCapacity     = 4096;
constexpr const char *kDefaultShmName    = "/pulseforge_stats";

/* Set from the signal handler; relaxed store is fine (lock-free bool). */
std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

enum class QueueKind { Mutex, Spsc };

struct ReceiverConfig {
    int         port            = kDefaultPort;
    int         workers         = kDefaultWorkers;
    int         pin_cpu         = -1; /* -1 = don't pin */
    uint64_t    slow_consumer_us = 0;
    QueueKind   queue_kind      = QueueKind::Mutex;
    std::string shm_name        = kDefaultShmName;
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT            UDP port to bind (default %d)\n"
        "  --queue mutex|spsc     queue backend (default mutex; spsc = Phase 4)\n"
        "  --workers N            consumer threads (default %d)\n"
        "  --pin-cpu CPU          pin receiver thread to a CPU (default off)\n"
        "  --slow-consumer-us US  artificial worker delay per msg (default 0)\n"
        "  --shared-memory-name N shared stats object (default %s)\n",
        prog, kDefaultPort, kDefaultWorkers, kDefaultShmName);
}

std::string next_arg(int argc, char **argv, int &i) {
    return (i + 1 < argc) ? std::string(argv[i + 1]) : std::string();
}

int parse_args(int argc, char **argv, ReceiverConfig &cfg) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port") {
            cfg.port = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--queue") {
            std::string val = next_arg(argc, argv, i);
            if (val == "mutex") {
                cfg.queue_kind = QueueKind::Mutex;
            } else if (val == "spsc") {
                std::fprintf(stderr,
                        "--queue spsc is not implemented yet (arrives in "
                        "Phase 4); use --queue mutex\n");
                return -1;
            } else {
                std::fprintf(stderr, "unknown queue kind: %s\n", val.c_str());
                return -1;
            }
            ++i;
        } else if (arg == "--workers") {
            cfg.workers = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--pin-cpu") {
            cfg.pin_cpu = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--slow-consumer-us") {
            cfg.slow_consumer_us =
                std::strtoull(next_arg(argc, argv, i).c_str(), nullptr, 10);
            ++i;
        } else if (arg == "--shared-memory-name") {
            std::string val = next_arg(argc, argv, i);
            if (val.empty() || val[0] != '/') {
                std::fprintf(stderr, "shared-memory name must start with '/'\n");
                return -1;
            }
            cfg.shm_name = val;
            ++i;
        } else {
            std::fprintf(stderr, "Unknown or malformed argument: %s\n",
                         arg.c_str());
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg.port <= 0 || cfg.port > 65535) {
        std::fprintf(stderr, "Invalid port: %d\n", cfg.port);
        return -1;
    }
    if (cfg.workers <= 0) {
        std::fprintf(stderr, "--workers must be > 0\n");
        return -1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        ReceiverConfig cfg;
        if (parse_args(argc, argv, cfg) != 0) {
            return EXIT_USAGE;
        }

        Socket sock(AF_INET, SOCK_DGRAM, 0);
        CHECK(sock.get(), "socket");

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(static_cast<uint16_t>(cfg.port));
        int one = 1;
        CHECK(setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one,
                         sizeof(one)),
              "setsockopt(SO_REUSEADDR)");
        /*
         * Grow the UDP socket receive buffer. The kernel buffer is the FIRST
         * queue: bursts that arrive while our bounded queue is full are held
         * here, and only overflow once it is exhausted. A bigger buffer
         * absorbs spikes but never removes loss under sustained overload.
         */
        int rcvbuf = 4 * 1024 * 1024;
        CHECK(setsockopt(sock.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                         sizeof(rcvbuf)),
              "setsockopt(SO_RCVBUF)");
        CHECK(bind(sock.get(), reinterpret_cast<const sockaddr *>(&addr),
                   static_cast<socklen_t>(sizeof(addr))),
              "bind");

        struct sigaction sa{};
        sa.sa_handler = handle_signal;
        sigemptyset(&sa.sa_mask);
        /*
         * Deliberately NO SA_RESTART: we want blocking recvfrom() to be
         * interrupted (returning EINTR) so the loop can observe g_stop and
         * shut down cleanly instead of spinning or hanging.
         */
        sa.sa_flags = 0;
        CHECK(sigaction(SIGINT, &sa, nullptr), "sigaction(SIGINT)");
        CHECK(sigaction(SIGTERM, &sa, nullptr), "sigaction(SIGTERM)");

        SharedStatsRegion stats = SharedStatsRegion::create(cfg.shm_name);
        if (!stats) {
            std::fprintf(stderr, "failed to create shared stats '%s'\n",
                         cfg.shm_name.c_str());
            return EXIT_SYSFAIL;
        }

        QueueMutex queue(kQueueCapacity);

        if (cfg.pin_cpu >= 0 && pin_to_cpu(cfg.pin_cpu) != 0) {
            return EXIT_SYSFAIL;
        }

        std::vector<WorkerConfig> wcfgs(static_cast<size_t>(cfg.workers));
        std::vector<std::thread>  threads;
        threads.reserve(static_cast<size_t>(cfg.workers));
        for (int i = 0; i < cfg.workers; i++) {
            wcfgs[i].queue            = &queue;
            wcfgs[i].stats            = stats.get();
            wcfgs[i].slow_consumer_us = cfg.slow_consumer_us;
            wcfgs[i].worker_id        = i;
            try {
                /* worker_run takes its config by value, so each thread owns
                 * a private copy. */
                threads.emplace_back(worker_run, wcfgs[i]);
            } catch (const std::system_error &) {
                std::fprintf(stderr, "thread creation for worker %d failed\n", i);
                queue.close();
                for (auto &t : threads) {
                    t.join();
                }
                return EXIT_SYSFAIL;
            }
        }

        std::printf("PulseForge receiver (queue=%s, workers=%d, shm=%s)\n",
                    cfg.queue_kind == QueueKind::Mutex ? "mutex" : "spsc",
                    cfg.workers, cfg.shm_name.c_str());
        if (cfg.pin_cpu >= 0) {
            std::printf("  pinned to CPU %d\n", cfg.pin_cpu);
        }
        if (cfg.slow_consumer_us > 0) {
            std::printf("  slow-consumer delay %" PRIu64 " us/msg\n",
                        cfg.slow_consumer_us);
        }
        std::printf("  listening on 127.0.0.1:%d  (Ctrl-C to stop)\n",
                    cfg.port);

        uint64_t received = 0, invalid = 0, gaps = 0, reorders = 0;
        uint64_t queue_full = 0;
        uint64_t last_seq   = 0;
        bool     have_last  = false;
        uint64_t start_ns   = now_ns();

        for (;;) {
            TickMessage msg;
            ssize_t n = recvfrom(sock.get(), &msg, sizeof(msg), 0, nullptr,
                                 nullptr);
            if (n < 0) {
                if (errno == EINTR) {
                    if (g_stop.load(std::memory_order_relaxed)) {
                        break;
                    }
                    continue; /* spurious interrupt; keep receiving */
                }
                std::perror("recvfrom");
                return EXIT_SYSFAIL; /* RAII cleans up */
            }

            if (n != static_cast<ssize_t>(sizeof(msg))) {
                ++invalid;
                stats->invalid.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!message_is_valid(msg)) {
                ++invalid;
                stats->invalid.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            ++received;
            stats->received.fetch_add(1, std::memory_order_relaxed);

            /* Sequence-gap detection (first message establishes baseline). */
            if (have_last) {
                if (msg.sequence > last_seq + 1) {
                    uint64_t g = msg.sequence - (last_seq + 1);
                    gaps += g;
                    stats->sequence_gaps.fetch_add(g, std::memory_order_relaxed);
                } else if (msg.sequence < last_seq) {
                    ++reorders;
                    stats->reorders.fetch_add(1, std::memory_order_relaxed);
                }
            }
            last_seq  = msg.sequence;
            have_last = true;

            /*
             * Enqueue with backpressure. We prefer try_push + a tiny backoff
             * over blocking in queue.push() so that Ctrl-C (g_stop) is
             * observed quickly even when the queue is full; the queue itself
             * still provides the classic blocking push/pop + close semantics
             * (proven by the unit tests).
             */
            bool full_reported = false;
            for (;;) {
                auto prc = queue.try_push(msg);
                if (prc == QueueMutex::Status::Ok) {
                    break;
                }
                if (prc == QueueMutex::Status::Closed) {
                    g_stop.store(true, std::memory_order_relaxed);
                    break;
                }
                /* Status::Full: count one queue-full event per message, then
                 * wait briefly and retry. */
                if (!full_reported) {
                    full_reported = true;
                    ++queue_full;
                    stats->queue_full.fetch_add(1, std::memory_order_relaxed);
                }
                if (g_stop.load(std::memory_order_relaxed)) {
                    break;
                }
                (void)sleep_ns(10000); /* 10 us backoff */
            }
            stats->queue_depth.store(queue.depth(), std::memory_order_relaxed);
            if (g_stop.load(std::memory_order_relaxed)) {
                break;
            }
        }

        std::printf("\nClosing queue; draining %d worker(s)...\n", cfg.workers);
        queue.close();
        for (auto &t : threads) {
            t.join();
        }

        uint64_t processed = stats->processed.load(std::memory_order_relaxed);
        uint64_t lat_total = stats->latency_total_ns.load(std::memory_order_relaxed);
        uint64_t lat_max   = stats->latency_max_ns.load(std::memory_order_relaxed);
        uint64_t end_ns    = now_ns();
        double   elapsed_s = static_cast<double>(end_ns - start_ns) / 1e9;

        std::printf("Receiver summary:\n");
        std::printf("  received    %" PRIu64 "\n", received);
        std::printf("  invalid     %" PRIu64 "\n", invalid);
        std::printf("  gaps        %" PRIu64 "\n", gaps);
        std::printf("  reorders    %" PRIu64 "\n", reorders);
        std::printf("  queue full  %" PRIu64 "\n", queue_full);
        std::printf("  processed   %" PRIu64 "\n", processed);
        std::printf("  avg latency %.1f us   max latency %.1f us\n",
                    processed
                        ? static_cast<double>(lat_total) /
                              static_cast<double>(processed) / 1000.0
                        : 0.0,
                    static_cast<double>(lat_max) / 1000.0);
        std::printf("  runtime     %.3f s\n", elapsed_s);

        /* Owner removes the shm object (unmaps via RAII on return). */
        stats.unlink();

        return EXIT_OK;
    } catch (const SystemError &e) {
        std::fprintf(stderr, "%s failed: %s\n", e.what(),
                     std::strerror(e.code()));
        return EXIT_SYSFAIL;
    }
}
