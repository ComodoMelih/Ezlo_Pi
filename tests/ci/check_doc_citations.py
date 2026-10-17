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

Three failure modes, which are worth different amounts:

  MISSING   the linked path does not exist. A rename or a deletion.
  SHORT     the path exists but is shorter than the cited line.
  MISMATCH  the link text ends `foo.c:123` and the fragment says `#L456`.
            Either is defensible alone; together one of them is wrong.

What it cannot do is tell whether `#L158` still points at the *right* line.
Nothing short of re-reading each citation can, and that is a person's job, not
a check's. So this is a check against rot that is mechanically detectable, and
a green run means the links resolve -- not that the prose is still true.

That limit was tested rather than assumed, after the HTTP body-logging change
inserted 63 lines into `ezlopi_core_http.c` and silently moved four citations
in older entries -- two of which then pointed at a blank line and at an
unrelated `mbedtls_net_free()`. This check passed on all four, correctly, by
the rules above. The obvious strengthening is to require that a link whose
text is one backticked code token has that token on or near the cited line.
Measured over the 47 citations whose text is a bare token: 42 sit on the exact
line, 4 are within 12 lines, 1 is 21 away. So a +/-12 window would flag exactly
one citation, and that one is right -- `GATED_SWITCHES` is deliberately cited
at the table *entry* rather than at the table's declaration. Widening the
window to clear it reaches 25 lines, which also clears the 14-line error that
prompted the measurement. And no ambiguity filter separates the two: the token
to catch occurs 7 times in its file, the token to spare occurs 10. (A first
cut also flagged three `CONFIG_EZPI_*` citations into Kconfig.projbuild, which
writes `config EZPI_*` without the prefix -- those are the prose being right
and the rule being wrong, and stripping the prefix fixes them.)

So the rule is not added, on measurement rather than on principle. What does
help is mechanical and needs no heuristic: when a commit edits a file, list
every citation that points into it and re-read them. That is a listing, not a
judgement, which is why it is not a gate here.

WHAT IT FOUND

One mismatch, on the day it was written:
`[ezlopi_core_factory_info.c:140](...#L135)` -- the prose cited the
`arg = NULL;` statement and the link pointed at the function signature five
lines above it. Both numbers were right about something and the pair was
wrong, which is the one shape a reader has no way to spot.

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


def line_count(path, cache={}):
    if path not in cache:
        try:
            with open(path, 'rb') as fh:
                cache[path] = fh.read().count(b'\n') + 1
        except OSError:
            cache[path] = None
    return cache[path]


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
    return problems


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
