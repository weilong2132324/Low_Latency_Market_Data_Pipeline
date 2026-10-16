/*
 * hugepage_alloc.hpp - RAII wrapper around a huge-page-backed mmap
 * region, with a graceful fallback to a regular anonymous mapping.
 *
 * Responsibility: a normal 4KiB page means the CPU's TLB (translation
 * lookaside buffer) can only cover a few MiB of address space before it
 * starts missing and walking page tables - a real, measurable latency
 * tax on any data structure (like a ring buffer) that is scanned or
 * accessed across a wide address range under load. A 2MiB huge page
 * covers 512x as much address space per TLB entry, trading a coarser
 * allocation granularity for far fewer TLB misses.
 *
 * Why this is a standalone allocator rather than wired into
 * SpscRing/MpmcFanout directly: those classes hold a plain
 * std::vector<TickMessage>, and swapping its allocator would mean
 * templating both classes on an Allocator parameter - a real but
 * separable change. This header demonstrates the mechanism (and is
 * exercised by bench/queue_bench.cpp's --huge-pages flag and
 * tests/test_hugepage_alloc.cpp) without that broader refactor.
 *
 * Fallback behavior, stated plainly: huge pages must be pre-reserved by
 * the kernel (see docs/hugepages.md for the `/proc/sys/vm/nr_hugepages`
 * one-liner) - if none are reserved, or hugetlbfs is unavailable (common
 * under WSL2 unless explicitly configured), mmap(MAP_HUGETLB) fails and
 * this class transparently falls back to a normal anonymous mmap. Call
 * used_huge_pages() to find out which one actually happened; never
 * assume the huge-page path succeeded just because you asked for it.
 */

#ifndef PULSEFORGE_HUGEPAGE_ALLOC_HPP
#define PULSEFORGE_HUGEPAGE_ALLOC_HPP

#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

class HugePageRegion {
public:
    explicit HugePageRegion(size_t bytes) : size_(bytes) {
#ifdef MAP_HUGETLB
        ptr_ = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr_ != MAP_FAILED) {
            used_huge_ = true;
            return;
        }
#endif
        /* Fall back to a normal anonymous mapping. */
        ptr_ = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        used_huge_ = false;
    }

    ~HugePageRegion() {
        if (ptr_ != nullptr && ptr_ != MAP_FAILED) {
            ::munmap(ptr_, size_);
        }
    }

    HugePageRegion(const HugePageRegion &)            = delete;
    HugePageRegion &operator=(const HugePageRegion &) = delete;

    /* True if the mapping succeeded at all (huge-page or fallback). */
    explicit operator bool() const noexcept { return ptr_ != MAP_FAILED; }

    bool used_huge_pages() const noexcept { return used_huge_; }

    void  *data() const noexcept { return ptr_ == MAP_FAILED ? nullptr : ptr_; }
    size_t size() const noexcept { return size_; }

private:
    void  *ptr_  = MAP_FAILED;
    size_t size_ = 0;
    bool   used_huge_ = false;
};

#endif /* PULSEFORGE_HUGEPAGE_ALLOC_HPP */
