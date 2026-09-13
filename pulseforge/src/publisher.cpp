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

struct PublisherConfig {
    // Struct that holds the configuration settings
    int      port      = kDefaultPort;
    uint64_t rate      = kDefaultRate;
    uint64_t count     = kDefaultCount;
    uint32_t symbol_id = kDefaultSymbol;
    uint64_t drop_every = kDefaultDropEvery;
    uint64_t burst     = kDefaultBurst;
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT      UDP destination port (default %d)\n"
        "  --rate MPS       average messages/second (default %" PRIu64 ")\n"
        "  --count N        sequence numbers to generate (default %" PRIu64 ")\n"
        "  --symbol ID      synthetic symbol id (default %u)\n"
        "  --drop-every N   skip every Nth sequence to simulate loss (default off)\n"
        "  --burst SIZE     datagrams sent per pacing step (default %" PRIu64 ")\n",
        prog, kDefaultPort, kDefaultRate, kDefaultCount, kDefaultSymbol,
        kDefaultBurst);
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

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(static_cast<uint16_t>(cfg.port));
        
        if (inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr) != 1) {
            std::fprintf(stderr, "inet_pton failed for 127.0.0.1\n");
            return EXIT_CFG;
        }

        std::printf("PulseForge publisher\n");
        std::printf("  destination 127.0.0.1:%d  rate %" PRIu64 "/s  count %" PRIu64
                    "  symbol %u  drop-every %" PRIu64 "  burst %" PRIu64 "\n",
                    cfg.port, cfg.rate, cfg.count, cfg.symbol_id, cfg.drop_every,
                    cfg.burst);

        uint64_t interval_ns = cfg.burst * 1000000000ULL / cfg.rate;
        uint64_t deadline_ns = now_ns();
        uint64_t start_ns    = now_ns();

        uint64_t sent = 0, dropped = 0, send_errors = 0;

        for (uint64_t seq = 1; seq <= cfg.count; seq++) {
            if (cfg.drop_every > 0 && (seq % cfg.drop_every == 0)) {
                /* Simulated loss: generate the sequence number, do not send it. */
                ++dropped;
            } else {
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
                
                // pause - 13/9/2026
                
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

        return EXIT_OK;
    } catch (const SystemError &e) {
        std::fprintf(stderr, "%s failed: %s\n", e.what(),
                     std::strerror(e.code()));
        return EXIT_SYSFAIL;
    }
}
