/* Host stub for esp_random.h.
 *
 * Declares exactly what the real header declares, with the same signatures:
 *
 *     uint32_t esp_random(void);
 *     void esp_fill_random(void *buf, size_t len);
 *
 * This stub exists so that ezlopi_util_uuid.c can call the hardware RNG and
 * still be compiled, unmodified, by the host suite. The alternative was an
 * #ifdef ESP_PLATFORM inside the component, which would have meant the bytes
 * the firmware generates and the bytes the test checks came from two
 * different pieces of code -- so the test would no longer be evidence about
 * the firmware. Here the translation unit is identical on both; only the
 * entropy source behind this header differs.
 *
 * The implementation in stub_random.c is a counter, not a PRNG, and that is
 * deliberate: a test cannot assert anything about where a random byte came
 * from, but it can assert that all sixteen bytes of the UUID came from this
 * source and were not left over from the stack. See stub_random.h.
 */
#ifndef _HOST_STUB_ESP_RANDOM_H_
#define _HOST_STUB_ESP_RANDOM_H_

#include <stddef.h>
#include <stdint.h>

uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);

#endif /* _HOST_STUB_ESP_RANDOM_H_ */
