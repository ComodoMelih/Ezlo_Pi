/* Host stub for esp_system.h.
 *
 * One declaration, for one caller: ezlopi_core_api.c reboots the device when
 * `register` has failed five times in a row.
 *
 * The real declaration carries __attribute__((noreturn)) and it is left off
 * here on purpose. A host test has to survive the call -- the point of
 * reaching it is to assert that it happened, and on the reply paths under test
 * it must NOT happen -- so the stand-in records and returns. Marking it
 * noreturn would let the compiler treat the rest of __check_for_no_error() as
 * unreachable and delete it, which is the opposite of what a test of that
 * function needs. The one thing this costs is that the test cannot observe the
 * target's "nothing after this line runs" behaviour; the line is the last
 * statement in its block either way, so there is nothing after it to observe.
 */
#ifndef _HOST_STUB_ESP_SYSTEM_H_
#define _HOST_STUB_ESP_SYSTEM_H_

void esp_restart(void);

#endif /* _HOST_STUB_ESP_SYSTEM_H_ */
