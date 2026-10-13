# REQUIRES is deliberately empty, as it is for ezlopi-util-localkey and
# ezlopi-util-ha. This component includes nothing but <string.h>, <stdbool.h>
# and <stddef.h>, which is what lets tests/host/test_mqttcfg.c compile it with
# a desktop gcc and no stubs at all.
#
# It stays that way on purpose. The moment this file needs esp_log.h or
# nvs_flash.h, the validation stops being testable and the awkward cases --
# a '#' in a topic prefix, a port of 65536, a scheme that differs only in
# case -- go back to being reasoned about rather than checked.

idf_component_register(SRCS "ezlopi_util_mqttcfg.c"
    INCLUDE_DIRS "."
    REQUIRES )
