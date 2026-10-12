/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    ezlopi_util_trace.h
 * @brief   Host-test stub for the firmware's trace macros.
 *
 * The real header pulls in "esp_log.h", "freertos/FreeRTOS.h" and the
 * generated "../../build/config/sdkconfig.h", none of which exist on a host.
 *
 * cjext.h includes this header but uses none of its macros, so the stub only
 * has to exist and be harmless. The definitions are kept rather than left out
 * entirely so that adding a trace call inside cjext does not break the host
 * build; they are variadic no-ops that still reference their arguments, so a
 * typo in a traced expression is still a compile error.
 */

#ifndef __EZLOPI_UTIL_TRACE_H__
#define __EZLOPI_UTIL_TRACE_H__

#include <string.h>

/* Copied verbatim from the real header. Drivers pass this as their log tag, so
 * a source that compiles on target only because the real trace header defined
 * it must still compile here. */
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

/* (void)sizeof(...) consumes the arguments without evaluating them, so an
 * undefined variable in a trace call is still caught, but nothing is printed
 * and no side effect runs. */
#define EZPI_TRACE_NOOP(...) ((void)sizeof(__VA_ARGS__))

#define TRACE_B(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_D(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_E(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_I(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_S(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_W(...) EZPI_TRACE_NOOP(__VA_ARGS__)

#define CJSON_TRACE(name, cj) ((void)(name), (void)(cj))

#endif // __EZLOPI_UTIL_TRACE_H__
