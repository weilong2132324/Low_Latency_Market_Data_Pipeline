/*
 * time_util.hpp - monotonic clock helpers.
 *
 * Responsibility: single source of time so latency math is consistent
 * (CLOCK_MONOTONIC_RAW everywhere).
 *
 * Thread-safety: functions are reentrant; they hold no shared state.
 */

#ifndef PULSEFORGE_TIME_UTIL_HPP
#define PULSEFORGE_TIME_UTIL_HPP

#include <cstdint>

/*
 * now_ns() - nanoseconds since an arbitrary, boot-relative origin
 * (CLOCK_MONOTONIC_RAW). Returns 0 on failure (clock_gettime fails only
 * in pathological cases; we still check it). Use this for anything
 * that measures ELAPSED TIME or cross-process LATENCY (both ends read
 * the same clock on the same machine, so the comparison is meaningful)
 * - but NOT as the source of a deadline passed to sleep_until_ns()
 * below; see its comment for why.
 */
uint64_t now_ns();

/*
 * now_ns_monotonic() - nanoseconds since boot on CLOCK_MONOTONIC (NOT
 * the _RAW variant). Exists for exactly one reason: clock_nanosleep()
 * does not accept CLOCK_MONOTONIC_RAW as a clock id (Linux-specific
 * restriction - it's a sampling-only clock), so sleep_until_ns() below
 * is necessarily implemented against plain CLOCK_MONOTONIC. A deadline
 * computed from now_ns() (RAW) and handed to sleep_until_ns() (plain
 * MONOTONIC) mixes two clocks with independent, unrelated epochs - on
 * some kernels/VMs the offset between them is large enough that
 * "future" RAW deadlines land in MONOTONIC's past, so
 * clock_nanosleep(TIMER_ABSTIME) returns immediately every time and
 * pacing silently stops working (messages fire back-to-back instead of
 * at the configured --rate). Compute every deadline you pass to
 * sleep_until_ns() from THIS function, never from now_ns().
 */
uint64_t now_ns_monotonic();

/*
 * now_realtime_ns() - CLOCK_REALTIME (wall clock) in nanoseconds since the
 * Unix epoch. The trading loop (exchange/trader) uses this one, on
 * purpose: kernel receive timestamps (SO_TIMESTAMPING) are CLOCK_REALTIME,
 * and so are exchange timestamps in real feeds - it is the clock PTP
 * keeps synchronised across machines. To subtract a kernel timestamp from
 * "now" you must read the same clock. (Within one host it is consistent;
 * across hosts it is only as good as your clock sync.)
 */
uint64_t now_realtime_ns();

/*
 * sleep_ns(ns) - sleep for at least ns nanoseconds (relative).
 * Returns 0 on success, or a positive error number on failure.
 * Loops on EINTR so a signal cannot truncate the sleep.
 */
int sleep_ns(uint64_t ns);

/*
 * sleep_until_ns(deadline_ns) - sleep until an absolute deadline on
 * CLOCK_MONOTONIC (see now_ns_monotonic() above - deadline_ns MUST come
 * from that function, not now_ns()). Used for drift-free pacing (the
 * publisher's primitive). Returns 0 on success, or a positive error
 * number on failure.
 */
int sleep_until_ns(uint64_t deadline_ns);

#endif /* PULSEFORGE_TIME_UTIL_HPP */
