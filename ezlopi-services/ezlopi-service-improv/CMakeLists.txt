# The transport-independent half of Improv, plus the serial transport.
#
# This component must not depend on ezlopi-service-uart even though the serial
# transport shares UART0 with it: ezlopi-service-uart already REQUIREs this
# component so that its reader can call the filter, and requiring it back would
# be a cycle. The transport therefore writes to the port directly rather than
# through EZPI_SERV_uart_tx_data() -- which it could not use anyway, because
# that function appends "\r\n" and splits the write in two.
#
# The BLE transport lives in ezlopi-service-ble, which also REQUIREs this.

file(GLOB_RECURSE src_files "./*.c*")

set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-wifi
        ezlopi-core-nvs
        ezlopi-core-factory-info
)

set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-improv
        ezlopi-util-version
)

idf_component_register(SRCS "${src_files}"
    INCLUDE_DIRS "."
    REQUIRES    driver
                ezlopi-user-config
                ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_UTIL_COMPONENTS}
)
