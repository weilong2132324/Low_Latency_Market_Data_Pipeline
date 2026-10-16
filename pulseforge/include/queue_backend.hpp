/*
 * queue_backend.hpp - common interface + factory over the three queue
 * implementations (QueueMutex, SpscRing, MpmcFanout).
 *
 * Responsibility: let receiver.cpp (and bench/queue_bench.cpp) pick a
 * queue backend with one --queue flag / one factory call, instead of an
 * if/else ladder sprinkled through main(). This is the project's
 * "factory pattern" example: make_queue_backend() hides which concrete
 * class gets constructed behind QueueKind.
 *
 * Shape: one producer handle (the receiver thread) + exactly
 * num_workers consumer handles (one per worker thread).
 *   - Mutex / Spsc: work-SHARING. All consumer handles pull from the
 *     same underlying queue; each message is processed by exactly one
 *     worker. (Spsc requires num_workers == 1 - it is single-consumer
 *     by construction; make_queue_backend() does not itself enforce
 *     this, callers must validate before calling, same as other CLI
 *     validation in this codebase.)
 *   - Disruptor: work-BROADCASTING. Every consumer handle is a distinct
 *     reader_id into one MpmcFanout, so every worker sees every
 *     message independently.
 *
 * Ownership: the returned QueueBackend owns the underlying queue via
 * shared_ptr (the producer handle and every consumer handle share one
 * instance), so the queue stays alive exactly as long as any handle
 * referencing it. close() + join all worker threads before letting the
 * QueueBackend go out of scope.
 *
 * Thread-safety: the producer handle is used by exactly one thread (the
 * receiver); each consumer handle is used by exactly one worker thread.
 */

#ifndef PULSEFORGE_QUEUE_BACKEND_HPP
#define PULSEFORGE_QUEUE_BACKEND_HPP

#include <cstddef>
#include <memory>
#include <vector>

#include "message.hpp"

enum class QueueKind { Mutex, Spsc, Disruptor };

const char *queue_kind_name(QueueKind kind);

enum class PushStatus { Ok, Full, Closed };
enum class PopStatus { Ok, Closed };

class IQueueProducer {
public:
    virtual ~IQueueProducer() = default;
    virtual PushStatus try_push(const TickMessage &msg) = 0;
    virtual void close()                                = 0;
    virtual size_t depth() const                         = 0;
};

class IQueueConsumer {
public:
    virtual ~IQueueConsumer() = default;
    /* Blocks (via condition variable for Mutex, via spin+yield for the
     * lock-free backends - see queue_backend.cpp) until a message is
     * available or the queue is closed and drained. */
    virtual PopStatus pop(TickMessage &msg) = 0;
    virtual size_t depth() const            = 0;
};

struct QueueBackend {
    std::unique_ptr<IQueueProducer> producer;
    std::vector<std::unique_ptr<IQueueConsumer>> consumers;
};

/* capacity applies to the whole backend (QueueMutex/SpscRing slot count,
 * or the MpmcFanout ring size for Disruptor). consumers.size() ==
 * num_workers always, regardless of which kind is chosen. */
QueueBackend make_queue_backend(QueueKind kind, size_t capacity,
                                 size_t num_workers);

#endif /* PULSEFORGE_QUEUE_BACKEND_HPP */
