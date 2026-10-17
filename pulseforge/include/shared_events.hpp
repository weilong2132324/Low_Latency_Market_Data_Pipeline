/*
 * shared_events.hpp - shared-memory event ring feeding the telemetry
 * bridge (src/telemetry_bridge.cpp), which is what feeds the 3D
 * visualizer.
 *
 * Responsibility: a second shared-memory region (same owner/guest
 * create-vs-open convention as shared_stats.hpp - the receiver owns
 * and creates it, telemetry_bridge only opens it) holding a fixed-size
 * circular buffer of small Event records: one per tick as it crosses a
 * pipeline stage (received, enqueued, processed, gap detected,
 * recovered). The receiver and its workers are the writers;
 * telemetry_bridge polls write_index and only reads NEW events each
 * tick, turning them into JSON for the browser.
 *
 * Thread/process-safety and its one honest gap: every writer reserves
 * a slot via `write_index.fetch_add(1, relaxed)` - two writers can
 * never collide on the same slot, since fetch_add hands out a unique
 * index to each caller. What is NOT synchronized is the handoff to the
 * reader: there is a brief window, between a writer reserving its slot
 * and finishing the field writes into it, during which write_index has
 * already advanced. A reader polling at exactly the wrong instant can
 * observe a slot whose fields are only partially written. This is the
 * same class of "racy by design" tradeoff as QueueMutex::depth() and
 * SpscRing::depth() - acceptable for best-effort visualization, not
 * something this ring is trying to make airtight. On wraparound, old
 * events are silently overwritten (it is a ring, not a log).
 */

#ifndef PULSEFORGE_SHARED_EVENTS_HPP
#define PULSEFORGE_SHARED_EVENTS_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PULSEFORGE_EVENTS_SHM_DEFAULT_NAME "/pulseforge_events"

constexpr size_t kEventRingCapacity = 16384;

enum class EventType : uint8_t {
    Received  = 0,
    Invalid   = 1,
    Gap       = 2,
    Recovered = 3,
    Processed = 4,
    QueueFull = 5,
};

struct Event {
    uint8_t  type;
    uint8_t  _pad[3];
    uint32_t worker_id;  /* meaningful for Processed; 0 otherwise */
    uint64_t sequence;
    uint64_t ts_ns;
    uint64_t latency_ns; /* meaningful for Processed/Recovered; 0 otherwise */
};

/*
 * SharedConfig - which networking/concurrency options THIS run of the
 * receiver actually chose (--transport, --queue, --batch, --busy-poll,
 * --bpf-filter, --snapshot-host), written ONCE at startup so the
 * visualizer can show real, live configuration instead of a static
 * guess. Written entirely before `ready` is set (release); readers
 * must check `ready` (acquire) first - the same happens-before pattern
 * spsc_ring.hpp uses for its producer/consumer handoff, applied here
 * to a single one-shot "publish" instead of a per-message one. Without
 * that gate, a reader could observe `ready == 0` (fine, it waits) or
 * `ready == 1` with some of the plain int fields still torn/stale -
 * the ordering guarantee is exactly what the release/acquire pair
 * rules out.
 */
struct SharedConfig {
    /* No default member initializer: this struct is never constructed
     * through normal C++ object lifetime, only reinterpret_cast onto
     * raw mmap'd bytes (same as every other shared-memory struct in
     * this codebase - see stats.hpp's top comment) - an initializer
     * here would just be dead code. event_ring_reset() is what
     * actually zeroes `ready` on the owner's create() path. */
    std::atomic<int> ready;
    int transport;             /* 0 = unicast, 1 = multicast           */
    int queue_kind;            /* 0 = mutex, 1 = spsc, 2 = disruptor   */
    int workers;
    int batch;                 /* recvmmsg batch size; 1 = plain recvfrom */
    int busy_poll_us;          /* -1 = SO_BUSY_POLL not set            */
    int bpf_filter;            /* 0/1: classic-BPF filter attached     */
    int recovery_enabled;      /* 0/1: --snapshot-host given           */
    int port;
    char multicast_group[32];
};

struct SharedEventRing {
    SharedConfig           config;
    std::atomic<uint64_t> write_index; /* total events ever recorded */
    Event                 events[kEventRingCapacity];
};

inline void event_ring_reset(SharedEventRing &ring) {
    ring.write_index.store(0, std::memory_order_relaxed);
    ring.config.ready.store(0, std::memory_order_relaxed);
}

inline void shared_config_set(SharedEventRing &ring, int transport, int queue_kind,
                              int workers, int batch, int busy_poll_us, int bpf_filter,
                              int recovery_enabled, int port,
                              const std::string &multicast_group) {
    SharedConfig &c = ring.config;
    c.transport         = transport;
    c.queue_kind        = queue_kind;
    c.workers           = workers;
    c.batch             = batch;
    c.busy_poll_us      = busy_poll_us;
    c.bpf_filter        = bpf_filter;
    c.recovery_enabled  = recovery_enabled;
    c.port              = port;
    std::snprintf(c.multicast_group, sizeof(c.multicast_group), "%s",
                  multicast_group.c_str());
    c.ready.store(1, std::memory_order_release);
}

inline void event_ring_record(SharedEventRing &ring, EventType type,
                              uint64_t sequence, uint64_t ts_ns,
                              uint64_t latency_ns = 0, uint32_t worker_id = 0) {
    uint64_t idx = ring.write_index.fetch_add(1, std::memory_order_relaxed);
    Event &slot      = ring.events[idx % kEventRingCapacity];
    slot.type        = static_cast<uint8_t>(type);
    slot.worker_id   = worker_id;
    slot.sequence    = sequence;
    slot.ts_ns       = ts_ns;
    slot.latency_ns  = latency_ns;
}

/*
 * RAII shared-memory wrapper for SharedEventRing, structurally
 * identical to shared_stats.hpp's SharedStatsRegion (same owner/guest
 * split, same header-only-to-avoid-ODR-issues reasoning). Kept as a
 * separate region/name from the stats region so a telemetry consumer
 * that only cares about events doesn't need to know the stats layout,
 * and vice versa.
 */
class SharedEventRegion {
public:
    /* Default-constructs to the "disabled" falsy state - unlike
     * SharedStatsRegion (always created), telemetry is opt-in
     * (--telemetry), so receiver.cpp needs a public default
     * constructor to declare the variable before conditionally
     * assigning create()'s result. */
    SharedEventRegion() = default;

    static SharedEventRegion create(const std::string &name) {
        shm_unlink(name.c_str()); /* clear any stale object; ignore ENOENT */
        int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            std::perror("shm_open(events, create)");
            return SharedEventRegion();
        }
        if (ftruncate(fd, sizeof(SharedEventRing)) != 0) {
            std::perror("ftruncate(events)");
            close(fd);
            return SharedEventRegion();
        }
        void *addr = mmap(nullptr, sizeof(SharedEventRing),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (addr == MAP_FAILED) {
            std::perror("mmap(events)");
            return SharedEventRegion();
        }
        auto *ring = static_cast<SharedEventRing *>(addr);
        event_ring_reset(*ring);
        return SharedEventRegion(name, ring);
    }

    static SharedEventRegion open(const std::string &name) {
        int fd = shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            std::perror("shm_open(events, open)");
            return SharedEventRegion();
        }
        void *addr = mmap(nullptr, sizeof(SharedEventRing),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (addr == MAP_FAILED) {
            std::perror("mmap(events)");
            return SharedEventRegion();
        }
        return SharedEventRegion(name, static_cast<SharedEventRing *>(addr));
    }

    ~SharedEventRegion() { release(); }

    SharedEventRegion(SharedEventRegion &&other) noexcept
        : name_(std::move(other.name_)), ring_(other.ring_) {
        other.ring_ = nullptr;
    }
    SharedEventRegion &operator=(SharedEventRegion &&other) noexcept {
        if (this != &other) {
            release();
            name_       = std::move(other.name_);
            ring_       = other.ring_;
            other.ring_ = nullptr;
        }
        return *this;
    }
    SharedEventRegion(const SharedEventRegion &)            = delete;
    SharedEventRegion &operator=(const SharedEventRegion &) = delete;

    SharedEventRing *get() const noexcept { return ring_; }
    SharedEventRing &operator*() const noexcept { return *ring_; }
    SharedEventRing *operator->() const noexcept { return ring_; }
    explicit operator bool() const noexcept { return ring_ != nullptr; }

    /* OWNER only. */
    void unlink() {
        if (!name_.empty()) {
            shm_unlink(name_.c_str());
        }
    }

private:
    SharedEventRegion(const std::string &name, SharedEventRing *ring)
        : name_(name), ring_(ring) {}

    void release() noexcept {
        if (ring_ != nullptr) {
            munmap(ring_, sizeof(SharedEventRing));
            ring_ = nullptr;
        }
    }

    std::string      name_;
    SharedEventRing *ring_ = nullptr;
};

#endif /* PULSEFORGE_SHARED_EVENTS_HPP */
