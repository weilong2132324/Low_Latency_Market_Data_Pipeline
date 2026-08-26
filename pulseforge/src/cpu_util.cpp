/*
 * cpu_util.cpp - CPU affinity helper implementations.
 *
 * Responsibility: pin a thread to a CPU with sched_setaffinity and
 * report the online CPU count.
 *
 * Thread-safety: sched_setaffinity(0, ...) affects only the calling
 * thread.
 */

/* sched_setaffinity / CPU_SET / CPU_ZERO are glibc extensions gated
 * behind _GNU_SOURCE (they are not plain POSIX). g++ defines
 * _GNU_SOURCE for C++ compilations already; the guard just makes this
 * file self-sufficient. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cpu_util.hpp"

#include <cstdio>
#include <sched.h>
#include <unistd.h>

int pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::perror("sched_setaffinity");
        return -1;
    }
    return 0;
}

int cpu_count() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) {
        return 0;
    }
    return static_cast<int>(n);
}
