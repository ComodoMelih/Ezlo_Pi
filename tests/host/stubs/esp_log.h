/* Host stub for esp_log.h.
 *
 * The real header is a logging front end that reaches into the ESP-IDF
 * configuration and the UART console. Here the macros only have to compile and
 * be silent: the tests check parser output, and a driver that logs on every
 * rejected frame would otherwise bury the result under the tens of thousands of
 * deliberately-malformed frames the suites feed it.
 *
 * "if (0) printf(...)" rather than a cast to void: the call is never reached,
 * but the compiler still parses it, so -Wformat checks the format string
 * against its arguments. A log call whose placeholders do not match its
 * arguments is therefore a host-build failure instead of garbage output on
 * target. The tag is consumed separately because it is not part of the format.
 */
#ifndef _HOST_STUB_ESP_LOG_H_
#define _HOST_STUB_ESP_LOG_H_

#include <stdio.h>

#define EZPI_HOST_LOG_NOOP(tag, ...) \
    do                               \
    {                                \
        (void)(tag);                 \
        if (0)                       \
        {                            \
            printf(__VA_ARGS__);     \
        }                            \
    } while (0)

#define ESP_LOGE(tag, ...) EZPI_HOST_LOG_NOOP(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) EZPI_HOST_LOG_NOOP(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) EZPI_HOST_LOG_NOOP(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) EZPI_HOST_LOG_NOOP(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) EZPI_HOST_LOG_NOOP(tag, __VA_ARGS__)

#endif /* _HOST_STUB_ESP_LOG_H_ */
