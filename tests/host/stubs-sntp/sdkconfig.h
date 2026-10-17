/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    sdkconfig.h
 * @brief   Host-test stub: deliberately empty, for test_tz.
 *
 * ezlopi_core_sntp.c includes "sdkconfig.h" but reads no CONFIG_ symbol from
 * it -- checked with grep before this file was written, which is why it is
 * empty rather than guessing at a configuration.
 *
 * Empty is the strict choice here, not the lazy one. Per
 * stubs-scenes/sdkconfig.h, the trap with this header is that defining a
 * symbol one way or leaving it undefined can preprocess the subject down to
 * nothing and let the compiler agree with an empty file. The test_tz rule
 * guards against that directly with an nm check for
 * EZPI_core_sntp_apply_stored_timezone in the compiled object, so an
 * accidentally-empty translation unit cannot pass silently.
 *
 * The one thing left undefined on purpose is CONFIG_EZPI_UTIL_TRACE_EN: with
 * it off, the TRACE_* macros in stubs/ezlopi_util_trace.h go quiet and the
 * test output stays readable. The traces are not what is being asserted.
 */

#ifndef _HOST_TEST_SNTP_SDKCONFIG_H_
#define _HOST_TEST_SNTP_SDKCONFIG_H_

#endif // _HOST_TEST_SNTP_SDKCONFIG_H_
