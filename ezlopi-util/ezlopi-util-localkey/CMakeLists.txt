# Deliberately has no REQUIRES, for the same reason as ezlopi-util-sanitize and
# ezlopi-util-ha: tests/host must be able to compile it with a plain host
# compiler. That matters more here than for the other two. This file decides
# whether a token presented to the local API is accepted, and the defects it
# exists to prevent -- a prefix match, an empty key that matches everything, a
# biased generation alphabet -- all produce a device that works perfectly in
# every manual test while accepting credentials it should not.
#
# Randomness is not sourced here either; the caller passes bytes in. That is
# what lets every case be driven from a fixed vector on a desktop.

idf_component_register(SRCS "ezlopi_util_localkey.c"
    INCLUDE_DIRS "."
    REQUIRES )
