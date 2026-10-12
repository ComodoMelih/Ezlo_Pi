/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    EZLOPI_USER_CONFIG.h
 * @brief   Host-test stub for the firmware's EZLOPI_USER_CONFIG.h.
 *
 * The real header (ezlopi-user-config/EZLOPI_USER_CONFIG.h) cannot be used on
 * a host because its first three includes are "driver/uart.h",
 * "ezlopi_core_heap.h" and "../build/config/sdkconfig.h" -- an ESP-IDF driver,
 * a component that depends on ESP-IDF, and a generated file that only exists
 * after a target build.
 *
 * cjext needs exactly one thing from it: the four allocator macros. It
 * references no CONFIG_* symbol and no TRACE_* macro, which is what makes a
 * stub this small sufficient. (Verified by grep over cjext.c and cjext.h; if
 * that ever stops being true the build breaks here rather than silently
 * testing something else.)
 *
 * test_ze08 reaches this header too, transitively: the real
 * ezlopi_hal_uart.h includes it. That path needs nothing from here either --
 * the HAL header uses it only for stdint/stdbool -- so the same stub serves
 * both. It does matter that the allocator macros map to plain libc, because
 * test_ze08 hands the driver exact-size heap blocks and relies on ASan seeing
 * their bounds; routing them through the heap-tracing branch would hide
 * exactly the over-reads being tested.
 *
 * The definitions below mirror the real header's !CONFIG_EZPI_HEAP_ENABLE
 * branch (lines 104-113), which is the branch that maps onto the plain C
 * library. Using the libc allocator directly is also what lets the suite run
 * cjext under AddressSanitizer: the heap-tracing branch would route every
 * allocation through EZPI_core_util_heap_malloc and hide the bounds from ASan.
 */

#ifndef __EZLOPI_USER_CONFIG_H__
#define __EZLOPI_USER_CONFIG_H__

/* These four mirror what the real header pulls in transitively through
 * ezlopi_core_heap.h (lines 45-48). cjext.h genuinely depends on that: it
 * declares a dozen functions returning or taking 'bool' but never includes
 * <stdbool.h> itself, so on target it only compiles because this header is
 * included first. Dropping stdbool.h here reproduces that as a wall of
 * "unknown type name 'bool'" errors. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#define ezlopi_malloc(who, x) malloc(x)
#define ezlopi_calloc(who, x, y) calloc(x, y)
#define ezlopi_realloc(who, x, y) realloc(x, y)

/* The real macro is written as a bare "if (NULL != x) free(x);" with no
 * enclosing do/while, which makes it swallow a following else at any call
 * site that has one. Wrapped here so that -Wall -Wextra -Werror does not trip
 * on a dangling else inside cjext. Behaviour is identical for every valid use:
 * free(NULL) is a no-op in C anyway, so the guard is belt-and-braces. */
#define ezlopi_free(who, x)   \
    do                        \
    {                         \
        if (NULL != (x))      \
        {                     \
            free(x);          \
        }                     \
    } while (0)

#endif // __EZLOPI_USER_CONFIG_H__
