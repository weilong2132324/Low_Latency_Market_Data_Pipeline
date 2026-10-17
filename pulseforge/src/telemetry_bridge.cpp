/*
 * telemetry_bridge.cpp - bridges the receiver's shared-memory stats
 * and event ring to any connected browser, over a hand-rolled
 * WebSocket server, as JSON. This is what the visualizer connects to.
 *
 * Responsibility: a third, independent process (same "guest, never
 * owner" role as monitor.cpp) - opens (never creates) the stats and
 * event shared-memory regions the receiver owns, polls both on a
 * fixed interval, and broadcasts a JSON snapshot to every connected
 * WebSocket client. No JSON library: the schema is small and fixed,
 * so this hand-builds the string (consistent with hand-rolling the
 * WebSocket protocol itself in ws_server.hpp/sha1.hpp - the project's
 * "no external libraries" rule extends to the telemetry/visualizer
 * bridge, not just the hot path).
 *
 * Ownership: guest only, like monitor.cpp - never creates or unlinks
 * either shared-memory region; the receiver (started with --telemetry)
 * owns both.
 */

#include "common.hpp"
#include "shared_events.hpp"
#include "shared_stats.hpp"
#include "time_util.hpp"
#include "ws_server.hpp"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr int kDefaultWsPort      = 9700;
constexpr int kDefaultIntervalMs  = 50;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

struct Config {
    std::string stats_shm_name  = PULSEFORGE_SHM_DEFAULT_NAME;
    std::string events_shm_name = PULSEFORGE_EVENTS_SHM_DEFAULT_NAME;
    int         ws_port         = kDefaultWsPort;
    int         interval_ms     = kDefaultIntervalMs;
};

void usage(const char *prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --shared-memory-name NAME   receiver's stats shm (default %s)\n"
        "  --events-shm-name NAME      receiver's event-ring shm (default %s)\n"
        "  --ws-port PORT              WebSocket port for the visualizer (default %d)\n"
        "  --interval-ms MS            poll/broadcast interval (default %d)\n",
        prog, PULSEFORGE_SHM_DEFAULT_NAME, PULSEFORGE_EVENTS_SHM_DEFAULT_NAME,
        kDefaultWsPort, kDefaultIntervalMs);
}

int parse_args(int argc, char **argv, Config &cfg) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (arg == "--shared-memory-name") {
            cfg.stats_shm_name = next();
        } else if (arg == "--events-shm-name") {
            cfg.events_shm_name = next();
        } else if (arg == "--ws-port") {
            cfg.ws_port = std::atoi(next().c_str());
        } else if (arg == "--interval-ms") {
            cfg.interval_ms = std::atoi(next().c_str());
        } else {
            std::fprintf(stderr, "Unknown or malformed argument: %s\n", arg.c_str());
            usage(argv[0]);
            return -1;
        }
    }
    if (cfg.ws_port <= 0 || cfg.ws_port > 65535 || cfg.interval_ms <= 0) {
        std::fprintf(stderr, "invalid --ws-port or --interval-ms\n");
        return -1;
    }
    return 0;
}

const char *event_type_name(uint8_t type) {
    switch (static_cast<EventType>(type)) {
    case EventType::Received:  return "received";
    case EventType::Invalid:   return "invalid";
    case EventType::Gap:       return "gap";
    case EventType::Recovered: return "recovered";
    case EventType::Processed: return "processed";
    case EventType::QueueFull: return "queue_full";
    }
    return "unknown";
}

/* Mirrors receiver.cpp's local `enum class Transport { Unicast,
 * Multicast };` ordinal values (0, 1) - SharedConfig stores the plain
 * int since shared memory can't carry a C++ enum type across process
 * boundaries cleanly. */
const char *transport_name(int t) { return t == 1 ? "multicast" : "unicast"; }

/* Mirrors queue_backend.hpp's `enum class QueueKind { Mutex, Spsc,
 * Disruptor };` ordinal values (0, 1, 2). */
const char *queue_kind_name(int k) {
    switch (k) {
    case 1:  return "spsc";
    case 2:  return "disruptor";
    default: return "mutex";
    }
}

/* Escapes the only character that actually shows up in a multicast
 * group string or could otherwise break naive JSON - good enough for
 * this one small, bounded field (no third-party JSON library, by
 * design - see the file header comment). */
std::string json_escape(const char *s) {
    std::string out;
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            out.push_back('\\');
        }
        out.push_back(*p);
    }
    return out;
}

template <typename T>
T load(const std::atomic<T> &p) { return p.load(std::memory_order_relaxed); }

std::string build_snapshot_json(SharedStats &stats, SharedEventRing &events,
                                uint64_t &last_seen) {
    std::ostringstream out;

    uint64_t received  = load(stats.received);
    uint64_t invalid    = load(stats.invalid);
    uint64_t gaps       = load(stats.sequence_gaps);
    uint64_t reorders   = load(stats.reorders);
    uint64_t queue_full = load(stats.queue_full);
    uint64_t recovered  = load(stats.recovered);
    uint64_t processed  = load(stats.processed);
    uint64_t queue_depth= load(stats.queue_depth);
    uint64_t lat_total  = load(stats.latency_total_ns);
    uint64_t lat_max    = load(stats.latency_max_ns);
    uint64_t avg_lat    = processed ? lat_total / processed : 0;

    out << "{\"stats\":{"
        << "\"received\":" << received << ','
        << "\"invalid\":" << invalid << ','
        << "\"gaps\":" << gaps << ','
        << "\"reorders\":" << reorders << ','
        << "\"queue_full\":" << queue_full << ','
        << "\"recovered\":" << recovered << ','
        << "\"processed\":" << processed << ','
        << "\"queue_depth\":" << queue_depth << ','
        << "\"avg_latency_ns\":" << avg_lat << ','
        << "\"max_latency_ns\":" << lat_max
        << "},";

    const SharedConfig &cfg = events.config;
    if (cfg.ready.load(std::memory_order_acquire)) {
        out << "\"config\":{"
            << "\"transport\":\"" << transport_name(cfg.transport) << "\","
            << "\"multicast_group\":\"" << json_escape(cfg.multicast_group) << "\","
            << "\"queue_kind\":\"" << queue_kind_name(cfg.queue_kind) << "\","
            << "\"workers\":" << cfg.workers << ','
            << "\"batch\":" << cfg.batch << ','
            << "\"busy_poll_us\":" << cfg.busy_poll_us << ','
            << "\"bpf_filter\":" << (cfg.bpf_filter ? "true" : "false") << ','
            << "\"recovery_enabled\":" << (cfg.recovery_enabled ? "true" : "false") << ','
            << "\"port\":" << cfg.port
            << "},";
    }

    out << "\"events\":[";

    uint64_t current = events.write_index.load(std::memory_order_relaxed);
    uint64_t count    = current - last_seen;
    if (count > kEventRingCapacity) {
        /* We fell behind the ring's wraparound: skip ahead and accept
         * that the oldest unseen events are simply lost to this
         * (best-effort, by design) telemetry consumer. */
        last_seen = current - kEventRingCapacity;
        count     = kEventRingCapacity;
    }

    bool first = true;
    for (uint64_t idx = last_seen; idx != current; idx++) {
        const Event &e = events.events[idx % kEventRingCapacity];
        if (!first) {
            out << ',';
        }
        first = false;
        out << "{\"type\":\"" << event_type_name(e.type) << "\","
            << "\"seq\":" << e.sequence << ','
            << "\"ts_ns\":" << e.ts_ns << ','
            << "\"latency_ns\":" << e.latency_ns << ','
            << "\"worker_id\":" << e.worker_id << '}';
    }
    last_seen = current;

    out << "]}";
    return out.str();
}

} // namespace

int main(int argc, char **argv) {
    try {
        Config cfg;
        if (parse_args(argc, argv, cfg) != 0) {
            return EXIT_USAGE;
        }

        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        CHECK(sigaction(SIGINT, &sa, nullptr), "sigaction(SIGINT)");
        CHECK(sigaction(SIGTERM, &sa, nullptr), "sigaction(SIGTERM)");

        SharedStatsRegion stats = SharedStatsRegion::open(cfg.stats_shm_name);
        if (!stats) {
            std::fprintf(stderr,
                    "Cannot open stats shm '%s' - start the receiver with "
                    "--telemetry first.\n",
                    cfg.stats_shm_name.c_str());
            return EXIT_CFG;
        }
        SharedEventRegion events = SharedEventRegion::open(cfg.events_shm_name);
        if (!events) {
            std::fprintf(stderr,
                    "Cannot open event-ring shm '%s' - start the receiver "
                    "with --telemetry first.\n",
                    cfg.events_shm_name.c_str());
            return EXIT_CFG;
        }

        WebSocketServer ws;
        ws.start(cfg.ws_port);

        std::printf("PulseForge telemetry_bridge\n");
        std::printf("  stats shm   %s\n", cfg.stats_shm_name.c_str());
        std::printf("  events shm  %s\n", cfg.events_shm_name.c_str());
        std::printf("  WebSocket   ws://127.0.0.1:%d\n", cfg.ws_port);
        std::printf("  (Ctrl-C to stop)\n");

        uint64_t last_seen = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            /*
             * Only advance last_seen (consume the event ring) when a
             * client is actually connected. Without this check, events
             * get drained into a broadcast nobody receives every tick
             * regardless of whether anyone is listening - so opening
             * the visualizer even a few seconds late means "last_seen"
             * has already caught up to "current" and every event looks
             * gone. Leaving last_seen frozen while idle means the first
             * client to connect gets a backlog (bounded by the ring's
             * capacity) instead of silence.
             */
            if (ws.client_count() > 0) {
                std::string json = build_snapshot_json(*stats, *events, last_seen);
                ws.broadcast_text(json);
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg.interval_ms));
        }

        std::printf("\ntelemetry_bridge exiting (%zu client(s) connected)\n",
                    ws.client_count());
        return EXIT_OK;
    } catch (const SystemError &e) {
        std::fprintf(stderr, "%s failed: %s\n", e.what(),
                     std::strerror(e.code()));
        return EXIT_SYSFAIL;
    }
}
