# hub.ezlopi.config.get / .set -- reading and replacing the device
# configuration over the API, which until now could only be done over BLE or a
# serial cable.
#
# Two sources, and the split is deliberate. ezlopi_cloud_devcfg.c holds the
# decisions and is compiled unchanged by tests/host; ezlopi_cloud_devcfg_port.c
# is the only file here that includes ezlopi-core-factory-info (which pulls in
# <esp_partition.h>) or ezlopi-core-devices (which pulls in the whole HAL), and
# it is six forwarding bodies. The host test links the first against fakes of
# the second. See ezlopi_cloud_devcfg_port.h for why the seam is there rather
# than somewhere tidier.
#
# ezlopi-util-devcfgval does the shape checking and was committed before this
# with nothing calling it; this is the caller. The one check it declines to make
# -- whether an id_item names a driver this image actually contains -- is why
# ezlopi-core-devices is in the list below, for its device table and for the
# chipset name the boot parser compares against.
#
# No #ifdef around the sources, unlike ezlopi-cloud-mqtt-config next door. MQTT
# is an optional feature; a device configuration is not something this firmware
# can be built without.

# Include all source files
file(GLOB_RECURSE config_src "*.c")

#Define SDK components
set(ESP_IDF_COMPONENTS
)

# Define core components
set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-errors
        ezlopi-core-devices
        ezlopi-core-factory-info
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
        ezlopi-util-devcfgval
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
