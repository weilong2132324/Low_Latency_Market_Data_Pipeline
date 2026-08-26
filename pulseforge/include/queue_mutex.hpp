/*
 * queue_mutex.hpp - bounded FIFO queue using a std::mutex + two
 * std::condition_variables.
 *
 * Responsibility: safe message handoff between the receiver (producer)
 * and worker thread(s) (consumers), with backpressure and clean
 * shutdown.
 *
 * Thread-safety: fully thread-safe (safe for many producers / many
 * consumers, i.e. MPSC or MPMC). All public calls are guarded by an
 * internal std::mutex; push/pop may block.
 *
 * Ownership: the QueueMutex is a value type you create on the stack or
 * heap; RAII destroys the internal storage. It is move-disabled (it
 * owns a mutex and condition variables, which are non-movable).
 *
 * Why `wait(lock, pred)` and not `if` around every wait:
 *   - Spurious wakeups are allowed by the standard: a thread can wake
 *     from condition_variable::wait() with no signal having been sent.
 *     The predicate overload re-checks the condition in a loop - the
 *     only correct pattern.
 *   - The predicate can also change between the notify and the woken
 *     thread re-acquiring the mutex (another thread may drain/fill the
 *     queue first).
 */

#ifndef PULSEFORGE_QUEUE_MUTEX_HPP
#define PULSEFORGE_QUEUE_MUTEX_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

#include "message.hpp"

class QueueMutex {
public:
    /* Return codes for push / try_push / pop. */
    enum class Status : int {
        Ok     = 0,  /* success */
        Full   = 1,  /* try_push only: capacity reached */
        Closed = -1, /* queue is closed */
    };

    /* Create a queue holding up to `capacity` messages. */
    explicit QueueMutex(size_t capacity);

    /* No custom teardown needed: std::vector + std::mutex unwind by
     * themselves. The caller must join all threads blocked on the queue
     * (after close()) before the object goes out of scope. */
    ~QueueMutex();

    QueueMutex(const QueueMutex &) = delete;
    QueueMutex &operator=(const QueueMutex &) = delete;

    /* Mark the queue closed and wake every blocked push/pop. After this:
     *   - push/try_push fail with Status::Closed,
     *   - pop keeps draining the remaining items, then returns
     *     Status::Closed once the queue is empty.
     * This is how shutdown wakes blocked producers AND consumers. */
    void close();

    /* Blocking push; returns Status::Ok or Status::Closed. Waits while
     * full (this is backpressure: a full queue makes the producer wait
     * instead of growing memory or dropping messages). */
    Status push(const TickMessage &msg);

    /* Non-blocking push; returns Status::Ok, Status::Full, or
     * Status::Closed. */
    Status try_push(const TickMessage &msg);

    /* Blocking pop; returns Status::Ok, or Status::Closed when closed &
     * empty. On success `msg` receives the dequeued message. */
    Status pop(TickMessage &msg);

    /* Number of messages currently queued. */
    size_t depth() const;

    /* Maximum number of messages the queue can hold. */
    size_t capacity() const { return capacity_; }

private:
    mutable std::mutex lock_;
    std::condition_variable not_empty_; /* signaled when a msg is pushed */
    std::condition_variable not_full_;  /* signaled when a msg is popped  */
    std::vector<TickMessage> buf_;      /* preallocated fixed-size storage */
    size_t capacity_;
    size_t head_ = 0;   /* index of next pop  (FIFO) */
    size_t tail_ = 0;   /* index of next push        */
    size_t count_ = 0;  /* messages currently queued */
    bool closed_ = false;
};

#endif /* PULSEFORGE_QUEUE_MUTEX_HPP */
