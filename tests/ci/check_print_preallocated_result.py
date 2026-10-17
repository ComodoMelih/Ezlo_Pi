#!/usr/bin/env python3
"""Fail if a cJSON_PrintPreallocated() result is discarded, or the shared
transmit buffer is printed into without being cleared first.

THE BUG THIS EXISTS TO PREVENT

__otel_publish() in ezlopi_service_otel.c did both:

    char *buffer = EZPI_core_buffer_acquire(__FUNCTION__, &buffer_len, 2000);
    if (buffer)
    {
        uint32_t rertries = 3;
        cJSON_PrintPreallocated(__FUNCTION__, cj_telemetry, buffer, buffer_len, false);

        while (rertries--)
        {
            if (EZPI_SUCCESS == EZPI_core_websocket_client_send(
                    __wss_client, buffer, strlen(buffer), 1000))

cJSON_PrintPreallocated() returns false when the object does not fit
(cjext.c:1537 -> print_value -> ensure, which refuses to grow a noalloc
buffer), and it returns false *having already written a truncated prefix*. It
writes no terminator at the truncation point. EZPI_core_buffer_acquire()
returns the shared buffer exactly as the previous holder left it
(ezlopi_core_buffer.c:158 takes the semaphore and returns the pointer, with no
clearing). So strlen() above did not stop at the end of the truncated JSON, nor
at the end of the previous holder's bytes -- it ran off the end of the buffer
entirely, and that length was handed to the websocket send.

The 2000 in that call is wait_to_acquired_ms and not a size, which is easy to
misread as one: the length arrives through buffer_len and is
CONFIG_EZPI_CORE_STATIC_BUFFER_SIZE, 10240 in all four committed
sdkconfig.defaults.* files. So the object has to exceed 10 KB to trigger this,
which is a high bar reached by content rather than by accident -- the trace
attributes include "method", "id" and "error" copied verbatim out of the
incoming message by ezlopi_service_otel_fetch_string_value_from_cjson().

Measured on the host with the destination pre-filled to stand in for a previous
holder, printing a 61-byte object into 40 bytes: returned false, 35 of 40 bytes
changed, and no NUL anywhere in the destination. The other live holders of this
buffer are the cloud response path (ezlopi_service_webprov.c), the local
websocket server's response path (ezlopi_service_ws_server.c) and the broadcast
path (ezlopi_core_broadcast.c), so the bytes past the truncation are response
payloads.

WHY TWO RULES, AND WHY THEY ARE NOT INTERCHANGEABLE

The two halves fail differently, which is the reason both are checked:

  * Result discarded, buffer cleared. The truncated prefix sits in zeroed
    memory, so strlen() stops at the first zero after it and a malformed JSON
    fragment is transmitted. Wrong, but bounded.
  * Result checked, buffer not cleared. The buffer is never read on failure, so
    nothing escapes. Safe today, and one edit away from not being.

Only the combination reads out of bounds. Rule A is therefore the one that
matters for memory safety and rule B is defence in depth, and a reviewer
tempted to drop one should drop neither: rule B is what makes rule A's failure
mode bounded instead of unbounded.

SCOPE

Rule A covers every cJSON_PrintPreallocated() call in the first-party trees.
There are nine, and all nine consume the result: ws_server.c, webprov.c,
otel.c, factory_info.c, broadcast.c x2, ble_provisioning.c,
ble_dynamic_config.c, coordinates.c. Four of those print into something other
than the shared buffer -- a local array, or the BLE response buffer -- which is
exactly why rule A is not restricted to shared-buffer users: coordinates.c
prints into an uninitialised char[256] and is correct only because it branches
on the return.

Rule B covers the five EZPI_core_buffer_acquire() holders -- four live, plus
the one behind #if 0. cjext itself and tests/ are out of scope: cjext defines
the function, and the host tests call it deliberately in both directions,
including one assertion that it must refuse a buffer one byte too small.

LIMITS

The scan is textual over comment-stripped source. Comments are stripped because
the fix in otel.c documents the old shape in prose directly above the new code,
and a check that matched its own explanation would fire on the fixed file --
asserted in the self-test as `comment_decoy`.

A call whose assignment is split so that the function name starts a line is
resolved by looking at the previous non-blank line for a trailing operator;
anything else that reaches the function name at statement position is reported.
An explicit `(void)` cast is reported rather than honoured: the point is to
branch on the result, not to announce that you are ignoring it.

The check does not understand `#if 0`. The disabled copy of
EZPI_core_broadcast_log_cjson() is scanned along with the rest; it conforms, so
including it is free, and if it is ever re-enabled it is already right. (It does
still call the pre-`who` two-argument form of EZPI_core_buffer_acquire(), which
is separate staleness and not this check's business.)

Run from the repository root.
"""
import os
import re
import sys

DIRS = [
    'ezlopi-core',
    'ezlopi-hal',
    'ezlopi-services',
    'ezlopi-cloud',
    'ezlopi-util',
    'ezlopi-user-config',
    'ezlopi-main',
]

PRINTER = 'cJSON_PrintPreallocated'
ACQUIRE = 'EZPI_core_buffer_acquire'
RELEASE = 'EZPI_core_buffer_release'

# How far below an acquire() to look for the clearing memset, when no
# release() turns up first.
WINDOW = 80

# A prefix that ends in one of these means the value is being consumed.
CONSUMED = re.compile(r'(?:[=!<>+\-*/%&|^,?:(\[]|\breturn\b|\bwhile\b|\bif\b)$')

ASSIGNED = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*' + re.escape(ACQUIRE))
CLEARED = re.compile(r'\b(?:memset|bzero)\s*\(\s*\(?[A-Za-z_* ]*\)?\s*%s\b')


def strip_comments(text):
    """Remove /* */ and // comments, preserving line and column structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            end = text.find('*/', i + 2)
            end = n if end < 0 else end + 2
            out.append(''.join(c if c == '\n' else ' ' for c in text[i:end]))
            i = end
        elif text.startswith('//', i):
            end = text.find('\n', i)
            end = n if end < 0 else end
            out.append(' ' * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def previous_code_line(lines, index):
    for j in range(index - 1, -1, -1):
        if lines[j].strip():
            return lines[j].strip()
    return ''


def scan_discarded(text):
    """Rule A: [(line_no, prefix)] for each call whose result is dropped."""
    found = []
    lines = strip_comments(text).split('\n')
    for i, line in enumerate(lines):
        for match in re.finditer(re.escape(PRINTER) + r'\s*\(', line):
            prefix = line[:match.start()].rstrip()
            # An explicit discard is still a discard.
            if prefix.endswith('(void)'):
                found.append((i + 1, prefix.strip()))
                continue
            if prefix:
                if not CONSUMED.search(prefix):
                    found.append((i + 1, prefix.strip()))
                continue
            # Name starts the line: the assignment may be on the line above.
            if not CONSUMED.search(previous_code_line(lines, i)):
                found.append((i + 1, '<statement position>'))
    return found


def scan_uncleared(text):
    """Rule B: [(line_no, variable)] for each acquire() with no memset."""
    found = []
    lines = strip_comments(text).split('\n')
    for i, line in enumerate(lines):
        match = ASSIGNED.search(line)
        if not match:
            continue
        var = match.group(1)
        end = min(len(lines), i + 1 + WINDOW)
        for j in range(i + 1, end):
            if RELEASE in lines[j]:
                end = j
                break
        body = '\n'.join(lines[i:end])
        if not re.search(CLEARED.pattern % re.escape(var), body):
            found.append((i + 1, var))
    return found


def sources():
    for directory in DIRS:
        for root, _dirs, files in os.walk(directory):
            for name in sorted(files):
                if name.endswith('.c'):
                    yield os.path.join(root, name).replace(os.sep, '/')


SELF_TEST = {
    # The shape as it actually was, which must be caught by both rules.
    'historical': (
        'char *buffer = EZPI_core_buffer_acquire(__FUNCTION__, &len, 2000);\n'
        'if (buffer)\n'
        '{\n'
        '    cJSON_PrintPreallocated(__FUNCTION__, cj, buffer, len, false);\n'
        '    send(buffer, strlen(buffer));\n'
        '    EZPI_core_buffer_release(__FUNCTION__);\n'
        '}\n',
        1, 1),
    # The fix.
    'fixed': (
        'char *buffer = EZPI_core_buffer_acquire(__FUNCTION__, &len, 2000);\n'
        'if (buffer && len)\n'
        '{\n'
        '    memset(buffer, 0, len);\n'
        '    if (true == cJSON_PrintPreallocated(__FUNCTION__, cj, buffer, len, false))\n'
        '    {\n'
        '        send(buffer, strlen(buffer));\n'
        '    }\n'
        '    EZPI_core_buffer_release(__FUNCTION__);\n'
        '}\n',
        0, 0),
    # Each half alone: the point of having two rules is that each is reported
    # on its own, so a half-fix cannot pass.
    'memset_only': (
        'char *buffer = EZPI_core_buffer_acquire(__FUNCTION__, &len, 2000);\n'
        'memset(buffer, 0, len);\n'
        'cJSON_PrintPreallocated(__FUNCTION__, cj, buffer, len, false);\n'
        'EZPI_core_buffer_release(__FUNCTION__);\n',
        1, 0),
    'check_only': (
        'char *buffer = EZPI_core_buffer_acquire(__FUNCTION__, &len, 2000);\n'
        'if (true == cJSON_PrintPreallocated(__FUNCTION__, cj, buffer, len, false))\n'
        '{\n'
        '    send(buffer, strlen(buffer));\n'
        '}\n'
        'EZPI_core_buffer_release(__FUNCTION__);\n',
        0, 1),
    # Every way the nine real sites consume the result.
    'assigned': (
        'status = cJSON_PrintPreallocated(__FUNCTION__, cj, buf, 512, false);\n',
        0, 0),
    'declared': (
        'bool ret = cJSON_PrintPreallocated(__FUNCTION__, cj, buf, 512, false);\n',
        0, 0),
    'returned': (
        'return cJSON_PrintPreallocated(__FUNCTION__, cj, buf, 512, false);\n',
        0, 0),
    'negated': (
        'if (!cJSON_PrintPreallocated(__FUNCTION__, cj, buf, 512, false))\n',
        0, 0),
    # An assignment broken across lines must not be reported.
    'split_assignment': (
        'bool ret =\n'
        '    cJSON_PrintPreallocated(__FUNCTION__, cj, buf, 512, false);\n',
        0, 0),
    # An explicit discard is a discard.
    'void_cast': (
        '(void)cJSON_PrintPreallocated(__FUNCTION__, cj, buf, 512, false);\n',
        1, 0),
    # The fixed otel.c explains the old shape in a comment immediately above
    # the new code. A check that reads its own documentation reports the file
    # it just fixed.
    'comment_decoy': (
        '/* EZPI_core_buffer_acquire() returns the buffer as the previous\n'
        '** holder left it, and for an object too large\n'
        '** cJSON_PrintPreallocated() returns false having written a\n'
        '** truncated prefix. */\n'
        'char *buffer = EZPI_core_buffer_acquire(__FUNCTION__, &len, 2000);\n'
        'memset(buffer, 0, len);\n'
        'if (true == cJSON_PrintPreallocated(__FUNCTION__, cj, buffer, len, false))\n'
        '{\n'
        '    EZPI_core_buffer_release(__FUNCTION__);\n'
        '}\n',
        0, 0),
    # A commented-out acquire is not an acquire. One exists, in
    # ezlopi_service_ble_provisioning.c.
    'commented_acquire': (
        '// decoded_config_json = EZPI_core_buffer_acquire(__FUNCTION__, &len, 5000);\n',
        0, 0),
    # A cast on the memset destination still counts as clearing it.
    'cast_memset': (
        'char *buffer = EZPI_core_buffer_acquire(__FUNCTION__, &len, 2000);\n'
        'memset((void *)buffer, 0, len);\n'
        'if (true == cJSON_PrintPreallocated(__FUNCTION__, cj, buffer, len, false))\n'
        '{\n'
        '    EZPI_core_buffer_release(__FUNCTION__);\n'
        '}\n',
        0, 0),
}


def self_test():
    ok = True
    for name, (sample, want_a, want_b) in sorted(SELF_TEST.items()):
        got_a = len(scan_discarded(sample))
        got_b = len(scan_uncleared(sample))
        if (got_a, got_b) != (want_a, want_b):
            print('  self-test %-18s FAILED: wanted (discarded=%d, '
                  'uncleared=%d), got (%d, %d)'
                  % (name, want_a, want_b, got_a, got_b))
            ok = False
        else:
            print('  self-test %-18s ok (discarded=%d, uncleared=%d)'
                  % (name, got_a, got_b))
    return ok


def main():
    print('self-test:')
    if not self_test():
        print('\nThe scanner is wrong, so its verdict on the tree means '
              'nothing. Fix the scanner.')
        return 2

    missing = [d for d in DIRS if not os.path.isdir(d)]
    if missing:
        print('not found: %s\nRun this from the repository root.'
              % ', '.join(missing))
        return 1

    discarded, uncleared = [], []
    printers = acquirers = files = 0

    for path in sources():
        with open(path, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
        files += 1
        if PRINTER not in text and ACQUIRE not in text:
            continue

        stripped = strip_comments(text)
        printers += len(re.findall(re.escape(PRINTER) + r'\s*\(', stripped))
        acquirers += len(ASSIGNED.findall(stripped))

        for line_no, prefix in scan_discarded(text):
            discarded.append((path, line_no, prefix))
        for line_no, var in scan_uncleared(text):
            uncleared.append((path, line_no, var))

    print('\nfiles scanned: %d; %s() calls: %d; %s() holders: %d'
          % (files, PRINTER, printers, ACQUIRE, acquirers))

    for path, line_no, prefix in discarded:
        print('::error file=%s,line=%d::the result of %s() is discarded here '
              '(prefix: %s). It returns false for an object that does not fit, '
              'after writing a truncated prefix with no terminator, so the '
              'destination must not be read unless it returned true.'
              % (path, line_no, PRINTER, prefix))

    for path, line_no, var in uncleared:
        print('::error file=%s,line=%d::%r comes from %s(), which returns the '
              'shared buffer exactly as the previous holder left it, and '
              'nothing clears it before it is printed into. Add '
              'memset(%s, 0, <len>) as the other holders do.'
              % (path, line_no, var, ACQUIRE, var))

    if discarded or uncleared:
        print('\n%d discarded result(s), %d uncleared buffer(s). See the '
              'docstring at the top of this file for why both matter and why '
              'only the combination reads out of bounds.'
              % (len(discarded), len(uncleared)))
        return 1

    print('every %s() result is consumed, and every shared buffer is cleared '
          'before it is printed into.' % PRINTER)
    return 0


if __name__ == '__main__':
    sys.exit(main())
