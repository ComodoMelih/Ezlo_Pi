/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    ezlopi_util_trace.h
 * @brief   Host-test stub for test_logseverity: ../stubs/ plus the log upcall
 *          registration the subject calls.
 *
 * WHY THIS FILE SHADOWS THE SHARED STUB
 * -------------------------------------
 * ezlopi_core_log.c is the implementation of the trace module's two upcalls,
 * so unlike every other host-compiled subject it calls *into*
 * ezlopi_util_trace.h rather than only using its macros:
 *
 *     ezlopi_util_set_log_upcalls(EZPI_core_send_cloud_log, EZPI_core_serial_log_upcall);
 *
 * With only ../stubs/ezlopi_util_trace.h on the path that is an implicit
 * declaration, which this suite builds as an error.
 *
 * The declaration could not simply be added to the shared stub, and the reason
 * is worth recording because it is the kind of thing that looks like laziness:
 * f_ezlopi_log_upcall_t returns ezlopi_error_t, which lives in
 * ezlopi_core_errors.h. The shared stub is reached by cjext.h, which a dozen
 * rules compile without -I$(ERRORS_DIR) on the command line -- so putting the
 * typedef there would break tests that have nothing to do with logging. The
 * include path ordering in the test_logseverity rule puts stubs-log/ ahead of
 * stubs/, so this file wins for this one rule and changes nothing elsewhere.
 *
 * The typedef and the prototype are copied from the real header, lines 143 and
 * 218, and must stay copies: if the upcall signature changes on target, a
 * divergence here would show up as the subject failing to compile on a host
 * rather than as a test that quietly passes against the old shape.
 */

#ifndef _HOST_TEST_LOG_UTIL_TRACE_H_
#define _HOST_TEST_LOG_UTIL_TRACE_H_

/* By relative path on purpose. "ezlopi_util_trace.h" would find this file
** again -- the include guard would stop the recursion and the shared stub's
** macros would never arrive, which is a silent failure rather than a loud
** one. */
#include "../stubs/ezlopi_util_trace.h"

#include "ezlopi_core_errors.h"

/* Verbatim from ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h:143. */
typedef ezlopi_error_t (*f_ezlopi_log_upcall_t)(int severity_level, const char *log_str);

/* Verbatim from ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h:218. Faked
** in test_logseverity.c, which keeps the two pointers -- that fake is the only
** way to reach the static EZPI_core_serial_log_upcall() without copying the
** severity gate under test into the test file. */
void ezlopi_util_set_log_upcalls(f_ezlopi_log_upcall_t cloud_log_upcall, f_ezlopi_log_upcall_t serial_log_upcall);

#endif // _HOST_TEST_LOG_UTIL_TRACE_H_
