#!/usr/bin/env python3
"""Fail if a credential is passed by value to a logging call.

THE BUG THIS EXISTS TO PREVENT

Eight lines across seven files printed a secret to the serial console or
exported one off the device: the local API key twice, the Wi-Fi PSK twice, the
cloud provisioning token, the device's SSL private key, and two whole
request-body dumps. All are listed in CHANGELOG.md under "Credentials". A
representative sample:

    TRACE_D("password: %s", password_saved)                  # the local API key
    TRACE_D("Trying to connect to %s with password %s", ssid, pass)
    TRACE_D("str_provisioning_data[len: %u]: %s", n, str_provisioning_data)
    TRACE_OTEL(..., "payload: %s", rx_message->payload)      # leaves the device

It is tempting to read a TRACE_D as debug scaffolding that a release build
drops. It is not. These are EzloPi's own macros, not ESP_LOG, so
CONFIG_LOG_DEFAULT_LEVEL does not gate them; serial_log_severity defaults to
ENUM_EZLOPI_TRACE_SEVERITY_MAX (ezlopi_core_log.c); and the switch that would
compile them away, ENABLE_TRACE in ezlopi_util_trace.h, is derived from
CONFIG_EZPI_UTIL_TRACE_EN, which is "default y". The format strings for these
lines were verified present in the linked .bin.

WHY A CHECK AND NOT JUST THE FIXES

Because the fixes were found one at a time and the last four were only found by
going looking. That is the signature of a class, and a class needs a structural
guard: nothing about fixing eight lines stops a ninth from being written, and
the ninth will look just as harmless as the first eight did. The lines are also
spread across ezlopi-core, ezlopi-services and the BLE, websocket and cloud
paths, so no single file owns the invariant.

WHAT COUNTS AS A VIOLATION

A TRACE_*/printf/TRACE_OTEL call whose format string contains an %s-class
conversion, and among whose arguments is an identifier named like a secret,
passed by value rather than measured. "Measured" means wrapped in strlen() or
sizeof() -- reporting a length is the accepted fix throughout this tree, so
`strlen(pass)` is fine and `pass` is not.

HOW TO SATISFY IT

Log a length, not a value:

    TRACE_D("Trying to connect to %s with a %d-char password",
            ssid, (int)strlen(pass));

Or put the whole dump behind a Kconfig option and add that option to
GATED_SWITCHES below, as ezlopi_service_ws_server.c does with
CONFIG_EZPI_WS_LOG_PAYLOADS. A switch is preferred over redacting individual
fields: redaction means parsing untrusted input purely to decide what to log,
and it fails silently the first time a new method adds a secret field nobody
adds to the list. A switch cannot drift that way.

GATED_SWITCHES records the default each such option is expected to carry, and
the check verifies it against Kconfig.projbuild. Note that it PINS the value
rather than requiring `n`: EZPI_LOCAL_KEY_ANNOUNCE is `default y` on purpose,
because a self-flashed device generates its own local key and with the banner
off that key is unknowable. The point of pinning is that changing when a
credential gets printed then cannot happen by accident -- it fails here until
someone updates the table, which is the moment to argue about it.

The exemption applies only to the branch where the guard holds. The #else of
an `#ifdef CONFIG_X` is the path a default build takes, so a credential dumped
there is still reported.

LIMITS

This is a name-based check, so it is bounded in both directions and says so.

It cannot see a secret that is not named like one -- the ezlopi_core_nvs.c leak
printed the Wi-Fi password as `&wifi_info[32]`, an offset into a buffer named
after neither, and this check would not have found it. That one was caught by
searching the built binary for the format string instead, which is the
technique to reach for when this check passes and you are still suspicious.

It also does not match arguments to particular conversions: it asks "does this
call have an %s and is a secret among its arguments", not "is that secret the
thing the %s consumes". Pairing them up would need real argument parsing for
no benefit, since there is no good reason to pass a bare credential to a
logging call in any argument position.

The sharper version of that limit is a whole-body dump whose buffer is named
after the transport rather than the contents. Four sites in ezlopi_core_http.c
printed HTTP bodies through `request`, `config->response`, `curr_chunk->ptr`
and `evt->data`, and this check passed on all four, because none of those
names says "credential" -- yet one of them is the provision-sync reply, which
carries the device's SSL private key in PEM. `payload` in SECRET_SUBSTRINGS
covers exactly one such buffer, the one the websocket server happens to call
payload; it is a name, not a rule. All four are now behind
CONFIG_EZPI_HTTP_LOG_PAYLOADS, which this file pins, but they were found by
reading the provisioning path and asking what was in the body -- not by this
check, and not by a check of any shape that works on identifiers.

A variable named exactly `key` is not matched, only compounds like local_key
and private_key. Bare `key` was tried and produced three false positives and
no true ones: ezlopi_core_mdns.c logs `mdns_context->key`, a TXT-record key
name. The names every actual credential in this tree uses are all still
covered, but a future `char key[32]` holding a PSK would slip through.

Conversely it will flag an identifier that merely *looks* like a secret.
EZPI_util_devcfgval_token() returns a rejection-reason name, not a token, and
is listed in ALLOWED for that reason. Add to ALLOWED with a comment saying
why, rather than loosening the patterns.

Comments are stripped before scanning, and that is load-bearing rather than
tidiness. Each of the fixes deliberately quotes the line it replaced inside a
`**` comment so the file records what it used to do, so a naive scan reports
every fixed site as still broken. That is not hypothetical: the shell checks
used while making these fixes did exactly that, and both apparent failures were
the check's fault. SELF_TEST['fixed_with_quoted_original'] is that case.
"""
import os
import re
import sys

ROOTS = ('ezlopi-core', 'ezlopi-services', 'ezlopi-main', 'ezlopi-util',
         'ezlopi-cloud', 'ezlopi-components', 'ezlopi-hal', 'ezlopi-sensors')

# The logging sinks. TRACE_OTEL matters most of the four: it does not print to
# a console that needs physical access, it ships what it is given to a
# collector over the network.
CALL = re.compile(r'\b(TRACE_[A-Za-z]+|TRACE_OTEL|printf|ets_printf)\s*\(')

# Any C identifier. Whether one names a credential is decided by secret_name()
# below rather than by the regex, because the interesting names are compounds:
# the first version of this check used \bpassword\b and therefore did not match
# `password_saved`, the actual variable in the actual bug. The self-test caught
# that, which is the entire reason the self-test exists.
IDENT = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')

# Matched as substrings, so local_key, g_local_key and local_key_str all hit.
SECRET_SUBSTRINGS = (
    'password', 'passwd', 'pwd',
    'token',
    'secret',
    'private_key', 'shared_key', 'local_key',
    'provisioning_data',
    'credential',
    # Not a credential itself, but the envelope around three of them: the
    # request bodies on the local API carry params.token (the local API key),
    # params.network.key (the Wi-Fi PSK) and params.password. Two of the four
    # leaks in this class were whole-payload dumps rather than named fields,
    # so the name-based check misses the class entirely without this. It costs
    # nothing: the only site it matches in the tree is the one deliberate dump,
    # which is exempt under GATED_SWITCHES.
    'payload',
)

# Matched as whole identifiers only. `pass` is the Wi-Fi PSK parameter in
# ezlopi_core_wifi.c, but as a substring it would hit passed/bypass/passive;
# `psk` as a substring would hit nothing useful either.
#
# Bare `key` is deliberately NOT here. It was, and it produced three false
# positives and no true ones: ezlopi_core_mdns.c logs `mdns_context->key`,
# which is a TXT-record key name. Every actual credential in this tree is
# named pass, password, token, local_key, private_key or shared_key, all of
# which are still matched. The cost is recorded under LIMITS.
SECRET_EXACT = ('pass', 'passw', 'psk')

# Names excluded by hand, with the reason. A check that cries wolf gets
# deleted, so anything legitimate is named here rather than tolerated.
# Compared case-insensitively, so entries are lowercase.
ALLOWED = {
    # EZPI_util_devcfgval_token() returns a const char* name for a
    # device-config rejection reason -- "token" in the lexical sense, not the
    # credential sense. Declared in ezlopi_util_devcfgval.h, logged at three
    # sites in the cloud, BLE and UART config paths.
    'ezpi_util_devcfgval_token',
}

# Compile-time switches under which dumping a credential is accepted, because
# dumping one on purpose is a legitimate thing to be able to ask for. Mapped to
# the default each is expected to carry.
#
# The default is PINNED here, not dictated. The first version of this check
# required `default n` for all of them and immediately failed on
# EZPI_LOCAL_KEY_ANNOUNCE -- which is `default y` deliberately, and for a good
# reason recorded in its help text: a self-flashed device generates its own
# local key, and with the banner off that key is unknowable, so the local API
# cannot be logged into at all. The repository had already thought about this
# and the check was simply wrong.
#
# Pinning is still worth doing. Changing one of these defaults is a security
# decision, and the point of the check is that it cannot be made by accident:
# flipping a value here fails until someone updates this table, which is the
# moment to argue about it.
GATED_SWITCHES = {
    # Prints a locally generated local API key once at boot. Default y: see
    # the help text at Kconfig.projbuild:447. Never prints a
    # cloud-provisioned key, and anyone with the serial port could reflash
    # the board anyway.
    'CONFIG_EZPI_LOCAL_KEY_ANNOUNCE': 'y',
    # Prints every inbound local-API request body. Default n: the first
    # payload of every session is hub.offline.login.ui, which carries the
    # local API key in params.token, so turning this on prints the key on
    # every connection attempt by anything.
    'CONFIG_EZPI_WS_LOG_PAYLOADS': 'n',
    # Prints the bodies of the HTTP traffic handled by ezlopi-core-http: the
    # request this firmware assembles, its response, and every body chunk
    # esp_http_client delivers. Default n: the provision-sync reply is one of
    # those bodies, and __provision_update() reads this device's
    # ssl_private_key, ssl_shared_key, signing_ca_certificate and a new
    # provision_token out of it.
    #
    # This entry pins the default; it does not grant an exemption that the
    # scan would otherwise withhold. None of the four gated calls names a
    # secret in its arguments -- see LIMITS.
    'CONFIG_EZPI_HTTP_LOG_PAYLOADS': 'n',
}

KCONFIG = os.path.join('ezlopi-main', 'Kconfig.projbuild')

# An %s-class conversion: %s, %.*s, %20s, %-*s and friends.
PCT_S = re.compile(r'%[-+ #0-9.*]*s')

# strlen(x) / sizeof(x) around the secret: it is being measured, not printed.
MEASURE_CALL = re.compile(r'\b(?:strlen|strnlen|sizeof)\s*\(')

SELF_TEST = {
    # The four real lines, as they actually stood.
    'wifi': 'TRACE_D("Trying to connect to %s with password %s", ssid, pass);',
    'login': 'TRACE_D("password: %s", password_saved);',
    'prov': 'TRACE_D("str_provisioning_data[len: %u]: %s", (unsigned)prov_len, '
            'str_provisioning_data);',
    'otel': 'TRACE_OTEL(ENUM_EZLOPI_TRACE_SEVERITY_ERROR, '
            '"web-provisioning [method: null], payload: %s", '
            'rx_message->payload);',
    # Split across lines, as most real ones are once a comment is added.
    'multiline': 'TRACE_D("connecting to %s with password %s",\n'
                 '        ssid,\n'
                 '        pass);',
    # Measures the secret AND prints it. The earlier lookback implementation of
    # measured() accepted this, because looking back from the second `pass`
    # found the first one's `strlen(`. See measured() for the full story.
    'measured_and_printed':
        'TRACE_D("password (%d chars): %s", (int)strlen(pass), pass);',
}

SELF_TEST_OK = {
    # The accepted fix: a length.
    'length': 'TRACE_D("Trying to connect to %s with a %d-char password",\n'
              '        ssid, (int)strlen(pass));',
    'length_u': 'TRACE_D("str_provisioning_data[len: %u] (body withheld)",\n'
                '        (unsigned)strlen(str_provisioning_data));',
    # A secret named in the format string but not passed: nothing leaks.
    'named_only': 'TRACE_D("password check failed for %s", user_id);',
    # No %s at all.
    'no_pct_s': 'TRACE_D("password length: %d", (int)strlen(pass));',
    # THE REGRESSION THIS CHECK WAS BUILT WRONG WITHOUT ONCE ALREADY:
    # a fixed site that quotes the original inside a comment.
    'fixed_with_quoted_original': '''
        /* The password is reported as a length, not a value. This line
        ** used to be
        **
        **     TRACE_D("Trying to connect to %s with password %s", ssid, pass);
        **
        ** which printed the Wi-Fi PSK on the serial console. */
        TRACE_D("Trying to connect to %s with a %d-char password",
                ssid, (int)strlen(pass));
''',
    # A // comment holding the same thing.
    'fixed_with_line_comment': '''
        // was: TRACE_D("password: %s", password_saved);
        TRACE_D("password: %d chars", (int)strlen(password_saved));
''',
    # Measured through a member access -- the real shape of the webprov fix.
    # The %s here is the method name, so the call does reach measured()
    # instead of being dropped for having no %s at all.
    'measured_member':
        'TRACE_OTEL(ENUM_EZLOPI_TRACE_SEVERITY_ERROR,\n'
        '           "web-provisioning [method: %s], payload[len: %d]",\n'
        '           method_str, (int)strlen(rx_message->payload));',
    # Measured through a struct field and through an array index.
    'measured_field':
        'TRACE_D("ssid %s, password %d chars",\n'
        '        cfg.ssid, (int)sizeof(cfg.password));',
    'measured_index':
        'TRACE_D("ssid %s, password %d chars",\n'
        '        nets[i].ssid, (int)strlen(nets[i].password));',
}


def strip_comments(text):
    """Remove /* */ and // comments, keeping newlines so line numbers hold.

    String literals are respected, so a "//" or "/*" inside one is not treated
    as the start of a comment.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            end = text.find('*/', i + 2)
            if end < 0:
                break
            out.append(''.join(ch for ch in text[i:end] if ch == '\n'))
            i = end + 2
        elif text.startswith('//', i):
            end = text.find('\n', i)
            if end < 0:
                break
            i = end
        elif c in '"\'':
            # Copy the literal verbatim, including escapes.
            out.append(c)
            i += 1
            while i < n:
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == c:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def call_text(text, open_paren):
    """Return the source of a call whose '(' is at open_paren, or None.

    Walks to the matching ')' while skipping over string literals, so a ')'
    inside a format string does not end the call early.
    """
    depth = 0
    i = open_paren
    n = len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            quote = c
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == quote:
                    break
                i += 1
            i += 1
            continue
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[open_paren:i + 1]
        i += 1
    return None


def format_and_args(call):
    """Split a call's parenthesised source into (format literals, rest).

    The format string may be several adjacent literals; the arguments are
    whatever is outside quotes. Returning the two separately is what lets the
    check distinguish a secret *named* in a message from one *passed* to it.
    """
    literals = []
    rest = []
    i = 0
    n = len(call)
    while i < n:
        if call[i] == '"':
            i += 1
            buf = []
            while i < n:
                if call[i] == '\\' and i + 1 < n:
                    buf.append(call[i:i + 2])
                    i += 2
                    continue
                if call[i] == '"':
                    i += 1
                    break
                buf.append(call[i])
                i += 1
            literals.append(''.join(buf))
            continue
        rest.append(call[i])
        i += 1
    return ''.join(literals), ''.join(rest)


def guards_by_line(text):
    """Map line number -> set of macros that must be DEFINED to reach it.

    Only positive #ifdef/#if defined() guards are collected, and only on the
    branch where they hold. The #else of an #ifdef CONFIG_X is the path taken
    when X is *off*, so it must not inherit the exemption -- that is where the
    withheld-body message lives, and a credential dumped there would be
    dumped in the default build.
    """
    out = {}
    stack = []
    for number, line in enumerate(text.split('\n'), start=1):
        bare = line.strip()
        if bare.startswith('#'):
            directive = bare[1:].strip()
            word = directive.split(None, 1)[0] if directive else ''
            if word in ('ifdef', 'if', 'ifndef'):
                names = set()
                if word == 'ifdef':
                    rest = directive.split(None, 1)
                    if len(rest) > 1:
                        names = {rest[1].strip().split()[0]}
                elif word == 'if':
                    # Only the simple all-positive forms are treated as
                    # guarding: `defined(A)`, `defined(A) || defined(B)`.
                    # Anything with a negation is not worth guessing at.
                    if '!' not in directive:
                        names = set(re.findall(r'defined\s*\(\s*(\w+)\s*\)',
                                               directive))
                stack.append(names)
                out[number] = union(stack)
                continue
            if word in ('else', 'elif'):
                if stack:
                    stack[-1] = set()
                out[number] = union(stack)
                continue
            if word == 'endif':
                if stack:
                    stack.pop()
                out[number] = union(stack)
                continue
        out[number] = union(stack)
    return out


def union(stack):
    result = set()
    for names in stack:
        result |= names
    return result


def kconfig_defaults_pinned(path=KCONFIG):
    """Return complaints where a GATED_SWITCHES default has drifted.

    The exemption granted to a gated dump is only as good as the gate. If the
    switch is gone, the exemption is hiding an ungated dump; if its default
    moved, the exposure changed without anybody saying so.
    """
    if not os.path.isfile(path):
        return ['%s not found, so the gated switches could not be verified'
                % path]
    with open(path, encoding='utf-8', errors='replace') as handle:
        lines = handle.read().split('\n')
    bad = []
    for switch, want in sorted(GATED_SWITCHES.items()):
        name = switch[len('CONFIG_'):]
        start = None
        for number, line in enumerate(lines):
            if line.strip() == 'config ' + name:
                start = number
                break
        if start is None:
            bad.append('%s: no `config %s` in %s -- a credential dump is '
                       'exempted by a switch that does not exist'
                       % (switch, name, path))
            continue
        # Read to the next `config` entry and find this one's default.
        default = None
        for line in lines[start + 1:]:
            if line.strip().startswith('config '):
                break
            found = re.match(r'\s*default\s+(\w+)', line)
            if found:
                default = found.group(1)
                break
        if default != want:
            bad.append(
                '%s: Kconfig says `default %s`, this check expects `default '
                '%s`. Changing when a credential gets printed is a security '
                'decision -- if the new value is right, say so by updating '
                'GATED_SWITCHES and its comment.'
                % (switch, default, want))
    return bad


def secret_name(ident):
    """True if ident names a credential."""
    low = ident.lower()
    if low in ALLOWED:
        return False
    if low in SECRET_EXACT:
        return True
    return any(part in low for part in SECRET_SUBSTRINGS)


def measure_spans(args):
    """Byte ranges of args that sit inside a strlen()/sizeof() argument list."""
    spans = []
    for found in MEASURE_CALL.finditer(args):
        depth = 0
        for i in range(found.end() - 1, len(args)):
            if args[i] == '(':
                depth += 1
            elif args[i] == ')':
                depth -= 1
                if depth == 0:
                    spans.append((found.end(), i))
                    break
        else:
            # Unbalanced -- the call runs off the end of what we were given.
            spans.append((found.end(), len(args)))
    return spans


def measured(args, name):
    """True if every mention of name in args is inside strlen()/sizeof().

    `strlen(pass)` is the accepted fix and must pass; a call that measures the
    secret *and* also prints it must still fail, which is why this asks about
    every occurrence rather than about the first one it finds.

    This works by paren-matching the measuring call and asking whether the
    occurrence falls inside its argument list, rather than by looking back from
    the occurrence for a nearby `strlen(`. The earlier lookback version got
    four cases wrong, three lenient and one strict, and a direct probe is the
    only reason that is known:

      - `strlen(rx_message->payload)` was NOT measured, because the pattern
        wanted the name to follow the open paren directly. That is the real
        shape of the webprov fix. The tree still passed, because that call's
        format string has no %s and so never reached here -- the check was
        right about the live tree for a reason unrelated to the name.
      - `sizeof(cfg.password)` and `strlen(buf[i].password)` failed likewise.
      - `TRACE_D("%d %s", strlen(pass), pass)` was reported measured, because
        the lookback window reached back over the *first* occurrence's
        `strlen(` and matched that. A real leak, accepted.

    The window was 60 characters, which is what made the last one possible;
    widening or narrowing it trades one of those failures for the other, which
    is the tell that offset-from-the-name was the wrong question to ask.
    """
    spans = measure_spans(args)
    for found in re.finditer(r'\b' + re.escape(name) + r'\b', args):
        at = found.start()
        if not any(lo <= at < hi for lo, hi in spans):
            return False
    return True


def check(text):
    """Return [(line, name, snippet)] for every leaking call in text."""
    text = strip_comments(text)
    guards = guards_by_line(text)
    bad = []
    for found in CALL.finditer(text):
        call = call_text(text, found.end() - 1)
        if call is None:
            continue
        fmt, args = format_and_args(call)
        if not PCT_S.search(fmt):
            continue
        line = text.count('\n', 0, found.start()) + 1
        if guards.get(line, set()) & set(GATED_SWITCHES):
            # Dumping on purpose, behind an opt-in switch. Whether that switch
            # is really opt-in is checked separately against Kconfig.
            continue
        for found_ident in IDENT.finditer(args):
            name = found_ident.group(0)
            if not secret_name(name):
                continue
            if measured(args, name):
                continue
            snippet = ' '.join((found.group(1) + call).split())[:100]
            bad.append((line, name, snippet))
            break
    return bad


def self_test():
    """Refuse to run unless the check demonstrably catches and clears."""
    for name, sample in sorted(SELF_TEST.items()):
        if not check(sample):
            print('SELF-TEST FAILED: a known credential leak was accepted.')
            print('  sample %r: %s' % (name, ' '.join(sample.split())))
            print('The check is not measuring anything.')
            return False
    for name, sample in sorted(SELF_TEST_OK.items()):
        hits = check(sample)
        if hits:
            print('SELF-TEST FAILED: an accepted form was reported as a leak.')
            print('  sample %r: %s' % (name, hits))
            if name.startswith('fixed_with'):
                print('  This is the comment-stripping case. A fix that quotes'
                      ' the line it replaced must not read as unfixed.')
            return False
    return True


def sources():
    for root in ROOTS:
        if not os.path.isdir(root):
            continue
        for base, _, names in os.walk(root):
            for name in names:
                if name.endswith('.c'):
                    yield os.path.join(base, name)


def main():
    if not self_test():
        return 1

    if not os.path.isdir('ezlopi-core'):
        print('ezlopi-core not found. Run this from the repository root.')
        return 1

    gate_problems = kconfig_defaults_pinned()
    if gate_problems:
        print('A switch that exempts a credential dump has drifted:\n')
        for item in gate_problems:
            print('  ' + item)
        print('\nSee GATED_SWITCHES in the docstring at the top of this file.')
        return 1

    problems = []
    scanned = 0
    for path in sorted(sources()):
        scanned += 1
        with open(path, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
        for line, name, snippet in check(text):
            problems.append((path, line, name, snippet))

    if problems:
        print('A credential is passed by value to a logging call:\n')
        for path, line, name, snippet in problems:
            print('  %s:%d' % (path.replace(os.sep, '/'), line))
            print('      %s  <- %r' % (snippet, name))
        print('\nLog a length instead of the value, or put the dump behind an'
              ' off-by-default Kconfig option. See the docstring at the top of'
              ' this file.')
        return 1

    print('no credentials passed by value to a logging call (%d .c files); '
          'gated dumps at pinned defaults: %s'
          % (scanned, ', '.join('%s=%s' % (s[len('CONFIG_'):], d)
                                for s, d in sorted(GATED_SWITCHES.items()))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
