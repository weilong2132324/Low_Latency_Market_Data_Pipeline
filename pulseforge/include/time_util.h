/*
 * time_util.h - monotonic clock helpers.
 *
 * Responsibility: single source of time so latency math is consistent
 * (CLOCK_MONOTONIC_RAW everywhere).
 *
 * Thread-safety: functions are reentrant; they hold no shared state.
 */

#ifndef PULSEFORGE_TIME_UTIL_H
#define PULSEFORGE_TIME_UTIL_H

#include <stdint.h>

/*
 * now_ns() - nanoseconds since an arbitrary, boot-relative origin
 * (CLOCK_MONOTONIC_RAW). Returns 0 on failure (clock_gettime fails only
 * in pathological cases; we still check it).
 */
uint64_t now_ns(void);

/*
 * sleep_ns(ns) - sleep for at least ns nanoseconds (relative).
 * Returns 0 on success, or a positive error number on failure.
 * Loops on EINTR so a signal cannot truncate the sleep.
 */
int sleep_ns(uint64_t ns);

/*
 * sleep_until_ns(deadline_ns) - sleep until an absolute monotonic
 * deadline. Used for drift-free pacing (the publisher's primitive).
 * Returns 0 on success, or a positive error number on failure.
 */
int sleep_until_ns(uint64_t deadline_ns);

#endif /* PULSEFORGE_TIME_UTIL_H */
