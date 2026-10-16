#include "spsc_ring.hpp"

namespace {
size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}
} // namespace

SpscRing::SpscRing(size_t capacity)
    : buf_(next_pow2(capacity)), mask_(next_pow2(capacity) - 1) {}

void SpscRing::close() {
    /* release: any producer writes before close() must be visible to
     * the consumer once it observes closed_ == true. */
    closed_.store(true, std::memory_order_release);
}

SpscRing::Status SpscRing::try_push(const TickMessage &msg) {
    if (closed_.load(std::memory_order_relaxed)) {
        return Status::Closed;
    }
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (tail - head >= buf_.size()) {
        return Status::Full;
    }
    buf_[tail & mask_] = msg;
    /* release: publish the written slot before advancing tail_, so the
     * consumer's acquire-load of tail_ guarantees it sees the message. */
    tail_.store(tail + 1, std::memory_order_release);
    return Status::Ok;
}

bool SpscRing::try_pop(TickMessage &msg) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
        return false; /* empty - caller checks closed_and_empty() for shutdown */
    }
    msg = buf_[head & mask_];
    /* release: the producer's acquire-load of head_ must see this slot
     * as free only after our read of buf_[] above has completed. */
    head_.store(head + 1, std::memory_order_release);
    return true;
}

bool SpscRing::closed_and_empty() const {
    if (!closed_.load(std::memory_order_acquire)) {
        return false;
    }
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head == tail;
}
