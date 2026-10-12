/* Host stub for freertos/queue.h.
 *
 * struct s_ezlopi_uart_object holds a QueueHandle_t, so the typedef has to
 * exist. The queue itself is never created or read in these tests: the UART
 * event task is the thing on the far side of it, and that task is what the
 * host suite cannot run.
 */
#ifndef _HOST_STUB_FREERTOS_QUEUE_H_
#define _HOST_STUB_FREERTOS_QUEUE_H_

typedef void *QueueHandle_t;

#endif /* _HOST_STUB_FREERTOS_QUEUE_H_ */
