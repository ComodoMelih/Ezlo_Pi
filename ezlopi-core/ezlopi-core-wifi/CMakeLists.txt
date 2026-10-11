# set(config_src "config.cpp")

file(GLOB_RECURSE wifi_src "./*.c*")

# The 'dns_hijacking' requirement went away with ezlopi_core_wifi_ap.c, the
# only file in this component that used it. What is left here is station-mode
# only.
set(EZLOPI_SERVICE_COMPONENTS
        ezlopi-service-uart
)

set (EZLOPI_CORE_COMPONENTS
        ezlopi-core-processes
        ezlopi-core-errors
        ezlopi-core-ping
)


idf_component_register(SRCS  "${wifi_src}"
    INCLUDE_DIRS "."
    REQUIRES    ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_SERVICE_COMPONENTS}
        )
