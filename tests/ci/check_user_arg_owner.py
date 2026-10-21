#!/usr/bin/env python3
"""Fail if a driver names more than one owner for one item user_arg.

THE BUG THIS EXISTS TO PREVENT

l_ezlopi_item_t carries a void *user_arg and a bool is_user_arg_unique. The
bool means "this item frees user_arg", and it is read in exactly one place,
ezlopi_device_free_item() in ezlopi-core/ezlopi-core-devices:

    if (NULL != (items->user_arg) && (true == items->is_user_arg_unique))
    {
        ezlopi_free(__FUNCTION__, items->user_arg);
        items->user_arg = NULL;
    }

That function recurses down the whole item list of a device. Clearing
items->user_arg afterwards protects nothing, because the duplicate is a
*different* item still holding the same address.

Nothing documented the field, and its name reads as "user_arg belongs to one
device". Four drivers were written to that reading -- they handed a single
allocation to every item of a device and flagged all of them:

    device_0009_other_RMT_SK6812.c   6 items, 1 malloc, 6 owners
    device_0022_PWM_dimmable_lamp.c  5 items, 1 malloc, 5 owners
    device_0038_other_RGB.c          3 items, 1 malloc, 3 owners
    sensor_0012_I2C_BME280.c         3 items, 1 malloc, 2 owners on the
                                     parent device (the third item is on a
                                     child device and was correctly false)

SK6812 even said so: "/*linked to only one device so 'TRUE'*/".

So removing one of those devices passed one pointer to free() two, three,
five or six times. CONFIG_EZPI_HEAP_ENABLE is unset in sdkconfig and in
sdkconfig.defaults for all four targets, which makes ezlopi_free a
NULL-checked free() -- a real double free. With heap tracing on,
EZPI_core_util_heap_free walks its node list for a match with freed == 0,
finds none, and returns having done nothing: the second free is absorbed and
the bug is invisible. It corrupted the shipped build and hid in the
instrumented one.

The path that reaches it needs no cloud traffic. __EZPI_initialize_devices_v3
drops any device whose item fails to initialise, and dropping it calls
EZPI_core_device_free_device. A dimmable lamp with a pin the hardware refuses
is enough.

WHY A CHECK AND NOT JUST THE FIVE FIXES

The field is a bare bool with no API around it, every driver in this tree was
written by copying an existing one, and the wrong reading of the name is the
intuitive one. Four of the eight drivers that share an arg between items got
it wrong. A fifth, sensor_0029_I2C_GXHTC3.c, got it wrong in the other
direction -- "item_temperature->is_user_arg_unique = false" where
item_humdity was meant, which cleared the only owner and leaked the
allocation on the normal path. Fixing five files does nothing about the
sixth.

THE RULE

At most one "is_user_arg_unique = true" per driver .c file.

That is a proxy, and the honest statement of it is: the check cannot see
which device an item belongs to, so it cannot distinguish "two owners of two
separate allocations" (fine) from "two owners of one allocation" (a double
free). It resolves that by requiring the stronger property. No file in the
tree needs two, so the rule costs nothing today; a file that genuinely makes
two independent user_arg allocations will have to be dealt with here, and the
right fix then is to look at the device membership by hand rather than to
widen the rule.

WHAT IS DELIBERATELY NOT CHECKED

The other half -- a heap allocation handed to items with *no* owner, which
leaks -- is not enforceable this way. 12 driver files assign user_arg without
naming any owner, and spot-checking them shows the field is overloaded:
sensor_0026_ADC_LDR.c and sensor_0061_digitalIn_reed_switch.c store a string
literal in user_arg as a last-reported-value slot, where flagging an owner
would free a literal. l_ezlopi_device_settings_v3_t has its own user_arg
field with no ownership flag at all, which is what the two *_settings.c files
in that list are using. So "assigns user_arg, therefore must name an owner"
is false, and a check asserting it would be wrong 12 times over.

The second invariant below covers the part of that which can be nailed down.

INVARIANT 2: THE CORE STILL HONOURS AND STILL COLLAPSES

Two things in ezlopi_core_devices.c have to stay true or the rule above stops
meaning anything:

  1. ezlopi_device_free_item() still frees user_arg on the flag. If that is
     deleted, every driver keeps compiling and every shared arg leaks.
  2. ezlopi_device_free_single() still collapses duplicate ownership before
     freeing. That loop is containment, not the fix: it turns a future
     miscount into a leak instead of heap corruption. It is invisible to the
     drivers, so nothing but this check would notice its removal.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DRIVERS = os.path.join(ROOT, 'ezlopi-sensors-devices')
CORE = os.path.join(ROOT, 'ezlopi-core', 'ezlopi-core-devices', 'ezlopi_core_devices.c')

OWNER = re.compile(r'is_user_arg_unique\s*=\s*true')


def strip_comments(text):
    """Remove /* */ and // so a comment quoting the pattern is not counted.

    The fixes this check landed with put explanatory comments next to the
    corrected lines, and several of those comments contain the literal
    "is_user_arg_unique = true" while describing what used to be there. A
    check that counted those would report the files it had just fixed.

    Every newline inside a removed region is kept. That is not cosmetic: the
    line numbers this check prints are the only thing a maintainer has to go
    on, and the first version of this function dropped the newlines inside
    /* */ blocks. Counts stayed correct, so the gate still passed and failed
    in the right places -- but every number it printed for a file with a
    multi-line block comment was short by the number of lines those comments
    spanned. Measured on Ezlo's device_0009 it reported lines 607-612 for
    assignments that are really at 680-685, a 73-line error pointing into an
    unrelated function. See the self-test cases that assert positions.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            end = text.find('*/', i + 2)
            stop = n if end < 0 else end + 2
            out.append('\n' * text.count('\n', i, stop))
            i = stop
        elif text.startswith('//', i):
            end = text.find('\n', i)
            # The newline itself is left in place for the next iteration.
            i = n if end < 0 else end
        elif text[i] in '"\'':
            quote = text[i]
            start = i
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == '\\' else 1
            i += 1
            out.append('\n' * text.count('\n', start, min(i, n)))
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def owners_in(text):
    """Line numbers of owner assignments, comments and strings excluded.

    The numbers are line numbers in the ORIGINAL text, which is only true
    because strip_comments() preserves newlines; see the note there.
    """
    lines = strip_comments(text).splitlines()
    return [i for i, line in enumerate(lines, 1) if OWNER.search(line)]


def self_test():
    """The check has to be seen failing before a pass means anything."""
    one = 'void f(void) { a->is_user_arg_unique = true; }\n'
    two = one + 'void g(void) { b->is_user_arg_unique = true; }\n'
    commented = ('/* b->is_user_arg_unique = true used to be here */\n'
                 '// and c->is_user_arg_unique = true here\n') + one

    cases = [
        ('one owner', one, 1),
        ('two owners', two, 2),
        ('one owner plus two in comments', commented, 1),
        ('no owner', 'void f(void) { a->is_user_arg_unique = false; }\n', 0),
        ('owner inside a string literal',
         'const char *s = "is_user_arg_unique = true";\n', 0),
    ]
    ok = True
    for name, text, expected in cases:
        got = len(owners_in(text))
        if got != expected:
            print('SELF-TEST FAILED: %s -- expected %d owner(s), counted %d'
                  % (name, expected, got))
            ok = False

    # Counting alone is not enough. Every case above passed while the printed
    # line numbers were wrong, because none of them contained a comment
    # spanning more than one line. These assert the POSITION.
    banner = '/*\n * a license header, several lines long\n * mentioning nothing\n */\n'
    position_cases = [
        ('owner on line 1', one, [1]),
        ('owner after a 4-line block comment', banner + one, [5]),
        ('owner after two block comments',
         banner + banner + one, [9]),
        ('owner between two block comments',
         banner + one + banner + one, [5, 10]),
        ('owner after a // comment line',
         '// nothing here\n' + one, [2]),
        ('owner after a multi-line string continuation',
         'const char *s = "abc"\n                "def";\n' + one, [3]),
    ]
    for name, text, expected in position_cases:
        got = owners_in(text)
        if got != expected:
            print('SELF-TEST FAILED: %s -- expected owner(s) at line(s) %s, '
                  'reported %s' % (name, expected, got))
            ok = False

    if not ok:
        print('\nThe counter is wrong, so neither a pass nor a failure below '
              'would mean anything.')
    return ok


def driver_sources():
    for here, dirs, files in os.walk(DRIVERS):
        dirs[:] = [d for d in dirs if d != 'build']
        for name in sorted(files):
            if name.endswith('.c'):
                yield os.path.join(here, name)


def check_core():
    """Invariant 2: the free path still honours and still collapses."""
    problems = []
    try:
        with open(CORE, encoding='utf-8', errors='replace') as handle:
            text = strip_comments(handle.read())
    except OSError as exc:
        return ['%s: cannot be read (%s), so neither half of invariant 2 can '
                'be confirmed' % (CORE, exc)]

    if not re.search(r'true\s*==\s*items->is_user_arg_unique', text):
        problems.append(
            '%s: ezlopi_device_free_item() no longer frees user_arg on '
            'is_user_arg_unique. Every driver still compiles and every shared '
            'user_arg now leaks.' % CORE)

    collapse = re.search(r'dup->is_user_arg_unique\s*=\s*false', text)
    if not collapse:
        problems.append(
            '%s: ezlopi_device_free_single() no longer collapses duplicate '
            'user_arg ownership before freeing. That loop is what keeps a '
            'miscounted flag to a leak instead of a double free.' % CORE)
    return problems


def main():
    if not self_test():
        return 1

    scanned = 0
    offenders = []
    with_owner = 0
    for path in driver_sources():
        scanned += 1
        with open(path, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
        if 'is_user_arg_unique' not in text:
            continue
        lines = owners_in(text)
        if lines:
            with_owner += 1
        if len(lines) > 1:
            offenders.append((os.path.relpath(path, ROOT).replace('\\', '/'),
                              lines))

    problems = check_core()

    if offenders or problems:
        if offenders:
            print('more than one is_user_arg_unique owner in one driver:\n')
            for rel, lines in offenders:
                print('  %s: %d owners, at lines %s'
                      % (rel, len(lines), ', '.join(map(str, lines))))
            print('\nIf those owners name items on the same device and share '
                  'one allocation, that device being freed passes one pointer '
                  'to free() once per owner. Flag exactly one item -- and an '
                  'item that cannot be removed while the others remain. See '
                  'the docstring at the top of this file.')
        for item in problems:
            print('\n' + item)
        return 1

    print('driver sources scanned: %d; %d name an owner for an item user_arg, '
          'none names two; ezlopi_device_free_item still frees on the flag and '
          'ezlopi_device_free_single still collapses duplicate ownership'
          % (scanned, with_owner))
    return 0


if __name__ == '__main__':
    sys.exit(main())
