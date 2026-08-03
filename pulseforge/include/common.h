/*
 * common.h - shared definitions, error-handling macro, and exit codes.
 *
 * Responsibility: the single place for cross-cutting conventions:
 *   - the CHECK() cleanup-driven error macro
 *   - project-wide exit codes
 *   - common headers
 *
 * Thread-safety: macros carry no state; safe to use anywhere.
 */

#ifndef PULSEFORGE_COMMON_H
#define PULSEFORGE_COMMON_H

/* Safety net: keep POSIX APIs visible even without the -D flag. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exit codes shared by all PulseForge programs. */
enum {
    EXIT_OK      = 0,
    EXIT_USAGE   = 2,  /* bad command line */
    EXIT_SYSFAIL = 3,  /* syscall / library failure */
    EXIT_CFG     = 4,  /* invalid configuration */
};

/*
 * CHECK(call, what) - run a call that returns < 0 on failure.
 *
 * On failure it prints "func: what failed: strerror(errno)" and jumps
 * to the `cleanup:` label with rc = EXIT_SYSFAIL.
 *
 * Contract for any function that uses CHECK:
 *   - declare `int rc = EXIT_OK;` before using CHECK
 *   - end the function with a `cleanup:` label followed by resource
 *     unwinding and `return rc;`
 *
 * This is the primary "goto cleanup" pattern we use for safe resource
 * ownership. It is one of the very few macros we allow; it exists
 * because C has no clean way to capture the failing expression text.
 */
#define CHECK(call, what)                                               \
    do {                                                                \
        if ((call) < 0) {                                               \
            fprintf(stderr, "%s: %s failed: %s\n",                      \
                    __func__, (what), strerror(errno));                 \
            rc = EXIT_SYSFAIL;                                          \
            goto cleanup;                                               \
        }                                                               \
    } while (0)

/* Silence -Wextra for intentionally unused parameters. */
#define UNUSED(x) ((void)(x))

#endif /* PULSEFORGE_COMMON_H */
