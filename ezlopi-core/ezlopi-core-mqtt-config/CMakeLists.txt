# Storage and precedence for the MQTT service's settings, which moved from
# CONFIG_EZPI_MQTT_* (fixed at build time) to NVS (writable at runtime).
#
# Compiled unconditionally, like ezlopi-service-mqtt itself, so that a build
# with CONFIG_EZPI_ENABLE_MQTT off still compiles this file. That is the reason
# ezlopi_core_mqtt_config.c wraps every CONFIG_EZPI_MQTT_* reference in an
# #ifdef: those Kconfig entries "depends on EZPI_ENABLE_MQTT", so with MQTT off
# the macros do not exist, and a bare reference would break all four committed
# default configurations while passing the one CI job that has MQTT enabled.
#
# ezlopi-util-mqttcfg holds the validation and has no ESP-IDF dependency, which
# is what lets tests/host/test_mqttcfg.c sweep it on a desktop.

set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-trace
        ezlopi-util-mqttcfg
)

set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-nvs
        ezlopi-core-errors
)

idf_component_register(SRCS "ezlopi_core_mqtt_config.c"
    INCLUDE_DIRS "."
    REQUIRES    ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_UTIL_COMPONENTS}
                ezlopi-user-config
)
