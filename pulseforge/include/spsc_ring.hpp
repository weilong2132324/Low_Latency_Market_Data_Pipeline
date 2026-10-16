/*
 * spsc_ring.hpp - lock-free bounded single-producer/single-consumer ring
 * buffer of TickMessage.
 *
 * Responsibility: the same handoff job as QueueMutex (receiver -> one
 * worker), but without a mutex or condition variable: the only
 * synchronization is two std::atomic<size_t> cursors.
 *
 * Thread-safety: exactly ONE thread may call push()/try_push(), and
 * exactly ONE (possibly different) thread may call pop(). Calling push()
 * from two threads concurrently, or pop() from two threads concurrently,
 * is undefined behavior - this is the trade a SPSC ring makes for
 * speed. (For N workers see mpmc_fanout.hpp, which gives each worker its
 * own ring instead of sharing one.)
 *
 * Ownership: value type; the backing storage is a preallocated
 * std::vector sized to the next power of two >= capacity (so the index
 * wrap is `& mask_` instead of `% capacity_`).
 *
 * Memory ordering - why this needs acquire/release, unlike SharedStats:
 *   - SharedStats counters are independent; nothing else about program
 *     order depends on them, so relaxed is correct and cheaper.
 *   - Here, correctness depends on *ordering*: when the consumer reads
 *     tail_.load(acquire) and sees the producer's new value, it must
 *     also see the message bytes the producer wrote into buf_[tail]
 *     *before* publishing that tail. release on the producer's store
 *     and acquire on the consumer's load create that happens-before
 *     edge. Relaxed would let the compiler/CPU reorder the write to
 *     buf_[] after the tail_ update, so the consumer could read a
 *     half-written message.
 *   - Symmetrically, the producer's head_.load(acquire) must see the
 *     consumer's release-store so it knows the slot it's about to
 *     overwrite has truly been consumed.
 */

#ifndef PULSEFORGE_SPSC_RING_HPP
#define PULSEFORGE_SPSC_RING_HPP

#include <atomic>
#include <cstddef>
#include <vector>

#include "message.hpp"

class SpscRing {
public:
    enum class Status : int {
        Ok     = 0,
        Full   = 1,
        Closed = -1,
    };

    /* Storage is sized to the next power of two >= capacity. */
    explicit SpscRing(size_t capacity);

    SpscRing(const SpscRing &) = delete;
    SpscRing &operator=(const SpscRing &) = delete;

    /* Producer-only. No condition variable to wake, so close() is just a
     * flag the consumer's pop() observes once it has drained the ring. */
    void close();

    /* Producer-only; never blocks. Returns Status::Full if the ring is
     * at capacity, Status::Closed if close() has been called. */
    Status try_push(const TickMessage &msg);

    /* Consumer-only; never blocks. Returns false if the ring is
     * currently empty (whether or not it's closed - check
     * closed_and_empty() to distinguish "empty for now" from "done"). */
    bool try_pop(TickMessage &msg);

    /* True once close() has been called AND the ring has been fully
     * drained by the consumer - mirrors QueueMutex's "closed" state for
     * callers that want a single shutdown check. */
    bool closed_and_empty() const;

    size_t capacity() const { return mask_ + 1; }

    /* Approximate depth for a stats gauge; racy by design (same spirit
     * as QueueMutex::depth(), just without a lock to make it exact). */
    size_t depth() const {
        return tail_.load(std::memory_order_relaxed) -
               head_.load(std::memory_order_relaxed);
    }

private:
    /* Cache-line padding: the producer touches tail_ on every push, the
     * consumer touches head_ on every pop. Without padding, both
     * cursors would share one cache line and every push/pop would
     * bounce it between cores (false sharing) even though the two
     * threads never touch the same logical field. */
    alignas(64) std::atomic<size_t> head_{0}; /* next slot to pop  */
    char _pad1[64 - sizeof(std::atomic<size_t>)];
    alignas(64) std::atomic<size_t> tail_{0}; /* next slot to push */
    char _pad2[64 - sizeof(std::atomic<size_t>)];
    alignas(64) std::atomic<bool> closed_{false};

    std::vector<TickMessage> buf_;
    size_t mask_; /* capacity (power of two) - 1 */
};

#endif /* PULSEFORGE_SPSC_RING_HPP */
