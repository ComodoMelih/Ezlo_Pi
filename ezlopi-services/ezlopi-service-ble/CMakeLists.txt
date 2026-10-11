# file( GLOB_RECURSE provisioning_src "*.c*" "actions/*.c*" )
file(GLOB_RECURSE src_files "*.c*")

set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-devices
        ezlopi-core-ble
        ezlopi-core-reset
        ezlopi-core-ping
        ezlopi-core-sntp
        ezlopi-core-info
        ezlopi-core-errors
        # ezlopi-user-config
)

set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-sanitize
)

# ezlopi-service-improv holds the transport-independent half of Improv: the
# provisioning gate, the state machine and what each RPC answers.
# ezlopi_service_ble_improv.c is only the GATT transport over it, and the serial
# transport in that same component is the other. Required unconditionally rather
# than behind CONFIG_EZPI_BLE_ENABLE_IMPROV, because component requirements are
# resolved before the Kconfig-guarded sources are compiled; the component's own
# sources compile to nothing when neither Improv option is set.
set(EZLOPI_SERVICE_COMPONENTS
        ezlopi-service-improv
)

idf_component_register(SRCS "ezlopi_service_ble.c" "ezlopi_service_ble_security.c" "ezlopi_service_ble_dynamic_config.c" "ezlopi_service_ble_device_info.c" "ezlopi_service_ble_provisioning.c" "ezlopi_service_ble_wifi.c" "ezlopi_service_ble_ble_auth.c" "${src_files}"
    INCLUDE_DIRS "."
    REQUIRES    ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_UTIL_COMPONENTS}
                ${EZLOPI_SERVICE_COMPONENTS}
    )
