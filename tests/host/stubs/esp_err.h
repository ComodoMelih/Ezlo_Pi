/* Host stub for esp_err.h.
 *
 * Only the error type and the two codes this project's drivers return. The
 * real header's values are ESP_OK == 0 and ESP_FAIL == -1, matched here so a
 * test can compare against the same constants the firmware does.
 */
#ifndef _HOST_STUB_ESP_ERR_H_
#define _HOST_STUB_ESP_ERR_H_

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#endif /* _HOST_STUB_ESP_ERR_H_ */
