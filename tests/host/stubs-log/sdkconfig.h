/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    sdkconfig.h
 * @brief   Host-test stub: the configuration test_logseverity compiles under.
 *
 * Unlike stubs-sntp/sdkconfig.h this one cannot be empty. The whole of
 * ezlopi_core_log.c -- every line of it, both the .c and the .h -- sits inside
 *
 *     #ifdef CONFIG_EZPI_UTIL_TRACE_EN
 *
 * so with the symbol undefined the subject preprocesses to nothing and the
 * compiler cheerfully produces an empty object. That is exactly the trap
 * stubs-scenes/sdkconfig.h warns about, and it is not hypothetical here: it is
 * the first thing that happened when this file was empty. The test_logseverity
 * rule also carries nm guards for the three functions under test, so an
 * empty translation unit cannot pass silently even if this define is lost.
 *
 * All four target-specific defaults files set CONFIG_EZPI_UTIL_TRACE_EN=y --
 * checked, not assumed:
 *
 *     $ grep -l 'CONFIG_EZPI_UTIL_TRACE_EN=y' sdkconfig.defaults.*
 *     sdkconfig.defaults.esp32
 *     sdkconfig.defaults.esp32c3
 *     sdkconfig.defaults.esp32s2
 *     sdkconfig.defaults.esp32s3
 *
 * The base sdkconfig.defaults does not name the symbol at all, which is why
 * the glob above carries the dot. So this is the configuration that actually
 * ships on every supported target, not a corner one invented to make a test
 * compile.
 *
 * Two symbols are deliberately left undefined:
 *
 *   CONFIG_EZPI_OPENTELEMETRY_ENABLE_LOGS -- off in the shipped defaults. With
 *   it on, the real header would declare ezlopi_util_log_otel() and the
 *   subject would need another fake; the subject itself calls no TRACE_OTEL.
 *
 *   CONFIG_EZPI_LOG_CLOUD_EN -- reaches ENABLE_TRACE only through an `||` with
 *   the symbol above it, so it changes nothing here. Named rather than silently
 *   omitted, because its absence is a choice.
 */

#ifndef _HOST_TEST_LOG_SDKCONFIG_H_
#define _HOST_TEST_LOG_SDKCONFIG_H_

#define CONFIG_EZPI_UTIL_TRACE_EN 1

#endif // _HOST_TEST_LOG_SDKCONFIG_H_
