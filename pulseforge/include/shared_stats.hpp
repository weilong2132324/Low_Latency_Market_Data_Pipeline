/*
 * shared_stats.hpp - lifecycle helpers for the shared-stats region.
 *
 * Responsibility: create/open/map/unmap/unlink the POSIX shared-memory
 * object that holds a SharedStats, wrapped in a small RAII class.
 *
 * Ownership:
 *   - The RECEIVER is the owner: it calls SharedStatsRegion::create()
 *     and, on shutdown, unlink() (the object is then removed once the
 *     last mapping is gone).
 *   - The MONITOR is a guest: it calls SharedStatsRegion::open() only
 *     and never unlinks.
 *
 * Why header-only? Keeping the class inline (like message.hpp's
 * validator) keeps the layout exact and the build simple. The class
 * methods are `inline`, so multiple TUs sharing this header do not
 * violate the ODR.
 *
 * Thread-safety: the mapping/object helpers are process-global; call
 * them once at startup/teardown, not from hot paths.
 */

#ifndef PULSEFORGE_SHARED_STATS_HPP
#define PULSEFORGE_SHARED_STATS_HPP

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "stats.hpp"

class SharedStatsRegion {
public:
    /* OWNER (receiver): create (or recreate) the shm object, size it
     * with ftruncate, and map it MAP_SHARED so changes are visible to
     * other processes. Returns a region whose operator bool() is false
     * on failure. Any stale object from a crashed previous run is
     * unlinked first so a restart always gets a clean, zeroed region. */
    static SharedStatsRegion create(const std::string &name) {
        const size_t sz = sizeof(SharedStats);
        (void)shm_unlink(name.c_str()); /* ignore ENOENT */

        int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            std::perror("shm_open(O_CREAT|O_RDWR)");
            return SharedStatsRegion{};
        }
        if (ftruncate(fd, static_cast<off_t>(sz)) != 0) {
            std::perror("ftruncate");
            ::close(fd);
            (void)shm_unlink(name.c_str());
            return SharedStatsRegion{};
        }
        void *p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            std::perror("mmap");
            ::close(fd);
            (void)shm_unlink(name.c_str());
            return SharedStatsRegion{};
        }
        ::close(fd);

        auto *stats = static_cast<SharedStats *>(p);
        shared_stats_reset(*stats);

        if (!std::atomic<uint64_t>::is_always_lock_free) {
            std::fprintf(stderr,
                         "warning: std::atomic<uint64_t> is not guaranteed "
                         "lock-free here; cross-process atomics would be "
                         "unsafe\n");
        }
        return SharedStatsRegion{name, stats};
    }

    /* GUEST (monitor): open and map an existing object. operator bool()
     * is false on failure. Does not create, and must not unlink. */
    static SharedStatsRegion open(const std::string &name) {
        const size_t sz = sizeof(SharedStats);
        int fd = shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            std::perror("shm_open(O_RDWR)");
            return SharedStatsRegion{};
        }
        void *p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            std::perror("mmap");
            ::close(fd);
            return SharedStatsRegion{};
        }
        ::close(fd);
        return SharedStatsRegion{name, static_cast<SharedStats *>(p)};
    }

    /* Both roles: release the mapping (RAII - no explicit close call). */
    ~SharedStatsRegion() { release(); }

    SharedStatsRegion(SharedStatsRegion &&other) noexcept
        : name_(std::move(other.name_)), stats_(other.stats_) {
        other.stats_ = nullptr;
    }

    SharedStatsRegion &operator=(SharedStatsRegion &&other) noexcept {
        if (this != &other) {
            release();
            name_ = std::move(other.name_);
            stats_ = other.stats_;
            other.stats_ = nullptr;
        }
        return *this;
    }

    SharedStatsRegion(const SharedStatsRegion &) = delete;
    SharedStatsRegion &operator=(const SharedStatsRegion &) = delete;

    /* Accessors. */
    SharedStats *get() const noexcept { return stats_; }
    SharedStats &operator*() const noexcept { return *stats_; }
    SharedStats *operator->() const noexcept { return stats_; }
    explicit operator bool() const noexcept { return stats_ != nullptr; }

    /* Only the OWNER unlinks: removes the object once the last mapping
     * is closed. */
    void unlink() {
        if (!name_.empty() && shm_unlink(name_.c_str()) != 0) {
            std::perror("shm_unlink");
        }
    }

private:
    SharedStatsRegion() = default;
    SharedStatsRegion(const std::string &name, SharedStats *stats)
        : name_(name), stats_(stats) {}

    void release() noexcept {
        if (stats_ != nullptr) {
            if (munmap(stats_, sizeof(SharedStats)) != 0) {
                std::perror("munmap");
            }
            stats_ = nullptr;
        }
    }

    std::string name_;
    SharedStats *stats_ = nullptr;
};

#endif /* PULSEFORGE_SHARED_STATS_HPP */
