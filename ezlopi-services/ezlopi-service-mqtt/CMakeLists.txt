# The MQTT service. Compiled unconditionally so that an enabled-by-Kconfig
# build and a disabled one differ only by the guard inside the source, which
# is what keeps the disabled build honest: a reference that only resolves when
# the option is on would otherwise pass CI and fail for whoever turns it on.
#
# 'mqtt' is the IDF's own esp-mqtt component. 'ezlopi-util-ha' holds the
# item -> entity mapping and the topic builders, kept apart from this service
# because they are pure and can therefore be tested on a desktop; see
# tests/host/test_ha.c.
#
# 'ezlopi-util-mqttcfg' is required directly, not only transitively through
# ezlopi-core-mqtt-config, because this file's source uses it in two places of
# its own: EZPI_MQTTCFG_PREFIX_MAX in the compile-time assert on the topic
# buffer, and EZPI_util_mqttcfg_uri_is_valid() on the resolved URI. Relying on
# the transitive edge would work until someone reorganised the core component.

set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-trace
        ezlopi-util-ha
        ezlopi-util-mqttcfg
)

set(SDK_COMPONENTS
        mqtt
        esp_hw_support
)

set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-ezlopi
        ezlopi-core-devices
        ezlopi-core-errors
        ezlopi-core-factory-info
        ezlopi-core-mqtt-config
)

set(EZLOPI_CLOUD_COMPONENTS
        ezlopi-cloud-constants
        ezlopi-cloud-items
)

idf_component_register(SRCS "ezlopi_service_mqtt.c"
    INCLUDE_DIRS "."
    REQUIRES    ${SDK_COMPONENTS}
                ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_UTIL_COMPONENTS}
                ${EZLOPI_CLOUD_COMPONENTS}
)
