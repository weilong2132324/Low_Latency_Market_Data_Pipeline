/*
 * queue_mutex.cpp - implementation of the mutex + condvar bounded queue.
 *
 * Responsibility: bounded FIFO with blocking push/pop, backpressure,
 * and cooperative shutdown.
 *
 * Thread-safety: all public methods are safe from any thread; guarded
 * by an internal std::mutex. push/pop may block on their condition
 * variable.
 *
 * Why NOT busy-spin: condition variables put the thread to sleep and
 * let the OS schedule it only when there is work - they burn ~zero CPU
 * while waiting. Busy-spinning a full/empty queue would waste a core
 * for no latency benefit at this scale.
 */

#include "queue_mutex.hpp"

QueueMutex::QueueMutex(size_t capacity)
    : buf_(capacity), capacity_(capacity) {}

QueueMutex::~QueueMutex() = default;

void QueueMutex::close() {
    std::lock_guard<std::mutex> lock(lock_);
    closed_ = true;
    /* Wake everyone: producers blocked on a full queue and consumers
     * blocked on an empty one. They re-check their predicates and see
     * `closed`, then return Status::Closed. */
    not_empty_.notify_all();
    not_full_.notify_all();
}

QueueMutex::Status QueueMutex::push(const TickMessage &msg) {
    std::unique_lock<std::mutex> lock(lock_);
    not_full_.wait(lock, [this] { return count_ < capacity_ || closed_; });
    if (closed_) {
        return Status::Closed;
    }
    buf_[tail_] = msg;
    tail_ = (tail_ + 1) % capacity_;
    ++count_;
    not_empty_.notify_one();
    return Status::Ok;
}

QueueMutex::Status QueueMutex::try_push(const TickMessage &msg) {
    std::lock_guard<std::mutex> lock(lock_);
    if (closed_) {
        return Status::Closed;
    }
    if (count_ == capacity_) {
        return Status::Full;
    }
    buf_[tail_] = msg;
    tail_ = (tail_ + 1) % capacity_;
    ++count_;
    not_empty_.notify_one();
    return Status::Ok;
}

QueueMutex::Status QueueMutex::pop(TickMessage &msg) {
    std::unique_lock<std::mutex> lock(lock_);
    not_empty_.wait(lock, [this] { return count_ > 0 || closed_; });
    if (count_ == 0 && closed_) {
        return Status::Closed;
    }
    msg = buf_[head_];
    head_ = (head_ + 1) % capacity_;
    --count_;
    not_full_.notify_one();
    return Status::Ok;
}

size_t QueueMutex::depth() const {
    std::lock_guard<std::mutex> lock(lock_);
    return count_;
}
