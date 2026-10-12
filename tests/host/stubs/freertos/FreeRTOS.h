/* Host stub for freertos/FreeRTOS.h.
 *
 * ezlopi_hal_uart.h stores a TaskHandle_t in struct s_ezlopi_uart_object, so
 * the name has to exist for that struct to be declared. No scheduler, no
 * tasks, and no port macros: a test that needed those would be testing
 * FreeRTOS rather than this project's code.
 */
#ifndef _HOST_STUB_FREERTOS_H_
#define _HOST_STUB_FREERTOS_H_

typedef void *TaskHandle_t;

#endif /* _HOST_STUB_FREERTOS_H_ */
