/* Host tests for ezlopi-util-wssession.
 *
 * WHY THIS FILE EXISTS
 *
 * The local websocket API on port 17001 gated every method behind one
 * file-static bool in ezlopi_core_offline_login.c:
 *
 *     static bool logged_in = false;
 *
 * set true by EZPI_core_offline_login_perform() when a client presented the
 * right local key, and read by __message_upcall() in ezlopi_service_ws_server.c
 * before handing the payload to EZPI_core_api_consume(). Nothing anywhere
 * associated that flag with the connection the request arrived on, and the
 * server binds INADDR_ANY rather than loopback, so the whole local network
 * shared one session:
 *
 *   - A second peer that merely connects passes the gate for as long as some
 *     other client is logged in, and may call any method without presenting
 *     any credential.
 *   - A websocket CLOSE frame reaches EZPI_core_offline_logout_perform() with
 *     no login check, so any peer can end the legitimate client's session.
 *   - login_perform() refuses when the flag is already set, so a genuine
 *     second client is turned away while the unauthenticated one is admitted.
 *
 * HOW THAT IS DEMONSTRATED RATHER THAN ASSERTED
 *
 * The two models below are driven through the same scenarios by the same code.
 * legacy_* is the old global bool, in the same shape as the new interface.
 * test_legacy_fails_the_properties() asserts that it DOES admit a stranger and
 * DOES let a stranger log everyone out -- so if someone believes the concern
 * was theoretical, running this file answers them, and if the old model ever
 * stops failing, the premise of this component is wrong and it should go.
 *
 * THE PROPERTY WORTH THE FILE
 *
 * test_no_connection_inherits() enumerates every ordered pair of distinct
 * descriptors and asserts that authorising one never authorises the other.
 * That is checked across the pairs rather than on an example I happened to
 * pick, which matters because the defect being fixed is precisely a case
 * nobody thought to try.
 *
 * THE SUBTLE ONE
 *
 * test_reused_descriptor_does_not_inherit() is the test that justifies keying
 * on the descriptor at all. The operating system reuses socket numbers. If an
 * entry could outlive its connection, the next connection handed that number
 * would inherit a live session -- the original bug with extra steps, and
 * harder to see. It is checked both for an orderly close and for a close that
 * never arrives, because correctness must not depend on the close hook firing.
 */

#include <stdbool.h>
#include <stdio.h>

#include "ezlopi_util_wssession.h"

static int g_checks;
static int g_fails;

static void ok(int cond, const char *what)
{
    g_checks++;
    if (!cond)
    {
        g_fails++;
        printf("  FAIL: %s\n", what);
    }
}

/* ------------------------------------------------------------------------- */
/* The behaviour being replaced, in the same shape as the new interface, so a
 * scenario can be run against either. The descriptor is accepted and ignored,
 * because ignoring it is exactly what the defect was. */

static bool legacy_logged_in;

static bool legacy_open(int fd)
{
    (void)fd;
    /* The old code had no accept hook at all; a connection simply existed. It
     * notably did NOT clear the flag, which is why a new connection could
     * arrive into someone else's session. */
    return true;
}

static void legacy_close(int fd)
{
    (void)fd;
    /* EZPI_core_offline_logout_perform(), reached from a CLOSE frame on any
     * connection, with no check of who sent it. */
    legacy_logged_in = false;
}

static bool legacy_authorise(int fd)
{
    (void)fd;
    legacy_logged_in = true;
    return true;
}

static bool legacy_is_authorised(int fd)
{
    (void)fd;
    return legacy_logged_in;
}

static void legacy_reset(void)
{
    legacy_logged_in = false;
}

/* A model of either implementation, so one scenario tests both. */
typedef struct s_model
{
    const char *name;
    bool (*open)(int fd);
    void (*close)(int fd);
    bool (*authorise)(int fd);
    bool (*is_authorised)(int fd);
    void (*reset)(void);
} s_model_t;

static const s_model_t model_new = {
    "per-connection",
    EZPI_util_wssession_open,
    EZPI_util_wssession_close,
    EZPI_util_wssession_authorise,
    EZPI_util_wssession_is_authorised,
    EZPI_util_wssession_reset_all,
};

static const s_model_t model_legacy = {
    "global-bool",
    legacy_open,
    legacy_close,
    legacy_authorise,
    legacy_is_authorised,
    legacy_reset,
};

/* ------------------------------------------------------------------------- */
/* The scenarios, each written once and answered by either model. */

/* A legitimate client logs in; a stranger then connects. Does the stranger get
 * to call the API? */
static bool scenario_stranger_is_admitted(const s_model_t *m)
{
    m->reset();

    const int legit = 4;
    const int stranger = 5;

    m->open(legit);
    m->authorise(legit);

    m->open(stranger);

    return m->is_authorised(stranger);
}

/* A legitimate client logs in; a stranger connects and disconnects. Is the
 * legitimate client still logged in? */
static bool scenario_stranger_can_log_everyone_out(const s_model_t *m)
{
    m->reset();

    const int legit = 4;
    const int stranger = 5;

    m->open(legit);
    m->authorise(legit);

    m->open(stranger);
    m->close(stranger);

    return !m->is_authorised(legit);
}

/* ------------------------------------------------------------------------- */

/* The property, over every ordered pair of distinct descriptors rather than
 * over one example. */
static void test_no_connection_inherits(void)
{
    /* Descriptors chosen to include 0, because a zero-initialised table would
     * claim to own descriptor 0 and this is the case that catches it. */
    static const int fds[] = {0, 1, 3, 4, 7, 11, 12, 29};
    const int n = (int)(sizeof(fds) / sizeof(fds[0]));

    for (int i = 0; i < n; i++)
    {
        for (int j = 0; j < n; j++)
        {
            if (i == j)
            {
                continue;
            }

            EZPI_util_wssession_reset_all();

            EZPI_util_wssession_open(fds[i]);
            EZPI_util_wssession_open(fds[j]);
            EZPI_util_wssession_authorise(fds[i]);

            if (EZPI_util_wssession_is_authorised(fds[j]))
            {
                printf("  fd %d inherited the session of fd %d\n", fds[j], fds[i]);
            }

            ok(EZPI_util_wssession_is_authorised(fds[i]),
               "the connection that logged in is authorised");
            ok(!EZPI_util_wssession_is_authorised(fds[j]),
               "no other connection inherits that authorisation");
        }
    }
}

/* Closing one connection must not disturb another, whichever order they were
 * opened in. */
static void test_close_affects_only_itself(void)
{
    static const int fds[] = {0, 2, 6, 9};
    const int n = (int)(sizeof(fds) / sizeof(fds[0]));

    for (int i = 0; i < n; i++)
    {
        for (int j = 0; j < n; j++)
        {
            if (i == j)
            {
                continue;
            }

            EZPI_util_wssession_reset_all();

            EZPI_util_wssession_open(fds[i]);
            EZPI_util_wssession_authorise(fds[i]);
            EZPI_util_wssession_open(fds[j]);
            EZPI_util_wssession_authorise(fds[j]);

            EZPI_util_wssession_close(fds[j]);

            ok(EZPI_util_wssession_is_authorised(fds[i]),
               "closing one connection leaves the other logged in");
            ok(!EZPI_util_wssession_is_authorised(fds[j]),
               "the closed connection is no longer authorised");
        }
    }
}

/* The subtle one. A descriptor number is reused by the next connection, and
 * that connection must start with nothing -- whether or not the close for the
 * previous owner was ever delivered. */
static void test_reused_descriptor_does_not_inherit(void)
{
    const int fd = 6;

    /* The orderly case: close, then reuse. */
    EZPI_util_wssession_reset_all();
    EZPI_util_wssession_open(fd);
    EZPI_util_wssession_authorise(fd);
    EZPI_util_wssession_close(fd);
    EZPI_util_wssession_open(fd);
    ok(!EZPI_util_wssession_is_authorised(fd),
       "a reused descriptor starts unauthorised after an orderly close");

    /* The case that matters: the close never arrived, and the number has been
     * handed to a new connection. Correctness must not depend on the close
     * hook firing, because a missed close is exactly the circumstance that
     * would otherwise give a stranger a live session. */
    EZPI_util_wssession_reset_all();
    EZPI_util_wssession_open(fd);
    EZPI_util_wssession_authorise(fd);
    EZPI_util_wssession_open(fd); /* no close in between */
    ok(!EZPI_util_wssession_is_authorised(fd),
       "a reused descriptor starts unauthorised even with no close");

    /* And reopening must not consume a second slot. */
    ok(1 == EZPI_util_wssession_count(),
       "reopening a descriptor does not leak a slot");
}

/* A descriptor the server never announced has no authority, and cannot be
 * given any. */
static void test_unknown_descriptor_is_refused(void)
{
    EZPI_util_wssession_reset_all();

    ok(!EZPI_util_wssession_is_authorised(3),
       "a descriptor with no session is not authorised");
    ok(!EZPI_util_wssession_authorise(3),
       "a descriptor with no session cannot be authorised");
    ok(!EZPI_util_wssession_is_authorised(3),
       "and is still not authorised after the attempt");

    /* Closing something that was never opened must not corrupt the table. */
    EZPI_util_wssession_close(3);
    ok(0 == EZPI_util_wssession_count(),
       "closing an unknown descriptor changes nothing");
}

/* Negative descriptors cannot be tracked -- httpd_req_to_sockfd() returns -1
 * on error, and that value must never become a session, least of all one that
 * several failed lookups could share. */
static void test_negative_descriptor_is_refused(void)
{
    EZPI_util_wssession_reset_all();

    ok(!EZPI_util_wssession_open(-1), "descriptor -1 is refused");
    ok(!EZPI_util_wssession_open(-7), "a negative descriptor is refused");
    ok(!EZPI_util_wssession_is_authorised(-1), "descriptor -1 is not authorised");
    ok(!EZPI_util_wssession_authorise(-1), "descriptor -1 cannot be authorised");
    ok(0 == EZPI_util_wssession_count(), "no slot was consumed");
}

/* Filling the table must fail closed. The server's own max_open_sockets is 7
 * against a table of 8, so this is unreachable in the shipped configuration --
 * which is the reason to test it, because nothing else will. */
static void test_full_table_fails_closed(void)
{
    EZPI_util_wssession_reset_all();

    for (int i = 0; i < EZPI_WSSESSION_MAX; i++)
    {
        ok(EZPI_util_wssession_open(100 + i), "a slot is available");
    }

    ok(EZPI_WSSESSION_MAX == EZPI_util_wssession_count(), "the table is full");

    const int overflow = 200;
    ok(!EZPI_util_wssession_open(overflow), "the next connection is refused");
    ok(!EZPI_util_wssession_authorise(overflow),
       "a refused connection cannot be authorised");
    ok(!EZPI_util_wssession_is_authorised(overflow),
       "a refused connection is not authorised");

    /* And the refusal must not have disturbed the sessions that were there. */
    for (int i = 0; i < EZPI_WSSESSION_MAX; i++)
    {
        ok(!EZPI_util_wssession_is_authorised(100 + i),
           "existing sessions are untouched by the refusal");
    }

    /* A slot freed by a close becomes usable again, so a full table is a
     * transient condition rather than a permanent one. */
    EZPI_util_wssession_close(100);
    ok(EZPI_util_wssession_open(overflow),
       "a freed slot is reusable");
}

/* Several clients logged in at once is the case the old model could not
 * express: login_perform() refused the second one. A local UI and the Ezlo app
 * being open together is ordinary use. */
static void test_several_clients_at_once(void)
{
    EZPI_util_wssession_reset_all();

    const int fds[] = {3, 4, 5};
    const int n = 3;

    for (int i = 0; i < n; i++)
    {
        ok(EZPI_util_wssession_open(fds[i]), "each client gets a session");
        ok(EZPI_util_wssession_authorise(fds[i]), "each client can log in");
    }

    for (int i = 0; i < n; i++)
    {
        ok(EZPI_util_wssession_is_authorised(fds[i]),
           "all of them stay logged in simultaneously");
    }

    ok(n == EZPI_util_wssession_count(), "all three sessions are held");
}

/* The new model must pass the two scenarios. */
static void test_new_model_passes_the_properties(void)
{
    ok(!scenario_stranger_is_admitted(&model_new),
       "a stranger is not admitted by someone else's login");
    ok(!scenario_stranger_can_log_everyone_out(&model_new),
       "a stranger cannot log the legitimate client out");
}

/* And the old one must fail both. If this test ever passes, the defect this
 * component exists for was not real. */
static void test_legacy_fails_the_properties(void)
{
    ok(scenario_stranger_is_admitted(&model_legacy),
       "old behaviour: a stranger IS admitted by someone else's login");
    ok(scenario_stranger_can_log_everyone_out(&model_legacy),
       "old behaviour: a stranger CAN log the legitimate client out");
}

/* Sessions must not survive a server restart, which is what reset_all is for:
 * every socket is gone, so every session must be. */
static void test_reset_drops_everything(void)
{
    EZPI_util_wssession_reset_all();

    EZPI_util_wssession_open(4);
    EZPI_util_wssession_authorise(4);
    EZPI_util_wssession_open(5);

    EZPI_util_wssession_reset_all();

    ok(0 == EZPI_util_wssession_count(), "no sessions survive a reset");
    ok(!EZPI_util_wssession_is_authorised(4),
       "an authorised session does not survive a reset");
}

int main(void)
{
    printf("ezlopi-util-wssession host tests\n\n");

    test_no_connection_inherits();
    test_close_affects_only_itself();
    test_reused_descriptor_does_not_inherit();
    test_unknown_descriptor_is_refused();
    test_negative_descriptor_is_refused();
    test_full_table_fails_closed();
    test_several_clients_at_once();
    test_new_model_passes_the_properties();
    test_legacy_fails_the_properties();
    test_reset_drops_everything();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    /* Deliberately NOT tested here:
     *
     *   - That ezlopi_service_ws_server.c actually calls open from the accept
     *     hook, close from the close hook, and passes the right descriptor to
     *     is_authorised before EZPI_core_api_consume(). That is the wiring,
     *     and it is the part this file cannot reach: the component can be
     *     perfect while the caller asks about the wrong descriptor. It is
     *     covered by reading the call site and by the firmware build.
     *   - Anything about the transport. The local API is plain ws://, so a
     *     peer able to inject on the path can still hijack an established
     *     connection at the TCP level, and per-connection sessions do not
     *     change that. Only TLS would, and that is not what this component
     *     claims to do.
     *   - Whether the local key itself is strong, which is tests/host/
     *     test_localkey.c, and whether it is compared in constant time, which
     *     is EZPI_util_localkey_match().
     *   - Concurrency. These functions are unlocked because ESP-IDF's httpd
     *     runs them all on one task; that is a property of the caller and a
     *     host test cannot establish it.
     */

    return g_fails ? 1 : 0;
}
