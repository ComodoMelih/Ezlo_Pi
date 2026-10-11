# Deliberately has no REQUIRES: this component must stay dependency-free so
# that tests/host can compile it with a plain host compiler. The mapping it
# holds is the kind that fails silently -- a device class Home Assistant
# rejects, or a unit it accepts and displays wrongly -- so it is worth being
# able to check every combination on a desktop rather than one at a time on a
# board with a broker attached.

idf_component_register(SRCS "ezlopi_util_ha.c"
    INCLUDE_DIRS "."
    REQUIRES )
