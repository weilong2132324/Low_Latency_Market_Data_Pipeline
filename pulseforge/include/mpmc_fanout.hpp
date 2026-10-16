/*
 * mpmc_fanout.hpp - Disruptor-style single-writer, multi-reader
 * broadcast ring.
 *
 * Responsibility: unlike QueueMutex/SpscRing, where N workers *compete*
 * for each message (every message is consumed exactly once), this ring
 * *broadcasts*: every reader sees every message. This models the
 * realistic shape of a market-data feed with several independent
 * downstream consumers reading the same tape - e.g. a book-builder, a
 * risk check, and a logger, each at its own pace, none stealing work
 * from another.
 *
 * Thread-safety: exactly ONE thread may call try_push() (the receiver).
 * Each reader index in [0, num_readers) may be polled by exactly one
 * thread, but different readers may run concurrently with each other
 * and with the writer.
 *
 * Backpressure: the writer may not overwrite a slot that the SLOWEST
 * reader has not yet consumed. try_push() computes the minimum read
 * cursor across all readers and refuses (Status::Full) if writing would
 * lap it. This is the one place this design costs more than a plain
 * SPSC ring: an O(num_readers) scan per push. For the handful of
 * readers this project uses that's negligible; a real Disruptor caches
 * the minimum and only rescans when the cache is stale.
 *
 * Memory ordering: same acquire/release handoff reasoning as
 * spsc_ring.hpp, applied between the writer and EACH reader
 * independently.
 */

#ifndef PULSEFORGE_MPMC_FANOUT_HPP
#define PULSEFORGE_MPMC_FANOUT_HPP

#include <atomic>
#include <cstddef>
#include <vector>

#include "message.hpp"

class MpmcFanout {
public:
    enum class Status : int {
        Ok     = 0,
        Full   = 1,
        Closed = -1,
    };

    MpmcFanout(size_t capacity, size_t num_readers);

    MpmcFanout(const MpmcFanout &) = delete;
    MpmcFanout &operator=(const MpmcFanout &) = delete;

    /* Writer-only. */
    void close();

    /* Writer-only; never blocks. */
    Status try_push(const TickMessage &msg);

    /* Reader-only (one thread per reader_id); never blocks. Returns
     * false if that reader has caught up to the writer (nothing new
     * yet) - check closed_and_empty(reader_id) to distinguish that from
     * "done". */
    bool try_pop(size_t reader_id, TickMessage &msg);

    bool closed_and_empty(size_t reader_id) const;

    size_t capacity() const { return mask_ + 1; }
    size_t num_readers() const { return read_seq_.size(); }

    /* Approximate backlog for one reader (how far behind the writer it
     * is); approximate depth across the slowest reader. Both racy by
     * design, same spirit as QueueMutex::depth(). */
    size_t reader_depth(size_t reader_id) const {
        return write_seq_.load(std::memory_order_relaxed) -
               read_seq_[reader_id].load(std::memory_order_relaxed);
    }
    size_t depth() const {
        return write_seq_.load(std::memory_order_relaxed) - min_read_seq();
    }

private:
    size_t min_read_seq() const;

    std::atomic<size_t> write_seq_{0};
    std::atomic<bool> closed_{false};
    std::vector<std::atomic<size_t>> read_seq_; /* one cursor per reader */
    std::vector<TickMessage> buf_;
    size_t mask_;
};

#endif /* PULSEFORGE_MPMC_FANOUT_HPP */
