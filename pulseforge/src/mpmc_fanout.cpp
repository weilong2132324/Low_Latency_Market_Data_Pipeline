#include "mpmc_fanout.hpp"

namespace {
size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}
} // namespace

MpmcFanout::MpmcFanout(size_t capacity, size_t num_readers)
    : read_seq_(num_readers), buf_(next_pow2(capacity)),
      mask_(next_pow2(capacity) - 1) {
    /* std::atomic's default constructor leaves the value UNINITIALIZED
     * in C++17 (this only became constexpr-zero-init in C++20), so a
     * vector of default-constructed atomics is not safely "0" without
     * this explicit loop. */
    for (auto &cursor : read_seq_) {
        cursor.store(0, std::memory_order_relaxed);
    }
}

size_t MpmcFanout::min_read_seq() const {
    size_t m = write_seq_.load(std::memory_order_relaxed);
    for (const auto &cursor : read_seq_) {
        const size_t v = cursor.load(std::memory_order_acquire);
        if (v < m) {
            m = v;
        }
    }
    return m;
}

void MpmcFanout::close() { closed_.store(true, std::memory_order_release); }

MpmcFanout::Status MpmcFanout::try_push(const TickMessage &msg) {
    if (closed_.load(std::memory_order_relaxed)) {
        return Status::Closed;
    }
    const size_t write = write_seq_.load(std::memory_order_relaxed);
    if (write - min_read_seq() >= buf_.size()) {
        return Status::Full; /* the slowest reader hasn't freed a slot yet */
    }
    buf_[write & mask_] = msg;
    write_seq_.store(write + 1, std::memory_order_release);
    return Status::Ok;
}

bool MpmcFanout::try_pop(size_t reader_id, TickMessage &msg) {
    std::atomic<size_t> &cursor = read_seq_[reader_id];
    const size_t read = cursor.load(std::memory_order_relaxed);
    const size_t write = write_seq_.load(std::memory_order_acquire);
    if (read == write) {
        return false; /* this reader has caught up to the writer */
    }
    msg = buf_[read & mask_];
    cursor.store(read + 1, std::memory_order_release);
    return true;
}

bool MpmcFanout::closed_and_empty(size_t reader_id) const {
    if (!closed_.load(std::memory_order_acquire)) {
        return false;
    }
    const size_t read = read_seq_[reader_id].load(std::memory_order_relaxed);
    const size_t write = write_seq_.load(std::memory_order_acquire);
    return read == write;
}
