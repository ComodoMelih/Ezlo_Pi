file(GLOB_RECURSE config_src "./*.c")

set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-nvs
        ezlopi-user-config
)

set(EZLOPI_UTIL_COMPONENTS
        # EZPI_util_pem_store_len bounds the certificate and key writes against
        # the 4 KB field they land in. The decision lives in a dependency-free
        # component because this file needs esp_partition and so cannot be
        # host-compiled, while the length arithmetic is exactly what wants
        # testing.
        ezlopi-util-sanitize
)

idf_component_register(SRCS  "${config_src}"
    INCLUDE_DIRS "."
    REQUIRES ${EZLOPI_CORE_COMPONENTS}
             ${EZLOPI_UTIL_COMPONENTS})
