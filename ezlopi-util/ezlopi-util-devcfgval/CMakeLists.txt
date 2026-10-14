# REQUIRES is deliberately only cjext and the keyword strings, for the same
# reason as ezlopi-util-devcfg next door: tests/host must be able to compile it
# with a plain host compiler. Both of those are leaves -- cjext pulls in
# ezlopi-util-trace and ezlopi-cloud-constants and nothing else -- so nothing
# here reaches ESP-IDF.
#
# The decision in here is whether a device configuration arriving from outside
# is worth writing to flash. Its natural home would be next to the code that
# consumes it, in ezlopi_core_devices.c, and that is exactly why it is not
# there: that file reads the factory-info partition, drives the device table and
# calls esp_restart(). The only way to ask it whether a configuration is
# acceptable is to store the configuration and reboot, which is also the only
# way to find out that it was not.
#
# Worse, the function that would answer the question -- ezlopi_device_parse_
# json_v3() -- cannot answer it even on hardware, because it instantiates
# devices as it goes. There is no dry run. So the judgement is separated out
# here, where tests/host/test_devcfgval.c can put a few hundred shapes through
# it in a few milliseconds, including the ones that are accepted and written to
# flash today.

idf_component_register(SRCS "ezlopi_util_devcfgval.c"
    INCLUDE_DIRS "."
    REQUIRES cjext
             ezlopi-cloud-constants)
