#!/usr/bin/env python3
"""Fail if a factory-info field is copied using the field's length as the count.

THE BUG THIS EXISTS TO PREVENT

EZPI_core_factory_info_v3_set_basic() takes a caller-allocated
s_basic_factory_info_t full of char pointers and writes each one into a
fixed-size region of the factory-info partition. It used to do that with a
memset/memcpy pair per field, where the count was the size of the *destination
field*:

    memset(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_DEVICE_UUID, 0, EZLOPI_FINFO_LEN_DEVICE_UUID);
    memcpy(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_DEVICE_UUID, cfg->device_uuid, EZLOPI_FINFO_LEN_DEVICE_UUID);

Nothing about that is safe on its own: the count has no relationship to the
size of the buffer the caller actually passed. Two of the three callers were
fine only because they happen to declare their stack arrays at exactly those
lengths. The third, __provision_update() in ezlopi_service_webprov.c, declared
tmp_dev_uuid[40], tmp_prov_uuid[40], tmp_cloud_server[128] and
tmp_provision_token[300] against fields of 0x7E, 0x7E, 0x100 and 0x200 -- so
those four copies read 86, 86, 128 and 212 bytes past the end of the arrays,
512 bytes of adjacent stack in total, and wrote all of it into the
factory-info partition, where it can be read back out over BLE device-info and
the UART status query without authenticating.

It is now one helper, __EZPI_core_factory_info_v3_store_str(), which bounds
the copy by strnlen() of the source. That is byte-for-byte identical for
callers that were already correct -- the field is zeroed first and their
sources are zero-filled past the string -- and it removes the over-read for
the one that was not.

WHY A CHECK

The fix lives in one place, which is also what makes it easy to undo. Adding a
fourteenth field, or "optimising" the helper back into an inline memcpy,
reintroduces the whole thing silently: it compiles without a warning, the
four images build clean, and the extra bytes only show up in flash contents
nobody diffs. No compiler flag catches it, because from the compiler's point
of view set_basic() received a pointer and was told how many bytes to read.

So the rule this enforces is narrow and mechanical: inside
ezlopi_core_factory_info.c, no memcpy may take its source from an
s_basic_factory_info_t member and its count from an EZLOPI_FINFO_LEN_*
constant. Route it through the helper instead.

LIMITS

Scalar members are exempt: "&cfg->id" with EZLOPI_FINFO_LEN_DEVICE_ID is a
fixed-size binary field whose source really is that long, and the address-of
makes it distinguishable. Comments are stripped before scanning, because this
file documents the old pattern in prose directly above the helper and an
earlier draft of this check matched the documentation rather than code. The
scan is textual and single-line; a memcpy split across lines would slip past.
That is accepted -- this guards against the pattern coming back the way it
left, not against someone determined to hide it.

Run from the repository root.
"""
import re
import sys

TARGET = 'ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c'
STRUCT_VAR = 'ezlopi_config_basic'
LEN_PREFIX = 'EZLOPI_FINFO_LEN_'

# memcpy whose source is a struct member (not its address) and whose count
# mentions a field-length constant.
OFFENDER = re.compile(
    r'\bmemcpy\s*\('
    r'[^;]*?,'                                    # destination
    r'\s*(?!&)\s*' + re.escape(STRUCT_VAR) + r'\s*->\s*([A-Za-z_][A-Za-z0-9_]*)'
    r'\s*,'                                       # source, by value
    r'\s*(' + re.escape(LEN_PREFIX) + r'[A-Za-z0-9_]*)'  # count
)


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


def scan(text):
    """Return [(line_no, member, length_constant)] for each offending memcpy."""
    found = []
    for n, line in enumerate(strip_comments(text).split('\n'), start=1):
        for match in OFFENDER.finditer(line):
            found.append((n, match.group(1), match.group(2)))
    return found


SELF_TEST = {
    # the pattern as it actually was, which must be caught
    'historical': (
        'memcpy(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_DEVICE_UUID, '
        'ezlopi_config_basic->device_uuid, EZLOPI_FINFO_LEN_DEVICE_UUID);',
        1),
    # the fix, which must not be
    'fixed': (
        '__EZPI_core_factory_info_v3_store_str(tmp_buffer + '
        'EZLOPI_FINFO_REL_OFFSET_DEVICE_UUID, ezlopi_config_basic->device_uuid, '
        'EZLOPI_FINFO_LEN_DEVICE_UUID);',
        0),
    # the helper's own bounded copy
    'helper': ('memcpy(field, src, strnlen(src, field_len));', 0),
    # scalar binary field, exempt via address-of
    'scalar_id': (
        'memcpy(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_DEVICE_ID, '
        '&ezlopi_config_basic->id, EZLOPI_FINFO_LEN_DEVICE_ID);',
        0),
    # a local array of exactly the right size is not a struct member
    'local_array': (
        'memcpy(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_CONFIG_VERSION, '
        'config_version_arry, EZLOPI_FINFO_LEN_CONFIG_VERSION);',
        0),
    # the prose above the helper describes the old pattern; must not match
    'commented_decoy': (
        '/* memcpy(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_DEVICE_UUID,\n'
        '** ezlopi_config_basic->device_uuid, EZLOPI_FINFO_LEN_DEVICE_UUID); */\n'
        'memcpy(field, src, strnlen(src, field_len));',
        0),
    # a regression on a different field
    'new_field': (
        'memcpy(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_LOCAL_KEY, '
        'ezlopi_config_basic->local_key, EZLOPI_FINFO_LEN_LOCAL_KEY);',
        1),
}


def self_test():
    ok = True
    for name, (sample, expected) in sorted(SELF_TEST.items()):
        got = len(scan(sample))
        if got != expected:
            print('  self-test %-16s FAILED: expected %d hit(s), got %d'
                  % (name, expected, got))
            ok = False
        else:
            print('  self-test %-16s ok' % name)
    return ok


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

    offenders = scan(text)
    print('\nscanned %s' % TARGET)
    if not offenders:
        print('no field-length memcpy from a %s member; copies are bounded '
              'by the source.' % STRUCT_VAR)
        return 0

    print('')
    for line_no, member, length in offenders:
        print('::error file=%s,line=%d::memcpy reads %s bytes from '
              '%s->%s, but the count is the size of the destination field and '
              'nothing guarantees the caller passed a source that long. '
              '__provision_update() in ezlopi_service_webprov.c passes '
              'buffers smaller than the fields they are assigned to, which is '
              'how this read 512 bytes of stack into flash. Use '
              '__EZPI_core_factory_info_v3_store_str() so the copy is bounded '
              'by the source string.'
              % (TARGET, line_no, length, STRUCT_VAR, member))
    print('\n%d unbounded field cop%s.'
          % (len(offenders), 'y' if len(offenders) == 1 else 'ies'))
    return 1


if __name__ == '__main__':
    sys.exit(main())
