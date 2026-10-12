# file( GLOB_RECURSE provisioning_src "*.c*" "actions/*.c*" )
file(GLOB_RECURSE src_files "./*.c*")


set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-wifi
        ezlopi-core-nvs
        ezlopi-core-factory-info
        ezlopi-core-net
        ezlopi-core-reset
        ezlopi-core-ezlopi
        ezlopi-core-sntp
        ezlopi-core-info
        ezlopi-core-processes
        ezlopi-core-ping
        ezlopi-core-log
        ezlopi-core-errors
)
set(EZLOPI_HAL_COMPONENTS
        ezlopi-hal-sysinfo
)
set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-sanitize
)
set(EZLOPI_CLOUD_COMPONENTS
        ezlopi-cloud-info
)

# ezlopi-service-improv supplies the Improv serial filter that
# ezpi_service_uart_task() calls on the raw byte buffer. The dependency only
# goes this way: that component must not REQUIRE this one back, so its
# transport writes to UART0 directly rather than through
# EZPI_SERV_uart_tx_data() -- which it could not use anyway, because that
# function appends "\r\n" and splits the write in two.
set(EZLOPI_SERVICE_COMPONENTS
        ezlopi-service-ble
        ezlopi-service-improv
)

# This component's USB-CDC code is already target-conditional in the source:
# ezlopi_service_uart.c wraps every tinyusb.h / tusb_cdc_acm.h include and every
# call behind
#     #if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C3)
# because neither of those chips has a USB-OTG peripheral. The REQUIRES below
# did not know that, and listed tinyusb unconditionally.
#
# On IDF 4.4.6 that is harmless by accident: IDF ships a tinyusb component whose
# Kconfig is "visible if USB_OTG_SUPPORTED", so on esp32 it registers nothing and
# the dependency resolves to an empty stub. It is still wrong to ask for it.
#
# It stops being harmless on IDF 5.x, where tinyusb is no longer an IDF component
# at all -- cmake configure fails outright with "Failed to resolve component
# 'tinyusb'". The registry replacement cannot stand in either: espressif/esp_tinyusb
# depends on espressif/tinyusb, which supports only chips that have the USB
# peripheral, so on esp32 it is not a naming problem to solve but a dependency that
# should not be requested. Hence: ask for it only where the source will use it.
idf_build_get_property(ezpi_target IDF_TARGET)
if(ezpi_target STREQUAL "esp32" OR ezpi_target STREQUAL "esp32c3")
    set(EZPI_USB_COMPONENT "")
else()
    set(EZPI_USB_COMPONENT "tinyusb")
endif()

idf_component_register(SRCS "ezlopi_service_uart.c" "${src_files}"
    INCLUDE_DIRS "."
    REQUIRES    ${EZPI_USB_COMPONENT} ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_HAL_COMPONENTS}
                ${EZLOPI_CLOUD_COMPONENTS}
                ${EZLOPI_SERVICE_COMPONENTS}
                ${EZLOPI_UTIL_COMPONENTS}
)
