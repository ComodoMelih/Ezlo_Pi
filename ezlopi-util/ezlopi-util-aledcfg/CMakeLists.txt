# REQUIRES cjext because it parses a stored config object, and
# ezlopi-cloud-constants because the key names belong with every other config
# key rather than as literals in the parser. ezlopi-util-aled for the colour
# order enum and for EZPI_aled_wire_len(), which carries the overflow check
# that bounds the strip buffer -- shared rather than reimplemented, so the
# driver's allocation and this component's limit are the same arithmetic.
#
# Note the asymmetry with ezlopi-util-aled next door, which has a deliberately
# EMPTY requires list: the colour arithmetic has to stay compilable by a plain
# host compiler so it can be swept exhaustively. This component does not need
# that -- the input here is a JSON document, and the interesting cases number
# in the thousands rather than the millions -- so it may depend on cjext. What
# neither of them may depend on is an IDF header. That is still the line, and
# it is why the pixel count is resolved here instead of in the driver: the
# driver cannot be compiled without an RMT peripheral, and the judgement about
# whether a stored config is usable is exactly the part worth testing.

idf_component_register(SRCS "ezlopi_util_aledcfg.c"
    INCLUDE_DIRS "."
    REQUIRES cjext
             ezlopi-cloud-constants
             ezlopi-util-aled)
