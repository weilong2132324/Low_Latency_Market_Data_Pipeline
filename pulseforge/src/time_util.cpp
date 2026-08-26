/*
 * time_util.cpp - implementation of the monotonic clock helpers.
 *
 * Responsibility: thin, checked wrappers around clock_gettime and
 * clock_nanosleep. Uses CLOCK_MONOTONIC_RAW so that NTP adjustments to
 * the wall clock never skew latency measurements.
 *
 * Thread-safety: reentrant.
 */

#include "time_util.hpp"

#include <cerrno>
#include <cstdio>
#include <ctime>

uint64_t now_ns() {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        std::perror("clock_gettime(CLOCK_MONOTONIC_RAW)");
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

int sleep_ns(uint64_t ns) {
    timespec ts;
    ts.tv_sec  = static_cast<time_t>(ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
    } while (rc == EINTR);
    return rc;
}

int sleep_until_ns(uint64_t deadline_ns) {
    timespec ts;
    ts.tv_sec  = static_cast<time_t>(deadline_ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(deadline_ns % 1000000000ULL);
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    } while (rc == EINTR);
    return rc;
}
