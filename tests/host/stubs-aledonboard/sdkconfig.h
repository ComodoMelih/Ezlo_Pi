/* Host-test sdkconfig.h for the LED-onboarding doc test.
 *
 * test_aledonboard.c includes the real ezlopi_service_uart.h so that the
 * command number printed in docs/addressable-led-onboarding.md is compared
 * against EZPI_UART_CMD_SET_CONFIG itself rather than against a second copy of
 * the number. That header's only include is "sdkconfig.h", which does not
 * exist on a host, so this supplies it.
 *
 * Deliberately empty of CONFIG_* symbols, which is the whole difference
 * between this file and stubs-ble/sdkconfig.h. The enum this test reads sits
 * outside every #ifdef in that header, so nothing needs switching on; the only
 * thing being defeated is a missing file. Leaving it empty also keeps the
 * `#if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C3)`
 * branch at ezlopi_service_uart.h:123 taken, which costs nothing here because
 * what it guards is a prototype this test never calls.
 *
 * It lives in its own directory rather than in the shared stubs/ for the same
 * reason stubs-ble/ and stubs-mqtt-api/ do. Seven other binaries put stubs/ on
 * their include path, and an sdkconfig.h appearing there would change what
 * those translation units see -- the Makefile's note above test_actions
 * measured that: an empty shared sdkconfig.h lets 52 more first-party files
 * compile, and all 52 then define nothing at all because their bodies are
 * switched off. Per-binary is the pattern that keeps that blast radius at one
 * test.
 */

#ifndef _HOST_TEST_SDKCONFIG_H_
#define _HOST_TEST_SDKCONFIG_H_

/* Intentionally no CONFIG_* definitions -- see the comment above. */

#endif // _HOST_TEST_SDKCONFIG_H_
