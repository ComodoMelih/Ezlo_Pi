# The API methods that write the MQTT settings.
#
# Compiled unconditionally, with the whole of the source and the whole of the
# header body inside #ifdef CONFIG_EZPI_ENABLE_MQTT -- the same shape as
# ezlopi-cloud-ota, and for the same reason: the method registry includes this
# header unconditionally, so the header has to exist and be harmless when the
# feature is off, and a component registered only in some configurations is a
# component whose CMake wiring is only tested in some configurations.
#
# ezlopi-util-mqttcfg is required directly because the field table names its
# validators and its EZPI_MQTTCFG_*_MAX bounds. ezlopi-core-mqtt-config is
# where the values are actually stored.

# Include all source files
file(GLOB_RECURSE config_src "*.c")

#Define SDK components
set(ESP_IDF_COMPONENTS
)

# Define core components
set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-errors
        ezlopi-core-mqtt-config
)

# Define HAL components
set(EZLOPI_HAL_COMPONENTS
)

# Define cloud components
set(EZLOPI_CLOUD_COMPONENTS
        ezlopi-cloud-constants
)

# Define service components
set(EZLOPI_SERV_COMPONENTS
)

# Define sensor device components
set(EZLOPI_SENSOR_DEVICE_COMPONENTS
)

# Define ezlopi-utils components
set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-trace
        ezlopi-util-mqttcfg
)

# Register the components
idf_component_register(SRCS  "${config_src}"
                        INCLUDE_DIRS "."
                        REQUIRES        ${ESP_IDF_COMPONENTS}
                                        ${EZLOPI_CORE_COMPONENTS}
                                        ${EZLOPI_HAL_COMPONENTS}
                                        ${EZLOPI_CLOUD_COMPONENTS}
                                        ${EZLOPI_SERV_COMPONENTS}
                                        ${EZLOPI_SENSOR_DEVICE_COMPONENTS}
                                        ${EZLOPI_UTIL_COMPONENTS}
)
