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

/* The three types below are what ezlopi_hal_gpio.h needs in order to declare
** its prototypes -- it is included transitively by the scenes then-methods
** translation unit, which test_httpurl compiles for real. They appear here
** rather than in that test's own stub directory because this file is already
** the place a host build looks for a GPIO type, and a second copy under
** stubs-httpurl/ would be a copy that can drift.
**
** The values are lifted from the installed IDF v4.4.6
** (components/hal/include/hal/gpio_types.h: gpio_int_type_t at :354,
** gpio_mode_t at :371 with its GPIO_MODE_DEF_* masks at :365-368, and
** gpio_pull_mode_t at :401) rather than written from memory. Nothing in this
** suite compares against them today, so an invented value would not fail any
** test -- which is exactly why copying was worth the two minutes: a stub that
** silently disagrees with the toolchain is worse than no stub, because the
** disagreement surfaces only once somebody writes the test that needs it.
**
** Still no function declarations here, for the reason given above. */
#define GPIO_MODE_DEF_DISABLE (0)
#define GPIO_MODE_DEF_INPUT (1 << 0)
#define GPIO_MODE_DEF_OUTPUT (1 << 1)
#define GPIO_MODE_DEF_OD (1 << 2)

typedef enum
{
    GPIO_MODE_DISABLE = GPIO_MODE_DEF_DISABLE,
    GPIO_MODE_INPUT = GPIO_MODE_DEF_INPUT,
    GPIO_MODE_OUTPUT = GPIO_MODE_DEF_OUTPUT,
    GPIO_MODE_OUTPUT_OD = ((GPIO_MODE_DEF_OUTPUT) | (GPIO_MODE_DEF_OD)),
    GPIO_MODE_INPUT_OUTPUT_OD = ((GPIO_MODE_DEF_INPUT) | (GPIO_MODE_DEF_OUTPUT) | (GPIO_MODE_DEF_OD)),
    GPIO_MODE_INPUT_OUTPUT = ((GPIO_MODE_DEF_INPUT) | (GPIO_MODE_DEF_OUTPUT)),
} gpio_mode_t;

typedef enum
{
    GPIO_PULLUP_ONLY,
    GPIO_PULLDOWN_ONLY,
    GPIO_PULLUP_PULLDOWN,
    GPIO_FLOATING,
} gpio_pull_mode_t;

typedef enum
{
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE = 1,
    GPIO_INTR_NEGEDGE = 2,
    GPIO_INTR_ANYEDGE = 3,
    GPIO_INTR_LOW_LEVEL = 4,
    GPIO_INTR_HIGH_LEVEL = 5,
    GPIO_INTR_MAX,
} gpio_int_type_t;

#endif /* _HOST_STUB_DRIVER_GPIO_H_ */
