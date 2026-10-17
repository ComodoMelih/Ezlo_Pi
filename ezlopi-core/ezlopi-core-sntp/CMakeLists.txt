file(GLOB_RECURSE config_src "*.c")

# ezlopi-core-wifi is gone from this list, and two entries arrived in its
# place. Both of those were previously reaching ezlopi_core_sntp.c through it,
# which is why dropping it alone would have broken the build:
#
#   ezlopi-user-config    EZLOPI_USER_CONFIG.h, for ezlopi_free. Reachable
#                         anyway via ezlopi-util-trace, which requires it --
#                         named here because the source includes it directly
#                         now, so the declaration should not depend on another
#                         component's requirements staying as they are.
#   ezlopi-util-version   ezlopi_util_version.h, the only definition of
#                         BUILD_DATE in the tree, which
#                         EZPI_core_sntp_get_current_time_sec_abc() uses as its
#                         sanity floor. This one is load-bearing: it was only
#                         ever on the include path by way of
#                         wifi -> ezlopi-service-uart -> ezlopi-core-info, and
#                         nothing else in this list reaches it.
#
# The wifi entry named no symbol this component uses -- the source included
# ezlopi_core_wifi.h without referring to a single wifi or netif function --
# which is exactly what made it look droppable. Worth stating plainly: a
# REQUIRES entry can be holding up a header it never mentions.
#
# And not only for this component. Because REQUIRES are transitive for include
# dirs, anything that requires ezlopi-core-sntp inherited the whole wifi
# closure, so removing the entry changed four components' include paths: this
# one, ezlopi-cloud-protect, ezlopi-core-http and ezlopi-core-protect. Only
# ezlopi-cloud-protect was actually using something from it -- a dead include of
# ezlopi_core_cjson_macros.h, reached via
# wifi -> ezlopi-service-uart -> ezlopi-core-ezlopi -- and all four target builds
# stopped there. The include was deleted rather than the dependency declared,
# since the file uses none of that header's macros. If wifi ever has to come
# back here, note that it would be restoring an include path for components that
# are not named anywhere in this file.
set(EZLOPI_CORE_COMPONENTS
        nvs_flash
        micro_tz_db
        ezlopi-util-trace
        ezlopi-util-version
        ezlopi-user-config
        ezlopi-core-nvs
        ezlopi-core-errors
)
set(EZLOPI_HAL_COMPONENTS

)
set(EZLOPI_CLOUD_COMPONENTS

)
set(EZLOPI_SENSOR_DEVICE_COMPONENTS

)


idf_component_register(SRCS "ezlopi_core_sntp.c" "${config_src}"
                        INCLUDE_DIRS "."
                        REQUIRES        ${EZLOPI_CORE_COMPONENTS}
                                        ${EZLOPI_HAL_COMPONENTS}
                                        ${EZLOPI_CLOUD_COMPONENTS}
                                        ${EZLOPI_SENSOR_DEVICE_COMPONENTS}
                        )
