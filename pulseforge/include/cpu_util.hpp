/*
 * cpu_util.hpp - CPU affinity helpers.
 *
 * Responsibility: small wrappers around sched_setaffinity so the
 * receiver (and later, workers) can pin themselves to a logical CPU.
 *
 * Thread-safety: pin_to_cpu() applies to the calling thread only.
 */

#ifndef PULSEFORGE_CPU_UTIL_HPP
#define PULSEFORGE_CPU_UTIL_HPP

/* Pin the calling thread to the given logical CPU.
 * Returns 0 on success, -1 on failure (errno set). */
int pin_to_cpu(int cpu);

/* Number of online logical CPUs on this host, or 0 on failure. */
int cpu_count();

#endif /* PULSEFORGE_CPU_UTIL_HPP */
