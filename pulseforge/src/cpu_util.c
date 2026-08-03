/*
 * cpu_util.c - CPU affinity helper implementations.
 *
 * Responsibility: pin a thread to a CPU with sched_setaffinity and
 * report the online CPU count.
 *
 * Thread-safety: sched_setaffinity(0, ...) affects only the calling
 * thread.
 */

/* sched_setaffinity / CPU_SET / CPU_ZERO are glibc extensions gated
 * behind _GNU_SOURCE (they are not plain POSIX). */
#define _GNU_SOURCE

#include "cpu_util.h"

#include <sched.h>
#include <stdio.h>
#include <unistd.h>

int pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        return -1;
    }
    return 0;
}

int cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) {
        return 0;
    }
    return (int)n;
}
