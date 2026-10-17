#!/usr/bin/env python3
"""Fail if a scanf-family conversion that writes a string has no field width.

THE BUG THIS EXISTS TO PREVENT

`%s` and `%[...]` in a scanf format write as many characters as the input
offers. The destination is almost always a fixed buffer, so the length of the
write is chosen by whoever supplied the input. The site this check was written
for is the last arm of the URL-parsing ladder in EZPI_parse_http_url():

    char host[100] = {0};
    char page[200] = {0};
    ...
    else if (sscanf(field_value_string, "%*[^:]%*[:/]%[^:]:%d%s",
                    host, &port, page) == 3)

`%[^:]` writes into host[100] and `%s` writes into page[200], neither with a
width. `field_value_string` is the `url` field of a sendHttpRequest scene
action; EZPI_scenes_populate_fields_get_value allocates it at
strlen(valuestring) + 1 with no cap, so its length is whatever the request
said.

Measured with that ladder reproduced verbatim under AddressSanitizer:
"ftp://" + 599 characters + ":80/x" is a stack-buffer-overflow write past the
end of host, and "ftp://h:80/" + 599 characters is a 601-byte write past the
end of page.

WHY THE EIGHT ARMS ABOVE IT DID NOT MAKE IT SAFE

Worth stating, because it is the reason the site survived review: every arm
above the last one is width-bounded, and the author plainly knew to write
`%99[^:]` and `%199[^\\n]`. The last arm looks like a corner that cannot be
reached. It can. Arms one to eight each open with a literal "http://" or
"https://", and arms seven and eight are catch-alls for those two prefixes, so
a 599-character host behind an http:// prefix lands on arm seven and is bounded
at 99 there. Any other scheme fails all eight literals and arrives at the one
arm without widths.

That is the shape this check is for: one unbounded conversion among many
bounded ones, in a fallback nobody expects to be reached.

WHY NO BUILD AND NO SANITIZER CATCHES IT

-Wall -Wformat has nothing to say about a missing scanf field width -- the
format is well-formed, and the compiler cannot know how large the destination
is. glibc's _FORTIFY_SOURCE does not cover it either. AddressSanitizer does
catch it, but only when an input long enough to overflow actually runs, which
on a target build means never: the host test suite cannot compile this
translation unit without stubbing seven IDF headers and four enum families,
and even then a test only covers the inputs somebody thought to write down.

The property being checked here is purely syntactic -- a conversion either
carries a width or it does not -- so a static check covers the whole class
instead of the inputs that were imagined. That is why this is a check and not
a host test.

WHAT IS AND IS NOT A VIOLATION

Only `s` and `[` write an unbounded run of characters. Everything else is
bounded by the type it converts into:

  * `%d`, `%i`, `%u`, `%x`, `%lg` and friends write one object of known size.
  * `%c` without a width writes exactly one character, which is bounded. With
    a width it writes that many. Neither is a violation.
  * An assignment-suppressed conversion, `%*s` or `%*[^:]`, writes nothing at
    all and takes no argument. The first two conversions of the site above are
    suppressed and are correctly not reported.
  * The GNU `m` modifier, `%ms`, allocates the destination, so no width is
    needed. Not used in this tree; recognised so that adding it later does not
    produce a false report.

A format that is not a string literal is reported separately rather than
skipped. The widths of a format held in a variable cannot be checked from
here, and silently passing such a call would be the one way for the class to
come back. No call in the tree does this today, so the branch costs nothing
until it is needed.

WHY THE CALL HAS TO BE LOCATED FIRST

A check that looked for `%s` without a width in any string literal would
report most of the tree, because that is what printf formats look like.
`EZPI_parse_http_url()` itself contains

    TRACE_S("host = '%s'", host);

eleven lines below the defect. So this walks to each scanf-family call by name
and takes the format from that call's own argument list. SELF_TEST
['decoy_trace_printf'] is that exact line.

WHAT IS SCANNED

The seven first-party directories plus ezlopi-components. The sibling
check_printf_star_int.py stops at the seven, but check_null_deref.py and
check_no_secret_logging.py already reach into the components tree, and the
cost of following them here was measured before deciding: all 107 .c and .h
files under ezlopi-components hold exactly one scanf-family call, cjext.c:546,
and it converts with "%lg" into a double. So the wider list adds coverage of
a vendored tree without adding anything that has to be suppressed.

Eleven calls in total, ten of them in the two files that parse a URL and an
HTTP status line.
"""
import os
import re
import sys

SEARCH_DIRS = ('ezlopi-sensors-devices', 'ezlopi-core', 'ezlopi-cloud',
               'ezlopi-hal', 'ezlopi-services', 'ezlopi-util', 'ezlopi-main',
               'ezlopi-components')

# The scanf family. \b before the name keeps a wrapper called my_sscanf() from
# matching, since an underscore is a word character.
SCANF_CALL = re.compile(r'\b(v?s?scanf|v?fscanf)\s*\(')

# Characters that may appear between "%" and the conversion, after the optional
# suppression flag and width. "m" is the GNU allocating modifier.
LENGTH_MODIFIERS = 'hlLqjztm'

SELF_TEST = {
    # The defect, verbatim, as it stood before the fix at
    # ezlopi_core_scenes_then_methods_helper_func.c:195. The comment the fix
    # left behind moved the arm itself down to :227.
    'bad_the_site':
        'else if (sscanf(field_value_string, "%*[^:]%*[:/]%[^:]:%d%s", '
        'host, &port, page) == 3)',
    # Each half of it on its own, so a parser that only noticed one still fails.
    'bad_scanset_only': 'sscanf(src, "%[^:]", host);',
    'bad_bare_s': 'sscanf(src, "%s", buf);',
    # A leading "]" inside a scanset is a literal "]", not the end of the set.
    # A parser that stopped at the first "]" would read the conversion as
    # finished and miss that there is no width.
    'bad_scanset_literal_bracket': 'sscanf(src, "%[]]", buf);',
    'bad_scanset_negated_bracket': 'sscanf(src, "%[^]]", buf);',
    # One unbounded conversion among bounded ones is the whole point.
    'bad_one_among_bounded':
        'sscanf(src, "http://%99[^:]:%i/%s", host, &port, page);',
    # Wrapped across lines, because calls in this tree are.
    'bad_wrapped': 'if (sscanf(field_value_string,\n'
                   '           "%*[^:]%*[:/]%[^:]:%d%s",\n'
                   '           host, &port, page) == 3)',
    # Adjacent literals concatenate into one format string.
    'bad_concat': 'sscanf(src, "http://" "%[^:]", host);',
    # The other members of the family, where the format is not argument 0.
    'bad_fscanf': 'fscanf(fp, "%s", buf);',
    'bad_plain_scanf': 'scanf("%s", buf);',
    # A format that cannot be read from here. Reported, not skipped.
    'bad_nonliteral_format': 'sscanf(src, fmt, host);',

    # The fix.
    'good_the_fix':
        'else if (sscanf(field_value_string, "%*[^:]%*[:/]%99[^:]:%d%199s", '
        'host, &port, page) == 3)',
    # The eight arms above it, which were already correct and must stay quiet.
    'good_existing_arms':
        'if (sscanf(field_value_string, "http://%99[^:]:%i/%199[^\\n]", '
        'host, &port, page) == 3)\n'
        'else if (sscanf(field_value_string, "http://%99[^:]:%i[^\\n]", '
        'host, &port) == 2)\n'
        'else if (sscanf(field_value_string, "http://%99[^/]/%199[^\\n]", '
        'host, page) == 2)\n'
        'else if (sscanf(field_value_string, "https://%99[^\\n]", host) == 1)',
    # ezlopi_cloud_scenes.c:598 and cjext.c:546, the tree's other two sites.
    'good_response_parse':
        'if (sscanf(tmp_http_data->response, "HTTP/1.1 %d %99s[^\\n]", '
        '&code, detail) == 2)',
    'good_number_probe':
        'if ((sscanf((char *)number_buffer, "%lg", &test) != 1))',
    # Suppressed conversions write nothing, so they need no width. These are
    # the first two conversions of the defect site, which must not be the
    # reason it is reported.
    'good_suppressed': 'sscanf(src, "%*[^:]%*[:/]%*s", NULL);',
    # %c writes one character without a width, and n with a width.
    'good_char': 'sscanf(src, "%c %4c", &one, four);',
    # The GNU allocating modifier needs no width.
    'good_alloc_modifier': 'sscanf(src, "%ms", &heap_dest);',

    # NOT a violation: a printf-family format, which is what most unbounded
    # "%s" in this tree are. This is the line eleven below the defect site.
    'decoy_trace_printf': 'TRACE_S("host = \'%s\'", host);\n'
                          'TRACE_D("*source = \'%s\'", field_value_string);\n'
                          'snprintf(dest, n, "%s", src);',
    # NOT a violation: a commented-out call, the decoy that tripped the first
    # version of check_trace_format_attr.
    'decoy_commented': '// sscanf(src, "%s", buf);\n'
                       '/* sscanf(src, "%[^:]", host); */',
    # NOT a violation: prose about the conversion. This docstring and the
    # CHANGELOG both discuss "%s" beside the word sscanf.
    'decoy_prose': '/* the last arm used %s with no width, so sscanf wrote '
                   'as far as the input reached */',
    # NOT a call: a wrapper whose name merely ends in scanf.
    'decoy_wrapper_name': 'int n = my_sscanf(src, "%s", buf);',
    # "%%" is a literal percent and consumes nothing.
    'decoy_percent_percent': 'sscanf(src, "100%% %99s", buf);',
    # A "//" inside a string literal is not a comment. If comment stripping is
    # not literal-aware it eats the rest of the line and the violation below
    # disappears -- so bad_after_url_literal must still be reported.
    'bad_after_url_literal':
        'esp_http_client_config_t c = { .url = "http://example.invalid/a" };\n'
        'sscanf(src, "%[^:]", host);',
}


def strip_comments(text):
    """Remove comments, skipping over string and character literals.

    Literal-aware on purpose: the tree has URLs containing "//", and a
    stripper that treats those as comments deletes real code. Newlines are
    preserved so reported line numbers stay correct.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if text.startswith('/*', i):
            end = text.find('*/', i + 2)
            if end < 0:
                end = n
            out.append(''.join(c for c in text[i:end] if c == '\n'))
            i = end + 2
        elif text.startswith('//', i):
            end = text.find('\n', i)
            i = n if end < 0 else end
        elif ch in '"\'':
            start = i
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == ch:
                    i += 1
                    break
                if text[i] == '\n':      # unterminated; do not run away
                    break
                i += 1
            out.append(text[start:i])
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def read_literal(text, i):
    """Read the string literal starting at text[i] == '"'.

    Returns (contents_with_escapes_intact, index_after). Adjacent literals are
    concatenated, since that is one format string to the compiler.
    """
    n = len(text)
    parts = []
    while i < n and text[i] == '"':
        i += 1
        start = i
        while i < n:
            if text[i] == '\\':
                i += 2
                continue
            if text[i] == '"':
                break
            i += 1
        parts.append(text[start:i])
        i += 1
        j = i
        while j < n and text[j] in ' \t\r\n':
            j += 1
        if j < n and text[j] == '"':
            i = j
        else:
            break
    return ''.join(parts), i


def call_format(text, i):
    """Find the format string of the scanf call whose "(" is at text[i].

    The format is the first argument that is a string literal: argument 0 for
    scanf(), argument 1 for sscanf() and fscanf(). Taking "the first literal
    argument" rather than a per-function position handles all of them without
    having to know which function this is.

    Returns (format_or_None, found_a_literal). found_a_literal is False when
    the argument list holds no literal at all, which means the format is a
    variable and the widths cannot be checked from here.
    """
    n = len(text)
    i += 1                               # past the "("
    depth = 0
    while i < n:
        ch = text[i]
        if ch == '"' and depth == 0:
            return read_literal(text, i)[0], True
        if ch == "'":                    # char literal: may hold a quote
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == "'":
                    break
                i += 1
            i += 1
            continue
        if ch == '"':                    # a literal nested in a sub-expression
            _, i = read_literal(text, i)
            continue
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            if ch == ')' and depth == 0:
                break
            depth -= 1
        i += 1
    return None, False


def unbounded_writes(fmt):
    """Conversions in a scanf format that write a string with no field width.

    Returns [(conversion_text, conversion_char)]. A conversion is reported when
    it is `s` or `[`, is not assignment-suppressed, carries no width, and does
    not use the GNU allocating modifier.
    """
    found = []
    i = 0
    n = len(fmt)
    while i < n:
        if fmt[i] != '%':
            i += 1
            continue
        start = i
        i += 1
        if i < n and fmt[i] == '%':      # "%%" is a literal percent
            i += 1
            continue
        suppressed = False
        if i < n and fmt[i] == '*':
            suppressed = True
            i += 1
        width = ''
        while i < n and fmt[i].isdigit():
            width += fmt[i]
            i += 1
        allocating = False
        while i < n and fmt[i] in LENGTH_MODIFIERS:
            if fmt[i] == 'm':
                allocating = True
            i += 1
        if i >= n:
            break
        conv = fmt[i]
        i += 1
        if conv == '[':
            # A "^" may come first, and then a "]" in the first position is a
            # literal "]" rather than the end of the set.
            if i < n and fmt[i] == '^':
                i += 1
            if i < n and fmt[i] == ']':
                i += 1
            while i < n and fmt[i] != ']':
                i += 1
            i += 1                       # past the closing "]"
        if conv in ('s', '[') and not suppressed and not width \
                and not allocating:
            found.append((fmt[start:i], conv))
    return found


def check_text(text):
    """Return [(line, call, detail)] for every unbounded scanf write."""
    stripped = strip_comments(text)
    problems = []
    for match in SCANF_CALL.finditer(stripped):
        name = match.group(1)
        paren = match.end() - 1
        line = stripped.count('\n', 0, paren) + 1
        fmt, is_literal = call_format(stripped, paren)
        if not is_literal:
            problems.append((line, name, 'format is not a string literal, so '
                                         'its field widths cannot be checked '
                                         'here'))
            continue
        for conversion, conv in unbounded_writes(fmt):
            problems.append((line, name,
                             '%s writes an unbounded %s' %
                             (conversion,
                              'character run' if conv == '[' else 'string')))
    return problems


def self_test():
    for name, body in sorted(SELF_TEST.items()):
        found = check_text(body)
        if name.startswith('bad_') and not found:
            print('SELF-TEST FAILED: %s was accepted. The check is not '
                  'measuring anything.' % name)
            return False
        if not name.startswith('bad_') and found:
            print('SELF-TEST FAILED: %s was reported as a violation: %s'
                  % (name, found))
            return False

    # The defect site must be reported for both of its conversions, not just
    # whichever one the walk reached first.
    site = unbounded_writes('%*[^:]%*[:/]%[^:]:%d%s')
    if site != [('%[^:]', '['), ('%s', 's')]:
        print('SELF-TEST FAILED: the defect format came out as %s; expected '
              'the bare scanset and the bare %%s, and nothing else.' % site)
        return False
    # The suppressed conversions ahead of them must not be among those two.
    if unbounded_writes('%*[^:]%*[:/]%99[^:]:%d%199s'):
        print('SELF-TEST FAILED: the fixed format still reports something.')
        return False
    return True


def sources():
    for root_dir in SEARCH_DIRS:
        if not os.path.isdir(root_dir):
            continue
        for here, _dirs, files in os.walk(root_dir):
            if 'build' in here.split(os.sep):
                continue
            for name in files:
                if name.endswith(('.c', '.h')):
                    yield os.path.join(here, name)


def main():
    if not self_test():
        return 1

    scanned = 0
    calls = 0
    problems = []
    for path in sorted(sources()):
        scanned += 1
        with open(path, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
        if 'scanf' not in text:
            continue
        calls += len(SCANF_CALL.findall(strip_comments(text)))
        for line, name, detail in check_text(text):
            problems.append('%s:%d: %s(): %s' % (path, line, name, detail))

    if problems:
        print('scanf conversion writing a string with no field width:\n')
        for item in problems:
            print('  ' + item)
        print('\nGive the conversion a field width one smaller than the '
              'destination buffer, as every other arm of the same ladder '
              'already does. "%s" and "%[...]" write as far as the input '
              'reaches; no compiler warning and no target build reports it. '
              'See the docstring at the top of this file.')
        return 1

    print('files scanned: %d; %d scanf-family calls, every string conversion '
          'either width-bounded or assignment-suppressed' % (scanned, calls))
    return 0


if __name__ == '__main__':
    sys.exit(main())
