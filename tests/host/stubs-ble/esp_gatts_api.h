/* Host-test stand-in for ESP-IDF's esp_gatts_api.h.
 *
 * WHAT IS AND IS NOT BEING STUBBED
 *
 * ezlopi_core_ble_buffer.c touches exactly two fields of the GATT callback
 * parameter -- param->write.value and param->write.len -- and calls no ESP-IDF
 * function whatsoever. It is the one file in ezlopi-core-ble that is pure data
 * handling, which is why it is the one that can be tested here.
 *
 * So this header declares that union member and nothing else. The field types
 * are copied from the real header's `struct gatts_write_evt_param`: `len` is
 * uint16_t and `value` is `uint8_t *`. Those two types are load-bearing in the
 * test, because the bound on how much the accumulator may hold is computed as
 * `total_len() + param->write.len`, and whether that addition can overflow
 * depends on the width of len.
 *
 * THE DRIFT THIS CANNOT CATCH, AND WHAT DOES
 *
 * A stub struct can disagree with ESP-IDF's. If a future IDF renames `value`
 * or widens `len`, this header keeps compiling and the real build breaks -- the
 * failure lands on the four-target build sweep, not here, which is the right
 * way round but worth stating plainly rather than discovering. What the stub
 * must never do is make the *test* compile against a shape the firmware does
 * not have, and the protection against that is that the firmware compiles the
 * same ezlopi_core_ble_buffer.c against the real header on every target.
 *
 * The alternative -- an #ifdef ESP_PLATFORM inside the component -- was
 * rejected for the reason recorded in the test_uuid note in the Makefile: it
 * would give the firmware and the test two different functions, and the test is
 * only evidence while they are the same one.
 */

#ifndef _HOST_TEST_ESP_GATTS_API_H_
#define _HOST_TEST_ESP_GATTS_API_H_

#include <stdint.h>

typedef union
{
    struct gatts_write_evt_param
    {
        uint16_t len;
        uint8_t *value;
    } write;
} esp_ble_gatts_cb_param_t;

#endif // _HOST_TEST_ESP_GATTS_API_H_
