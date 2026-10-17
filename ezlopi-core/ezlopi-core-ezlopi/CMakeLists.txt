file(GLOB_RECURSE config_src "*.c")

set(EZLOPI_CORE_COMPONENTS
        ezlopi-util-trace
        ezlopi-core-ble
        ezlopi-core-wifi
        ezlopi-core-sntp
        ezlopi-core-mdns
        ezlopi-core-modes
        ezlopi-core-scenes
        ezlopi-core-buffer
        ezlopi-core-log
        ezlopi-core-errors
        ezlopi-core-ezlopi
)

set(EZLOPI_CLOUD_COMPONENTS
        ezlopi-cloud-data
        ezlopi-cloud-devices
        ezlopi-cloud-scenes
        ezlopi-cloud-registration
        ezlopi-cloud-favorite
        ezlopi-cloud-gateways
        ezlopi-cloud-info
        ezlopi-cloud-modes
        ezlopi-cloud-items
        ezlopi-cloud-room
        ezlopi-cloud-settings
        ezlopi-cloud-network
        ezlopi-cloud-ota
        ezlopi-cloud-location
        ezlopi-cloud-coordinates
        ezlopi-cloud-offline-login
        ezlopi-cloud-status
        ezlopi-core-setting-commands
)

# spi_flash is PRIV_REQUIRES rather than REQUIRES because the dependency is an
# implementation detail of ezlopi_core_pin_reserved.c, which calls
# esp_mspi_get_io() from esp_private/spi_flash_os.h.
# ezlopi_core_pin_reserved.h deliberately exposes no ESP-IDF type, so nothing
# including this component's headers needs spi_flash as well.
#
# Declared even though the build already succeeds without it: the include path
# reaches esp_private/ transitively through other components, which was measured
# rather than assumed. That is an accident of the dependency graph and not a
# guarantee -- it would break on whichever unrelated component stopped
# requiring spi_flash first, and the error would point here.
idf_component_register(SRCS  "${config_src}"
    INCLUDE_DIRS "."
    REQUIRES    freertos
                ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_CLOUD_COMPONENTS}
                ezlopi-service-system-temperature-sensor
    PRIV_REQUIRES spi_flash
)

