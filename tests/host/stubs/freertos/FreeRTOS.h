/* Host stub for freertos/FreeRTOS.h.
 *
 * ezlopi_hal_uart.h stores a TaskHandle_t in struct s_ezlopi_uart_object, so
 * the name has to exist for that struct to be declared. No scheduler and no
 * tasks: a test that needed those would be testing FreeRTOS rather than this
 * project's code.
 *
 * The three port macros below are the exception, added for the I2C HAL, which
 * compares xSemaphoreTake() against pdTRUE and waits portMAX_DELAY. FreeRTOS
 * supplies them from portmacro.h by way of FreeRTOS.h, so this is where a
 * translation unit including only FreeRTOS.h expects to find them. They are
 * constants, not behaviour -- the semaphore emulation lives in semphr.h.
 */
#ifndef _HOST_STUB_FREERTOS_H_
#define _HOST_STUB_FREERTOS_H_

typedef void *TaskHandle_t;
typedef long BaseType_t;

#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)

/* Not cast to TickType_t: that lives in task.h, and FreeRTOS.h does not include
 * it. The conversion happens at the xSemaphoreTake() call instead. */
#define portMAX_DELAY 0xffffffffU

#endif /* _HOST_STUB_FREERTOS_H_ */
