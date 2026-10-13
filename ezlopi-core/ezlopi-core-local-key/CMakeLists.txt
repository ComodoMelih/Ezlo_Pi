# Owns one decision: where the local API key comes from when nobody has
# provisioned the device. The comparison, validation and generation themselves
# are in ezlopi-util-localkey, which has no REQUIRES at all so that tests/host
# can exercise them on a desktop. What is left here is the part that genuinely
# needs a device -- the factory partition, NVS, and esp_fill_random() -- kept
# deliberately thin for that reason.

file(GLOB_RECURSE config_src "./*.c")

set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-nvs
        ezlopi-core-factory-info
        ezlopi-core-errors
        ezlopi-user-config
)

set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-localkey
)

idf_component_register(SRCS "${config_src}"
    INCLUDE_DIRS "."
    REQUIRES ${EZLOPI_CORE_COMPONENTS}
             ${EZLOPI_UTIL_COMPONENTS})
