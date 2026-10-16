/*
 * test_hugepage_alloc.cpp - unit tests for the huge-page mmap wrapper.
 *
 * Deliberately does NOT assert used_huge_pages() == true: whether huge
 * pages are actually reserved on the test machine is environment-
 * dependent (see docs/hugepages.md). What must always hold is that the
 * mapping succeeds one way or the other, and that the memory is
 * actually usable.
 */

#include "hugepage_alloc.hpp"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_mapping_succeeds_and_is_writable() {
    HugePageRegion region(2 * 1024 * 1024); /* 2 MiB: one typical huge page */
    CHECK(static_cast<bool>(region));
    CHECK(region.data() != nullptr);
    CHECK(region.size() == 2 * 1024 * 1024);

    std::memset(region.data(), 0x42, region.size());
    auto *bytes = static_cast<unsigned char *>(region.data());
    CHECK(bytes[0] == 0x42);
    CHECK(bytes[region.size() - 1] == 0x42);

    std::printf("  (info) used_huge_pages() = %s\n",
                region.used_huge_pages() ? "true" : "false (fell back - fine)");
}

int main() {
    test_mapping_succeeds_and_is_writable();

    if (g_failures == 0) {
        std::printf("test_hugepage_alloc: ALL PASSED\n");
        return 0;
    }
    std::printf("test_hugepage_alloc: %d FAILURE(S)\n", g_failures);
    return 1;
}
