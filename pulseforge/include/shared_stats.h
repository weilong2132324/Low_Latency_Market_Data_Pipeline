/*
 * shared_stats.h - lifecycle helpers for the shared-stats region.
 *
 * Responsibility: create/open/map/unmap/unlink the POSIX shared-memory
 * object that holds a SharedStats.
 *
 * Ownership:
 *   - The RECEIVER is the owner: it calls shared_stats_create() and,
 *     on shutdown, shared_stats_unlink() (the object is then removed
 *     once the last mapping is gone).
 *   - The MONITOR is a guest: it calls shared_stats_open()/close()
 *     only, and never unlinks.
 *
 * Why header-only static inline functions? The required tree has
 * include/shared_stats.h but no src/shared_stats.c. These helpers are
 * small and self-contained, so inlining them (like message.h's
 * validator) keeps the layout exact. Each TU that uses them gets its
 * own private copy - fine for a lab.
 *
 * Thread-safety: the mapping/object helpers are process-global; call
 * them once at startup/teardown, not from hot paths.
 */

#ifndef PULSEFORGE_SHARED_STATS_H
#define PULSEFORGE_SHARED_STATS_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stats.h"

/* OWNER (receiver): create (or recreate) the shm object, size it with
 * ftruncate, and map it MAP_SHARED so changes are visible to other
 * processes. Returns a pointer to the mapped region, or NULL on
 * failure. On success *size_out (if non-NULL) receives the region size.
 *
 * We unlink any stale object from a crashed previous run first so a
 * restart always gets a clean, zeroed region. */
static inline SharedStats *shared_stats_create(const char *name,
                                               size_t *size_out) {
    size_t sz = sizeof(SharedStats);
    (void)shm_unlink(name); /* ignore ENOENT */

    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open(O_CREAT|O_RDWR)");
        return NULL;
    }
    if (ftruncate(fd, (off_t)sz) != 0) {
        perror("ftruncate");
        close(fd);
        (void)shm_unlink(name);
        return NULL;
    }
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(fd);
        (void)shm_unlink(name);
        return NULL;
    }
    close(fd);

    shared_stats_reset((SharedStats *)p);
    if (size_out) {
        *size_out = sz;
    }

    if (ATOMIC_LLONG_LOCK_FREE != 2) {
        fprintf(stderr,
                "warning: _Atomic uint64_t is not guaranteed lock-free here "
                "(%d); cross-process atomics would be unsafe\n",
                ATOMIC_LLONG_LOCK_FREE);
    }
    return (SharedStats *)p;
}

/* GUEST (monitor): open and map an existing object. NULL on failure.
 * Does not create, and must not unlink. */
static inline SharedStats *shared_stats_open(const char *name) {
    size_t sz = sizeof(SharedStats);
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        perror("shm_open(O_RDWR)");
        return NULL;
    }
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return NULL;
    }
    close(fd);
    return (SharedStats *)p;
}

/* Both roles: release the mapping. */
static inline void shared_stats_close(SharedStats *s) {
    if (s != NULL) {
        if (munmap(s, sizeof(SharedStats)) != 0) {
            perror("munmap");
        }
    }
}

/* Only the OWNER unlinks: removes the object once the last mapping is
 * closed. */
static inline void shared_stats_unlink(const char *name) {
    if (shm_unlink(name) != 0) {
        perror("shm_unlink");
    }
}

#endif /* PULSEFORGE_SHARED_STATS_H */
