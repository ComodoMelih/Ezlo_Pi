/* Host stub for driver/gpio.h.
 *
 * Deliberately minimal. This exists so that headers which include
 * driver/gpio.h only to get GPIO_NUM_* constants for pin-number macros can be
 * compiled on a host. It declares no function, so any source that actually
 * drives a pin will fail to link rather than silently appear to work -- which
 * is the point: a driver that touches real hardware does not belong in this
 * suite, and should be refactored until its parsing logic is separable.
 */
#ifndef _HOST_STUB_DRIVER_GPIO_H_
#define _HOST_STUB_DRIVER_GPIO_H_

typedef enum
{
    GPIO_NUM_NC = -1,
    GPIO_NUM_0 = 0,
    GPIO_NUM_45 = 45,
    GPIO_NUM_48 = 48,
    GPIO_NUM_MAX = 49
} gpio_num_t;

#endif /* _HOST_STUB_DRIVER_GPIO_H_ */
