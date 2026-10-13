#!/usr/bin/env python3
"""Fail if the TRACE_* plumbing loses its printf format attribute.

THE BUG THIS EXISTS TO PREVENT

Every TRACE_E/W/I/D/S expands to trace_color_print(), a variadic function whose
fifth parameter is a format string:

    void trace_color_print(const char *txt_color, uint8_t severity,
                           const char *file, int line, const char *format, ...)

For years it carried no __attribute__((format(printf, 5, 6))). The macro form
that might have given the compiler a hint is sitting behind an "#if 0" in
ezlopi_util_trace.h, so the real function is what gets called, and a format
string that disagreed with its arguments was nobody's problem until runtime.

The cost was concrete. ezlopi_service_webprov.c's __provision_update() had:

    TRACE_E("key \"%s\" not found.", cj_root_prov_data);

cj_root_prov_data is a cJSON *, not a char *. The logger therefore began
reading at the first byte of a cJSON struct -- whose leading member is the
"next" pointer -- and continued until it chanced upon a zero byte, printing
whatever it passed through. That branch is reached whenever the provisioning
response parses as JSON but carries neither "data" nor "error_code".

CONFIG_EZPI_UTIL_TRACE_EN=y in all four shipped sdkconfig.defaults.*, so the
line was compiled into every image. Adding the attribute turned it into a
compile error immediately -- and it was the ONLY such defect in the whole
tree across all four targets, so the attribute costs nothing to keep.

WHY A CHECK AND NOT JUST THE ATTRIBUTE

The build already rejects format mismatches: -Werror=format is on, so the
attribute alone is self-enforcing *while it is there*. The failure mode this
check covers is the attribute being deleted -- by a refactor, a header
reshuffle, or someone silencing a warning the quick way. Nothing would fail.
The build would go green and every TRACE_* in the tree would quietly stop
being checked again.

That is the same shape as the BLE bug recorded in check_ble_chunk_clamp.py:
the fix was correct and the sibling copy went unguarded. So the invariant is
structural, not "does the tree currently compile".

LIMITS

This checks for the attribute on the declarations, not that it is correct.
A wrong index pair would be caught by the first build instead, loudly.

Comments are stripped before scanning. The header carries a commented-out
historical prototype of trace_color_print() above the real one, and the first
version of this check matched that -- reporting the attribute missing when it
was present two lines down. Note that the self-test did not catch that: the
samples had no comments in them. SELF_TEST['commented_decoy'] does now.
"""
import os
import re
import sys

HEADER = os.path.join('ezlopi-util', 'ezlopi-util-trace', 'ezlopi_util_trace.h')

# The variadic sinks every TRACE_* macro funnels into, and the argument
# positions their format string and varargs actually occupy.
REQUIRED = {
    'trace_color_print': (5, 6),
    'trace_otel': (4, 5),
}

# __attribute__((format(printf, N, M))), however it is spelled or wrapped --
# directly, or through the EZPI_PRINTF_FMT(N, M) alias.
ATTR = re.compile(
    r'__attribute__\s*\(\s*\(\s*format\s*\(\s*printf\s*,\s*(\d+)\s*,\s*(\d+)\s*\)'
    r'|EZPI_PRINTF_FMT\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)')

SELF_TEST = {
    # The declaration as it actually stood: no attribute, no diagnostics.
    'bad': 'void trace_color_print(const char *txt_color, uint8_t severity, '
           'const char *file, int line, const char *format, ...);',
    'good': 'void trace_color_print(const char *txt_color, uint8_t severity, '
            'const char *file, int line, const char *format, ...) '
            'EZPI_PRINTF_FMT(5, 6);',
    'good_raw': 'void trace_color_print(const char *txt_color, uint8_t severity, '
                'const char *file, int line, const char *format, ...) '
                '__attribute__((format(printf, 5, 6)));',
    # Right attribute, wrong positions -- must not be accepted.
    'wrong_idx': 'void trace_color_print(const char *txt_color, uint8_t severity, '
                 'const char *file, int line, const char *format, ...) '
                 'EZPI_PRINTF_FMT(1, 2);',
    # The shape of the real header: a commented-out historical prototype sits
    # above the live declaration. Matching the comment instead of the code is
    # what the first version of this check actually did.
    'commented_decoy': '''
    // void trace_color_print(const char* txt_color, uint8_t severity, const char* format, ...);
    /* void trace_color_print(const char *a, uint8_t b, const char *format, ...); */
    void trace_color_print(const char *txt_color, uint8_t severity, const char *file, int line, const char *format, ...) EZPI_PRINTF_FMT(5, 6);
''',
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


def declaration_of(text, func):
    """Return the text of func's declaration, from its name to the ';'."""
    found = re.search(r'\b' + re.escape(func) + r'\s*\(', text)
    if not found:
        return None
    end = text.find(';', found.start())
    if end < 0:
        return None
    return text[found.start():end + 1]


def indices_in(decl):
    """Return the (fmt, args) index pair declared on decl, or None."""
    found = ATTR.search(decl)
    if not found:
        return None
    groups = found.groups()
    pair = groups[0:2] if groups[0] is not None else groups[2:4]
    return (int(pair[0]), int(pair[1]))


def check(text):
    """Return a list of complaints about text."""
    text = strip_comments(text)
    bad = []
    for func, want in sorted(REQUIRED.items()):
        decl = declaration_of(text, func)
        if decl is None:
            # Renamed or removed. Not this check's job to guess, but say so
            # rather than passing silently on a file that no longer matches.
            bad.append('%s(): declaration not found' % func)
            continue
        got = indices_in(decl)
        if got is None:
            bad.append('%s(): no printf format attribute -- format strings '
                       'in every TRACE_* are unchecked' % func)
        elif got != want:
            bad.append('%s(): format attribute is (%d, %d), expected (%d, %d)'
                       % (func, got[0], got[1], want[0], want[1]))
    return bad


def self_test():
    if not check(SELF_TEST['bad']):
        print('SELF-TEST FAILED: the known-bad declaration was accepted.')
        print('The check is not measuring anything.')
        return False
    for name in ('good', 'good_raw', 'commented_decoy'):
        # Only trace_color_print is present in these samples, so ignore the
        # unavoidable trace_otel complaint and assert on the rest.
        noise = [c for c in check(SELF_TEST[name])
                 if not c.startswith('trace_otel')]
        if noise:
            print('SELF-TEST FAILED: %s declaration rejected: %s'
                  % (name, noise))
            return False
    wrong = [c for c in check(SELF_TEST['wrong_idx'])
             if c.startswith('trace_color_print')]
    if not wrong:
        print('SELF-TEST FAILED: an attribute with the wrong argument '
              'positions was accepted.')
        return False
    return True


def main():
    if not self_test():
        return 1

    if not os.path.isfile(HEADER):
        print('%s not found. Run this from the repository root.' % HEADER)
        return 1

    with open(HEADER, encoding='utf-8', errors='replace') as handle:
        text = handle.read()

    problems = check(text)

    if problems:
        print('%s:\n' % HEADER)
        for item in problems:
            print('  ' + item)
        print('\nSee the docstring at the top of this file. Without the'
              ' attribute, a TRACE_* whose arguments disagree with its format'
              ' string compiles silently and misreads memory at runtime.')
        return 1

    print('trace format attributes present: %s'
          % ', '.join('%s(%d, %d)' % (f, a, b)
                      for f, (a, b) in sorted(REQUIRED.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
