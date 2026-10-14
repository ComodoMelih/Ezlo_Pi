#!/usr/bin/env python3
"""Fail if a 4 KB factory-info field can be written with an unbounded length.

THE BUG THIS EXISTS TO PREVENT

__EZPI_core_factory_info_v3_set_4kb() erases exactly EZLOPI_FINFO_READ_LEN_4KB
bytes and then writes len + 1, and it used to do that without bounding len.
The four PEM setters -- private key, public key, shared key, CA certificate --
each took len from ezlopi_calculate_certificate_length(), which returns the
offset of a PEM end marker inside a client-supplied string and has no upper
bound:

    cj_data->str_value_len = ezlopi_calculate_certificate_length(cj_data->valuestring, "-----END CERTIFICATE-----");
    ret = __EZPI_core_factory_info_v3_set_4kb(cj_data->valuestring, ..., cj_data->str_value_len);

The fields are contiguous inside one 32 KB partition:

    EZLOPI_CONFIG_JSON  0x1000
    CA_CERTIFICATE      0x3000
    SSL_PRIVATE_KEY     0x4000
    SSL_SHARED_KEY      0x5000
    (hub data)          0x6000   serial number, Wi-Fi SSID and password, MAC

So an over-long CA certificate wrote into the private key, and an over-long
shared key wrote into the hub data. Worse than a plain overwrite: the spilled
bytes land in flash this call did not erase, and NOR flash can only clear bits
on write, so the neighbouring field is corrupted rather than replaced. The
write stays inside the partition, so esp_partition_write() does not refuse it.

Both write paths are reachable without authenticating -- BLE provisioning,
which is unauthenticated in every shipped sdkconfig.defaults, and the UART
command channel, which checks nothing.

A missing end marker was the other half. The helper returned 0 for "not
found", and 0 reached the writer, which erased the field and stored an empty
string -- so a malformed certificate destroyed the stored one.

EZPI_util_pem_store_len() now answers both questions, returns 0 to mean "do
not write", and is host-tested at 4094 / 4095 / 4096 bytes.

WHY A CHECK

The length arithmetic is tested on the host, but the two things that make it
load-bearing are not reachable from a host test: that the setters actually
call it, and that the backstop inside set_4kb still exists. This file needs
esp_partition and so cannot be host-compiled at all.

Both are easy to undo by accident and silent when undone. Deleting the
backstop leaves a function whose name promises a 4 KB write and whose body
writes whatever it is handed; it compiles without a warning and all four
images build clean. Passing cj_data->str_value_len straight through again --
the obvious edit for anyone who finds the length "wrong" -- restores the
original bug exactly. Nothing in the compiler's view is amiss either way,
because from its point of view the function received a pointer and a count.

LIMITS

The scan is textual and single-line. A call split across lines, or a bound
written in a form this does not recognise, slips past. Comments are stripped
first, because this file documents the old pattern in prose -- including the
name of the deleted helper -- and a check that matched its own documentation
would fire on the fixed file. The backstop rule only proves a comparison
against EZLOPI_FINFO_READ_LEN_4KB appears ahead of the allocation and write,
not that the comparison is the right way round; the host tests cover the
arithmetic, and this covers its presence.

This guards against the pattern coming back the way it left, not against
someone determined to hide it.

Run from the repository root.
"""
import re
import sys

TARGET = 'ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c'

WRITER = '__EZPI_core_factory_info_v3_set_4kb'
FIELD_LEN = 'EZLOPI_FINFO_READ_LEN_4KB'
BOUNDED_HELPER = 'EZPI_util_pem_store_len'
DELETED_HELPER = 'ezlopi_calculate_certificate_length'

# A call to the 4 KB writer whose length argument is a cJSON string length
# taken straight from the request, which is the shape the bug had.
RAW_LEN_CALL = re.compile(
    re.escape(WRITER) + r'\s*\('
    r'[^;]*?,'                                        # data, offset
    r'\s*[A-Za-z_][A-Za-z0-9_]*\s*->\s*str_value_len\s*\)'
)

# The deleted helper, reintroduced either as a definition or a call.
DELETED_HELPER_USE = re.compile(r'\b' + re.escape(DELETED_HELPER) + r'\b')

# A comparison of some length against the field size. Either operand order.
BOUND_PRESENT = re.compile(
    r'(>\s*' + re.escape(FIELD_LEN) + r')'
    r'|(' + re.escape(FIELD_LEN) + r'\s*<)'
)

# Where the writer stops being safe: it allocates, then writes.
WRITER_ACTS = re.compile(r'\bezlopi_malloc\b|\besp_partition_write\b')


def strip_comments(text):
    """Remove /* */ and // comments, preserving line structure."""
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


def writer_body(code):
    """Return the text of the writer's definition, or None if not found.

    The definition is the occurrence followed by a '{' rather than a ';', so a
    forward declaration is skipped. The body ends at the first '}' in column
    zero, which is this file's brace style throughout.
    """
    for match in re.finditer(r'\b' + re.escape(WRITER) + r'\s*\([^;{]*\)\s*\{',
                             code):
        start = match.end()
        end = code.find('\n}', start)
        end = len(code) if end < 0 else end
        return code[match.start():end]
    return None


def scan(text):
    """Return a list of (line_no_or_None, kind, detail) findings."""
    code = strip_comments(text)
    found = []

    for n, line in enumerate(code.split('\n'), start=1):
        if RAW_LEN_CALL.search(line):
            found.append((n, 'raw_len',
                          'length comes straight from ->str_value_len'))
        for match in DELETED_HELPER_USE.finditer(line):
            del match
            found.append((n, 'deleted_helper',
                          '%s is back' % DELETED_HELPER))

    body = writer_body(code)
    if body is None:
        found.append((None, 'no_writer',
                      '%s is not defined in this file; this check no longer '
                      'knows what it is guarding' % WRITER))
    else:
        acts = WRITER_ACTS.search(body)
        guard_region = body[:acts.start()] if acts else body
        if not BOUND_PRESENT.search(guard_region):
            found.append((None, 'no_bound',
                          'the backstop comparing the length against %s is '
                          'gone from %s, or now sits after the allocation and '
                          'write it is supposed to prevent'
                          % (FIELD_LEN, WRITER)))

    return found


# Each sample is a whole miniature file, because two of the three rules are
# about the writer's body and need one to look at.
GOOD_WRITER = (
    'static int %s(const char *data, uint32_t offset, uint32_t len)\n'
    '{\n'
    '    if ((len + 1u) > %s)\n'
    '    {\n'
    '        return 0;\n'
    '    }\n'
    '    char *tmp = ezlopi_malloc(__FUNCTION__, len + 1);\n'
    '    esp_partition_write(p, offset, tmp, len + 1);\n'
    '}\n' % (WRITER, FIELD_LEN)
)

UNBOUNDED_WRITER = (
    'static int %s(const char *data, uint32_t offset, uint32_t len)\n'
    '{\n'
    '    char *tmp = ezlopi_malloc(__FUNCTION__, len + 1);\n'
    '    esp_partition_write(p, offset, tmp, len + 1);\n'
    '}\n' % WRITER
)

# The guard is present but below the write, so it prevents nothing.
LATE_BOUND_WRITER = (
    'static int %s(const char *data, uint32_t offset, uint32_t len)\n'
    '{\n'
    '    char *tmp = ezlopi_malloc(__FUNCTION__, len + 1);\n'
    '    esp_partition_write(p, offset, tmp, len + 1);\n'
    '    if ((len + 1u) > %s)\n'
    '    {\n'
    '        return 0;\n'
    '    }\n'
    '}\n' % (WRITER, FIELD_LEN)
)

FIXED_SETTER = (
    '    const uint32_t store_len = %s(cj_data->valuestring, '
    '"-----END CERTIFICATE-----", EZLOPI_FINFO_LEN_CA_CERTIFICATE);\n'
    '    if (store_len)\n'
    '    {\n'
    '        ret = %s(cj_data->valuestring, addr, store_len);\n'
    '    }\n' % (BOUNDED_HELPER, WRITER)
)

HISTORICAL_SETTER = (
    '    cj_data->str_value_len = %s(cj_data->valuestring, '
    '"-----END CERTIFICATE-----");\n'
    '    ret = %s(cj_data->valuestring, addr, cj_data->str_value_len);\n'
    % (DELETED_HELPER, WRITER)
)

SELF_TEST = {
    # the fixed file: bounded writer, setter routed through the helper
    'fixed': (GOOD_WRITER + FIXED_SETTER, 0),
    # the bug as it actually was: two findings, the raw length and the helper
    'historical': (GOOD_WRITER + HISTORICAL_SETTER, 2),
    # backstop deleted
    'backstop_removed': (UNBOUNDED_WRITER + FIXED_SETTER, 1),
    # backstop present but after the write
    'backstop_too_late': (LATE_BOUND_WRITER + FIXED_SETTER, 1),
    # both undone at once
    'fully_regressed': (UNBOUNDED_WRITER + HISTORICAL_SETTER, 3),
    # the prose in the real file names the deleted helper and describes the
    # old call; neither may count
    'commented_decoy': (
        GOOD_WRITER
        + '/* %s() used to live here. It returned an unbounded offset, and\n'
          '** ret = %s(cj_data->valuestring, addr, cj_data->str_value_len);\n'
          '** wrote past the field. */\n' % (DELETED_HELPER, WRITER)
        + FIXED_SETTER,
        0),
    # a // comment form of the same decoy
    'line_comment_decoy': (
        GOOD_WRITER + FIXED_SETTER
        + '    // old: %s(data, addr, cj_data->str_value_len);\n' % WRITER,
        0),
    # the writer renamed or removed: the check must say so rather than pass
    'writer_gone': (FIXED_SETTER.replace(WRITER, 'something_else'), 1),
    # the bound reversed in operand order is still a bound
    'bound_other_order': (
        UNBOUNDED_WRITER.replace(
            '    char *tmp',
            '    if (%s < (len + 1u)) { return 0; }\n    char *tmp' % FIELD_LEN)
        + FIXED_SETTER,
        0),
    # a forward declaration must not be mistaken for the definition
    'forward_decl': (
        'static int %s(const char *data, uint32_t offset, uint32_t len);\n'
        % WRITER + GOOD_WRITER + FIXED_SETTER,
        0),
    # a different setter regressing on a different field
    'other_field': (GOOD_WRITER +
                    '    ret = %s(cj_data->valuestring, addr, '
                    'cj_data->str_value_len);\n' % WRITER, 1),
}


def self_test():
    ok = True
    for name, (sample, expected) in sorted(SELF_TEST.items()):
        got = len(scan(sample))
        if got != expected:
            print('  self-test %-20s FAILED: expected %d finding(s), got %d'
                  % (name, expected, got))
            for line_no, kind, detail in scan(sample):
                print('      %s %s: %s' % (line_no, kind, detail))
            ok = False
        else:
            print('  self-test %-20s ok' % name)
    return ok


EXPLAIN = {
    'raw_len':
        'the length handed to %s comes straight from the parsed request. '
        'That function erases %s bytes and writes len + 1, so a longer value '
        'runs into the next field -- an over-long shared key reaches the hub '
        'data, which holds the serial number and the Wi-Fi credentials. Route '
        'the length through %s(), which refuses rather than truncates and '
        'returns 0 when the PEM end marker is missing.'
        % (WRITER, FIELD_LEN, BOUNDED_HELPER),
    'deleted_helper':
        '%s() was removed because it returned an unbounded marker offset and '
        '0 for "marker absent", and both answers were written to flash. Use '
        '%s() instead.' % (DELETED_HELPER, BOUNDED_HELPER),
    'no_bound':
        'every caller is bounded before it reaches %s, but the backstop there '
        'is what makes the next caller safe too. Restore the comparison '
        'against %s ahead of the allocation and the write.'
        % (WRITER, FIELD_LEN),
    'no_writer':
        'if the writer was renamed, update WRITER in this file so the guard '
        'keeps applying; if the write moved elsewhere, this check needs '
        'rewriting rather than deleting.',
}


def main():
    print('self-test:')
    if not self_test():
        print('\nThe scanner is wrong, so its verdict on the real file means '
              'nothing. Fix the scanner.')
        return 2

    try:
        with open(TARGET, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
    except OSError as exc:
        print('::error::cannot read %s: %s' % (TARGET, exc))
        return 1

    findings = scan(text)
    print('\nscanned %s' % TARGET)
    if not findings:
        print('4 KB field writes are bounded: %s is guarded against %s and no '
              'caller passes a raw request length.' % (WRITER, FIELD_LEN))
        return 0

    print('')
    for line_no, kind, detail in findings:
        where = ('file=%s,line=%d' % (TARGET, line_no) if line_no
                 else 'file=%s' % TARGET)
        print('::error %s::%s. %s' % (where, detail, EXPLAIN[kind]))
    print('\n%d unbounded-write finding%s.'
          % (len(findings), '' if len(findings) == 1 else 's'))
    return 1


if __name__ == '__main__':
    sys.exit(main())
