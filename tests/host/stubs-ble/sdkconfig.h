/* Host-test sdkconfig.h for the BLE chunk accumulator.
 *
 * ezlopi_core_ble_buffer.c is wrapped end to end in
 * `#ifdef CONFIG_EZPI_BLE_ENABLE`, so without this the translation unit
 * preprocesses away to nothing, the link still succeeds, and the suite reports
 * a pass while testing an empty object file. The Makefile rule therefore also
 * runs `nm` on the object and fails the build if the symbols are absent --
 * exactly the arrangement used for stubs-scenes/sdkconfig.h, and for the same
 * reason: the first attempt at that rule "passed" this way.
 *
 * This lives in its own directory rather than in the shared stubs/ for the
 * same reason stubs-mqtt-api/sdkconfig.h does. Six other binaries put stubs/
 * on their include path, and defining CONFIG_EZPI_BLE_ENABLE for all of them
 * would switch on BLE code paths in files that have nothing to do with this
 * test.
 */

#ifndef _HOST_TEST_SDKCONFIG_H_
#define _HOST_TEST_SDKCONFIG_H_

#define CONFIG_EZPI_BLE_ENABLE 1

#endif // _HOST_TEST_SDKCONFIG_H_
