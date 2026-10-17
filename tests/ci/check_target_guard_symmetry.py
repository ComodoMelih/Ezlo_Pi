#!/usr/bin/env python3
"""Fail if a target guard leaves a call with no definition to link against.

THE BUG THIS EXISTS TO PREVENT

dht11.c wraps its entire body, readDHT11 included, in

    #if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)

because the bit-banged 1-wire timing loop needs the per-target
<target>/rom/ets_sys.h for ets_delay_us and no esp32c3 arm was ever written.
sensor_0015_oneWire_DHT11.c, which calls readDHT11, carried no guard at all.
So on esp32c3 that file compiled happily into a 190228-byte object carrying an
undefined reference to a function that no object in the build defines --
measured with nm, which reported exactly one undefined dht symbol, "U
readDHT11".

The build did not fail. It cannot: the object sits in a static archive, and the
only thing that would reference it is the EZLOPI_DEVICE_LIST entry for device
15, which is guarded by the same condition in
ezlopi_core_devices_list_modules.h. The linker never pulls the member out, so
the dangling reference is never resolved and never reported. The failure was
armed and waiting for whoever enabled the device on esp32c3 -- and it would
have surfaced as "undefined reference to readDHT11", naming the symbol rather
than the guard three files away that actually caused it.

sensor-0016's DHT22 equivalent guards all three of dht22.c, dht22.h and its
wrapper, so it never had the problem. The two components were written from the
same template and drifted.

WHY A CHECK AND NOT JUST THE ONE GUARD

Because the compiler and the linker both decline to catch this, and they will
go on declining. 24 files in this tree carry target guards and 30 distinct
guard conditions appear among them, including negations and #elif chains. The
next driver that gets an esp32c3 arm, or loses one, re-arms the same trap, and
the build stays green until someone flips an unrelated device macro.

So the invariant is the one the linker would enforce if it could see the whole
program at once:

  1. no line that is active for a target may call a function that no .c file
     defines for that target, when some other target does define it
  2. no header may declare such a function outside the guard that its
     definition is inside

Invariant 2 is why dht11.h is guarded too. Without it the prototype silently
promises every target an implementation that only three have, which is what
turns a compile error into a link error in the first place.

HOW THE CONDITIONS ARE EVALUATED

Not by set algebra over the target names, which cannot express the
"#if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C3)"
and "#elif" forms this tree really uses. Instead each file is walked once per
target in TARGETS with a small evaluator that knows only about
CONFIG_IDF_TARGET_* and integer literals.

Any condition it cannot decide -- "#if (1 == SENSOR_0015_ONEWIRE_DHT11)",
"#ifdef __cplusplus", anything mentioning a macro that is not a target -- is
treated as ACTIVE, deliberately. Guessing a device macro's value would make
this check's answer depend on EZLOPI_USER_CONFIG.h, and a guard that is only
sometimes present is worse than one that is always assumed present. Integer
literals are decided, so the "#if 0" block of dead prototypes in dht22.h does
not count as declaring anything.

LIMITS

Only the target-guard class. A function that is undefined on every target is
not reported, because then the definition is simply somewhere this check
cannot see -- another component, the IDF, a ROM stub -- and reporting it would
be noise. That is also what keeps a function whose definition this file's
regex fails to recognise from being reported: a name must be seen defined for
at least one target before its absence on another counts for anything.

Macro-generated calls are invisible here, and so is anything reached only
through a function pointer stored by a macro; EZLOPI_DEVICE_LIST is exactly
that, which is why the DHT11 reference it makes is not what this check keys
on.

Object-like macros are not expanded either, so a guard written as
"#if EZPI_LED_INDICATOR_SUPPORTED" would be undecidable here and therefore
treated as active, discarding the restriction. That is why the LED indicator's
condition is spelled out in all three of its files instead of being hoisted
into one macro -- the preprocessor is the only thing that needs to agree with
this check, and keeping the condition literal is what lets it.
"""
import os
import re
import sys

# The four targets this project builds, per .github/workflows/ci.yml. A target
# added to the build matrix must be added here too, or guards that exclude it
# go unexamined.
TARGETS = ('esp32', 'esp32s2', 'esp32s3', 'esp32c3')

SEARCH_DIRS = ('ezlopi-sensors-devices', 'ezlopi-core', 'ezlopi-cloud',
               'ezlopi-hal', 'ezlopi-service', 'ezlopi-services',
               'ezlopi-util', 'ezlopi-main', 'ezlopi-user-config')

TARGET_MACRO = re.compile(r'\bCONFIG_IDF_TARGET_([A-Z0-9]+)\b')

# A file-scope function definition: a line starting in column 0, a name, a
# parenthesised list, then a brace rather than a semicolon. Control-flow
# keywords are excluded by name below.
DEFINITION = re.compile(
    r'^(?!\s)(?P<static>static\s+)?(?:[A-Za-z_][\w]*[\w\s\*]*?)\b'
    r'([A-Za-z_]\w*)\s*\(([^;{)]*)\)\s*\{',
    re.M)

# A prototype: something type-shaped, a name, a parenthesised list, then a
# semicolon. The prefix is captured so that statements of the same shape can be
# rejected -- "return readDHT11(&t, &h, 4);" matches this pattern perfectly and
# treating it as a prototype hid the one violation the check exists for.
DECLARATION = re.compile(
    r'^[ \t]*(?P<prefix>[A-Za-z_][\w]*[\w\s\*]*?)\b(?P<name>[A-Za-z_]\w*)'
    r'\s*\((?P<args>[^;{)]*)\)\s*;',
    re.M)

STATEMENT_KEYWORDS = frozenset((
    'return', 'if', 'while', 'for', 'switch', 'else', 'do', 'case', 'goto',
    'sizeof', 'break', 'continue',
))

NOT_A_FUNCTION = frozenset((
    'if', 'else', 'for', 'while', 'switch', 'do', 'return', 'sizeof',
    'defined', 'case', 'typedef', 'struct', 'union', 'enum', 'extern',
    'static', 'inline', 'const', 'volatile', 'void', 'int', 'char', 'long',
    'short', 'float', 'double', 'unsigned', 'signed',
))

CONDITIONAL = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b[ \t]*(.*)$')

SELF_TEST_GUARD = {
    # The real condition, and the real asymmetry it produced.
    'dht11_shape': {
        'header': '''#include "sdkconfig.h"
int readDHT11(float *t, float *h, int pin);
''',
        'impl': '''#include "sdkconfig.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int readDHT11(float *t, float *h, int pin)
{
    return 0;
}
#endif
''',
        'caller': '''#include "dht11.h"
int wrapper(void)
{
    float t, h;
    return readDHT11(&t, &h, 4);
}
''',
        # esp32c3 has no definition; the caller and the declaration are both
        # active there.
        'want_call_flag': True,
        'want_decl_flag': True,
    },
    # The DHT22 shape: everything guarded alike, nothing to report.
    'dht22_shape': {
        'header': '''#include "sdkconfig.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int readDHT22(float *t, float *h, int pin);
#endif
''',
        'impl': '''#include "sdkconfig.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int readDHT22(float *t, float *h, int pin)
{
    return 0;
}
#endif
''',
        'caller': '''#include "dht22.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int wrapper(void)
{
    float t, h;
    return readDHT22(&t, &h, 4);
}
#endif
''',
        'want_call_flag': False,
        'want_decl_flag': False,
    },
    # An #elif chain that covers all four targets in separate arms. The union
    # of the arms leaves nobody out, so neither rule may fire. This is the
    # shape the ADC and PWM HALs use and flagging it would make the check
    # unusable.
    'elif_chain_covers_all': {
        'header': '''#include "sdkconfig.h"
void hal_set(int v);
''',
        'impl': '''#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32)
void hal_set(int v) { (void)v; }
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
void hal_set(int v) { (void)v; }
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
void hal_set(int v) { (void)v; }
#else
void hal_set(int v) { (void)v; }
#endif
''',
        'caller': '''#include "hal.h"
int use(void) { hal_set(1); return 0; }
''',
        'want_call_flag': False,
        'want_decl_flag': False,
    },
    # A negated guard, which set algebra over target names gets wrong. The
    # definition is active on exactly s2 and s3; the caller is unguarded.
    'negated_guard': {
        'header': '''#include "sdkconfig.h"
void only_some(void);
''',
        'impl': '''#include "sdkconfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C3)
void only_some(void) { }
#endif
''',
        'caller': '''#include "h.h"
int use(void) { only_some(); return 0; }
''',
        'want_call_flag': True,
        'want_decl_flag': True,
    },
    # Prototypes inside #if 0 are not declarations of anything. dht22.h really
    # has such a block, and treating it as live would report two functions
    # that are not defined anywhere on any target.
    'if_zero_block': {
        'header': '''#include "sdkconfig.h"
#if 0
void dead_proto(uint32_t pin);
#endif
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int live(void);
#endif
''',
        'impl': '''#include "sdkconfig.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int live(void) { return 0; }
#endif
''',
        'caller': '''#include "h.h"
''',
        'want_call_flag': False,
        'want_decl_flag': False,
    },
    # ezlopi_service_led_indicator.c's real shape: a block of static forward
    # declarations sits outside the #if/#elif chain that holds the
    # definitions. A prototype is not a call and nothing is wrong here. The
    # first version of this check reported ten of these.
    'static_prototypes_are_not_calls': {
        'header': '',
        'impl': '''#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
#define LED_MODE 1
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define LED_MODE 2
#endif

static void power_on_effect();
static int led_init(void);

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
static void power_on_effect() { }
static int led_init(void) { return 0; }
#elif defined(CONFIG_IDF_TARGET_ESP32)
static void power_on_effect() { }
static int led_init(void) { return 0; }
#endif
''',
        'caller': '',
        'want_call_flag': False,
        'want_decl_flag': False,
    },
    # The same shape but with a real call outside the chain. esp32s2 gets no
    # definition from either arm, so this one must fire.
    'static_called_outside_guard': {
        'header': '',
        'impl': '''#include "sdkconfig.h"
static void power_on_effect();

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
static void power_on_effect() { }
#elif defined(CONFIG_IDF_TARGET_ESP32)
static void power_on_effect() { }
#endif

void run(void)
{
    power_on_effect();
}
''',
        'caller': '',
        'want_call_flag': True,
        'want_decl_flag': False,
    },
    # A static of the same name in an unrelated file must not stand in as the
    # definition. impl.c defines it for three targets only; caller.c has its
    # own static for all four. Nothing may be reported against caller.c, and
    # impl.c's own guarded definition is complete, so the whole case is clean.
    'static_does_not_cross_files': {
        'header': '',
        'impl': '''#include "sdkconfig.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
static void helper(void) { }
void impl_entry(void) { helper(); }
#endif
''',
        'caller': '''static void helper(void) { }
void caller_entry(void) { helper(); }
''',
        'want_call_flag': False,
        'want_decl_flag': False,
    },
    # A guard spanning physical lines with backslash continuations, which is
    # how ezlopi_service_led_indicator.c now restricts itself. Reading only
    # the first physical line leaves a condition that mentions no target, so
    # the restriction would be silently dropped and the call reported anyway.
    'continued_guard_line': {
        'header': '',
        'impl': '''#include "sdkconfig.h"
#if defined(CONFIG_EZPI_ENABLE_SOMETHING) && \\
    (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \\
     defined(CONFIG_IDF_TARGET_ESP32))

static void effect(void);

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
static void effect(void) { }
#elif defined(CONFIG_IDF_TARGET_ESP32)
static void effect(void) { }
#endif

void run(void) { effect(); }

#elif defined(CONFIG_EZPI_ENABLE_SOMETHING)
#error "no implementation for this target"
#endif
''',
        'caller': '',
        'want_call_flag': False,
        'want_decl_flag': False,
    },
    # A commented-out call must not count, the decoy that tripped the first
    # version of check_trace_format_attr.
    'commented_decoy': {
        'header': '''#include "sdkconfig.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int guarded_only(void);
#endif
''',
        'impl': '''#include "sdkconfig.h"
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)
int guarded_only(void) { return 0; }
#endif
''',
        'caller': '''#include "h.h"
int use(void)
{
    // return guarded_only();
    /* return guarded_only(); */
    return 0;
}
''',
        'want_call_flag': False,
        'want_decl_flag': False,
    },
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


UNKNOWN = 'unknown'


def _eval(tokens, pos, target):
    """Tiny recursive-descent evaluator over ||, &&, !, (), literals.

    Returns (value, pos) where value is True, False or UNKNOWN. UNKNOWN
    propagates so that any condition mentioning a non-target macro ends up
    UNKNOWN and is treated as active by the caller.
    """
    value, pos = _eval_and(tokens, pos, target)
    while pos < len(tokens) and tokens[pos] == '||':
        rhs, pos = _eval_and(tokens, pos + 1, target)
        if value is True or rhs is True:
            value = True
        elif value is UNKNOWN or rhs is UNKNOWN:
            value = UNKNOWN
        else:
            value = False
    return value, pos


def _eval_and(tokens, pos, target):
    value, pos = _eval_unary(tokens, pos, target)
    while pos < len(tokens) and tokens[pos] == '&&':
        rhs, pos = _eval_unary(tokens, pos + 1, target)
        if value is False or rhs is False:
            value = False
        elif value is UNKNOWN or rhs is UNKNOWN:
            value = UNKNOWN
        else:
            value = True
    return value, pos


def _eval_unary(tokens, pos, target):
    if pos >= len(tokens):
        return UNKNOWN, pos
    tok = tokens[pos]
    if tok == '!':
        value, pos = _eval_unary(tokens, pos + 1, target)
        if value is UNKNOWN:
            return UNKNOWN, pos
        return (not value), pos
    if tok == '(':
        value, pos = _eval(tokens, pos + 1, target)
        if pos < len(tokens) and tokens[pos] == ')':
            pos += 1
        return value, pos
    if tok == 'defined':
        # defined(X) or defined X
        if pos + 1 < len(tokens) and tokens[pos + 1] == '(':
            inner, pos = _eval(tokens, pos + 2, target)
            if pos < len(tokens) and tokens[pos] == ')':
                pos += 1
            return inner, pos
        return _eval_unary(tokens, pos + 1, target)
    pos += 1
    if re.fullmatch(r'\d+', tok):
        return (int(tok) != 0), pos
    match = TARGET_MACRO.fullmatch(tok)
    if match:
        return (match.group(1).lower() == target), pos
    return UNKNOWN, pos


def condition_holds(expr, target):
    """True/False/UNKNOWN for a #if expression under the named target."""
    tokens = re.findall(r'\d+|[A-Za-z_]\w*|\|\||&&|[!()]|\S', expr)
    if not tokens:
        return UNKNOWN
    if not TARGET_MACRO.search(expr) and not all(
            re.fullmatch(r'\d+|[!()]|\|\||&&', t) for t in tokens):
        # Mentions no target and is not pure arithmetic: not our business.
        return UNKNOWN
    try:
        value, _ = _eval(tokens, 0, target)
    except RecursionError:
        return UNKNOWN
    return value


def active_lines(text, target):
    """The 1-based line numbers of text that survive preprocessing for target.

    Conditions the evaluator cannot decide are treated as taken, on both arms
    of an #else, which is the conservative choice for this check: it can only
    make the check see more code, never less.
    """
    active = set()
    # Each stack entry: [currently_active, any_arm_taken_so_far, decidable]
    stack = []
    for number, line in enumerate(join_continuations(text), 1):
        match = CONDITIONAL.match(line)
        if match:
            kind, rest = match.group(1), match.group(2).strip()
            if kind in ('if', 'ifdef', 'ifndef'):
                if kind == 'ifdef':
                    verdict = condition_holds(rest, target)
                elif kind == 'ifndef':
                    inner = condition_holds(rest, target)
                    verdict = inner if inner is UNKNOWN else (not inner)
                else:
                    verdict = condition_holds(rest, target)
                taken = True if verdict is UNKNOWN else verdict
                parent = all(frame[0] for frame in stack)
                stack.append([parent and taken, taken, verdict is not UNKNOWN])
            elif kind == 'elif':
                if not stack:
                    continue
                frame = stack[-1]
                verdict = condition_holds(rest, target)
                taken = True if verdict is UNKNOWN else verdict
                # An arm is live unless a previous decidable arm already won.
                live = taken and not frame[1]
                if not frame[2]:
                    # The previous arm was undecidable, so this one might also
                    # run as far as we know.
                    live = taken
                parent = all(f[0] for f in stack[:-1])
                frame[0] = parent and live
                frame[1] = frame[1] or taken
                frame[2] = frame[2] and verdict is not UNKNOWN
            elif kind == 'else':
                if not stack:
                    continue
                frame = stack[-1]
                parent = all(f[0] for f in stack[:-1])
                live = (not frame[1]) if frame[2] else True
                frame[0] = parent and live
                frame[1] = True
            elif kind == 'endif':
                if stack:
                    stack.pop()
            continue
        if all(frame[0] for frame in stack):
            active.add(number)
    return active


def join_continuations(text):
    """Split into lines with backslash-continued directives folded together.

    The folded line keeps the position of the first physical line and the
    absorbed lines become empty, so every other line number in the file stays
    correct. Without this a guard written as

        #if defined(CONFIG_EZPI_ENABLE_LED_INDICATOR) && \\
            (defined(CONFIG_IDF_TARGET_ESP32S3) || ...)

    is read as "#if defined(CONFIG_EZPI_ENABLE_LED_INDICATOR) &&", which
    mentions no target, is therefore undecidable, and is therefore treated as
    active for every target -- silently discarding the restriction the guard
    exists to express.
    """
    lines = text.split('\n')
    out = []
    index = 0
    while index < len(lines):
        current = lines[index]
        absorbed = 0
        while current.endswith('\\') and index + absorbed + 1 < len(lines):
            absorbed += 1
            current = current[:-1] + ' ' + lines[index + absorbed]
        out.append(current)
        out.extend([''] * absorbed)
        index += absorbed + 1
    return out


def line_of(text, index):
    return text.count('\n', 0, index) + 1


def definitions(text):
    """(name, line, offset, is_static) for every file-scope definition.

    offset is the index of the name itself, not of the line, because a
    definition written on one line -- "int use(void) { only_some(); }" -- puts
    a call on the same line as the definition. Excluding by line here would
    have hidden exactly the SELF_TEST_GUARD['negated_guard'] violation.
    """
    found = []
    for match in DEFINITION.finditer(text):
        name = match.group(2)
        if name in NOT_A_FUNCTION:
            continue
        found.append((name, line_of(text, match.start()), match.start(2),
                      bool(match.group('static'))))
    return found


def declarations(text):
    """(name, line, offset) for every prototype in text."""
    found = []
    for match in DECLARATION.finditer(text):
        name = match.group('name')
        if name in NOT_A_FUNCTION:
            continue
        first = match.group('prefix').split()[0] if match.group('prefix').split() else ''
        if first in STATEMENT_KEYWORDS:
            continue
        found.append((name, line_of(text, match.start()), match.start('name')))
    return found


def references(text, names):
    """(name, line) for every call-shaped use of one of names in text.

    Neither a definition nor a prototype counts. The prototype exclusion is
    not cosmetic: ezlopi_service_led_indicator.c carries a block of static
    forward declarations outside the target guards that hold the definitions,
    and counting those as calls reported ten violations that were not calls at
    all.
    """
    found = []
    skip = {offset for _n, _line, offset, _s in definitions(text)}
    skip |= {offset for _n, _line, offset in declarations(text)}
    for name in names:
        for match in re.finditer(r'\b%s\s*\(' % re.escape(name), text):
            if match.start() in skip:
                continue
            found.append((name, line_of(text, match.start())))
    return found


def sources():
    for root_dir in SEARCH_DIRS:
        if not os.path.isdir(root_dir):
            continue
        for here, _dirs, files in os.walk(root_dir):
            for name in sorted(files):
                if name.endswith(('.c', '.h')):
                    yield os.path.join(here, name)


def analyse(files):
    """files maps path -> text. Returns (call_problems, decl_problems)."""
    stripped = {path: strip_comments(text) for path, text in files.items()}
    live = {path: {t: active_lines(text, t) for t in TARGETS}
            for path, text in stripped.items()}

    # A static definition is reachable only from its own translation unit, so
    # pooling static names tree-wide would let an unrelated file's static of
    # the same name stand in as the definition. Keep the two scopes apart:
    # external definitions are visible everywhere, static ones only at home.
    external = {}
    internal = {}
    for path, text in stripped.items():
        if not path.endswith('.c'):
            continue
        for name, line, _offset, is_static in definitions(text):
            for target in TARGETS:
                if line in live[path][target]:
                    if is_static:
                        internal.setdefault((path, name), set()).add(target)
                    else:
                        external.setdefault(name, set()).add(target)

    def defined_for(path, name):
        """Targets that can resolve name from path, or None if never seen."""
        here = internal.get((path, name))
        there = external.get(name)
        if here is None and there is None:
            return None
        return (here or set()) | (there or set())

    all_targets = set(TARGETS)
    # Only names defined for at least one target but not all are interesting;
    # see LIMITS.
    candidates = set(external)
    candidates |= {name for _p, name in internal}

    call_problems = []
    decl_problems = []
    for path, text in sorted(stripped.items()):
        if path.endswith('.c'):
            interesting = {n for n in candidates
                           if (defined_for(path, n) or all_targets) != all_targets}
            for name, line in references(text, interesting):
                have = defined_for(path, name)
                for target in TARGETS:
                    if line in live[path][target] and target not in have:
                        call_problems.append(
                            '%s:%d: calls %s(), which nothing defines for %s '
                            '(defined for %s)'
                            % (path, line, name, target, ', '.join(sorted(have))))
                        break
        else:
            for name, line, _offset in declarations(text):
                have = external.get(name)
                if not have or have == all_targets:
                    continue
                for target in TARGETS:
                    if line in live[path][target] and target not in have:
                        decl_problems.append(
                            '%s:%d: declares %s() for %s, where no .c file '
                            'defines it (defined for %s)'
                            % (path, line, name, target, ', '.join(sorted(have))))
                        break
    return call_problems, decl_problems


def self_test():
    for label, case in sorted(SELF_TEST_GUARD.items()):
        files = {
            os.path.join('ezlopi-core', 'h.h'): case['header'],
            os.path.join('ezlopi-core', 'impl.c'): case['impl'],
            os.path.join('ezlopi-core', 'caller.c'): case['caller'],
        }
        calls, decls = analyse(files)
        if bool(calls) != case['want_call_flag']:
            print('SELF-TEST FAILED: %s call rule wanted %s, got %s'
                  % (label, case['want_call_flag'], calls or 'nothing'))
            return False
        if bool(decls) != case['want_decl_flag']:
            print('SELF-TEST FAILED: %s declaration rule wanted %s, got %s'
                  % (label, case['want_decl_flag'], decls or 'nothing'))
            return False

    # The evaluator itself, on the three condition shapes that actually appear.
    cases = [
        ('(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || '
         'CONFIG_IDF_TARGET_ESP32S2)', {'esp32': True, 'esp32c3': False,
                                        'esp32s2': True, 'esp32s3': True}),
        ('!defined(CONFIG_IDF_TARGET_ESP32) && '
         '!defined(CONFIG_IDF_TARGET_ESP32C3)', {'esp32': False,
                                                 'esp32c3': False,
                                                 'esp32s2': True,
                                                 'esp32s3': True}),
        ('0', {'esp32': False, 'esp32c3': False}),
        ('1', {'esp32': True, 'esp32c3': True}),
    ]
    for expr, expected in cases:
        for target, want in expected.items():
            got = condition_holds(expr, target)
            if got is not want:
                print('SELF-TEST FAILED: "%s" on %s gave %s, wanted %s'
                      % (expr, target, got, want))
                return False
    # An undecidable condition must come back UNKNOWN, not a guess.
    if condition_holds('(1 == SENSOR_0015_ONEWIRE_DHT11)', 'esp32') is not UNKNOWN:
        print('SELF-TEST FAILED: a device macro was treated as decidable.')
        return False
    return True


def main():
    if not self_test():
        return 1

    paths = list(sources())
    if not paths:
        print('No sources found. Run this from the repository root.')
        return 1

    files = {}
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as handle:
            files[path] = handle.read()

    calls, decls = analyse(files)

    if calls or decls:
        print('Target guards that leave a call or a prototype unbacked:\n')
        for item in calls + decls:
            print('  ' + item)
        print('\nGuard the caller and the prototype with the same condition as '
              'the definition, the way sensor-0016 does. See the docstring at '
              'the top of this file: the linker will not report this, because '
              'the object sits in an archive that nothing pulls in.')
        return 1

    print('files examined: %d; targets: %s; no unbacked calls or prototypes'
          % (len(paths), ', '.join(TARGETS)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
