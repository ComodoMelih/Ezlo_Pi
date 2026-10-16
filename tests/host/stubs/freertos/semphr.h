/* Host stub for freertos/semphr.h.
 *
 * ezlopi_hal_i2c_master.c guards every I2C transfer with a binary semaphore, so
 * the names have to exist for it to compile. Unlike stubs/driver/i2c.h, these
 * are implemented here rather than left to the test, because a semaphore is a
 * scheduler primitive rather than hardware and can be emulated exactly in a
 * single-threaded process: with no second thread, "take" always succeeds and
 * the count is bookkeeping.
 *
 * Being explicit about what that does and does not prove: this makes the
 * transfer paths reachable on a host. It is NOT a test of mutual exclusion.
 * Whether the real semaphore actually serialises two tasks sharing one I2C bus
 * cannot be answered without the scheduler, and is left to the target.
 *
 * task.h is included for TickType_t, which is where FreeRTOS itself puts it and
 * where stubs/freertos/task.h already declares it.
 */
#ifndef _HOST_STUB_FREERTOS_SEMPHR_H_
#define _HOST_STUB_FREERTOS_SEMPHR_H_

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct
{
    int count;
} host_semaphore_t;

typedef host_semaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    host_semaphore_t *sem = (host_semaphore_t *)calloc(1, sizeof(host_semaphore_t));
    return sem; /* count 0: created empty, as FreeRTOS does */
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (NULL == sem)
    {
        return pdFALSE;
    }
    if (sem->count > 0)
    {
        return pdFALSE; /* a binary semaphore already given does not stack */
    }
    sem->count = 1;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)ticks;
    if ((NULL == sem) || (sem->count <= 0))
    {
        /* Single-threaded: nothing can give it back, so blocking forever would
         * hang the test. Reporting failure is the honest answer and is what the
         * firmware would see on a timeout. */
        return pdFALSE;
    }
    sem->count = 0;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    free(sem);
}

#endif /* _HOST_STUB_FREERTOS_SEMPHR_H_ */
