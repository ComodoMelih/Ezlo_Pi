# Deliberately has no REQUIRES: this component must stay dependency-free so
# that tests/host can compile it with a plain host compiler. Following
# ezlopi-util-sanitize and ezlopi-util-improv, and for a reason specific to
# this one -- the colour arithmetic here is swept exhaustively (all 16.7M
# (h,s,v) triples) under ASan and UBSan, which is only possible on a desktop.
#
# If this component ever needs an IDF header, that is the signal that the thing
# needing it belongs in the device driver or the RMT layer instead, not here.

idf_component_register(SRCS "ezlopi_util_aled.c"
    INCLUDE_DIRS "."
    REQUIRES )
