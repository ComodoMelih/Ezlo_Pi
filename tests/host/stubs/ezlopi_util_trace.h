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
 *
 * That last claim used to be half true. The no-op was
 * ((void)sizeof(__VA_ARGS__)), which works for a single argument but turns
 * TRACE_W("%s", x) into a comma expression -- and -Wunused-value, which this
 * suite builds with as an error, rejects it. Nothing noticed for as long as no
 * host-compiled file logged anything with a format argument;
 * ezlopi_cloud_mqtt_config.c is the first that does, and it failed to compile
 * for a reason that had nothing to do with it.
 *
 * It is now a variadic function instead. An unused static inline is discarded,
 * every argument is still named at the call site so an undefined variable is
 * still an error, and nothing is evaluated for its side effects because the
 * body is empty. Deliberately not routed to printf(): that would add format
 * checking, which sounds like a bonus, but it would also start failing the
 * build on format/argument mismatches in unrelated driver files that no test
 * currently reaches -- a change worth making on purpose rather than as a side
 * effect of fixing this.
 */

#ifndef __EZLOPI_UTIL_TRACE_H__
#define __EZLOPI_UTIL_TRACE_H__

#include <string.h>

/* Mirrors line 48 of the real header. Nothing in cjext needs it, but
 * ezlopi_core_event_group.h spells its enumerators BIT0..BIT4 while including
 * nothing that defines them -- on target they arrive through exactly this
 * edge, FreeRTOS.h pulling in esp_bit_defs.h. A host test compiling a file
 * that includes event_group.h needs the same edge, and reproducing the
 * target's include graph beats defining the bits a second time somewhere they
 * do not belong. */
#include "freertos/FreeRTOS.h"

/* Copied verbatim from the real header. Drivers pass this as their log tag, so
 * a source that compiles on target only because the real trace header defined
 * it must still compile here. */
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

/* Consumes any number of arguments, evaluates none of them for output, and
 * prints nothing. The arguments are still type-checked and still have to
 * exist, which is the part that makes this better than defining the macros
 * away to nothing. */
static inline void ezpi_trace_noop(const char *fmt, ...)
{
    (void)fmt;
}

#define EZPI_TRACE_NOOP(...) ezpi_trace_noop(__VA_ARGS__)

#define TRACE_B(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_D(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_E(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_I(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_S(...) EZPI_TRACE_NOOP(__VA_ARGS__)
#define TRACE_W(...) EZPI_TRACE_NOOP(__VA_ARGS__)

/* CJSON_TRACE is deliberately NOT defined here, and that is a correction
 * rather than an omission. This stub stands in for ezlopi_util_trace.h, and
 * the real one does not define it: CJSON_TRACE belongs to
 * ezlopi_core_cjson_macros.h, which forwards it to EZPI_core_cjson_trace().
 *
 * Defining it here broke as soon as a host-compiled file included both.
 * ezlopi_core_api.c reaches this file first, through cjext.h, and
 * ezlopi_core_cjson_macros.h four includes later -- so the real definition
 * arrived second and -Werror rejected the redefinition. An #ifndef guard here
 * would have compiled, by winning the race and leaving the file logging
 * through a no-op while believing it called EZPI_core_cjson_trace(), which is
 * worse than either.
 *
 * Nothing needs the fallback: of the 28 files in the tree that use
 * CJSON_TRACE, the only one compiled on a host is ezlopi_core_api.c, and it
 * includes the header that owns the macro. A host-compiled file that uses it
 * without that include now fails to build, which is the right outcome -- on
 * target it would fail the same way. */

#endif // __EZLOPI_UTIL_TRACE_H__
