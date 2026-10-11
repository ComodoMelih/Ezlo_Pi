#!/usr/bin/env python3
"""Fail if a CJSON_GET_VALUE_STRING_BY_COPY destination can be read uninitialised.

The macro expands to EZPI_core_cjson_get_value_string_by_copy, in
ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_cjson_macros.c:

    if (cj_root && item_name_str && item_val && item_val_len)
    {
        cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
        if (o_item && o_item->valuestring && o_item->str_value_len)
            snprintf(item_val, item_val_len, ...);
    }

Note what is missing: there is no else. When the key is absent, or present but
not a string, the destination buffer is left exactly as the caller left it. So
every call site must either initialise its buffer at declaration or memset it
first, otherwise a JSON payload that simply omits the key hands uninitialised
stack to whatever reads the buffer next.

That was not a hypothetical. Twelve of seventy-six call sites got this wrong,
and the payloads involved arrive over BLE, UART and the cloud socket. The worst
of them compared the stack contents against stored credentials and, on a device
that had not been claimed yet, wrote them into NVS.

This check is deliberately shallow: it only understands a local "char buf[N];"
declaration within 80 lines above the call. Anything it cannot resolve to such a
declaration is skipped rather than guessed at, so it under-reports rather than
producing noise. Run from the repository root.
"""
import re
import subprocess
import sys

MACRO = 'CJSON_GET_VALUE_STRING_BY_COPY'


def is_comment(line):
    s = line.strip()
    return s.startswith('//') or s.startswith('*') or s.startswith('/*')


def main():
    files = subprocess.run(
        ['git', 'grep', '-l', MACRO, '--', '*.c'],
        capture_output=True, text=True, check=True).stdout.split()

    total = 0
    bad = []
    for path in files:
        with open(path, encoding='utf-8', errors='replace') as handle:
            lines = handle.read().split('\n')

        for i, line in enumerate(lines):
            if MACRO not in line or is_comment(line):
                continue
            match = re.search(
                MACRO + r'\s*\([^,]+,[^,]+,\s*([A-Za-z_][A-Za-z0-9_]*)', line)
            if not match:
                continue
            total += 1
            buf = match.group(1)

            decl = None
            for j in range(i, max(-1, i - 80), -1):
                found = re.search(
                    r'\bchar\s+' + re.escape(buf) + r'\s*\[[^\]]*\]\s*(=)?', lines[j])
                if found:
                    decl = (j, found.group(1) is not None, lines[j].strip())
                    break
            if decl is None:
                continue                      # not a local char array we can reason about
            j, initialised, text = decl
            if initialised:
                continue
            between = '\n'.join(lines[j:i])
            if re.search(r'\b(memset|bzero)\s*\(\s*&?' + re.escape(buf) + r'\b', between):
                continue
            bad.append((path, i + 1, buf, j + 1, text))

    print('%s call sites examined: %d' % (MACRO, total))
    if not bad:
        print('all destinations are initialised or memset')
        return 0

    print('')
    for path, use, buf, declared, text in bad:
        print('::error file=%s,line=%d::%r is neither initialised nor memset '
              '(declared line %d: %s). %s leaves it untouched when the key is '
              'absent, so this reads uninitialised stack.'
              % (path, use, buf, declared, text, MACRO))
    print('\n%d uninitialised destination(s).' % len(bad))
    return 1


if __name__ == '__main__':
    sys.exit(main())
