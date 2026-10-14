# Deliberately has no REQUIRES, for the same reason as ezlopi-util-sanitize,
# ezlopi-util-ha and ezlopi-util-localkey: tests/host must be able to compile it
# with a plain host compiler.
#
# The decision in here is about whether the hub is allowed to restart itself
# during boot. Its old form lived inline in EZPI_device_prepare(), a function
# that reads the factory-info partition, walks cJSON and calls esp_restart(),
# and so could only be exercised by flashing a board and deliberately storing a
# broken configuration on it. The failure it got wrong -- restart, fail the same
# way, restart -- also takes away BLE, UART and the websocket, because it fires
# before those services start, so the board under test gives no way of watching
# it happen beyond a serial log.
#
# Separated out, the whole thing is three booleans, and tests/host/test_devcfg.c
# can prove the property that matters: no input restarts more than once.

idf_component_register(SRCS "ezlopi_util_devcfg.c"
    INCLUDE_DIRS "."
    REQUIRES )
