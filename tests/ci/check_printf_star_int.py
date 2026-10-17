#!/usr/bin/env python3
"""Fail if a printf "*" width or precision is fed a size_t.

THE BUG THIS EXISTS TO PREVENT

A "*" in a conversion specification reads one argument of type int -- not
size_t, not unsigned long. C11 7.21.6.1p5 says so for both the field width
and the precision. So

    TRACE_D("name: %.*s", sizeof(node->name), node->name);

passes a size_t where an int is read. On a target where size_t is wider than
int, vfprintf pulls four bytes off a va_list that holds eight and every
argument after it is misaligned, which in this tree means the pointer that
was supposed to be printed.

WHY NO BUILD CATCHES THIS

Measured rather than assumed, because the answer is the whole reason this
check exists. A direct two-compiler probe of

    __attribute__((format(printf, 1, 2))) void trc(const char *f, ...);
    size_t n = sizeof(field); trc("%.*s", n, field);

gives, with -Wall -Wformat:

  | compiler                   | sizeof(size_t) | precision complaints |
  | xtensa-esp32-elf-gcc 8.4.0 | == sizeof(int) | 0                    |
  | riscv32-esp-elf-gcc 8.4.0  | == sizeof(int) | 0                    |
  | host gcc (x86-64)          | 8, int is 4    | 2                    |

size_t is int-sized on every ESP target, so -Wformat has nothing to object
to and the whole class is invisible to all four target builds -- including
through TRACE_*, which does carry a printf format attribute, and even under
-Werror=format. Confirmed against the build logs: 0 format warnings on
esp32, esp32s2, esp32s3 and esp32c3.

That is also why these sites are not live defects on hardware today. They
are a portability and host-testability problem: they are well-defined at
32 bits and wrong at 64, which is exactly the configuration a host unit test
runs in. The two sites in ezlopi_core_cjson_macros.c were found by a 64-bit
host build for precisely this reason.

So the enforcement cannot come from the compiler the project actually uses.
It has to come from here.

WHAT IS AND IS NOT A VIOLATION

Only a *size_t* argument is wrong. Narrower unsigned types are fine and
flagging them would be noise:

  * uint16_t and unsigned short promote to int before the call.
  * uint32_t is the same width as int, and C11 6.2.5p9 makes a value
    representable in both types interchange without reinterpretation, so
    passing one where the other is read is well-defined.

That distinction is what the tree actually relies on: ezlopi_core_http.c
passes an int, the BLE services pass a uint32_t copy_size, and
ezlopi_service_ble_provisioning.c passes a uint16_t param->write.len. None
of those are defects and none are reported.

WHY THE ARGUMENT INDEX HAS TO BE COMPUTED

The tempting version of this check -- "a line with %.*s that also mentions
sizeof" -- is wrong, and wrong in a direction that would have made it
useless. ezlopi_service_ble_dynamic_config.c:475 is

    snprintf(data_buffer, sizeof(data_buffer), "%.*s", copy_size, ...);

The sizeof there is snprintf's own size parameter, which is *supposed* to be
a size_t, and the precision argument beside it is correct. A textual check
reports that site and misses nothing in exchange. So this check parses the
format string, counts how many variadic arguments each specifier consumes
(a "*" takes one, "%%" takes none), and looks only at the argument that
actually lands on the star. SELF_TEST['decoy_dest_size'] is that exact line.

LIMITS

Recognition is by spelling: sizeof, strlen and ->str_value_len, the three
ways a size_t is written in this tree. A size_t reached through a plain
member or local is not caught, because knowing its type means knowing the
type of every local in the tree, which is a compiler's job.

That is not hypothetical. ezlopi_service_ws_server.c prints ws_pkt.len,
which is the size_t field of httpd_ws_frame_t; the site was corrected along
with the rest, and stripping the cast back out again does not make this
check complain. Widening the spelling list to catch it is the wrong trade:
".len" is an int in ezlopi_core_http.c and a uint32_t in the BLE services,
both correct, so the check would start reporting working code -- and a
check that reports working code gets switched off.

So a 64-bit host build of the file remains the complete check. This is the
cheap guard that runs on every commit and covers the three spellings that
account for every site found so far.
"""
import os
import re
import sys

SEARCH_DIRS = ('ezlopi-sensors-devices', 'ezlopi-core', 'ezlopi-cloud',
               'ezlopi-hal', 'ezlopi-services', 'ezlopi-util', 'ezlopi-main')

# The three ways a size_t is spelled in this tree.
SIZE_T_SPELLING = re.compile(r'\bsizeof\b|\bstrlen\s*\(|\bstr_value_len\b')

# An argument already corrected. Must be a cast at the front of the argument,
# not an (int) appearing anywhere in it.
INT_CAST = re.compile(r'^\(\s*int\s*\)')

SELF_TEST = {
    # The eight live sites as they actually stood.
    'bad_sizeof': 'TRACE_D("\\t\\t|-- _tempId: %.*s", '
                  'sizeof(action_block->_tempId), action_block->_tempId);',
    'bad_strlen': 'snprintf(curr_expr->code, strlen(code_str) + 1, "%.*s", '
                  'strlen(code_str), code_str);',
    'bad_str_value_len': 'snprintf(ret, cj_item->str_value_len + 1, "%.*s", '
                         'cj_item->str_value_len, cj_item->valuestring);',
    # Wrapped across lines, because several calls in the tree are.
    'bad_wrapped': 'snprintf(expn_node->code,\n'
                   '         cj_code->str_value_len + 1,\n'
                   '         "%.*s",\n'
                   '         cj_code->str_value_len,\n'
                   '         cj_code->valuestring);',
    # Adjacent string literals concatenate into one format string.
    'bad_concat': 'TRACE_D("payload" "[len: %d]: %.*s", n, sizeof(buf), buf);',
    # "%%" consumes no argument, so the precision is argument 0 here. If the
    # parser miscounted, it would look at "x" and report nothing.
    'bad_percent_percent': 'TRACE_D("100%% done %.*s", sizeof(x), x);',
    # "%*d" consumes two arguments, the width and the value, so the precision
    # here is argument 2. Same trap as above in the other direction.
    'bad_star_width': 'TRACE_D("%*d %.*s", w, v, sizeof(x), x);',
    # The width star is read as an int too, not just the precision.
    'bad_star_width_itself': 'TRACE_D("%*s", sizeof(x), x);',
    # Precision applies to any conversion, not only %s.
    'bad_not_s': 'TRACE_D("%.*f", sizeof(x), d);',

    # The fixed forms.
    'good_cast': 'TRACE_D("\\t\\t|-- _tempId: %.*s", '
                 '(int)sizeof(action_block->_tempId), action_block->_tempId);',
    'good_cast_expr': 'snprintf(d, n + 1, "%.*s", (int)(strlen(s) - 1), s);',
    'good_two_stars': 'TRACE_D("payload[len: %d]:\\r\\n%.*s", (int)ws_pkt.len, '
                      '(int)ws_pkt.len, (char *)ws_pkt.payload);',

    # NOT a violation: the sizeof is snprintf's size parameter, which is
    # supposed to be a size_t. The precision beside it is a uint32_t and is
    # fine. This is ezlopi_service_ble_dynamic_config.c:475 verbatim, and it is
    # what a line-level textual check gets wrong.
    'decoy_dest_size': 'snprintf(data_buffer, sizeof(data_buffer), "%.*s", '
                       'copy_size, g_dynamic_config_base64 + (n * 400));',
    # NOT a violation: uint16_t promotes to int, uint32_t interchanges with it.
    'decoy_narrow_unsigned':
        'TRACE_D("GATT_WRITE_EVT value: %.*s", param->write.len, '
        'param->write.value);\n'
        'TRACE_D("Sending: [len = %d]\\r\\n%.*s", copy_size, copy_size, jstr);\n'
        'TRACE_D("%.*s", curr_chunk->len, curr_chunk->ptr);',
    # A commented-out bad call must not be reported, the same decoy that
    # tripped the first version of check_trace_format_attr.
    'decoy_commented': '// TRACE_D("%.*s", sizeof(x), x);\n'
                       '/* snprintf(d, n, "%.*s", strlen(s), s); */\n'
                       'TRACE_D("%.*s", (int)sizeof(x), x);',
    # A "//" inside a string literal is not a comment. If comment stripping is
    # not literal-aware it eats the rest of the line and the real violation
    # below it disappears -- so this one must still be reported.
    'bad_after_url_literal': 'esp_http_client_config_t c = { .url = '
                             '"http://example.invalid/a" };\n'
                             'TRACE_D("%.*s", sizeof(x), x);',
    # "%.*s" inside a comment is prose, not a call. ezlopi_cloud_mqtt_config.c
    # and the host tests both discuss this conversion in comments.
    'decoy_prose': '/* this tree uses the "%.*s" precision form with '
                   'str_value_len for the same reason */',
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
        # Skip whitespace between concatenated literals.
        j = i
        while j < n and text[j] in ' \t\r\n':
            j += 1
        if j < n and text[j] == '"':
            i = j
        else:
            break
    return ''.join(parts), i


def star_slots(fmt):
    """Which variadic argument indices a format string reads as an int.

    Returns [(arg_index, 'width'|'precision', conversion_char)] for every "*"
    in the format. Conversion characters are carried only so the diagnostic can
    name the specifier the reader will be looking at.
    """
    slots = []
    arg = 0
    i = 0
    n = len(fmt)
    while i < n:
        if fmt[i] != '%':
            i += 1
            continue
        i += 1
        if i < n and fmt[i] == '%':      # "%%" consumes nothing
            i += 1
            continue
        pending = []
        while i < n and fmt[i] in "-+ #0'":
            i += 1
        if i < n and fmt[i] == '*':      # field width
            pending.append((arg, 'width'))
            arg += 1
            i += 1
        else:
            while i < n and fmt[i].isdigit():
                i += 1
        if i < n and fmt[i] == '.':
            i += 1
            if i < n and fmt[i] == '*':  # precision
                pending.append((arg, 'precision'))
                arg += 1
                i += 1
            else:
                while i < n and fmt[i].isdigit():
                    i += 1
        while i < n and fmt[i] in 'hlLqjzt':
            i += 1
        conv = fmt[i] if i < n else '?'
        i += 1
        for slot, kind in pending:
            slots.append((slot, kind, conv))
        if conv != 'n':
            arg += 1
    return slots


def split_args(text, i):
    """Split the argument list that follows a format literal.

    Starts just after the format string and walks forward to the ")" that
    closes the enclosing call, so the call does not have to be located first.
    Returns the arguments after the format string, in order.
    """
    args = []
    current = []
    depth = 0
    n = len(text)
    # Skip to the comma after the format string, if there is one.
    while i < n and text[i] in ' \t\r\n':
        i += 1
    if i >= n or text[i] != ',':
        return args
    i += 1
    while i < n:
        ch = text[i]
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            if ch == ')' and depth == 0:
                break
            depth -= 1
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
                i += 1
            current.append(text[start:i])
            continue
        elif ch == ',' and depth == 0:
            args.append(''.join(current))
            current = []
            i += 1
            continue
        current.append(ch)
        i += 1
    tail = ''.join(current).strip()
    if tail:
        args.append(tail)
    return args


def check_text(text):
    """Return [(line, kind, conv, argument)] for stars fed a size_t."""
    stripped = strip_comments(text)
    problems = []
    i = 0
    n = len(stripped)
    while i < n:
        ch = stripped[i]
        if ch == "'":                    # char literal: may hold a quote
            i += 1
            while i < n:
                if stripped[i] == '\\':
                    i += 2
                    continue
                if stripped[i] == "'":
                    break
                i += 1
            i += 1
            continue
        if ch != '"':
            i += 1
            continue
        literal_at = i
        fmt, after = read_literal(stripped, i)
        i = after
        slots = star_slots(fmt)
        if not slots:
            continue
        args = split_args(stripped, after)
        line = stripped.count('\n', 0, literal_at) + 1
        for slot, kind, conv in slots:
            if slot >= len(args):
                continue
            arg = args[slot].strip()
            if INT_CAST.match(arg):
                continue
            if SIZE_T_SPELLING.search(arg):
                problems.append((line, kind, conv, ' '.join(arg.split())))
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
    # The index really is computed, not guessed: in bad_star_width the
    # precision is argument 1, and nothing else would put sizeof there.
    slots = star_slots('%*d %.*s')
    if slots != [(0, 'width', 'd'), (2, 'precision', 's')]:
        print('SELF-TEST FAILED: argument indices for "%%*d %%.*s" came out as '
              '%s; expected the width at 0 and the precision at 2.' % slots)
        return False
    if star_slots('100%% done %.*s') != [(0, 'precision', 's')]:
        print('SELF-TEST FAILED: "%%%%" was counted as consuming an argument.')
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
    problems = []
    for path in sorted(sources()):
        scanned += 1
        with open(path, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
        if '*' not in text:
            continue
        for line, kind, conv, arg in check_text(text):
            problems.append('%s:%d: the %s of %%.*%s is given %s'
                            % (path, line, kind, conv, arg))

    if problems:
        print('printf "*" width or precision fed a size_t:\n')
        for item in problems:
            print('  ' + item)
        print('\nCast the argument to int. A "*" reads an int (C11 '
              '7.21.6.1p5); size_t is int-sized on every ESP target, so no '
              'target build reports this, and it breaks on a 64-bit host '
              'build. See the docstring at the top of this file.')
        return 1

    print('files scanned: %d; no "*" width or precision is given a size_t '
          '(sizeof, strlen or str_value_len)' % scanned)
    return 0


if __name__ == '__main__':
    sys.exit(main())
