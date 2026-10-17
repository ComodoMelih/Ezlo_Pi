#!/usr/bin/env python3
"""Find a pointer dereferenced above the NULL test written to guard it.

WHY THIS EXISTS ALONGSIDE check_null_deref.py

That check runs cppcheck, and 34 defects of this class shipped before it was
written. But its recall is 1 of 2 on the two known check-after-use defects in
this branch: it finds the `isprint(ret[0])` in factory_info and misses the
`strlen()`s in `ezpi_provisioning_info_read_func()`, even at
`--enable=all --check-level=exhaustive`, with or without the real build's 244
include paths. The missed shape is a guard sitting in a *sibling* block of the
one holding the dereference. That is a structural blind spot, not a tuning
problem, so this scanner exists to cover it. Recall here is 2 of 2, and both
post-fix versions of those files are silent.

It is a line-oriented scanner, not a parser. It is therefore wrong in both
directions on code it does not understand, and the measured cost of that is
recorded below rather than left to be rediscovered.

PRECISION ANCHOR

A finding requires all three of: the pointer is assigned from a call inside
the function (so its lifetime starts here and NULL is a possible value), it is
dereferenced, and *the author's own NULL test appears below that dereference*.
The third is what keeps this from being a general unchecked-pointer scan -- the
later guard is evidence the author believed the pointer could be NULL. Where
there is no later test, nothing is reported however unchecked the use, which
is a deliberate limit on scope and not an oversight.

WHAT IT FOUND

One live defect, in `EZPI_core_modes_init()`:
`sg_custom_modes = EZPI_core_modes_cjson_parse_modes(...)` was dereferenced on
the next line, and that function returns NULL when its own allocation fails
(single `return parsed_mode;`, everything else inside `if (parsed_mode)`).
Worse, `_is_custom_mode_ok = 1` was set *before* the call, so the
default-mode fallback written for exactly that case was skipped. Fixed with
this check.

WHAT IT IS WRONG ABOUT, MEASURED ON THIS TREE

The first tree run produced 41 findings. Every one was read. 40 were false,
in five classes, each fixed here and pinned by a fixture:

  * a bare assignment with no declaration had a prefix of its own name eaten
    by the optional type group in ASSIGN_CALL, which required no separator
    before the name. The group is greedy, so it took everything it could and
    left the remainder: measured on the provisioning defect's own line,
    `g_provisioning_info_base64 = f();` registered as an assignment to `e64`,
    and every later dereference of the real name was then invisible. This is
    why an earlier version of this scanner missed the one defect it was
    written to catch, while finding the other: that one is
    `char *ret = __EZPI_core_factory_info_v3_read_string(...)`, a declaration,
    where the type group has a real type to match and leaves the name alone.
  * a short-circuit guard: `if (p && strlen(p))` is correct, and so is
    `if ((NULL == p) || strcmp(p, q))`. Which polarity guards depends on the
    operator, and the order within the condition decides it, since
    `if (strlen(p) && p)` is a real defect.
  * the same guard written in a ternary or a call argument rather than an `if`.
  * a dereferencing call's argument list read past its closing paren, so
    `strlen(buf), now` looked like `now` being passed to strlen.
  * an assertion macro -- `PING_CHECK(ep, ...)` expanding to
    `if (!(ep)) goto err;` -- which is an early-exit guard written as a macro.
    ping_sock.c alone gave 26 findings, all of them pointers checked on the
    line after they were assigned. Such macros are now found by their shape
    rather than by name.

One class remains, and is suppressed inline rather than solved: a guard
enforced through data flow instead of a NULL test. In the wifi scan handler
`found` is set to 0 in the `NULL == records` branch, so the
`for (i = 0; i < found; i++)` loop that dereferences `records` cannot iterate.
No line-based scanner will see that. `check-after-use: <name> <reason>` in a
comment vouches for one name within one function body, and the reason is
required -- a bare marker does not suppress, which is pinned by a fixture.
The cost of that escape hatch is real: it would also hide a genuine defect
introduced later in the same function on the same name.
"""
import os
import re
import shutil
import sys
import tempfile

DIRS = [
    'ezlopi-core', 'ezlopi-hal', 'ezlopi-services', 'ezlopi-cloud',
    'ezlopi-util', 'ezlopi-user-config', 'ezlopi-main',
]

# Library functions that dereference the pointer argument named here. Position
# matters for a couple, but a conservative "any argument" rule is used: being
# wrong in this direction costs a false positive, which the later-guard anchor
# then almost always removes.
DEREFERENCING = (
    'strlen|strnlen|strcpy|strncpy|strcat|strncat|strcmp|strncmp|strcasecmp|'
    'strncasecmp|strstr|strchr|strrchr|strtok|strdup|strndup|atoi|atol|atof|'
    'strtol|strtoul|strtod|sscanf|puts|cJSON_Parse|cJSON_ParseWithRef|'
    'cJSON_ParseWithOpts'
)

# The optional leading group is a type, as in `char *p = f();`. It must be
# separated from the name by whitespace or a `*`: without that requirement it
# matches a prefix of the name itself on a bare assignment, so
# `g_prov = f();` registers as an assignment to `rov` and every later
# dereference of `g_prov` is invisible. That is not hypothetical -- it is why
# an earlier version of this file missed the provisioning defect below.
ASSIGN_CALL = re.compile(
    r'(?:^|[;{}\s(])(?:[A-Za-z_][A-Za-z0-9_]*[\s*]+\**\s*)?'
    r'(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*[^=][^;]*?'
    r'[A-Za-z_][A-Za-z0-9_]*\s*\('
)

NULL_TEST = [
    re.compile(r'^\s*(?:NULL|0)\s*(?:!=|==)\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*$'),
    re.compile(r'^\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:!=|==)\s*(?:NULL|0)\s*$'),
    re.compile(r'^\s*!?\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*$'),
]


def strip_comments(text):
    """Blank out comments and string bodies, preserving line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                out.append(' ')
                i += 1
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            while i < n and not (text[i] == '*' and i + 1 < n and text[i + 1] == '/'):
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            out.append('  ')
            i += 2
        elif c in '"\'':
            quote = c
            out.append(c)
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\' and i + 1 < n:
                    out.append('  ')
                    i += 2
                    continue
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            if i < n:
                out.append(quote)
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def _strip_parens(s):
    """Remove layers of genuinely balanced surrounding parentheses."""
    s = s.strip()
    while len(s) > 1 and s[0] == '(' and s[-1] == ')':
        depth = 0
        for i, ch in enumerate(s):
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0 and i != len(s) - 1:
                    return s           # the opening paren closed early
        if depth != 0:
            return s
        s = s[1:-1].strip()
    return s


def _innermost_operand(s):
    """Text after the last unmatched '(' -- the operand being built.

    `TRACE_D("x", (p && p->v) ? ...` splits on && into an operand of
    `TRACE_D("x", (p `, whose actual test is just `p`. Without this, a
    short-circuit guard written inside a call argument or a ternary reads as
    no guard at all.
    """
    opens = []
    for i, ch in enumerate(s):
        if ch == '(':
            opens.append(i)
        elif ch == ')' and opens:
            opens.pop()
    return s[opens[-1] + 1:] if opens else s


def bare_null_test(part):
    """(name, negated) if `part` is exactly a NULL test of one identifier."""
    for cand in (_strip_parens(part), _innermost_operand(part).strip()):
        for pattern in NULL_TEST:
            m = pattern.match(cand)
            if m:
                name = m.group('name')
                if name not in ('NULL', 'true', 'false', 'if', 'while'):
                    negated = cand.startswith('!') or '==' in cand
                    return name, negated
    return None, False


def null_tested_name(cond):
    """The name this condition proves non-NULL for the block it opens.

    Only `&&` conjuncts count. `if (A || B) { ... }` proves nothing inside
    the block, so a NULL test joined by `||` must not register as a guard
    here -- that is a different question from whether a test exists at all,
    which is any_null_test_names below.
    """
    for part in cond.split('&&'):
        name, negated = bare_null_test(part)
        if name:
            return name, negated
    return None, False


def any_null_test_names(cond):
    """Every identifier NULL-tested anywhere in `cond`, either polarity.

    This is the precision anchor only: evidence that the author believed the
    pointer could be NULL. It splits on `||` as well as `&&`, because
    `if ((0 != strcmp(s, k)) || (NULL == s))` dereferences above its own test
    just as surely as the `&&` form does, and returns every name rather than
    the first so that a condition testing two pointers reports both.
    """
    names = set()
    for part in SPLIT_LOGICAL.split(cond):
        if part in ('&&', '||'):
            continue
        name, _ = bare_null_test(part)
        if name:
            names.add(name)
    return names


def deref_names(line, candidates):
    """Names in `candidates` dereferenced on this line."""
    hits = set()
    for name in candidates:
        esc = re.escape(name)
        if re.search(r'\b%s\s*(?:\[|->)' % esc, line):
            hits.add(name)
            continue
        if re.search(r'(?:[(=,;{!&|+\-*/%%]|\breturn\b)\s*\*\s*%s\b' % esc, line):
            hits.add(name)
            continue
        # `[^;)]*` and not `[^;]*`: the name has to be inside this call's own
        # parentheses. Allowing the match to run past the closing paren made
        # `strlen(buf), now` read as though `now` were an argument to strlen.
        if re.search(r'\b(?:%s)\s*\([^;)]*\b%s\b' % (DEREFERENCING, esc), line):
            hits.add(name)
    return hits


def guard_macros(text):
    """Names of macros defined here that early-exit when their first argument
    is false.

    `PING_CHECK(ep, "no memory", err, ESP_ERR_NO_MEM)` expands to
    `if (!(ep)) { ...; goto err; }`, which is the same early-exit guard the
    line scanner already understands when it is written out longhand. Without
    recognising it, ping_sock.c alone produced 26 findings against correctly
    guarded code -- every one of them a pointer checked on the line after it
    was assigned.

    The shape is found rather than hardcoded: a macro whose body tests its own
    first parameter with `!` and then leaves via goto/return/break/continue.
    """
    names = set()
    for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+'
                         r'(?P<name>[A-Za-z_][A-Za-z0-9_]*)\('
                         r'(?P<args>[^)]*)\)', text, re.M):
        args = [a.strip() for a in m.group('args').split(',')]
        if not args or not re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', args[0]):
            continue
        # the macro body runs to the first line not ending in a backslash
        body, i = [], m.end()
        while i < len(text):
            j = text.find('\n', i)
            if j < 0:
                j = len(text)
            seg = text[i:j]
            body.append(seg)
            if not seg.rstrip().endswith('\\'):
                break
            i = j + 1
        body = ' '.join(body)
        first = re.escape(args[0])
        if re.search(r'!\s*\(?\s*%s\b' % first, body) and \
                re.search(r'\b(?:goto|return|break|continue)\b', body):
            names.add(m.group('name'))
    return names


def deref_offset(text, name):
    """Offset of the earliest dereference of `name` in `text`, or None."""
    esc = re.escape(name)
    best = None
    for pat in (r'\b%s\s*(?:\[|->)' % esc,
                r'(?:[(=,;{!&|+\-*/%%]|\breturn\b)\s*\*\s*%s\b' % esc,
                r'\b(?:%s)\s*\([^;)]*\b%s\b' % (DEREFERENCING, esc)):
        m = re.search(pat, text)
        if m and (best is None or m.start() < best):
            best = m.start()
    return best


SPLIT_LOGICAL = re.compile(r'(&&|\|\|)')

# An inline vouch, for a pointer whose guard this scanner cannot see -- a
# count zeroed in the NULL branch, say, so the loop that dereferences it
# cannot run. Scoped to one name in one function body, and it must carry a
# reason, so it cannot be used as a blanket silencer.
SUPPRESS = re.compile(
    r'check-after-use:\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)(?P<why>.*)')


def suppression_name(raw_line):
    """The name an inline `check-after-use:` comment vouches for, if any.

    The reason is mandatory, and the comment's own terminator does not count
    as one: `/* check-after-use: p */` left `*/` sitting where the reason
    should be and suppressed silently, so the punctuation is stripped before
    deciding and real words are required.
    """
    m = SUPPRESS.search(raw_line)
    if not m:
        return None
    why = m.group('why').replace('*/', ' ')
    if len(re.findall(r'[A-Za-z]', why)) < 3:
        return None
    return m.group('name')


def shortcircuit_guarded(expr, name):
    """True if a NULL test short-circuits every dereference of `name`.

    `if (p && strlen(p))` and `if ((NULL == p) || strcmp(p, q))` are both
    correct: the operator stops evaluating before the dereference when p is
    NULL. Which polarity guards depends on the operator -- a positive test
    guards across `&&`, a negative one across `||` -- and order decides it,
    since `if (strlen(p) && p)` is the real defect and must still be
    reported. So this walks the operands left to right and compares offsets
    rather than noting that a test and a dereference share a line.
    """
    toks = SPLIT_LOGICAL.split(expr)
    guard_at, off = None, 0
    for i, tok in enumerate(toks):
        if i % 2 == 0 and guard_at is None:
            nm, negated = bare_null_test(tok)
            if nm == name:
                nxt = toks[i + 1] if i + 1 < len(toks) else None
                if (nxt == '&&' and not negated) or (nxt == '||' and negated):
                    guard_at = off + len(tok)
        off += len(tok)
    if guard_at is None:
        return False
    d = deref_offset(expr, name)
    return d is not None and d > guard_at


def scan_file(path):
    source = open(path, encoding='utf-8', errors='replace').read()
    # Suppressions are read from the source before comments are blanked.
    raw_lines = source.split('\n')
    text = strip_comments(source)
    lines = text.split('\n')
    gmacros = guard_macros(text)
    gmacro_call = re.compile(
        r'\b(?:%s)\s*\(\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*[,)]'
        % '|'.join(re.escape(n) for n in gmacros)) if gmacros else None

    findings = []
    depth = 0
    # guard_stack[d] = set of names proven non-NULL for the block at depth d
    guard_stack = [set()]
    pending_guard = None     # (names, negated) seen on an `if` awaiting its `{`
    # per function body: name -> (assign_line, assign_depth)
    assigned = {}
    # per function body: name -> list of (line_no, line_text) unguarded derefs
    suspect = {}
    # per function body: names an inline comment vouches for
    suppressed = set()
    func_start_depth = None

    for idx, raw in enumerate(lines, start=1):
        line = raw

        if depth == 0:
            assigned, suspect, suppressed = {}, {}, set()

        sname = suppression_name(
            raw_lines[idx - 1] if idx <= len(raw_lines) else '')
        if sname:
            suppressed.add(sname)

        # An assertion macro taking the pointer as its first argument is an
        # early-exit guard, exactly like `if (!p) return;`.
        if gmacro_call is not None:
            for gm in gmacro_call.finditer(line):
                guard_stack[-1].add(gm.group('name'))

        m = re.search(r'\bif\s*\((?P<cond>.*)\)\s*(?P<tail>.*)$', line)
        if m:
            name, negated = null_tested_name(m.group('cond'))
            if name:
                tail = m.group('tail').strip()
                if negated and re.match(r'^\{?\s*$', tail) is None and \
                        re.search(r'\b(?:return|continue|break|goto)\b', tail):
                    # if (!p) return;  -- p is non-NULL from here on
                    guard_stack[-1].add(name)
                    pending_guard = None
                else:
                    pending_guard = (name, negated)

        opens = line.count('{')
        closes = line.count('}')

        # Dereferences on this line are evaluated in the scope as it stands
        # before any brace on the line changes it.
        guarded = set()
        for s in guard_stack:
            guarded |= s
        # A short-circuit guard can sit in an `if` condition, but equally in a
        # ternary or a call argument, so the bare line is tried as well.
        exprs = [line]
        if m:
            exprs.append(m.group('cond'))
        for name in deref_names(line, set(assigned) - guarded - suppressed):
            if any(shortcircuit_guarded(e, name) for e in exprs):
                continue
            suspect.setdefault(name, []).append((idx, line.strip()))

        # A NULL test below a suspect dereference is the finding.
        if m:
            for name in sorted(any_null_test_names(m.group('cond'))):
                if name in suspect:
                    for dline, dtext in suspect[name]:
                        findings.append((dline, name, dtext, idx,
                                         assigned[name][0]))
                    suspect.pop(name, None)

        for mm in ASSIGN_CALL.finditer(line):
            name = mm.group('name')
            if name in ('if', 'while', 'for', 'return', 'switch', 'sizeof'):
                continue
            assigned[name] = (idx, depth)
            suspect.pop(name, None)
            for s in guard_stack:
                s.discard(name)

        for _ in range(opens):
            depth += 1
            if pending_guard and not pending_guard[1]:
                guard_stack.append({pending_guard[0]})
            else:
                guard_stack.append(set())
            pending_guard = None
        for _ in range(closes):
            if depth > 0:
                depth -= 1
            if len(guard_stack) > 1:
                guard_stack.pop()
        if opens == 0 and closes == 0 and pending_guard and ';' in line:
            pending_guard = None

    return findings


# Fixtures. The must-fire half proves the check can fail; the must-be-silent
# half proves it does not fire on correct code. Each silent case is paired
# with the same shape written in the order that IS a defect, so a suppression
# broad enough to kill the noise is caught killing the signal too.
FIXTURES = [
    # (name, must_fire, source)
    ('sibling-block guard', True, '''\
static char *g_p;
void f(int value)
{
    if (value)
    {
        if (NULL == g_p)
        {
            g_p = get_it();
            n = strlen(g_p) / 4;
        }

        if (NULL != g_p)
        {
            use(g_p);
        }
    }
}
'''),
    ('bare assignment, no declaration', True, '''\
static char *g_p;
void f(void)
{
    g_p = get_it();
    n = strlen(g_p) / 4;
    if (NULL != g_p)
    {
        use(g_p);
    }
}
'''),
    ('short-circuit guard', False, '''\
void f(void)
{
    char *s = read_it();
    if (s && strstr(s, "https://"))
    {
        use(s);
    }
}
'''),
    ('short-circuit, reversed', True, '''\
void f(void)
{
    char *s = read_it();
    if (strstr(s, "https://") && s)
    {
        use(s);
    }
}
'''),
    ('parenthesised && guard', False, '''\
void f(void)
{
    char *s = read_it();
    if ((NULL != s) && ('\\0' != s[0]))
    {
        use(s);
    }
}
'''),
    ('parenthesised && reversed', True, '''\
void f(void)
{
    char *s = read_it();
    if (('\\0' != s[0]) && (NULL != s))
    {
        use(s);
    }
}
'''),
    ('negative || guard', False, '''\
void f(void)
{
    char *s = read_it();
    if ((NULL == s) || (0 != strcmp(s, "x")))
    {
        return;
    }
}
'''),
    ('negative || reversed', True, '''\
void f(void)
{
    char *s = read_it();
    if ((0 != strcmp(s, "x")) || (NULL == s))
    {
        return;
    }
}
'''),
    ('ternary guard', False, '''\
void f(void)
{
    cJSON *u = get_it();
    log("%s", (u && u->valuestring) ? u->valuestring : "null");
    if (u)
    {
        use(u);
    }
}
'''),
    ('ternary reversed', True, '''\
void f(void)
{
    cJSON *u = get_it();
    log("%s", (u->valuestring && u) ? u->valuestring : "null");
    if (u)
    {
        use(u);
    }
}
'''),
    ('assertion macro guard', False, '''\
#define PING_CHECK(a, str, goto_tag, ret_value, ...) \\
    do {                                             \\
        if (!(a)) {                                  \\
            ret = ret_value;                         \\
            goto goto_tag;                           \\
        }                                            \\
    } while (0)

esp_err_t f(void)
{
    esp_ping_t *ep = NULL;
    ep = calloc(1, sizeof(esp_ping_t));
    PING_CHECK(ep, "no memory", err, ESP_ERR_NO_MEM);
    ep->flags |= PING_FLAGS_INIT;
    return ESP_OK;
err:
    if (ep)
    {
        free(ep);
    }
    return ret;
}
'''),
    # The same macro shape without the early exit is not a guard, so the
    # defect after it must still be reported.
    ('macro that does not exit', True, '''\
#define LOG_IF_NULL(a, str) \\
    do {                    \\
        if (!(a)) {         \\
            TRACE_E(str);   \\
        }                   \\
    } while (0)

void f(void)
{
    char *p = NULL;
    p = get_it();
    LOG_IF_NULL(p, "null");
    n = strlen(p);
    if (p)
    {
        use(p);
    }
}
'''),
    ('later argument, not the call\'s', False, '''\
void f(void)
{
    time_t now = get_time();
    char *buf = get_buf();
    if (0 == upcall(buf, strlen(buf), now))
    {
        done();
    }
    if (0 == now)
    {
        again();
    }
}
'''),
    ('no later guard at all', False, '''\
static char *g_p;
void f(void)
{
    g_p = get_it();
    if (NULL != g_p)
    {
        n = strlen(g_p) / 4;
    }
}
'''),
    ('suppression with a reason', False, '''\
static char *g_p;
void f(void)
{
    /* check-after-use: g_p the caller has already checked it */
    g_p = get_it();
    n = strlen(g_p) / 4;
    if (NULL != g_p)
    {
        use(g_p);
    }
}
'''),
    # A marker with no reason must not suppress, or the escape hatch becomes
    # a blanket silencer.
    ('suppression with no reason', True, '''\
static char *g_p;
void f(void)
{
    /* check-after-use: g_p */
    g_p = get_it();
    n = strlen(g_p) / 4;
    if (NULL != g_p)
    {
        use(g_p);
    }
}
'''),
]


def self_test():
    """Run every fixture before trusting a tree scan."""
    work = tempfile.mkdtemp(prefix='checkafteruse_')
    ok = True
    try:
        for name, must_fire, source in FIXTURES:
            path = os.path.join(work, re.sub(r'\W+', '_', name) + '.c')
            with open(path, 'w', encoding='utf-8') as handle:
                handle.write(source)
            hits = scan_file(path)
            if must_fire and not hits:
                print('SELF-TEST FAILED: the "%s" fixture produced no '
                      'finding, so this check cannot fail on that shape.'
                      % name)
                ok = False
            elif not must_fire and hits:
                print('SELF-TEST FAILED: the "%s" fixture was flagged, so '
                      'this check fires on correct code:' % name)
                for dline, nm, dtext, gline, aline in hits:
                    print('    :%d `%s` -- %s' % (dline, nm, dtext[:90]))
                ok = False
            else:
                print('  self-test %-34s %s'
                      % (name, 'flagged' if hits else 'clean'))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return ok


def main():
    targets = sys.argv[1:]
    if not targets:
        if not self_test():
            return 1
        missing = [d for d in DIRS if not os.path.isdir(d)]
        if missing:
            print('not found: %s\nRun this from the repository root.'
                  % ', '.join(missing))
            return 1
        targets = DIRS

    files, total = [], 0
    for t in targets:
        if os.path.isfile(t):
            files.append(t)
        else:
            for dirpath, _, names in os.walk(t):
                for n in names:
                    if n.endswith('.c'):
                        files.append(os.path.join(dirpath, n))
    rc = 0
    for path in sorted(files):
        total += 1
        for dline, name, dtext, gline, aline in scan_file(path):
            rc = 1
            print('%s:%d: `%s` dereferenced here, assigned at :%d, '
                  'NULL-tested at :%d' % (path, dline, name, aline, gline))
            print('    %s' % dtext[:150])
    print('\nfiles scanned: %d' % total)
    if rc:
        print('\nA pointer is dereferenced above the NULL test written for '
              'it. Either move the dereference below the guard, or -- if a\n'
              'guard this scanner cannot see is doing the work -- vouch for '
              'it with a `check-after-use: <name> <reason>` comment.\n'
              'See the docstring at the top of this file for the five shapes '
              'that are known to be false and already handled.')
    return rc


if __name__ == '__main__':
    sys.exit(main())
