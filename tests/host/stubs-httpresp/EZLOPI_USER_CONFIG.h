/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    EZLOPI_USER_CONFIG.h
 * @brief   Host-test stub for test_httpresp: stubs/EZLOPI_USER_CONFIG.h with
 *          one allocator routed through a function the test can fail on demand.
 *
 * WHY A SECOND COPY OF THIS HEADER
 *
 * The shared stub maps ezlopi_realloc straight onto realloc(), which is right
 * for every other test in the suite -- it keeps the libc allocator visible to
 * AddressSanitizer, as that file explains at length. It cannot serve this one.
 * EZPI_core_http_resp_append's contract includes what happens when growth
 * fails: nothing is appended, and *buf, *cap and *used are all left as they
 * were, so the caller's accounting still matches the buffer. The only way to
 * check that is to make the growth fail, and the only reliable way to make
 * realloc() fail is not to call it.
 *
 * The alternatives were weighed and are worse. Requesting a size large enough
 * for the real realloc to refuse makes the test depend on the host's overcommit
 * settings, and under ASan a multi-gigabyte request aborts the process rather
 * than returning NULL. -Wl,--wrap=realloc works but puts the mechanism in the
 * link line where nobody reading the test will find it, and it has to coexist
 * with ASan's own interceptor for the same symbol. Defining the macro on the
 * command line collides with the shared header's #define and trips -Werror.
 *
 * So this file shadows the shared one on the include path -- stubs-httpresp
 * comes before stubs in test_httpresp's -I list -- and changes exactly one of
 * the four macros. malloc, calloc and free are byte-for-byte the shared stub's,
 * for the reason given there: the test wants ASan watching the bounds of every
 * block the subject touches.
 *
 * Nothing else in the suite includes this directory, so no other test's
 * allocator behaviour changes. If a future test needs the failing hook too,
 * move the hook rather than copying this file again.
 */

#ifndef __EZLOPI_USER_CONFIG_H__
#define __EZLOPI_USER_CONFIG_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Defined in test_httpresp.c.
 *
 * Counts every call, and returns NULL without calling realloc() when the test
 * has armed it to. Returning NULL is the whole point: a realloc that fails
 * must leave the original block valid and unmoved, which is what lets the test
 * assert that a rejected chunk left the accumulated body intact. */
extern void *EZPI_test_realloc(void *ptr, size_t size);

#define ezlopi_malloc(who, x) malloc(x)
#define ezlopi_calloc(who, x, y) calloc(x, y)
#define ezlopi_realloc(who, x, y) EZPI_test_realloc(x, y)

#define ezlopi_free(who, x)   \
    do                        \
    {                         \
        if (NULL != (x))      \
        {                     \
            free(x);          \
        }                     \
    } while (0)

#endif // __EZLOPI_USER_CONFIG_H__
