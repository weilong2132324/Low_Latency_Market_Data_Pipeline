/*
 * publisher.cpp - UDP tick publisher for the PulseForge lab.
 *
 * Responsibility: generate synthetic fixed-size TickMessage datagrams,
 * pace them at a configured rate, and send them to 127.0.0.1:PORT.
 *
 * Thread-safety: single-threaded; no shared state.
 */

#include "common.hpp"
#include "message.hpp"
#include "multicast.hpp"
#include "snapshot_protocol.hpp"
#include "snapshot_server.hpp"
#include "time_util.hpp"

#include <arpa/inet.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>

namespace {

/* Defaults. */
constexpr int      kDefaultPort       = 9000;
constexpr uint64_t kDefaultRate       = 100000;  /* average msgs/second   */
constexpr uint64_t kDefaultCount      = 1000000; /* sequence numbers      */
constexpr uint32_t kDefaultSymbol     = 1;
constexpr uint64_t kDefaultDropEvery  = 0;       /* 0 == no artificial drops */
constexpr uint64_t kDefaultBurst      = 1;
constexpr int      kDefaultTtl        = 1;

enum class Transport { Unicast, Multicast };

struct PublisherConfig {
    // Struct that holds the configuration settings
    int         port       = kDefaultPort;
    uint64_t    rate       = kDefaultRate;
    uint64_t    count      = kDefaultCount;
    uint32_t    symbol_id  = kDefaultSymbol;
    uint64_t    drop_every = kDefaultDropEvery;
    uint64_t    burst      = kDefaultBurst;
    Transport   transport  = Transport::Unicast;
    std::string group      = kDefaultMulticastGroup;
    int         ttl        = kDefaultTtl;
    int         snapshot_port = 0; /* 0 = snapshot/recovery server disabled */
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT      UDP destination port (default %d)\n"
        "  --rate MPS       average messages/second (default %" PRIu64 ")\n"
        "  --count N        sequence numbers to generate (default %" PRIu64 ")\n"
        "  --symbol ID      synthetic symbol id (default %u)\n"
        "  --drop-every N   skip every Nth sequence to simulate loss (default off)\n"
        "  --burst SIZE     datagrams sent per pacing step (default %" PRIu64 ")\n"
        "  --transport unicast|multicast  (default unicast, to 127.0.0.1)\n"
        "  --group IP       multicast group address (default %s)\n"
        "  --ttl N          multicast TTL / router hop limit (default %d)\n"
        "  --snapshot-port PORT  run a TCP gap-recovery server on PORT\n"
        "                        (default off; pair with receiver's\n"
        "                        --snapshot-host/--snapshot-port)\n",
        prog, kDefaultPort, kDefaultRate, kDefaultCount, kDefaultSymbol,
        kDefaultBurst, kDefaultMulticastGroup, kDefaultTtl);
}

/* Fetch argv[i+1] if present, else an empty string. */
std::string next_arg(int argc, char **argv, int &i) {
    return (i + 1 < argc) ? std::string(argv[i + 1]) : std::string();
}

int parse_args(int argc, char **argv, PublisherConfig &cfg) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port") {
            cfg.port = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--rate") {
            cfg.rate = std::strtoull(next_arg(argc, argv, i).c_str(), nullptr, 10);
            ++i;
        } else if (arg == "--count") {
            cfg.count = std::strtoull(next_arg(argc, argv, i).c_str(), nullptr, 10);
            ++i;
        } else if (arg == "--symbol") {
            cfg.symbol_id =
                static_cast<uint32_t>(std::strtoul(next_arg(argc, argv, i).c_str(), nullptr, 10));
            ++i;
        } else if (arg == "--drop-every") {
            cfg.drop_every = std::strtoull(next_arg(argc, argv, i).c_str(), nullptr, 10);
            ++i;
        } else if (arg == "--burst") {
            cfg.burst = std::strtoull(next_arg(argc, argv, i).c_str(), nullptr, 10);
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
        } else if (arg == "--ttl") {
            cfg.ttl = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else if (arg == "--snapshot-port") {
            cfg.snapshot_port = std::atoi(next_arg(argc, argv, i).c_str());
            ++i;
        } else {
            std::fprintf(stderr, "Unknown or malformed argument: %s\n", arg.c_str());
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg.rate == 0 || cfg.count == 0 || cfg.burst == 0) {
        std::fprintf(stderr, "--rate, --count, and --burst must be > 0\n");
        return -1;
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        std::fprintf(stderr, "Invalid port: %d\n", cfg.port);
        return -1;
    }
    if (cfg.transport == Transport::Multicast &&
        !is_multicast_address(cfg.group)) {
        std::fprintf(stderr,
                "--group %s is not a valid multicast address "
                "(224.0.0.0-239.255.255.255)\n",
                cfg.group.c_str());
        return -1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        PublisherConfig cfg;
        if (parse_args(argc, argv, cfg) != 0) {
            return EXIT_USAGE;
        }

        /* RAII: the socket fd is closed automatically on exit. */
        Socket sock(AF_INET, SOCK_DGRAM, 0);
        CHECK(sock.get(), "socket");

        const std::string dst_ip =
            cfg.transport == Transport::Multicast ? cfg.group : "127.0.0.1";

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(static_cast<uint16_t>(cfg.port));

        if (inet_pton(AF_INET, dst_ip.c_str(), &dst.sin_addr) != 1) {
            std::fprintf(stderr, "inet_pton failed for %s\n", dst_ip.c_str());
            return EXIT_CFG;
        }

        if (cfg.transport == Transport::Multicast) {
            multicast_set_ttl(sock.get(), cfg.ttl);
            /* Loop back to receivers on this same host - otherwise this
             * lab's publisher and receiver(s), which all run on one
             * box, would never see each other's traffic. */
            multicast_set_loop(sock.get(), true);
        }

        std::printf("PulseForge publisher\n");
        std::printf("  destination %s:%d (%s)  rate %" PRIu64 "/s  count %" PRIu64
                    "  symbol %u  drop-every %" PRIu64 "  burst %" PRIu64 "\n",
                    dst_ip.c_str(), cfg.port,
                    cfg.transport == Transport::Multicast ? "multicast" : "unicast",
                    cfg.rate, cfg.count, cfg.symbol_id, cfg.drop_every, cfg.burst);

        /* RAII: SnapshotServer's destructor stops its accept thread, so
         * there is nothing to clean up on any return path below. */
        SnapshotServer snapshot_server;
        if (cfg.snapshot_port > 0) {
            snapshot_server.start(cfg.snapshot_port);
            std::printf("  snapshot/recovery TCP server on port %d\n",
                        cfg.snapshot_port);
        }

        uint64_t interval_ns = cfg.burst * 1000000000ULL / cfg.rate;
        /* Pacing deadlines MUST use now_ns_monotonic(), not now_ns() -
         * see time_util.hpp's comment on why mixing CLOCK_MONOTONIC_RAW
         * deadlines into sleep_until_ns()'s CLOCK_MONOTONIC wait breaks
         * pacing silently (it did, before this fix: --rate was ignored
         * and every message fired back-to-back). */
        uint64_t deadline_ns = now_ns_monotonic();
        uint64_t start_ns    = now_ns();

        uint64_t sent = 0, dropped = 0, send_errors = 0;

        for (uint64_t seq = 1; seq <= cfg.count; seq++) {
            TickMessage msg{};
            msg.sequence          = seq;
            msg.send_timestamp_ns = now_ns();
            msg.symbol_id         = cfg.symbol_id;
            msg.price             = 10000 + static_cast<int64_t>(seq % 100);
            msg.quantity          = static_cast<uint32_t>(100 + (seq % 50));
            msg.flags             = TICK_FLAG_TRADE;
            if (seq == cfg.count) {
                msg.flags |= TICK_FLAG_LAST; /* mark the final message */
            }

            if (cfg.snapshot_port > 0) {
                /* Record EVERY generated message, even ones about to be
                 * skipped below - the snapshot ring models the
                 * authoritative source; only the UDP wire loses packets. */
                snapshot_server.record(msg);
            }

            if (cfg.drop_every > 0 && (seq % cfg.drop_every == 0)) {
                /* Simulated loss: generate the sequence number, do not send it. */
                ++dropped;
            } else {
                ssize_t n = sendto(sock.get(), &msg, sizeof(msg), 0,
                                   reinterpret_cast<const sockaddr *>(&dst),
                                   static_cast<socklen_t>(sizeof(dst)));
                if (n != static_cast<ssize_t>(sizeof(msg))) {
                    if (n < 0) {
                        std::perror("sendto");
                    }
                    ++send_errors;
                } else {
                    ++sent;
                }
            }

            if (seq % cfg.burst == 0) {
                deadline_ns += interval_ns;
                (void)sleep_until_ns(deadline_ns);
            }
        }

        uint64_t end_ns    = now_ns();
        double   elapsed_s = static_cast<double>(end_ns - start_ns) / 1e9;

        std::printf("Publish complete: generated %" PRIu64 "  sent %" PRIu64
                    "  dropped %" PRIu64 "  send errors %" PRIu64 "\n",
                    cfg.count, sent, dropped, send_errors);

        std::printf("  elapsed %.3f s  average %.0f msgs/s\n",
                    elapsed_s,
                    elapsed_s > 0.0 ? static_cast<double>(sent) / elapsed_s : 0.0);

        if (cfg.snapshot_port > 0) {
            /* Grace period: give the receiver a window to notice and
             * repair any trailing gaps before this process (and its
             * snapshot server) goes away. */
            std::printf("  keeping snapshot server up for 500ms for trailing "
                        "recovery requests...\n");
            (void)sleep_ns(500ULL * 1000 * 1000);
        }

        return EXIT_OK;
    } catch (const SystemError &e) {
        std::fprintf(stderr, "%s failed: %s\n", e.what(),
                     std::strerror(e.code()));
        return EXIT_SYSFAIL;
    }
}
