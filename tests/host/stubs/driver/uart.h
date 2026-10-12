/* Host stub for driver/uart.h.
 *
 * Supplies only the channel-count constants that ezlopi_hal_uart.h needs to
 * build its e_ezlopi_uart_channel_t enum. The values match an esp32, which has
 * three UARTs; they are only ever used here as enum initialisers, so the exact
 * numbers do not affect any test outcome.
 *
 * Nothing else from driver/uart.h is declared. Stubbing the ESP-IDF headers
 * underneath ezlopi_hal_uart.h, rather than writing a host copy of
 * ezlopi_hal_uart.h itself, is a deliberate choice: the real header is then
 * the one being compiled, so its struct layouts and callback signature cannot
 * drift away from what the tests assume.
 */
#ifndef _HOST_STUB_DRIVER_UART_H_
#define _HOST_STUB_DRIVER_UART_H_

#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2
#define UART_NUM_MAX 3

#endif /* _HOST_STUB_DRIVER_UART_H_ */
