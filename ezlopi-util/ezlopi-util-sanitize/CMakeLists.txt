# Deliberately has no REQUIRES: this component must stay dependency-free so
# that tests/host can compile it with a plain host compiler.

idf_component_register(SRCS "ezlopi_util_sanitize.c"
    INCLUDE_DIRS "."
    REQUIRES )
