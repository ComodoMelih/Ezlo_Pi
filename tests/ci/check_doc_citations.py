#!/usr/bin/env python3
"""Every markdown link into this tree must resolve, and agree with its text.

WHY THIS EXISTS

The CHANGELOG is this fork's main deliverable and it carries 431 links into the
source, 317 of them with a `#L<n>` line fragment. (It said 230-odd when this
was written; the count is re-measured whenever something here quotes it,
because a stale number in the prose of a staleness check is a poor
advertisement.) Line numbers move
whenever anything above them is edited, and nothing in the toolchain notices:
a link to a renamed file, or to a line past the end of a file, renders as
ordinary text and reads as evidence. Several entries here turn on a specific
line -- "returns NULL on three paths", "the guard is the next block down" --
so a citation that no longer points anywhere is worse than no citation.

WHAT IT CHECKS, AND WHAT IT CANNOT

Four failure modes, which are worth different amounts:

  MISSING   the linked path does not exist. A rename or a deletion.
  SHORT     the path exists but is shorter than the cited line.
  MISMATCH  the link text ends `foo.c:123` and the fragment says `#L456`.
            Either is defensible alone; together one of them is wrong.
  ANCHOR    the link text is one backticked code token and that token is not
            on the cited line. See below -- this one was declined once on
            measurement and then added when the measurement changed.

What none of them can do is tell whether `#L158` still points at the *right*
line in the general case. A citation whose text is prose -- "appended", "the
per-chunk dump" -- has nothing mechanical to check against, and that is still a
person's job. So a green run means the links resolve and the ones that name a
token point at it; not that the prose is still true.

WHY ANCHOR EXISTS, AND WHY IT DID NOT AT FIRST

This limit was tested rather than assumed, after the HTTP body-logging change
inserted 63 lines into `ezlopi_core_http.c` and silently moved four citations
in older entries -- two of which then pointed at a blank line and at an
unrelated `mbedtls_net_free()`. The first three rules passed on all four,
correctly, by their own terms.

The strengthening measured then was a *window*: require the token within +/-12
lines. Over the 47 bare-token citations, 42 sat on the exact line, 4 within 12,
1 at 21. So the window flagged exactly one citation and that one was right --
`GATED_SWITCHES` was deliberately cited at the table *entry* rather than the
table's declaration -- and widening the window to clear it reached 25 lines,
which also cleared the 14-line error that prompted the measurement. No
ambiguity filter separated the two: the token to catch occurred 7 times in its
file, the token to spare 10. The rule was declined on those numbers.

The next change into the same file rotted six more citations, which is what
changed the answer. Re-measured on a tree where the rot had been repaired by
hand, all 50 bare-token citations sit on the *exact* line -- no window needed,
and no exception to carve out. The one citation that forced the window was
fixed at the source instead: it now links twice, once at `GATED_SWITCHES` for
the name and once at the entry for the pin, so each link text names what its
own line contains. A rule with no exceptions is a different proposition from a
rule with one, and this is the shape that was addable.

So ANCHOR requires the token on the cited line exactly, with two refinements
that are about notation rather than tolerance. A trailing `()` in the link text
is dropped, since `foo()` in prose cites `foo` in the source. And a `CONFIG_`
prefix is dropped, because a Kconfig option is cited by the name the generated
`sdkconfig` uses while `Kconfig.projbuild` itself writes `config EZPI_FOO` --
that was a first cut flagging three citations where the prose was right and the
rule was wrong.

What ANCHOR deliberately does not do is guess at prose link text. Of the 320
citations carrying a line fragment across the root documents, ANCHOR bears on
the 140 that are one backticked token and MISMATCH on the 102 whose text ends
`:N`, leaving 78 with nothing mechanical on them beyond the path existing and
being long enough. This said "150 of the 200" when ANCHOR was added; the ratio
improved because citing by token became the house style, not because anything
was relaxed. For those 78 the mechanical advice still stands: when a commit
edits a file, list every citation that points into it and re-read them.

WHAT IT FOUND

One mismatch, on the day it was written:
`[ezlopi_core_factory_info.c:140](...#L135)` -- the prose cited the
`arg = NULL;` statement and the link pointed at the function signature five
lines above it. Both numbers were right about something and the pair was
wrong, which is the one shape a reader has no way to spot.

ANCHOR was then seen to catch the six citations whose rot prompted it, rather
than only the synthetic fixture below. On a full copy of the repaired tree the
check exits 0 with no ANCHOR findings; with the six rotted numbers put back it
exits 1 and names all six. MISSING, SHORT and MISMATCH report nothing on those
same six -- every one of them resolved to a real line in a real file, which is
why they shipped unnoticed twice.

Then a third time, and larger: 24 citations into
ezlopi_core_scenes_when_methods_helper_functions.c, from entries written before
the later scenes work edited it. Two separate shifts (-44 and -9), so two
commits, neither of which re-read what rested on it. Three of the 24 had moved
*file* rather than line -- s_sunstate_data_t was relocated into the header --
and that class is reachable by ANCHOR alone: the .c still exists, so MISSING
passes, and it is 2338 lines, so SHORT passes. Four more citations into the
same file were outside every rule, and two of those four are the instructive
ones: their text ends `:547` and `:597` and their fragments said `#L547` and
`#L597`, so MISMATCH compared them and was satisfied. MISMATCH asks whether a
citation agrees with itself, not whether it is right, and two numbers that rot
together rot invisibly.

WHERE IT DOES NOT LOOK AT ALL

main() lists *.md in one directory, so docs/ is not scanned -- but pointing it
there would cover little, because LINK requires a repo-root-relative path
(`[A-Za-z0-9_]` first) and every citation in a subdirectory document is written
`../ezlopi-core/...`. Measured: the three files in docs/ carry 23 local links
between them and this regex can see exactly 1. Covering them means resolving
each link against its own document's directory, which is a real change and not
a wider glob.

Links to directories are counted and skipped: `tests/host/` is linked that way
deliberately and has no line to resolve.
"""
import os
import re
import sys

DOCS_DIR = '.'

# [text](path), [text](path#L123) and [text](path#L110-L129). The path has to
# look like a repo-relative file, so external URLs and bare `#anchor` links are
# untouched: the charset excludes ':', so `https://...` fails to match and is
# skipped rather than reported as a missing file.
#
# A leading '.' is allowed because three real citations point at
# `.github/workflows/ci.yml`, and without it they were not examined at all --
# the path could have rotted silently. For a range only the first number is
# checked; that is the one that has to exist.
LINK = re.compile(r'\[(?P<text>[^\]]*)\]\((?P<path>[A-Za-z0-9_.][A-Za-z0-9_./-]*)'
                  r'(?:#L(?P<line>\d+)(?:-L?\d+)?)?\)')

# A link text that ends in `:123` or `:123`` is making a claim about the line
# number, which the fragment then has to match.
TEXT_LINE = re.compile(r':(?P<line>\d+)`?\s*$')

# A link text that *begins* "line 123" / "lines 123" is making the same claim in
# prose. Worth its own rule because TEXT_LINE is anchored at the end and so
# never saw these: `[line 898](file.c#L898)` was checked by nothing at all, and
# one such citation went stale in both halves at once -- the text and the
# fragment agreed with each other while both pointed at the wrong line.
#
# This only catches the halves disagreeing. Agreeing-but-wrong is not
# detectable from the link alone, which is why the preferred form for a new
# citation is `` [`symbol`:123](file.c#L123) ``: TOKEN then checks the line's
# actual contents, not just the link's internal consistency.
PROSE_LINE = re.compile(r'^\s*lines?\s+(?P<line>\d+)\b')

# A link text that is nothing but one backticked code token -- `TRACE_I`,
# `foo()`, `EZPI_core_http_get_request`. Such a link is claiming the token is
# at the cited line, which is checkable. Three characters minimum, so `n` or
# `ok` in prose cannot drag a citation into the rule. Not prose, not a
# `file.c:12` claim (MISMATCH already owns those), not a format string.
#
# An optional `:123` after the closing backtick is accepted so a citation can
# be checked on BOTH axes at once: `` [`foo`:123](file.c#L123) `` has TEXT_LINE
# compare 123 against the fragment and TOKEN confirm `foo` really is on that
# line. That combination is the only form the gate can verify completely, so
# it is the one to reach for when writing a new citation.
TOKEN = re.compile(r'^`([A-Za-z_][A-Za-z0-9_]{2,})(?:\(\))?`(?::\d+)?$')


def line_count(path, cache={}):
    if path not in cache:
        try:
            with open(path, 'rb') as fh:
                cache[path] = fh.read().count(b'\n') + 1
        except OSError:
            cache[path] = None
    return cache[path]


def cited_line_text(path, n, cache={}):
    """The text of line `n` of `path`, or None if there is no such line."""
    if path not in cache:
        try:
            with open(path, 'rb') as fh:
                cache[path] = fh.read().decode('utf-8', 'replace').splitlines()
        except OSError:
            cache[path] = []
    lines = cache[path]
    return lines[n - 1] if 0 < n <= len(lines) else None


def check_text(text, root):
    """Every problem in one document's text, as (kind, detail) pairs.

    Returned rather than printed so the self-test can call this directly
    instead of writing temporary files and parsing output.
    """
    problems = []
    for n, raw in enumerate(text.splitlines(), 1):
        for m in LINK.finditer(raw):
            rel = m.group('path')
            full = os.path.join(root, rel)
            if os.path.isdir(full):
                continue
            if not os.path.isfile(full):
                problems.append(('MISSING', ':%d  %s' % (n, rel)))
                continue
            cited = m.group('line')
            if cited is None:
                continue
            total = line_count(full)
            if total is None or int(cited) > total:
                problems.append(('SHORT', ':%d  %s#L%s  (file has %s lines)'
                                 % (n, rel, cited, total)))
                continue
            t = TEXT_LINE.search(m.group('text'))
            if t and t.group('line') != cited:
                problems.append(('MISMATCH',
                                 ':%d  text says :%s, link says #L%s  (%s)'
                                 % (n, t.group('line'), cited, rel)))
                continue
            p = PROSE_LINE.match(m.group('text'))
            if p and p.group('line') != cited:
                problems.append(('MISMATCH',
                                 ':%d  text says line %s, link says #L%s  (%s)'
                                 % (n, p.group('line'), cited, rel)))
                continue
            tok = TOKEN.match(m.group('text').strip())
            if tok:
                name = tok.group(1)
                # A Kconfig option is cited by the name the generated sdkconfig
                # uses; Kconfig.projbuild writes `config EZPI_FOO`.
                if name.startswith('CONFIG_'):
                    name = name[len('CONFIG_'):]
                body = cited_line_text(full, int(cited))
                if body is not None and name not in body:
                    problems.append(('ANCHOR',
                                     ':%d  %s#L%s does not contain `%s`'
                                     % (n, rel, cited, name)))
    return problems


def _find_line(rel, needle):
    """1-based line number of the first line of `rel` containing `needle`.

    The ANCHOR fixtures below have to cite a line whose contents are known, and
    hardcoding the number would make the self-test the very thing this check
    exists to catch -- a citation that rots when the file above it is edited.
    So the number is looked up. A needle that stops matching raises here rather
    than quietly turning a fixture into a no-op.
    """
    here = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    with open(os.path.join(here, rel), encoding='utf-8') as fh:
        for n, line in enumerate(fh, 1):
            if needle in line:
                return n
    raise AssertionError(
        'self-test anchor %r is no longer in %s, so the ANCHOR fixtures '
        'would not test anything. Update the needle.' % (needle, rel))


_TOKEN_DEF_LINE = _find_line('tests/ci/check_doc_citations.py',
                             "TOKEN = re.compile(r'^`")
_HTTP_LOG_PAYLOADS_LINE = _find_line('ezlopi-main/Kconfig.projbuild',
                                     'config EZPI_HTTP_LOG_PAYLOADS')

FIXTURES = [
    # (name, must_fire, text)
    ('resolving link with no line', False,
     'See [`tests/ci/check_doc_citations.py`](tests/ci/check_doc_citations.py).'),
    ('resolving link with a line', False,
     'See [`tests/ci/check_doc_citations.py:1`](tests/ci/check_doc_citations.py#L1).'),
    ('link to a directory', False,
     'Covered by [`tests/host/`](tests/host/).'),
    ('external URL is not ours to check', False,
     'Upstream is [ezloteam/Ezlo_Pi](https://github.com/ezloteam/Ezlo_Pi).'),
    ('bare anchor is not a path', False,
     'See [Known gaps](#known-gaps).'),
    ('text line matches the fragment', False,
     '[`check_doc_citations.py:1`](tests/ci/check_doc_citations.py#L1)'),
    ('missing file', True,
     'See [`gone.c`](ezlopi-core/ezlopi-core-gone/gone.c).'),
    ('line past the end of the file', True,
     '[`check_doc_citations.py:999999`](tests/ci/check_doc_citations.py#L999999)'),
    ('text and fragment disagree', True,
     '[`check_doc_citations.py:2`](tests/ci/check_doc_citations.py#L1)'),
    # ANCHOR. `TOKEN` is the name of the regex defined in this file, so a
    # citation of it has to land on the line that defines it. These two
    # fixtures move together if that definition moves, which is the point:
    # a self-test that cannot rot is testing something weaker than the rule.
    ('token is on the cited line', False,
     '[`TOKEN`](tests/ci/check_doc_citations.py#L%d)' % _TOKEN_DEF_LINE),
    ('token is not on the cited line', True,
     '[`TOKEN`](tests/ci/check_doc_citations.py#L%d)' % (_TOKEN_DEF_LINE + 1)),
    ('prose link text is not a token claim', False,
     '[the regex above](tests/ci/check_doc_citations.py#L1)'),
    ('a two-letter token is too short to claim', False,
     '[`ok`](tests/ci/check_doc_citations.py#L1)'),
    ('CONFIG_ prefix is stripped for Kconfig', False,
     '[`CONFIG_EZPI_HTTP_LOG_PAYLOADS`](ezlopi-main/Kconfig.projbuild#L%d)'
     % _HTTP_LOG_PAYLOADS_LINE),

    # PROSE_LINE. `[line N](file#LN)` used to be checked by nothing: TEXT_LINE
    # is anchored at the end of the text and so never matched it.
    ('prose line number matches the fragment', False,
     'See [line 1](tests/ci/check_doc_citations.py#L1) for the header.'),
    ('prose line number contradicts the fragment', True,
     'See [line 2](tests/ci/check_doc_citations.py#L1) for the header.'),
    ('plural prose line number contradicts the fragment', True,
     'See [lines 7](tests/ci/check_doc_citations.py#L1).'),
    ('"line" inside ordinary prose is not a claim', False,
     'The [inline cache](tests/ci/check_doc_citations.py#L1) is per-path.'),

    # TOKEN with a trailing `:N` -- the fully-checked citation form, verified
    # on the line number AND on the line's contents.
    ('token with line suffix, both correct', False,
     '[`TOKEN`:%d](tests/ci/check_doc_citations.py#L%d)'
     % (_TOKEN_DEF_LINE, _TOKEN_DEF_LINE)),
    ('token with line suffix, token not on that line', True,
     '[`TOKEN`:%d](tests/ci/check_doc_citations.py#L%d)'
     % (_TOKEN_DEF_LINE + 1, _TOKEN_DEF_LINE + 1)),
    ('token with line suffix disagreeing with the fragment', True,
     '[`TOKEN`:%d](tests/ci/check_doc_citations.py#L%d)'
     % (_TOKEN_DEF_LINE, _TOKEN_DEF_LINE + 1)),

    # LINK widening: a leading-dot path is a real citation in these docs and
    # was previously skipped entirely, so a rotted path could not be noticed.
    ('leading-dot path resolves', False,
     'Defined in [`.github/workflows/ci.yml`](.github/workflows/ci.yml).'),
    ('leading-dot path that does not resolve', True,
     'See [`.github/workflows/gone.yml`](.github/workflows/gone.yml).'),

    # A line range cites its first line; that is the one that must exist.
    ('line range whose start exists', False,
     '[`check_doc_citations.py:1-5`](tests/ci/check_doc_citations.py#L1-L5)'),
    ('line range starting past the end of the file', True,
     '[`check_doc_citations.py`](tests/ci/check_doc_citations.py#L999999-L1000000)'),
]


def self_test(root):
    ok = True
    for name, must_fire, text in FIXTURES:
        problems = check_text(text, root)
        if must_fire and not problems:
            print('SELF-TEST FAILED: the "%s" fixture produced no problem, so '
                  'this check cannot fail on that shape.' % name)
            ok = False
        elif not must_fire and problems:
            print('SELF-TEST FAILED: the "%s" fixture was flagged, so this '
                  'check fires on a correct link: %s' % (name, problems))
            ok = False
        else:
            print('  self-test %-38s %s'
                  % (name, 'flagged' if problems else 'clean'))
    return ok


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else DOCS_DIR
    if not os.path.isfile(os.path.join(root, 'CHANGELOG.md')):
        print('CHANGELOG.md not found in %r. Run this from the repository '
              'root.' % root)
        return 1
    if not self_test(root):
        return 1

    docs = [n for n in sorted(os.listdir(root)) if n.endswith('.md')]
    rc, total = 0, 0
    for doc in docs:
        with open(os.path.join(root, doc), encoding='utf-8') as fh:
            text = fh.read()
        total += len(LINK.findall(text))
        for kind, detail in check_text(text, root):
            rc = 1
            print('%-8s %s%s' % (kind, doc, detail))
    print('\ndocuments: %s' % ' '.join(docs))
    print('markdown links examined: %d' % total)
    if rc:
        print('\nA citation does not resolve, or disagrees with its own link '
              'text. Fix the number or the path -- do not delete the '
              'citation,\nsince the prose around it was written to rest on it.')
    return rc


if __name__ == '__main__':
    sys.exit(main())
