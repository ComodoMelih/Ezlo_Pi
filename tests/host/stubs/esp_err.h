/* Host stub for esp_err.h.
 *
 * Only the error type and the two codes this project's drivers return. The
 * real header's values are ESP_OK == 0 and ESP_FAIL == -1, matched here so a
 * test can compare against the same constants the firmware does.
 */
#ifndef _HOST_STUB_ESP_ERR_H_
#define _HOST_STUB_ESP_ERR_H_

#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

/* The real ESP_ERROR_CHECK aborts on a non-ESP_OK result. The abort is kept
 * rather than softened into a return, because that is what the firmware does:
 * a failed i2c_param_config() crashes the device. A stub that returned instead
 * would let a test assert on a path the hardware never reaches. */
#define ESP_ERROR_CHECK(x)                                      \
    do                                                          \
    {                                                           \
        const esp_err_t _host_esp_rc = (x);                     \
        if (ESP_OK != _host_esp_rc)                             \
        {                                                       \
            fprintf(stderr, "ESP_ERROR_CHECK(%d) failed %s:%d\n", \
                    _host_esp_rc, __FILE__, __LINE__);          \
            abort();                                            \
        }                                                       \
    } while (0)

#endif /* _HOST_STUB_ESP_ERR_H_ */
