#include "queue_backend.hpp"

#include <thread>

#include "mpmc_fanout.hpp"
#include "queue_mutex.hpp"
#include "spsc_ring.hpp"

const char *queue_kind_name(QueueKind kind) {
    switch (kind) {
    case QueueKind::Mutex:     return "mutex";
    case QueueKind::Spsc:      return "spsc";
    case QueueKind::Disruptor: return "disruptor";
    }
    return "unknown";
}

namespace {

/* How many times to spin on try_pop() before yielding the CPU. Lock-free
 * backends have no condition variable to sleep on, so an idle consumer
 * either burns CPU spinning (lowest latency to react) or yields
 * (lower CPU, a little more latency) - this picks "mostly spin, yield
 * occasionally" as a middle ground, and is the concrete trade-off the
 * README/bench numbers should call out: lock-free queues trade idle CPU
 * for lower wake-up latency. */
constexpr int kSpinsBeforeYield = 1000;

/* ---- QueueMutex adapter: work-sharing, blocks on its own condvar ---- */

class MutexProducer : public IQueueProducer {
public:
    explicit MutexProducer(std::shared_ptr<QueueMutex> q) : q_(std::move(q)) {}

    PushStatus try_push(const TickMessage &msg) override {
        switch (q_->try_push(msg)) {
        case QueueMutex::Status::Ok:     return PushStatus::Ok;
        case QueueMutex::Status::Full:   return PushStatus::Full;
        case QueueMutex::Status::Closed: return PushStatus::Closed;
        }
        return PushStatus::Closed;
    }
    void close() override { q_->close(); }
    size_t depth() const override { return q_->depth(); }

private:
    std::shared_ptr<QueueMutex> q_;
};

class MutexConsumer : public IQueueConsumer {
public:
    explicit MutexConsumer(std::shared_ptr<QueueMutex> q) : q_(std::move(q)) {}

    PopStatus pop(TickMessage &msg) override {
        return q_->pop(msg) == QueueMutex::Status::Closed ? PopStatus::Closed
                                                           : PopStatus::Ok;
    }
    size_t depth() const override { return q_->depth(); }

private:
    std::shared_ptr<QueueMutex> q_;
};

/* ---- SpscRing adapter: work-sharing but single-consumer only ---- */

class SpscProducer : public IQueueProducer {
public:
    explicit SpscProducer(std::shared_ptr<SpscRing> r) : r_(std::move(r)) {}

    PushStatus try_push(const TickMessage &msg) override {
        switch (r_->try_push(msg)) {
        case SpscRing::Status::Ok:     return PushStatus::Ok;
        case SpscRing::Status::Full:   return PushStatus::Full;
        case SpscRing::Status::Closed: return PushStatus::Closed;
        }
        return PushStatus::Closed;
    }
    void close() override { r_->close(); }
    size_t depth() const override { return r_->depth(); }

private:
    std::shared_ptr<SpscRing> r_;
};

class SpscConsumer : public IQueueConsumer {
public:
    explicit SpscConsumer(std::shared_ptr<SpscRing> r) : r_(std::move(r)) {}

    PopStatus pop(TickMessage &msg) override {
        int spins = 0;
        while (!r_->try_pop(msg)) {
            if (r_->closed_and_empty()) {
                return PopStatus::Closed;
            }
            if (++spins >= kSpinsBeforeYield) {
                std::this_thread::yield();
                spins = 0;
            }
        }
        return PopStatus::Ok;
    }
    size_t depth() const override { return r_->depth(); }

private:
    std::shared_ptr<SpscRing> r_;
};

/* ---- MpmcFanout adapter: broadcast, one reader_id per consumer ---- */

class DisruptorProducer : public IQueueProducer {
public:
    explicit DisruptorProducer(std::shared_ptr<MpmcFanout> f)
        : f_(std::move(f)) {}

    PushStatus try_push(const TickMessage &msg) override {
        switch (f_->try_push(msg)) {
        case MpmcFanout::Status::Ok:     return PushStatus::Ok;
        case MpmcFanout::Status::Full:   return PushStatus::Full;
        case MpmcFanout::Status::Closed: return PushStatus::Closed;
        }
        return PushStatus::Closed;
    }
    void close() override { f_->close(); }
    size_t depth() const override { return f_->depth(); }

private:
    std::shared_ptr<MpmcFanout> f_;
};

class DisruptorConsumer : public IQueueConsumer {
public:
    DisruptorConsumer(std::shared_ptr<MpmcFanout> f, size_t reader_id)
        : f_(std::move(f)), reader_id_(reader_id) {}

    PopStatus pop(TickMessage &msg) override {
        int spins = 0;
        while (!f_->try_pop(reader_id_, msg)) {
            if (f_->closed_and_empty(reader_id_)) {
                return PopStatus::Closed;
            }
            if (++spins >= kSpinsBeforeYield) {
                std::this_thread::yield();
                spins = 0;
            }
        }
        return PopStatus::Ok;
    }
    size_t depth() const override { return f_->reader_depth(reader_id_); }

private:
    std::shared_ptr<MpmcFanout> f_;
    size_t reader_id_;
};

} // namespace

QueueBackend make_queue_backend(QueueKind kind, size_t capacity,
                                 size_t num_workers) {
    QueueBackend backend;
    switch (kind) {
    case QueueKind::Mutex: {
        auto q = std::make_shared<QueueMutex>(capacity);
        backend.producer = std::make_unique<MutexProducer>(q);
        for (size_t i = 0; i < num_workers; i++) {
            backend.consumers.push_back(std::make_unique<MutexConsumer>(q));
        }
        break;
    }
    case QueueKind::Spsc: {
        auto r = std::make_shared<SpscRing>(capacity);
        backend.producer = std::make_unique<SpscProducer>(r);
        for (size_t i = 0; i < num_workers; i++) {
            backend.consumers.push_back(std::make_unique<SpscConsumer>(r));
        }
        break;
    }
    case QueueKind::Disruptor: {
        auto f = std::make_shared<MpmcFanout>(capacity, num_workers);
        backend.producer = std::make_unique<DisruptorProducer>(f);
        for (size_t i = 0; i < num_workers; i++) {
            backend.consumers.push_back(
                std::make_unique<DisruptorConsumer>(f, i));
        }
        break;
    }
    }
    return backend;
}
