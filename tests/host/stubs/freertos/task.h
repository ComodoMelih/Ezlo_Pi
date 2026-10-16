/* Host stub for freertos/task.h.
 *
 * ezlopi_core_scenes_v2.h includes it unconditionally. Nothing in the scene
 * destructor -- the only scenes translation unit compiled on a host -- calls
 * into the scheduler, so the names are all that is needed. TickType_t is here
 * rather than in FreeRTOS.h to match where FreeRTOS itself declares it, so
 * that a test which one day needs a real tick type knows where to look.
 */
#ifndef _HOST_STUB_FREERTOS_TASK_H_
#define _HOST_STUB_FREERTOS_TASK_H_

#include <stdint.h>

typedef uint32_t TickType_t;

#endif /* _HOST_STUB_FREERTOS_TASK_H_ */
