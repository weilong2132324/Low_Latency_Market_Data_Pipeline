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
 *   - QueueBackend (see queue_backend.hpp) + std::thread own the queue
 *     and worker threads (close() the producer handle to wake blocked
 *     workers, join them, then the backend's destructor frees storage).
 *   - The workers see (do not own) their consumer handle and stats.
 *
 * Thread-safety: the receive loop is the only producer; N worker
 * threads are the consumers; every counter that crosses threads or
 * processes is std::atomic.
 */

#include "bpf_filter.hpp"
#include "common.hpp"
#include "cpu_util.hpp"
#include "message.hpp"
#include "multicast.hpp"
#include "queue_backend.hpp"
#include "shared_events.hpp"
#include "shared_stats.hpp"
#include "snapshot_client.hpp"
#include "time_util.hpp"
#include "worker.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <csignal>
#include <future>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/uio.h> /* struct iovec, used by recvmmsg's struct mmsghdr */
#endif

namespace {

constexpr int         kDefaultPort       = 9000;
constexpr int         kDefaultWorkers    = 1;
constexpr size_t      kQueueCapacity     = 4096;
constexpr const char *kDefaultShmName    = "/pulseforge_stats";

/*
 * Each dispatched recovery request is a real std::async thread doing a
 * blocking TCP round trip. Under sustained, heavy loss (far worse than
 * this project's synthetic --drop-every, but a real possibility during
 * an actual network incident) dispatching one unbounded per gap is a
 * thundering-herd bug: thread-creation overhead on the receive-loop
 * thread competes with draining the UDP socket buffer, which causes
 * MORE loss, which detects MORE gaps, which dispatches MORE threads.
 * Capping in-flight recoveries turns "runs away" into "falls behind and
 * catches up later" - the gaps past the cap simply stay unrecovered
 * until an in-flight one finishes and frees a slot.
 */
constexpr size_t kMaxPendingRecoveries = 8;

/* Set from the signal handler; relaxed store is fine (lock-free bool). */
std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

enum class Transport { Unicast, Multicast };

struct ReceiverConfig {
    int         port            = kDefaultPort;
    int         workers         = kDefaultWorkers;
    int         pin_cpu         = -1; /* -1 = don't pin */
    uint64_t    slow_consumer_us = 0;
    QueueKind   queue_kind      = QueueKind::Mutex;
    std::string shm_name        = kDefaultShmName;
    Transport   transport       = Transport::Unicast;
    std::string group           = kDefaultMulticastGroup;
    int         busy_poll_us    = -1; /* -1 = don't set SO_BUSY_POLL */
    int         batch           = 1;  /* recvmmsg batch size; 1 = plain recvfrom */
    bool        bpf_filter      = false;
    std::string snapshot_host;        /* empty = gap recovery disabled */
    int         snapshot_port   = 0;
    bool        telemetry       = false;
    std::string events_shm_name = PULSEFORGE_EVENTS_SHM_DEFAULT_NAME;
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT            UDP port to bind (default %d)\n"
        "  --queue mutex|spsc|disruptor  queue backend (default mutex)\n"
        "                         mutex/spsc: workers share one queue\n"
        "                         (spsc requires --workers 1); disruptor:\n"
        "                         every worker independently sees every msg\n"
        "  --workers N            consumer threads (default %d)\n"
        "  --pin-cpu CPU          pin receiver thread to a CPU (default off)\n"
        "  --slow-consumer-us US  artificial worker delay per msg (default 0)\n"
        "  --shared-memory-name N shared stats object (default %s)\n"
        "  --transport unicast|multicast  (default unicast)\n"
        "  --group IP             multicast group to join (default %s)\n"
        "  --busy-poll US         SO_BUSY_POLL budget in us (default off;\n"
        "                         needs NAPI driver support - see docs/)\n"
        "  --batch N              recvmmsg() batch size (default 1 = plain\n"
        "                         recvfrom per packet)\n"
        "  --bpf-filter           attach a classic-BPF filter that drops\n"
        "                         zero-quantity datagrams in the kernel\n"
        "  --snapshot-host HOST   publisher's snapshot server (enables TCP\n"
        "                         gap recovery; pair with --snapshot-port)\n"
        "  --snapshot-port PORT   publisher's --snapshot-port\n"
        "  --telemetry            record pipeline events into shared memory\n"
        "                         for src/telemetry_bridge.cpp / the\n"
        "                         visualizer (default off)\n"
        "  --events-shm-name N    event-ring shm object (default %s)\n",
        prog, kDefaultPort, kDefaultWorkers, kDefaultShmName,
        kDefaultMulticastGroup, PULSEFORGE_EVENTS_SHM_DEFAULT_NAME);
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
                cfg.queue_kind = QueueKind::Spsc;
            } else if (val == "disruptor") {
                cfg.queue_kind = QueueKind::Disruptor;
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
        } else if (arg == "--transport") {
            std::string val = next_arg(argc, argv, i);
            if (val == "unicast") {
                cfg.transport = Transport::Unicast;
            } else if (val == "multicast") {
                cfg.transport = Transport::Multicast;
            } else {
                std::fprintf(stderr, "unknown transport: %s\n", val.c_str());
                return -1;
            }
            ++i;
        } else if (arg == "--group") {
            cfg.group = next_arg(argc, argv, i);
            ++i;
        } else if (arg == "--busy-poll") {
            cfg.busy_poll_us = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--batch") {
            cfg.batch = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--bpf-filter") {
            cfg.bpf_filter = true;
        } else if (arg == "--snapshot-host") {
            cfg.snapshot_host = next_arg(argc, argv, i);
            ++i;
        } else if (arg == "--snapshot-port") {
            cfg.snapshot_port = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--telemetry") {
            cfg.telemetry = true;
        } else if (arg == "--events-shm-name") {
            std::string val = next_arg(argc, argv, i);
            if (val.empty() || val[0] != '/') {
                std::fprintf(stderr, "events shm name must start with '/'\n");
                return -1;
            }
            cfg.events_shm_name = val;
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
    if (cfg.queue_kind == QueueKind::Spsc && cfg.workers != 1) {
        std::fprintf(stderr,
                "--queue spsc is single-consumer by construction; "
                "use --workers 1 (or --queue disruptor for N workers)\n");
        return -1;
    }
    if (cfg.transport == Transport::Multicast && !is_multicast_address(cfg.group)) {
        std::fprintf(stderr,
                "--group %s is not a valid multicast address "
                "(224.0.0.0-239.255.255.255)\n",
                cfg.group.c_str());
        return -1;
    }
    if (cfg.batch <= 0) {
        std::fprintf(stderr, "--batch must be > 0\n");
        return -1;
    }
    if (!cfg.snapshot_host.empty() != (cfg.snapshot_port > 0)) {
        std::fprintf(stderr,
                "--snapshot-host and --snapshot-port must be given together\n");
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
        addr.sin_family = AF_INET;
        /*
         * Multicast: bind to INADDR_ANY (0.0.0.0), not the group address -
         * on Linux a UDP socket binds to a local interface address, then
         * IP_ADD_MEMBERSHIP below tells the kernel which group's datagrams
         * to additionally deliver to it. Unicast keeps binding to loopback
         * only, as before.
         */
        addr.sin_addr.s_addr = cfg.transport == Transport::Multicast
                                    ? htonl(INADDR_ANY)
                                    : htonl(INADDR_LOOPBACK);
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

        if (cfg.transport == Transport::Multicast) {
            multicast_join_group(sock.get(), cfg.group);
        }

        if (cfg.bpf_filter) {
            bpf_attach_drop_invalid_quantity_filter(sock.get());
        }

        if (cfg.busy_poll_us >= 0) {
            /*
             * SO_BUSY_POLL asks the NIC driver to poll for incoming packets
             * from the calling thread instead of waiting for an interrupt,
             * trading CPU for lower latency - but only drivers that
             * implement NAPI busy-polling honor it. On loopback (this lab)
             * or under a VM/WSL2 virtual NIC it typically has no measurable
             * effect; setsockopt still succeeds, it just does nothing. This
             * is deliberately a soft failure (warn, don't exit) rather than
             * CHECK(), because whether it "works" depends on hardware this
             * project cannot assume.
             */
            int budget = cfg.busy_poll_us;
            if (setsockopt(sock.get(), SOL_SOCKET, SO_BUSY_POLL, &budget,
                           sizeof(budget)) != 0) {
                std::fprintf(stderr,
                        "warning: SO_BUSY_POLL not supported here (%s); "
                        "continuing without it\n",
                        std::strerror(errno));
            }
        }

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

        SharedEventRegion events;
        if (cfg.telemetry) {
            events = SharedEventRegion::create(cfg.events_shm_name);
            if (!events) {
                std::fprintf(stderr, "failed to create event ring '%s'\n",
                             cfg.events_shm_name.c_str());
                return EXIT_SYSFAIL;
            }
            /* Publish which networking/concurrency options THIS run
             * chose, once, so the visualizer can show real config
             * instead of guessing. */
            shared_config_set(*events, static_cast<int>(cfg.transport),
                              static_cast<int>(cfg.queue_kind), cfg.workers,
                              cfg.batch, cfg.busy_poll_us, cfg.bpf_filter ? 1 : 0,
                              !cfg.snapshot_host.empty() ? 1 : 0, cfg.port,
                              cfg.group);
        }

        QueueBackend backend = make_queue_backend(
            cfg.queue_kind, kQueueCapacity, static_cast<size_t>(cfg.workers));

        if (cfg.pin_cpu >= 0 && pin_to_cpu(cfg.pin_cpu) != 0) {
            return EXIT_SYSFAIL;
        }

        std::vector<WorkerConfig> wcfgs(static_cast<size_t>(cfg.workers));
        std::vector<std::thread>  threads;
        threads.reserve(static_cast<size_t>(cfg.workers));
        for (int i = 0; i < cfg.workers; i++) {
            wcfgs[i].queue            = backend.consumers[static_cast<size_t>(i)].get();
            wcfgs[i].stats            = stats.get();
            wcfgs[i].slow_consumer_us = cfg.slow_consumer_us;
            wcfgs[i].worker_id        = i;
            wcfgs[i].events           = cfg.telemetry ? events.get() : nullptr;
            try {
                /* worker_run takes its config by value, so each thread owns
                 * a private copy. */
                threads.emplace_back(worker_run, wcfgs[i]);
            } catch (const std::system_error &) {
                std::fprintf(stderr, "thread creation for worker %d failed\n", i);
                backend.producer->close();
                for (auto &t : threads) {
                    t.join();
                }
                return EXIT_SYSFAIL;
            }
        }

        std::printf("PulseForge receiver (queue=%s, workers=%d, shm=%s)\n",
                    queue_kind_name(cfg.queue_kind),
                    cfg.workers, cfg.shm_name.c_str());
        if (cfg.pin_cpu >= 0) {
            std::printf("  pinned to CPU %d\n", cfg.pin_cpu);
        }
        if (cfg.slow_consumer_us > 0) {
            std::printf("  slow-consumer delay %" PRIu64 " us/msg\n",
                        cfg.slow_consumer_us);
        }
        if (cfg.transport == Transport::Multicast) {
            std::printf("  listening on group %s:%d  (Ctrl-C to stop)\n",
                        cfg.group.c_str(), cfg.port);
        } else {
            std::printf("  listening on 127.0.0.1:%d  (Ctrl-C to stop)\n",
                        cfg.port);
        }
        if (cfg.busy_poll_us >= 0) {
            std::printf("  SO_BUSY_POLL budget %d us\n", cfg.busy_poll_us);
        }
        if (cfg.batch > 1) {
            std::printf("  recvmmsg batch size %d\n", cfg.batch);
        }
        if (cfg.bpf_filter) {
            std::printf("  classic-BPF filter attached (drops quantity==0 "
                        "in-kernel)\n");
        }

        uint64_t received = 0, invalid = 0, gaps = 0, reorders = 0;
        uint64_t queue_full = 0, recovered = 0;
        uint64_t last_seq   = 0;
        bool     have_last  = false;
        uint64_t start_ns   = now_ns();

        const bool snapshot_enabled = !cfg.snapshot_host.empty();
        std::vector<std::future<std::vector<TickMessage>>> pending_recoveries;

        /* Called once per receive-loop iteration (both the plain and
         * batch paths below): harvest any recovery requests that have
         * finished since the last check, and push their recovered
         * messages into the queue - on THIS thread, the receive loop's,
         * which is the only thread allowed to call try_push() (see
         * snapshot_client.hpp's header comment on why). */
        auto drain_recoveries = [&] {
            size_t i = 0;
            while (i < pending_recoveries.size()) {
                if (pending_recoveries[i].wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready) {
                    ++i;
                    continue;
                }
                std::vector<TickMessage> msgs = pending_recoveries[i].get();
                for (const auto &m : msgs) {
                    if (backend.producer->try_push(m) == PushStatus::Ok) {
                        ++recovered;
                        stats->recovered.fetch_add(1, std::memory_order_relaxed);
                        if (cfg.telemetry) {
                            event_ring_record(*events, EventType::Recovered,
                                              m.sequence, now_ns());
                        }
                    }
                }
                pending_recoveries[i] =
                    std::move(pending_recoveries.back());
                pending_recoveries.pop_back();
                /* don't ++i: the swapped-in element still needs checking */
            }
        };

        /* Shared per-message body for both the plain-recvfrom path and
         * the recvmmsg batch path below: validate, detect gaps/reorders,
         * enqueue with backpressure. `size` is the byte count actually
         * received for this datagram (recvfrom's return value, or one
         * mmsghdr::msg_len in the batch path). */
        auto process_one = [&](TickMessage &msg, ssize_t size) {
            if (size != static_cast<ssize_t>(sizeof(msg)) ||
                !message_is_valid(msg)) {
                ++invalid;
                stats->invalid.fetch_add(1, std::memory_order_relaxed);
                if (cfg.telemetry) {
                    event_ring_record(*events, EventType::Invalid, msg.sequence,
                                      now_ns());
                }
                return;
            }

            ++received;
            stats->received.fetch_add(1, std::memory_order_relaxed);
            if (cfg.telemetry) {
                event_ring_record(*events, EventType::Received, msg.sequence,
                                  now_ns());
            }

            /* Sequence-gap detection (first message establishes baseline). */
            if (have_last) {
                if (msg.sequence > last_seq + 1) {
                    uint64_t gap_start = last_seq + 1;
                    uint64_t g = msg.sequence - gap_start;
                    gaps += g;
                    stats->sequence_gaps.fetch_add(g, std::memory_order_relaxed);
                    if (snapshot_enabled &&
                        pending_recoveries.size() < kMaxPendingRecoveries) {
                        pending_recoveries.push_back(std::async(
                            std::launch::async, request_snapshot_recovery,
                            cfg.snapshot_host, cfg.snapshot_port, gap_start, g));
                    }
                    if (cfg.telemetry) {
                        event_ring_record(*events, EventType::Gap, gap_start,
                                          now_ns(), g);
                    }
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
                auto prc = backend.producer->try_push(msg);
                if (prc == PushStatus::Ok) {
                    break;
                }
                if (prc == PushStatus::Closed) {
                    g_stop.store(true, std::memory_order_relaxed);
                    break;
                }
                /* PushStatus::Full: count one queue-full event per message,
                 * then wait briefly and retry. */
                if (!full_reported) {
                    full_reported = true;
                    ++queue_full;
                    stats->queue_full.fetch_add(1, std::memory_order_relaxed);
                    if (cfg.telemetry) {
                        event_ring_record(*events, EventType::QueueFull,
                                          msg.sequence, now_ns());
                    }
                }
                if (g_stop.load(std::memory_order_relaxed)) {
                    break;
                }
                (void)sleep_ns(10000); /* 10 us backoff */
            }
            stats->queue_depth.store(backend.producer->depth(),
                                     std::memory_order_relaxed);
        };

        if (cfg.batch <= 1) {
            /* Plain path: one recvfrom() syscall per datagram. */
            for (;;) {
                TickMessage msg;
                ssize_t n = recvfrom(sock.get(), &msg, sizeof(msg), 0,
                                     nullptr, nullptr);
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
                process_one(msg, n);
                if (snapshot_enabled) {
                    drain_recoveries();
                }
                if (g_stop.load(std::memory_order_relaxed)) {
                    break;
                }
            }
        } else {
            /*
             * Batch path: recvmmsg() pulls up to cfg.batch datagrams out of
             * the kernel socket buffer in ONE syscall instead of one
             * syscall per datagram - under sustained load this is the
             * single biggest "shave milliseconds" lever available without
             * touching the NIC driver (one syscall's fixed overhead,
             * amortized over many packets, instead of paid per packet).
             */
            std::vector<TickMessage> batch_msgs(static_cast<size_t>(cfg.batch));
            std::vector<iovec>       iovecs(static_cast<size_t>(cfg.batch));
            std::vector<mmsghdr>     headers(static_cast<size_t>(cfg.batch));
            for (int i = 0; i < cfg.batch; i++) {
                iovecs[static_cast<size_t>(i)].iov_base = &batch_msgs[static_cast<size_t>(i)];
                iovecs[static_cast<size_t>(i)].iov_len  = sizeof(TickMessage);
                headers[static_cast<size_t>(i)].msg_hdr.msg_iov    = &iovecs[static_cast<size_t>(i)];
                headers[static_cast<size_t>(i)].msg_hdr.msg_iovlen = 1;
            }

            for (;;) {
                /*
                 * MSG_WAITFORONE: block for the FIRST datagram only, then
                 * take whatever else is already queued and return. Without
                 * it, a blocking recvmmsg() waits until all `batch` slots
                 * are filled - at 400 msgs/s with --batch 16 that parked
                 * every tick for ~20 ms waiting for its batch-mates (found
                 * via the visualizer: avg latency jumped from ~50 us to
                 * 19 ms). Batching must amortise syscalls, never add delay.
                 */
                int n = recvmmsg(sock.get(), headers.data(), static_cast<unsigned>(cfg.batch),
                                 MSG_WAITFORONE, nullptr);
                if (n < 0) {
                    if (errno == EINTR) {
                        if (g_stop.load(std::memory_order_relaxed)) {
                            break;
                        }
                        continue;
                    }
                    std::perror("recvmmsg");
                    return EXIT_SYSFAIL;
                }
                for (int i = 0; i < n; i++) {
                    process_one(batch_msgs[static_cast<size_t>(i)],
                               static_cast<ssize_t>(headers[static_cast<size_t>(i)].msg_len));
                }
                if (snapshot_enabled) {
                    drain_recoveries();
                }
                if (g_stop.load(std::memory_order_relaxed)) {
                    break;
                }
            }
        }

        /* Give any in-flight recovery requests a brief last chance before
         * shutdown; anything still pending after this is abandoned
         * (std::future's destructor blocks until the async task finishes
         * regardless, so there's no leak - just an unused result). */
        if (snapshot_enabled) {
            drain_recoveries();
        }

        std::printf("\nClosing queue; draining %d worker(s)...\n", cfg.workers);
        backend.producer->close();
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
        if (snapshot_enabled) {
            std::printf("  recovered   %" PRIu64 "\n", recovered);
        }
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
        if (cfg.telemetry) {
            events.unlink();
        }

        return EXIT_OK;
    } catch (const SystemError &e) {
        std::fprintf(stderr, "%s failed: %s\n", e.what(),
                     std::strerror(e.code()));
        return EXIT_SYSFAIL;
    }
}
