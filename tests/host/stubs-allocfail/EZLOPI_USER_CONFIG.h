/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    EZLOPI_USER_CONFIG.h
 * @brief   stubs/EZLOPI_USER_CONFIG.h with a controllable allocator.
 *
 * Identical to the shared stub except that ezlopi_malloc and ezlopi_free route
 * through two functions the test defines, instead of straight to libc. That is
 * the whole reason this directory exists: the defects under test are on the
 * allocation-failure paths of settings initialization, and those paths are
 * unreachable from a host test unless an allocation can be made to fail on
 * demand.
 *
 * Both functions still call libc malloc and free underneath, so
 * AddressSanitizer sees the same block bounds and the same frees it would see
 * through the plain macros. Nothing is pooled, reused or suppressed here --
 * routing allocations through a wrapper that kept its own arena would hide
 * exactly the use-after-free this test is about.
 *
 * ezlopi_calloc and ezlopi_realloc are left on libc. Neither subject that uses
 * this header calls either -- both were counted rather than assumed -- and
 * cjext's use of them has nothing to do with these tests; wrapping them would
 * put cJSON's allocations under the same failure budget and make which
 * allocation failed depend on how cJSON happens to be implemented.
 *
 * This directory was called stubs-ens160 while test_ens160set.c was the only
 * caller. It is named for what it does now that test_hilinkset.c needs the same
 * thing -- which it needs because the two files carry the same defect: a
 * settings initializer that frees a node the core has already linked into
 * device->settings when a payload allocation fails.
 */

#ifndef __EZLOPI_USER_CONFIG_H__
#define __EZLOPI_USER_CONFIG_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

/* Defined by whichever test is being linked -- test_ens160set.c or
** test_hilinkset.c. Declared here rather than in a header of their own because
** this file is the only thing that needs to name them, and the subject must not
** be able to see them. */
void *EZPI_test_alloc(unsigned long size);
void EZPI_test_release(void *ptr);

#define ezlopi_malloc(who, x) EZPI_test_alloc((unsigned long)(x))
#define ezlopi_calloc(who, x, y) calloc(x, y)
#define ezlopi_realloc(who, x, y) realloc(x, y)

/* Same do/while wrapping and NULL guard as the shared stub, for the same
** reason: the real macro is a bare "if (NULL != x) free(x);" that would
** swallow a following else under -Werror. */
#define ezlopi_free(who, x)      \
    do                           \
    {                            \
        if (NULL != (x))         \
        {                        \
            EZPI_test_release(x); \
        }                        \
    } while (0)

#endif // __EZLOPI_USER_CONFIG_H__
