#!/usr/bin/env python3
"""Fail if cppcheck finds a null-pointer dereference in the first-party sources.

THE BUG CLASS THIS EXISTS TO PREVENT

Thirty-four defects across thirteen files, of which cppcheck reported 33 as 34
findings. Twenty-nine of the findings were one copy-pasted mistake:

    char *updated_rooms = cJSON_PrintBuffered(__FUNCTION__, cj_rooms, 4096, false);
    TRACE_D("length of 'updated_rooms': %d", strlen(updated_rooms));
    if (updated_rooms)                       /* the guard, one line too late */
    {
        EZPI_core_nvs_write_rooms(updated_rooms);
        ...

Both halves of that are load-bearing. cJSON_PrintBuffered() (cjext.c:1440)
really does return NULL -- on a negative prebuffer (1389), on ezlopi_malloc()
failure (1395), and on print_value() failure (1406) -- which is precisely why
every one of these sites has an `if (ptr)` written for it. And TRACE_D really
is compiled in: CONFIG_EZPI_UTIL_TRACE_EN is `default y` in Kconfig.projbuild
and =y in all four committed sdkconfig.defaults.* files. So the strlen(NULL)
ran, in every shipped image, one line before the test meant to prevent it.

A thirtieth instance of that same idiom, at ezlopi_core_scenes_v2.c:2091, is
NOT in cppcheck's output and was found by grepping for the trace text once the
shape was known. It is fixed along with the rest. Keep that in mind when
reading a green run here: the analyser found 29 of the 30, which is the same
story as the recall measurement below.

The remaining five findings were four defects of a different shape. Note the
`scripts.c` one accounts for two of the five findings on its own, which is why
34 findings cover 33 defects:

  * ezlopi_core_scenes_scripts.c -- `curr_script->next = __scripts_create_node();
    if (curr_script)`. The test was on the wrong pointer, and the loop above had
    already proved that one non-NULL, so it was always taken and ->next->id was
    dereferenced regardless. __scripts_create_node() returns NULL for any script
    object missing a string "name" or "code", which arrives over the cloud API.
  * ezlopi_core_scenes_operators.c x2 -- `if (a && b && value && comparator)`
    with the else branch dereferencing `comparator` unconditionally.
  * ezlopi_core_scenes_value.c -- `if (cj_root && (NULL == cj_root->valuestring))`
    with the else branch dereferencing cj_root, which that condition is false
    for when cj_root is NULL.

WHY AN ANALYSER AND NOT A GREP

A grep was tried first and is the wrong tool. The defect is an ordering
relation between a dereference and a test of the same pointer, which means
knowing which expressions dereference what -- and the 116
EZPI_core_factory_info_v3_get_* call sites alone put a hand audit out of reach.
cppcheck's nullPointerRedundantCheck is exactly this relation: "either the
condition is redundant or there is a possible null pointer dereference".

WHAT THIS CHECK IS WORTH, MEASURED

Recall was tested before the check was trusted, against the two
check-after-use defects already fixed in this branch, each extracted at the
commit before its fix:

  * EZPI_core_factory_info_v3_get_ezlopi_config() -- isprint(ret[0]) above
    `if (ret)`: FOUND.
  * ezpi_provisioning_info_read_func() -- two strlen()s above
    `if (NULL != g_provisioning_info_base64)`: NOT FOUND, even at
    --enable=all --check-level=exhaustive.

So recall is 1 of 2 on known cases. That is worth stating plainly: a green run
here does not mean the tree is free of this class. It means the 34 instances
this did find cannot come back silently, which is the regression the eleven
files above would otherwise be wide open to.

SCOPE, AND WHY ezlopi-components IS OUT OF IT

In scope: ezlopi-core, ezlopi-hal, ezlopi-services, ezlopi-cloud, ezlopi-util,
ezlopi-user-config, ezlopi-main. Baseline measured at zero across all seven.

ezlopi-components is excluded, and the reason was measured rather than
assumed. It reports 70 findings, and every one of them is in vendored code:
31 in cjext/, 39 in the upstream Lua interpreter under lua/lua/. Nothing
outside those two trees is flagged, so excluding the directory loses no
coverage of this project's own drivers today. Thirteen of its sixteen
subdirectories are vendored libraries (Bosch BSEC2, bme280, ld2410, led_strip,
lib8tion, micro_tz_db, protothreads and so on), so putting it in scope would
pin a zero baseline on third-party sources that nobody here should be editing
and that a vendor bump would break.

No include paths are passed. Resolving the IDF header graph per translation
unit needs a compile_commands.json from a configured build, which would make
this check depend on which target was last configured and on a full build.
Without headers cppcheck cannot expand IDF macros and will miss things -- that
is part of why recall is 1 of 2 -- but it still found all 34.

SELF-TEST

Three fixtures run before the tree is scanned, because a check that cannot
fail is worse than no check: it reports a clean tree either way. They assert
that the broken ordering IS flagged, that the fixed ordering is NOT, and that
the misdirected-guard shape IS. If cppcheck is missing, too old for
--check-level, or has had the check renamed, the self-test fails loudly
instead of the scan passing vacuously.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

DIRS = [
    'ezlopi-core',
    'ezlopi-hal',
    'ezlopi-services',
    'ezlopi-cloud',
    'ezlopi-util',
    'ezlopi-user-config',
    'ezlopi-main',
]

# Only the warning category is enabled. style/performance contribute ~460
# further findings in this tree, none of them in this class, and a check whose
# baseline is coupled to style churn is a check that gets switched off.
FLAGS = [
    '--enable=warning',
    '--inline-suppr',
    '--suppress=missingInclude',
    '--suppress=missingIncludeSystem',
    '--suppress=unusedFunction',
    '--suppress=checkersReport',
    '--check-level=exhaustive',
    '-j2',
]

# nullPointer, nullPointerRedundantCheck, nullPointerArithmetic, and any
# sibling cppcheck adds later.
FINDING = re.compile(r'\[nullPointer[A-Za-z]*\]\s*$')

BROKEN = '''\
#include <string.h>
char *make_string(void);
void trace(const char *fmt, ...);
void store(const char *s);
void sink(void)
{
    char *p = make_string();
    trace("length of 'p': %d", strlen(p));
    if (p)
    {
        store(p);
    }
}
'''

FIXED = '''\
#include <string.h>
char *make_string(void);
void trace(const char *fmt, ...);
void store(const char *s);
void sink(void)
{
    char *p = make_string();
    if (p)
    {
        trace("length of 'p': %d", strlen(p));
        store(p);
    }
}
'''

# The scripts.c shape: the test names a pointer already known non-NULL, while
# the one that can be NULL is dereferenced inside.
MISDIRECTED = '''\
struct node { struct node *next; int id; };
struct node *create_node(void);
int tail_id(struct node *head)
{
    int id = 0;
    struct node *cur = head;
    while (cur->next)
    {
        cur = cur->next;
    }
    cur->next = create_node();
    if (cur)
    {
        id = cur->next->id;
    }
    return id;
}
'''


def run_cppcheck(targets):
    """Return (findings, stderr_text). Findings are lines in the null class."""
    proc = subprocess.run(
        ['cppcheck'] + FLAGS + list(targets),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    lines = [l.rstrip() for l in proc.stdout.splitlines()]
    return [l for l in lines if FINDING.search(l)], proc.stdout


def self_test():
    if shutil.which('cppcheck') is None:
        print('SELF-TEST FAILED: cppcheck is not installed, so this check '
              'would pass on any tree.')
        return False

    version = subprocess.run(['cppcheck', '--version'],
                             stdout=subprocess.PIPE, universal_newlines=True)
    print('cppcheck: %s' % version.stdout.strip())

    cases = [
        ('broken ordering', BROKEN, True),
        ('fixed ordering', FIXED, False),
        ('misdirected guard', MISDIRECTED, True),
    ]

    work = tempfile.mkdtemp(prefix='nullcheck_')
    try:
        for name, source, want_hit in cases:
            path = os.path.join(work, re.sub(r'\W+', '_', name) + '.c')
            with open(path, 'w', encoding='utf-8') as handle:
                handle.write(source)
            hits, raw = run_cppcheck([path])
            if want_hit and not hits:
                print('SELF-TEST FAILED: the %s fixture produced no '
                      'null-class finding, so this check cannot fail.\n'
                      '--- cppcheck said ---\n%s' % (name, raw))
                return False
            if not want_hit and hits:
                print('SELF-TEST FAILED: the %s fixture was flagged, so this '
                      'check fires on correct code:\n  %s'
                      % (name, '\n  '.join(hits)))
                return False
            print('  self-test %-20s %s' % (name, 'flagged' if hits else 'clean'))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    return True


def main():
    if not self_test():
        return 1

    missing = [d for d in DIRS if not os.path.isdir(d)]
    if missing:
        print('not found: %s\nRun this from the repository root.'
              % ', '.join(missing))
        return 1

    hits, raw = run_cppcheck(DIRS)

    if hits:
        print('\nnull-pointer findings in first-party sources:\n')
        for line in hits:
            print('  ' + line)
        print('\n%d finding(s). See the docstring at the top of this file for '
              'the shapes this catches.\n'
              'The usual cause is a dereference above the NULL test written '
              'for it -- a length trace, or a guard naming the wrong pointer.'
              % len(hits))
        return 1

    print('\nno null-pointer findings in: %s' % ' '.join(DIRS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
