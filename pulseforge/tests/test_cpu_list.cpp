/*
 * test_cpu_list.cpp - unit tests for the sysfs CPU-list parser.
 */

#include "cpu_list.hpp"

#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_single_and_ranges() {
    auto v = parse_cpu_list("0-3,8,10-11");
    CHECK(v.has_value());
    CHECK((*v == std::vector<int>{0, 1, 2, 3, 8, 10, 11}));
}

static void test_trailing_newline_like_sysfs() {
    auto v = parse_cpu_list("0-1\n");
    CHECK(v.has_value());
    CHECK((*v == std::vector<int>{0, 1}));
}

static void test_empty_is_valid_empty_set() {
    auto v = parse_cpu_list("\n"); /* /sys/.../isolated with no isolcpus= */
    CHECK(v.has_value());
    CHECK(v->empty());
}

static void test_malformed_rejected() {
    CHECK(!parse_cpu_list("3-1").has_value()); /* reversed range */
    CHECK(!parse_cpu_list("a-b").has_value());
    CHECK(!parse_cpu_list("1,,2").has_value()); /* empty item */
}

int main() {
    test_single_and_ranges();
    test_trailing_newline_like_sysfs();
    test_empty_is_valid_empty_set();
    test_malformed_rejected();

    if (g_failures == 0) {
        std::printf("test_cpu_list: ALL PASSED\n");
        return 0;
    }
    std::printf("test_cpu_list: %d FAILURE(S)\n", g_failures);
    return 1;
}
