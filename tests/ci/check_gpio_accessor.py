#!/usr/bin/env python3
"""Fail if a device-configuration pin number is read without being bounded.

THE BUG THIS EXISTS TO PREVENT

GPIO_IS_VALID_GPIO() looks like a range check and is not one. In
driver/gpio.h:50 it is

    #define GPIO_IS_VALID_GPIO(gpio_num) (((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0)

A shift count past the width of the register is truncated rather than
rejected, so an out-of-range pin number does not fail the test -- it aliases
onto a different, real pin and passes. Measured against all four target masks:
pin 70 is accepted as bit 6, pin 64 as bit 0. GPIO 6 is the SPI flash clock.

So a device configuration naming "gpio": 70 was accepted by the drivers'
own validation and then handed to gpio_config() as a mask selecting GPIO 6,
reconfiguring the flash clock as a push-pull output.

The fix was one bound in EZPI_core_cjson_get_value_gpio(), which refuses
anything outside [0, GPIO_NUM_MAX) for the target being built. That function
is reached through CJSON_GET_VALUE_GPIO. Pin reads that use any of the other
CJSON_GET_VALUE_* macros -- _INT, _UINT32 and friends -- skip the bound
entirely, because those are general-purpose integer readers shared with
hundreds of non-pin keys and the bound does not belong in them.

Sixteen pin reads were on the wrong macro when the bound was added, across
five drivers, and the worst of them (pms5003) had no GPIO_IS_VALID_GPIO check
of its own either, so nothing anywhere would have refused the pin.

WHY A CHECK AND NOT JUST THE SIXTEEN FIXES

Fixing sixteen sites does nothing about a seventeenth. This is the same shape
as the pasted BLE chunk assembler in check_ble_chunk_clamp.py: the fix was
right, and what was missing was anything to keep it right. A new driver that
copies an existing one -- which is how every driver in this tree was written,
visibly so -- will copy whichever macro its model used.

So the rule is structural: in the .c sources, a read of a pin key must use
CJSON_GET_VALUE_GPIO. Two invariants, because either one failing silently
removes the protection from all of them:

  1. every pin-key read goes through the GPIO accessor
  2. the accessor still bounds against GPIO_NUM_MAX

Invariant 2 matters as much as the first. Delete the range test and all
144 call sites keep compiling and silently stop being protected.

WHICH KEYS COUNT AS PIN KEYS

Taken from the ezlopi_gpio*_str declarations in the cloud keywords file
rather than listed here, so a pin key added later is covered without editing
this check. Bare "gpio..." string literals are also matched: when this was
written, pms5003 read its fourth pin as a literal "gpio4" rather than through
ezlopi_gpio4_str, and a check keyed only on the keyword variables would have
missed exactly the site that was least protected.

Keys whose *destination* merely mentions gpio are not pin keys and must not be
flagged -- ezlopi_is_ip_str, ezlopi_logic_inv_str and the other flags are read
into fields like item->interface.gpio.gpio_in.invert. There are 26 of those,
they are not pin numbers, and SELF_TEST['flag_not_a_pin'] pins that down.

LIMITS

This checks which macro reads the key, not what the driver then does with the
value. A driver that reads a pin correctly and then ignores
GPIO_IS_VALID_GPIO is out of scope here; the reserved-pin half of that problem
is recorded in the changelog as still open.
"""
import os
import re
import sys

KEYWORDS = os.path.join('ezlopi-cloud', 'ezlopi-cloud-constants',
                        'ezlopi_cloud_keywords.c')
ACCESSOR_FILE = os.path.join('ezlopi-core', 'ezlopi-core-ezlopi',
                             'ezlopi_core_cjson_macros.c')
ACCESSOR_FUNC = 'EZPI_core_cjson_get_value_gpio'
GOOD_MACRO = 'CJSON_GET_VALUE_GPIO'

SEARCH_DIRS = ('ezlopi-sensors-devices', 'ezlopi-core', 'ezlopi-cloud',
               'ezlopi-hal', 'ezlopi-service', 'ezlopi-util')

# CJSON_GET_VALUE_<KIND>(root, key, dest) -- newlines allowed inside the call,
# since a few sites in this tree wrap their arguments.
CALL = re.compile(
    r'\bCJSON_GET_VALUE_([A-Z0-9_]+)\s*\(\s*([^,()]+?)\s*,\s*([^,()]+?)\s*,',
    re.S)

# A pin key written as a literal rather than through its keyword variable.
LITERAL_PIN = re.compile(r'^"gpio[a-z0-9_]*"$')

SELF_TEST = {
    # The sixteen sites as they actually stood.
    'bad_uint32': 'CJSON_GET_VALUE_UINT32(cj_device, ezlopi_gpio1_str, '
                  'user_data->TCS230_pin.gpio_s0);',
    'bad_int': 'CJSON_GET_VALUE_INT(cj_device, ezlopi_gpio_str, '
               'item->interface.adc.gpio_num);',
    # The one that was a bare literal, which is the site a keyword-only check
    # would have missed.
    'bad_literal': 'CJSON_GET_VALUE_UINT32(cj_properties, "gpio4", '
                   'pms_object->pms_reset_pin);',
    # Wrapped across lines, because a few calls in the tree are.
    'bad_wrapped': 'CJSON_GET_VALUE_UINT32(cj_device,\n'
                   '                       ezlopi_gpio2_str,\n'
                   '                       user_data->pin);',
    # The fixed form.
    'good': 'CJSON_GET_VALUE_GPIO(cj_device, ezlopi_gpio1_str, '
            'user_data->TCS230_pin.gpio_s0);',
    'good_literal': 'CJSON_GET_VALUE_GPIO(cj_properties, "gpio4", '
                    'pms_object->pms_reset_pin);',
    # NOT a pin read. The destination mentions gpio; the key is a flag. There
    # are 26 of these and flagging them would make the check useless.
    'flag_not_a_pin': 'CJSON_GET_VALUE_INT(cj_device, ezlopi_logic_inv_str, '
                      'item->interface.gpio.gpio_in.invert);\n'
                      'CJSON_GET_VALUE_BOOL(cjson_device, ezlopi_is_ip_str, '
                      'item->interface.gpio.gpio_in.enable);\n'
                      'CJSON_GET_VALUE_INT(cjson_device, ezlopi_val_op_str, '
                      'item->interface.gpio.gpio_out.value);',
    # A commented-out bad call must not be reported, the same decoy that
    # tripped the first version of check_trace_format_attr.
    'commented_decoy': '// CJSON_GET_VALUE_INT(cj, ezlopi_gpio1_str, old);\n'
                       '/* CJSON_GET_VALUE_UINT32(cj, "gpio2", older); */\n'
                       'CJSON_GET_VALUE_GPIO(cj, ezlopi_gpio1_str, live);',
}

# Invariant 2: the accessor body must still bound the value. Both the upper
# bound against the symbol and the fact that it is the symbol and not a
# literal -- GPIO_NUM_MAX is 40 on esp32 and 22 on esp32c3, so a literal would
# be wrong on three targets out of four.
BOUND = re.compile(r'<\s*\(?\s*double\s*\)?\s*\)?\s*GPIO_NUM_MAX'
                   r'|<\s*GPIO_NUM_MAX')

SELF_TEST_ACCESSOR = {
    'bad': '''int EZPI_core_cjson_get_value_gpio(cJSON *r, const char *n)
{
    int item_val = -1;
    cJSON *o = cJSON_GetObjectItem(__FUNCTION__, r, n);
    if (o && (o->type & cJSON_Number)) { item_val = (int)o->valuedouble; }
    return item_val;
}''',
    'bad_literal_bound': '''int EZPI_core_cjson_get_value_gpio(cJSON *r, const char *n)
{
    int item_val = -1;
    cJSON *o = cJSON_GetObjectItem(__FUNCTION__, r, n);
    if (o && (o->type & cJSON_Number)) {
        const double raw = o->valuedouble;
        if ((raw >= 0.0) && (raw < 40.0)) { item_val = (int)raw; }
    }
    return item_val;
}''',
    'good': '''int EZPI_core_cjson_get_value_gpio(cJSON *r, const char *n)
{
    int item_val = -1;
    cJSON *o = cJSON_GetObjectItem(__FUNCTION__, r, n);
    if (o && (o->type & cJSON_Number)) {
        const double raw = o->valuedouble;
        if (((raw >= 0.0) && (raw < (double)GPIO_NUM_MAX)) ||
            (raw == -1.0) || (raw == (double)EZPI_GPIO_UNUSED_255)) {
            item_val = (int)raw;
        }
    }
    return item_val;
}''',
}


def strip_comments(text):
    """Remove /* */ and // comments, preserving newlines so lines still align."""
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
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def pin_keys(text):
    """The ezlopi_gpio*_str keyword variable names declared in text."""
    return set(re.findall(r'\bconst\s+char\s*\*\s*(ezlopi_gpio[a-z0-9_]*_str)\b',
                          text))


def is_pin_key(arg, keys):
    arg = arg.strip()
    return arg in keys or bool(LITERAL_PIN.match(arg))


def check_text(text, keys):
    """Return [(line, macro, key)] for pin reads not using the GPIO accessor."""
    stripped = strip_comments(text)
    bad = []
    for found in CALL.finditer(stripped):
        kind, _root, key = found.group(1), found.group(2), found.group(3)
        if kind == 'GPIO':
            continue
        if is_pin_key(key, keys):
            line = stripped.count('\n', 0, found.start()) + 1
            bad.append((line, 'CJSON_GET_VALUE_' + kind, key.strip()))
    return bad


def accessor_is_bounded(text):
    """True if the accessor body still has a GPIO_NUM_MAX upper bound."""
    stripped = strip_comments(text)
    found = re.search(r'\b' + re.escape(ACCESSOR_FUNC) + r'\s*\([^)]*\)\s*\{',
                      stripped)
    if not found:
        return None
    # Walk braces to the end of the function.
    depth = 0
    start = stripped.index('{', found.start())
    for i in range(start, len(stripped)):
        if stripped[i] == '{':
            depth += 1
        elif stripped[i] == '}':
            depth -= 1
            if depth == 0:
                body = stripped[start:i + 1]
                return bool(BOUND.search(body))
    return None


def self_test():
    keys = {'ezlopi_gpio_str', 'ezlopi_gpio1_str', 'ezlopi_gpio2_str',
            'ezlopi_gpio4_str'}
    for name in ('bad_uint32', 'bad_int', 'bad_literal', 'bad_wrapped'):
        if not check_text(SELF_TEST[name], keys):
            print('SELF-TEST FAILED: %s was accepted. The check is not '
                  'measuring anything.' % name)
            return False
    for name in ('good', 'good_literal', 'flag_not_a_pin', 'commented_decoy'):
        noise = check_text(SELF_TEST[name], keys)
        if noise:
            print('SELF-TEST FAILED: %s was reported as a violation: %s'
                  % (name, noise))
            return False
    # Invariant 2, both directions.
    if accessor_is_bounded(SELF_TEST_ACCESSOR['bad']):
        print('SELF-TEST FAILED: the unbounded accessor was accepted.')
        return False
    if accessor_is_bounded(SELF_TEST_ACCESSOR['bad_literal_bound']):
        print('SELF-TEST FAILED: a bound written as a literal 40 was '
              'accepted. GPIO_NUM_MAX is 22 on esp32c3.')
        return False
    if not accessor_is_bounded(SELF_TEST_ACCESSOR['good']):
        print('SELF-TEST FAILED: the real accessor shape was rejected.')
        return False
    return True


def sources():
    for root_dir in SEARCH_DIRS:
        if not os.path.isdir(root_dir):
            continue
        for here, _dirs, files in os.walk(root_dir):
            for name in files:
                if name.endswith('.c'):
                    yield os.path.join(here, name)


def main():
    if not self_test():
        return 1

    if not os.path.isfile(KEYWORDS):
        print('%s not found. Run this from the repository root.' % KEYWORDS)
        return 1

    with open(KEYWORDS, encoding='utf-8', errors='replace') as handle:
        keys = pin_keys(handle.read())
    if not keys:
        print('No ezlopi_gpio*_str declarations found in %s. Either the pin '
              'keys were renamed or this check is looking in the wrong '
              'place; both need a human.' % KEYWORDS)
        return 1

    problems = []
    for path in sorted(sources()):
        with open(path, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
        for line, macro, key in check_text(text, keys):
            problems.append('%s:%d: %s reads the pin key %s'
                            % (path, line, macro, key))

    if not os.path.isfile(ACCESSOR_FILE):
        print('%s not found.' % ACCESSOR_FILE)
        return 1
    with open(ACCESSOR_FILE, encoding='utf-8', errors='replace') as handle:
        bounded = accessor_is_bounded(handle.read())
    if bounded is None:
        problems.append('%s: %s() not found -- renamed or removed, so whether '
                        'pins are bounded at all is now unknown'
                        % (ACCESSOR_FILE, ACCESSOR_FUNC))
    elif not bounded:
        problems.append('%s: %s() no longer bounds the pin against '
                        'GPIO_NUM_MAX. Every CJSON_GET_VALUE_GPIO site is '
                        'unprotected.' % (ACCESSOR_FILE, ACCESSOR_FUNC))

    if problems:
        print('Unbounded device-configuration pin reads:\n')
        for item in problems:
            print('  ' + item)
        print('\nUse %s for pin keys. See the docstring at the top of this '
              'file: GPIO_IS_VALID_GPIO is a shift, not a range check, so an '
              'out-of-range pin aliases onto a real one and passes '
              'validation.' % GOOD_MACRO)
        return 1

    print('pin keys checked: %d; all pin reads use %s; accessor bound present'
          % (len(keys), GOOD_MACRO))
    return 0


if __name__ == '__main__':
    sys.exit(main())
