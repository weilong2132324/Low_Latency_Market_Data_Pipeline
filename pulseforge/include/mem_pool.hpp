/*
 * mem_pool.hpp - fixed-capacity free-list object pool.
 *
 * Responsibility: demonstrate the "pre-allocate once, never call
 * new/delete on the hot path" strategy that low-latency code uses
 * instead of the general-purpose allocator (which may take a lock, walk
 * free lists of its own, or trigger a page fault on growth - all
 * latency outliers you do not control).
 *
 * How it works: Capacity slots of sizeof(T) are allocated once, up
 * front, as a union with an intrusive singly-linked free list threaded
 * through the unused slots. acquire()/release() are O(1) pointer
 * operations - no allocation, no syscalls, no locks.
 *
 * Thread-safety: deliberately NOT thread-safe (no atomics, no mutex) -
 * a pool like this is meant to be owned by one thread (e.g. one
 * worker's private pool of downstream event objects), which is also why
 * it is faster than the general allocator. Sharing one instance across
 * threads without external synchronization is undefined behavior; give
 * each thread its own pool instead of adding a lock to this one (a lock
 * here would defeat the point).
 *
 * Ownership: acquire() returns a raw T* that stays valid until you call
 * release() on it (or the pool itself is destroyed). The pool does not
 * run T's destructor on release() - this is a slot allocator, not a
 * smart pointer; call the destructor yourself first if T owns
 * resources (TickMessage and OrderBookEvent below are both trivial
 * PODs, so this project never needs to).
 */

#ifndef PULSEFORGE_MEM_POOL_HPP
#define PULSEFORGE_MEM_POOL_HPP

#include <array>
#include <cstddef>
#include <type_traits>

template <typename T, size_t Capacity>
class MemPool {
public:
    MemPool() {
        /* Thread every slot onto the free list up front: slot i's
         * "next free" pointer is slot i+1, last one is nullptr. */
        for (size_t i = 0; i + 1 < Capacity; i++) {
            slots_[i].next_free = &slots_[i + 1];
        }
        if (Capacity > 0) {
            slots_[Capacity - 1].next_free = nullptr;
        }
        free_head_ = Capacity > 0 ? &slots_[0] : nullptr;
    }

    MemPool(const MemPool &)            = delete;
    MemPool &operator=(const MemPool &) = delete;

    /* Returns nullptr if the pool is exhausted (no silent allocation
     * fallback - that would reintroduce the exact latency spike this
     * class exists to avoid). */
    T *acquire() {
        if (free_head_ == nullptr) {
            return nullptr;
        }
        Slot *slot = free_head_;
        free_head_ = slot->next_free;
        --available_;
        return reinterpret_cast<T *>(&slot->storage);
    }

    /* `ptr` must be a value previously returned by acquire() on this
     * same pool and not yet released. */
    void release(T *ptr) {
        Slot *slot = reinterpret_cast<Slot *>(ptr);
        slot->next_free = free_head_;
        free_head_       = slot;
        ++available_;
    }

    constexpr size_t capacity() const { return Capacity; }
    size_t available() const { return available_; }

private:
    union Slot {
        Slot() {}  /* storage's constructor is never run by the union */
        ~Slot() {} /* ditto for the destructor; T's lifetime is managed
                    * by acquire()/release(), not by Slot */
        typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
        Slot *next_free;
    };

    std::array<Slot, Capacity> slots_;
    Slot  *free_head_  = nullptr;
    size_t available_  = Capacity;
};

#endif /* PULSEFORGE_MEM_POOL_HPP */
