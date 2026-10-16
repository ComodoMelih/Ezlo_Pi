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

/* ESP-IDF's FreeRTOS.h includes esp_bit_defs.h, which is where BIT0..BIT31
 * come from. ezlopi_core_event_group.h writes its enumerators as BIT0..BIT4
 * and includes only ezlopi_core_errors.h, so on target it compiles solely
 * because something upstream of it reached FreeRTOS.h first. Defined here for
 * the same reason, spelled the same way esp_bit_defs.h spells them. Only the
 * five the event group uses, extended to 8 so the next bit added there does
 * not have to come back to this file.
 *
 * #ifndef-guarded individually: these are plain object macros, so defining
 * them identically twice is legal C, but a stub directory that is first on the
 * include path for one rule and second for another should not care which of
 * the two got there first. */
#ifndef BIT0
#define BIT0 0x00000001U
#define BIT1 0x00000002U
#define BIT2 0x00000004U
#define BIT3 0x00000008U
#define BIT4 0x00000010U
#define BIT5 0x00000020U
#define BIT6 0x00000040U
#define BIT7 0x00000080U
#endif

#endif /* _HOST_STUB_FREERTOS_H_ */
