#!/usr/bin/env python3
"""Every markdown link into this tree must resolve, and agree with its text.

WHY THIS EXISTS

The CHANGELOG is this fork's main deliverable and it carries 230-odd links
into the source, most of them with a `#L<n>` line fragment. Line numbers move
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

What ANCHOR deliberately does not do is guess at prose link text. 150 of the
200 citations with a line fragment are skipped for that reason, so the mechanical
advice still stands for them: when a commit edits a file, list every citation
that points into it and re-read them.

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

Links to directories are counted and skipped: `tests/host/` is linked that way
deliberately and has no line to resolve.
"""
import os
import re
import sys

DOCS_DIR = '.'

# [text](path) and [text](path#L123). The path has to look like a
# repo-relative file, so external URLs and bare `#anchor` links are untouched.
LINK = re.compile(r'\[(?P<text>[^\]]*)\]\((?P<path>[A-Za-z0-9_][A-Za-z0-9_./-]*)'
                  r'(?:#L(?P<line>\d+))?\)')

# A link text that ends in `:123` or `:123`` is making a claim about the line
# number, which the fragment then has to match.
TEXT_LINE = re.compile(r':(?P<line>\d+)`?\s*$')

# A link text that is nothing but one backticked code token -- `TRACE_I`,
# `foo()`, `EZPI_core_http_get_request`. Such a link is claiming the token is
# at the cited line, which is checkable. Three characters minimum, so `n` or
# `ok` in prose cannot drag a citation into the rule. Not prose, not a
# `file.c:12` claim (MISMATCH already owns those), not a format string.
TOKEN = re.compile(r'^`([A-Za-z_][A-Za-z0-9_]{2,})(?:\(\))?`$')


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
