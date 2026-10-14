# Deliberately has no REQUIRES, for the same reason as ezlopi-util-sanitize,
# ezlopi-util-devcfg and ezlopi-util-localkey: tests/host must be able to
# compile it with a plain host compiler.
#
# This one decides who is allowed to call the local API, so being able to test
# it without hardware is not a convenience. The behaviour it replaces was a
# single file-static bool in ezlopi_core_offline_login.c, and the defects in it
# -- a second peer inheriting an open session, and any peer being able to log
# everyone out -- are invisible to a single-client test. They need two
# connections driven independently, which is awkward on a board and trivial
# here.
#
# Keeping it free of esp_http_server also keeps the component honest about what
# it is: a table keyed on an integer the caller supplies. It never asks ESP-IDF
# anything, so there is nothing in it that only makes sense on target.

idf_component_register(SRCS "ezlopi_util_wssession.c"
    INCLUDE_DIRS "."
    REQUIRES )
