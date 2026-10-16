/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    sdkconfig.h
 * @brief   Host-test stub: turns meshbots on for test_scenefields only.
 *
 * Same reasoning as stubs-mqtt-api/sdkconfig.h -- see the long note there for
 * why a shared stubs/sdkconfig.h would be a liability. This one exists for one
 * rule and defines one symbol.
 *
 * WHY IT IS NEEDED
 *
 * ezlopi_core_scenes_v2.h is wrapped end to end in
 * #ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS, and so is
 * ezlopi_core_scenes_delete.c. The first attempt at compiling the destructor
 * on a host reported success after stubbing nothing but sdkconfig.h, and that
 * success was worthless: with the macro undefined the whole translation unit
 * preprocesses away and the compiler was agreeing with an empty file. The
 * test_scenefields rule therefore checks with nm that
 * EZPI_scenes_delete_fields is actually present in the object file before it
 * links, so the same mistake cannot pass silently again.
 */

#ifndef _HOST_TEST_SCENES_SDKCONFIG_H_
#define _HOST_TEST_SCENES_SDKCONFIG_H_

#define CONFIG_EZPI_SERV_ENABLE_MESHBOTS 1

#endif // _HOST_TEST_SCENES_SDKCONFIG_H_
