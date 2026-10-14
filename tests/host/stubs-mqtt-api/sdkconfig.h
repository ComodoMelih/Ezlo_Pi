/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    sdkconfig.h
 * @brief   Host-test stub: turns MQTT on for test_mqtt_api only.
 *
 * WHY THIS IS NOT IN tests/host/stubs/
 *
 * On a target build sdkconfig.h is generated into build/config/ and every
 * CONFIG_* macro in the firmware comes from it. There is no stub of it in the
 * shared stubs/ directory, and that is on purpose: five of the fourteen test
 * binaries put stubs/ on their include path -- test_cjext, test_ze08,
 * test_pms5003, test_gps6mv2 and this one -- so a shared sdkconfig.h would
 * silently decide the configuration of all of them, three of which are drivers
 * whose behaviour is selected by #ifdef. A file that quietly switches #ifdef
 * branches in four unrelated tests is a file whose next edit breaks something
 * far away from itself.
 *
 * So this one lives in its own directory and is reached only by the
 * test_mqtt_api rule.
 *
 * WHY ANY OF IT IS NEEDED
 *
 * ezlopi_cloud_mqtt_config.c is wrapped end to end in
 * #ifdef CONFIG_EZPI_ENABLE_MQTT -- the same shape as ezlopi_cloud_ota.c --
 * because the API method registry includes its header unconditionally and the
 * option is `default n`. Without the macro defined, the translation unit
 * compiles to nothing at all and the test would link against an empty object
 * file and report that every check passed. That failure mode is the reason
 * test_mqtt_api.c asserts the handlers are present before testing them.
 *
 * Only the one symbol is defined. Nothing else the file touches is
 * configuration-dependent, and defining more "just in case" would make this
 * stub a second, divergent copy of the project's Kconfig defaults.
 */

#ifndef _HOST_TEST_SDKCONFIG_H_
#define _HOST_TEST_SDKCONFIG_H_

#define CONFIG_EZPI_ENABLE_MQTT 1

#endif // _HOST_TEST_SDKCONFIG_H_
