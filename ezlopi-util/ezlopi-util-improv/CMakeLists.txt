# Deliberately has no REQUIRES: this component must stay dependency-free so
# that tests/host can compile it with a plain host compiler. The Improv frame
# parser handles attacker-chosen lengths, which is precisely the code worth
# running under ASan on a desktop rather than only on a board.

idf_component_register(SRCS "ezlopi_util_improv.c"
    INCLUDE_DIRS "."
    REQUIRES )
