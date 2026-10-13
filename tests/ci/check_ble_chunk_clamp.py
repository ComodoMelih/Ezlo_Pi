#!/usr/bin/env python3
"""Fail if a BLE chunk-reassembly memcpy is not clamped.

THE BUG THIS EXISTS TO PREVENT

The BLE service splits a large payload across GATT writes. Each write carries
an envelope:

    {"total_len": <base64 chars in all>, "sequence": <1-based>,
     "len": <base64 chars in this chunk>, "data": "<base64>"}

The reassembly functions allocate a staging buffer of total_len + 1 bytes and
walk the accumulated chunks copying each one in:

    uint32_t len = CJ_GET_NUMBER(ezlopi_len_str);
    char *data = CJ_GET_STRING(ezlopi_data_str);
    memcpy(base64_buffer + pos, data, len);

Both "len" and "data" come straight off the air. "len" is a JSON number with
no enforced relation to the actual length of "data", and nothing required the
chunks' lengths to sum to total_len. So a single write of

    {"total_len":1,"sequence":1,"len":100000,"data":"AA"}

read ~100KB past the end of a two-byte string and wrote it past the end of a
two-byte heap block. The GATT characteristic is declared ESP_GATT_PERM_WRITE,
not ESP_GATT_PERM_WRITE_ENCRYPTED, and CONFIG_EZPI_BLE_ENABLE_PASSKEY is unset
in every shipped sdkconfig.defaults.*, so no pairing or authentication stands
between a client in radio range and that memcpy. The payload's own "user_id"
is only checked much later, after the decode.

WHY A CHECK AND NOT JUST A FIX

There were two copies of this function -- ezpi_base64_decode_provisioning_info()
and ezpi_base64_decode_dynamic_config() -- line-for-line near-identical,
because one was pasted from the other. The clamp was added to the dynamic-config
copy and the provisioning copy was left alone for a release. Fixing the second
copy does not stop a third from being pasted later, and the two files are far
enough apart that nothing would notice.

So the rule is structural: in the BLE service sources, a function that reads a
chunk length out of a JSON envelope and then calls memcpy must also call
EZPI_util_clamp_copy_len. The baseline is zero violations.

The companion rule is about lifetime, from the same episode: every cJSON tree
parsed in those files must be freed through the variable it was parsed into.
Three handlers got this wrong -- the provisioning write handler, the
provisioning reassembly loop, and ezpi_service_ble_serial_config_write() in
ezlopi_service_ble_device_info.c, which this check found and a careful manual
read of the same files had missed. Any BLE client could leak the heap away one
write at a time without ever authenticating.

Pairing by variable rather than by presence is the whole point of that rule.
Stated as "the function must call cJSON_Delete somewhere", it is satisfied by
the provisioning write handler, which parsed two trees and freed one of them.
SELF_TEST['one_of_two'] below is exactly that shape and is asserted to report
exactly one leak.

LIMITS

Function bodies are found by brace depth from a column-0 "{", which is the
style these files use throughout. A construct that defeats that would be
skipped rather than guessed at. The check under-reports instead of inventing
violations -- but it self-tests on every run (see SELF_TEST below), because a
check that cannot fail is not a check, and the first draft of this one passed
happily against the unfixed file.
"""
import os
import re
import sys

BLE_DIRS = [
    os.path.join('ezlopi-services', 'ezlopi-service-ble'),
    os.path.join('ezlopi-core', 'ezlopi-core-ble'),
]

# Reading a length out of the chunk envelope. Either spelling of the accessor
# counts; what matters is that a wire-supplied length reaches a memcpy.
LEN_FROM_JSON = re.compile(
    r'CJ_GET_NUMBER\s*\(\s*ezlopi_len_str\s*\)'
    r'|cJSON_GetNumberValue[^;]*ezlopi_len_str')

CLAMP = 'EZPI_util_clamp_copy_len'

# "cJSON *root = cJSON_ParseWithLength(...)" and friends. Captures the variable
# so the free can be matched against that same variable rather than against any
# cJSON_Delete that happens to appear in the function.
PARSE_ASSIGN = re.compile(
    r'cJSON\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*cJSON_Parse[A-Za-z]*\s*\(')

# A bad sample and a good one. The bad one is the provisioning function as it
# actually stood; if the scanner stops flagging it, the scanner is broken.
SELF_TEST = {
    'bad': '''
static char *decode_thing(uint32_t total_size)
{
    char *base64_buffer = ezlopi_malloc(__FUNCTION__, total_size + 1);
    while (tmp)
    {
        cJSON *root = cJSON_ParseWithLength(__FUNCTION__, tmp->buffer, tmp->len);
        if (root)
        {
            uint32_t len = CJ_GET_NUMBER(ezlopi_len_str);
            char *data = CJ_GET_STRING(ezlopi_data_str);
            memcpy(base64_buffer + pos, data, len);
            pos += len;
        }
    }
    return base64_buffer;
}
''',
    'good': '''
static char *decode_thing(uint32_t total_size)
{
    char *base64_buffer = ezlopi_malloc(__FUNCTION__, total_size + 1);
    while (tmp)
    {
        cJSON *root = cJSON_ParseWithLength(__FUNCTION__, tmp->buffer, tmp->len);
        if (root)
        {
            uint32_t len = CJ_GET_NUMBER(ezlopi_len_str);
            char *data = CJ_GET_STRING(ezlopi_data_str);
            uint32_t safe = EZPI_util_clamp_copy_len(len, strlen(data), total_size - pos);
            memcpy(base64_buffer + pos, data, safe);
            pos += safe;
            cJSON_Delete(__FUNCTION__, root);
        }
    }
    return base64_buffer;
}
''',
    # Two trees, one free. This is the shape that defeated the first version of
    # the lifetime rule, and it is the shape the provisioning write handler had.
    'one_of_two': '''
static void write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    cJSON *root = cJSON_ParseWithLength(__FUNCTION__, param->write.value, param->write.len);
    if (root)
    {
        char *decoded = decode_thing(CJ_GET_NUMBER(ezlopi_total_len_str));
        if (decoded)
        {
            cJSON *cj_config = cJSON_ParseWithRef(__FUNCTION__, decoded);
            if (cj_config)
            {
                cJSON_Delete(__FUNCTION__, cj_config);
            }
            ezlopi_free(__FUNCTION__, decoded);
        }
    }
}
''',
}


def strip_comments(text):
    """Remove /* */ and // comments, preserving newlines so lines still line up."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith('/*', i):
            end = text.find('*/', i + 2)
            if end < 0:
                break
            out.append(''.join(c for c in text[i:end] if c == '\n'))
            i = end + 2
        elif text.startswith('//', i):
            end = text.find('\n', i)
            if end < 0:
                break
            i = end
        elif text[i] in '"\'':
            quote = text[i]
            out.append(text[i])
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    out.append(text[i])
                    i += 1
                if i < n:
                    out.append(text[i])
                    i += 1
            if i < n:
                out.append(text[i])
                i += 1
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def functions(text):
    """Yield (name, start_line, body) for each top-level function definition."""
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        if lines[i].startswith('{'):
            # Walk back to the signature line to name it.
            name = '?'
            for j in range(i - 1, max(-1, i - 6), -1):
                found = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', lines[j])
                if found:
                    name = found.group(1)
                    break

            depth = 0
            body = []
            k = i
            while k < len(lines):
                depth += lines[k].count('{') - lines[k].count('}')
                body.append(lines[k])
                if depth <= 0:
                    break
                k += 1

            yield name, i + 1, '\n'.join(body)
            i = k + 1
        else:
            i += 1


def scan(text):
    """Return a list of (function, line, rule) violations."""
    bad = []
    for name, line, body in functions(strip_comments(text)):
        if LEN_FROM_JSON.search(body) and 'memcpy' in body and CLAMP not in body:
            bad.append((name, line, 'wire length reaches memcpy without ' + CLAMP))

        # Pair by variable, not by mere presence of a cJSON_Delete somewhere in
        # the function. The first version of this check asked only "does this
        # function call cJSON_Delete at all", and the BLE provisioning write
        # handler passed it -- it freed the decoded config but leaked the chunk
        # envelope it had parsed first. Two trees, one free, check satisfied.
        for var in PARSE_ASSIGN.findall(body):
            freed = re.search(
                r'cJSON_Delete\s*\([^;]*\b' + re.escape(var) + r'\b', body)
            if not freed:
                bad.append((name, line,
                            'parsed into "%s" and never freed it' % var))
    return bad


def self_test():
    if not scan(SELF_TEST['bad']):
        print('SELF-TEST FAILED: the scanner did not flag the known-bad sample.')
        print('The check is not measuring anything. Fix the scanner.')
        return False
    if scan(SELF_TEST['good']):
        print('SELF-TEST FAILED: the scanner flagged the known-good sample.')
        return False
    leaks = [rule for _, _, rule in scan(SELF_TEST['one_of_two'])
             if 'never freed' in rule]
    if len(leaks) != 1:
        print('SELF-TEST FAILED: the two-trees-one-free sample should report '
              'exactly one leak, got %d.' % len(leaks))
        print('The lifetime rule has regressed to "any cJSON_Delete will do", '
              'which is what let the original leak through.')
        return False
    return True


def main():
    if not self_test():
        return 1

    violations = []
    scanned = 0

    for directory in BLE_DIRS:
        if not os.path.isdir(directory):
            continue
        for entry in sorted(os.listdir(directory)):
            if not entry.endswith('.c'):
                continue
            path = os.path.join(directory, entry)
            with open(path, encoding='utf-8', errors='replace') as handle:
                text = handle.read()
            scanned += 1
            for name, line, rule in scan(text):
                violations.append('%s:%d  %s(): %s' % (path, line, name, rule))

    if not scanned:
        print('No BLE sources found. Run this from the repository root.')
        return 1

    print('scanned %d BLE source file(s)' % scanned)

    if violations:
        print('\n%d violation(s):\n' % len(violations))
        for item in violations:
            print('  ' + item)
        print('\nSee the docstring at the top of this file for why this'
              ' matters. A wire-supplied length must be passed through'
              ' %s before it reaches a memcpy.' % CLAMP)
        return 1

    print('no violations')
    return 0


if __name__ == '__main__':
    sys.exit(main())
