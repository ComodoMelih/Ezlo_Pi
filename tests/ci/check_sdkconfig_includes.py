#!/usr/bin/env python3
"""Fail if any file pins ESP-IDF's generated config header by relative path.

The problem
-----------
This tree used to include ESP-IDF's generated configuration header like this:

    #include "../../build/config/sdkconfig.h"

That pins two things it has no business pinning: the build directory's *name*
("build") and its *position* relative to the including source file. So it
breaks for any build directory other than ``<project>/build`` -- including the
perfectly ordinary ``idf.py -B <dir>``, and including out-of-tree builds and
most IDE integrations.

It is also unnecessary. ESP-IDF already puts the generated config directory on
every component's include path, so the plain ``#include "sdkconfig.h"`` works.

All 83 have now been converted, so the baseline is zero and this is a
prohibition rather than a ratchet: the relative form must not come back.

Why the conversion was safe to do in one go
-------------------------------------------
Because it was verifiable rather than merely plausible. The preprocessor is
handed the same ``sdkconfig.h`` either way, and the converter replaced one line
with one line in every file, so not even the ``__LINE__`` values that ``TRACE_*``
embeds could shift. That is a falsifiable prediction, and it was tested: an
esp32 image built before and after differs only in the app descriptor's build
*time* string, the ELF sha256 that covers it, and the trailing image checksum.
``.flash.text``, ``.flash.rodata``, ``.dram0.data`` and ``.iram0.text`` compare
byte-identical, and all 13800 symbols match including their addresses.

The script keeps the baseline mechanism, and still fails if the count falls
below the baseline without this constant being updated. At zero that branch
cannot fire -- the count cannot go lower -- but it is kept rather than deleted
because the mechanism is what makes the check honest if the number ever has to
move again, and because a drop is only good news if the floor rises with it.

Counting this correctly is fiddlier than it looks
-------------------------------------------------
Two traps, both of which this script was written to avoid, and both of which
are the reason it exists as a script rather than as a one-line grep:

1. **Matching a fixed ``../`` depth misses files.** Almost every offender sat
   two directories deep and wrote ``"../../build/config/sdkconfig.h"``, but
   ``ezlopi-user-config/EZLOPI_USER_CONFIG.h`` sits one shallower and wrote
   ``"../build/config/sdkconfig.h"``. A pattern anchored on ``../../`` waves it
   through. This script matches on the path *suffix* at any depth.

2. **Mentioning the path is not including it.** Several files name the old form
   in a comment while deliberately *not* using it -- the converted files explain
   what they moved away from, and two host-test stubs describe what they stub
   out. A naive ``git grep -l`` for the string counts those as offenders, which
   is why it reported 89 when the real number was 83. This script only counts
   lines that are actually ``#include`` directives, and strips comments before
   looking.
"""

import re
import subprocess
import sys

# The number of files allowed to pin the generated config by path. All 83
# inherited from upstream have been converted, so this is zero and must stay
# zero. Never raise it: the relative form breaks idf.py -B <dir>, and a file
# that uses it is a file someone has to convert later.
BASELINE = 0

# An #include whose quoted path ends in build/config/sdkconfig.h, at any depth.
# Anchored at the start of the line (allowing leading whitespace) so a mention
# inside a comment or a string is not mistaken for a directive.
PINNED = re.compile(r'^\s*#\s*include\s*"[^"]*build/config/sdkconfig\.h"')

# The form new code should use.
PLAIN = re.compile(r'^\s*#\s*include\s*"sdkconfig\.h"')


def tracked_sources():
    """Every tracked .c/.h file. Uses git so generated build output, which
    contains a real sdkconfig.h and would otherwise be scanned, cannot
    contribute to the count."""
    out = subprocess.run(
        ["git", "ls-files", "-z", "*.c", "*.h"],
        capture_output=True, text=True, check=True,
    ).stdout
    return [p for p in out.split("\0") if p]


def blank_comments(text):
    """Replace every comment character with a space, preserving newlines and
    offsets, so the anchored regexes below only ever see real code.

    This is a character scanner rather than a regex for a specific reason. The
    obvious implementation,

        re.sub(r"/\\*.*?\\*/", "", text, flags=re.S)

    is wrong on this codebase, and wrong in a way that silently *undercounts*.
    Many files here use banner comments like::

        //*******************************************************
        //*                   Include Files
        //*******************************************************
        #include "../../build/config/sdkconfig.h"

    ``//*`` contains the substring ``/*``. The regex treats that as the start of
    a block comment and deletes everything up to the next ``*/`` -- which
    swallows the real ``#include`` underneath. That is not hypothetical: it hid
    ``ezlopi_core_scenes_cjson.c`` from this very check, and the count came out
    as 82 instead of 83. A scanner that understands ``//`` starts a line comment
    (so a ``/*`` inside one is not an opener) does not have the problem.

    String literals are tracked too, so a path mentioned inside one is not
    mistaken for a comment delimiter and vice versa."""
    out = []
    i, n = 0, len(text)
    in_line = in_block = in_str = in_chr = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            else:
                out.append(" ")
            i += 1
        elif in_block:
            if c == "*" and nxt == "/":
                in_block = False
                out.append("  ")
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        elif in_str or in_chr:
            # Keep string contents: the regexes need to read the quoted path.
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if (in_str and c == '"') or (in_chr and c == "'"):
                in_str = in_chr = False
            i += 1
        else:
            if c == "/" and nxt == "/":
                in_line = True
                out.append("  ")
                i += 2
            elif c == "/" and nxt == "*":
                in_block = True
                out.append("  ")
                i += 2
            else:
                if c == '"':
                    in_str = True
                elif c == "'":
                    in_chr = True
                out.append(c)
                i += 1
    return "".join(out)


def _self_test():
    """Guard the scanner against the banner-comment trap that already bit this
    check once. Runs on every invocation: it is microseconds, and a counter
    whose counting is wrong is worse than no counter."""
    banner = (
        '//*********************************\n'
        '//*         Include Files\n'
        '//*********************************\n'
        '#include "../../build/config/sdkconfig.h"\n'
    )
    lines = blank_comments(banner).splitlines()
    assert any(PINNED.match(ln) for ln in lines), \
        "scanner lost an include under a //* banner comment"

    commented_out = '/* #include "../../build/config/sdkconfig.h" */\n'
    lines = blank_comments(commented_out).splitlines()
    assert not any(PINNED.match(ln) for ln in lines), \
        "scanner counted an include that was commented out"

    multiline = (
        "/* this file used to say\n"
        ' *     #include "../../build/config/sdkconfig.h"\n'
        " * and no longer does.\n"
        " */\n"
        '#include "sdkconfig.h"\n'
    )
    lines = blank_comments(multiline).splitlines()
    assert not any(PINNED.match(ln) for ln in lines), \
        "scanner counted a path mentioned inside a block comment"
    assert any(PLAIN.match(ln) for ln in lines), \
        "scanner lost the real include after a block comment"


def classify():
    pinned, plain = [], []
    for path in tracked_sources():
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            print(f"::error::could not read {path}: {exc}")
            sys.exit(1)
        lines = blank_comments(text).splitlines()
        if any(PINNED.match(ln) for ln in lines):
            pinned.append(path)
        if any(PLAIN.match(ln) for ln in lines):
            plain.append(path)
    return sorted(pinned), sorted(plain)


def main():
    _self_test()
    pinned, plain = classify()
    n = len(pinned)

    print(f"files pinning the generated config by path: {n} (baseline {BASELINE})")
    print(f"files using the plain #include \"sdkconfig.h\": {len(plain)}")
    # The plain-form list used to be seven files and was worth printing. It is
    # now ninety, which is noise in a CI log, so only the offenders get named.

    if n > BASELINE:
        added = n - BASELINE
        print()
        print(f"::error::{added} file(s) pin the generated config by relative path "
              f"({n}, baseline {BASELINE}).")
        print("::error::New code must use the plain #include \"sdkconfig.h\" instead.")
        print("::error::ESP-IDF already puts the generated config directory on every")
        print("::error::component's include path. The relative form pins the build")
        print("::error::directory's name and position, so it breaks idf.py -B <dir>.")
        print()
        print("--- files using the relative form ---")
        for path in pinned:
            print(f"  {path}")
        return 1

    if n < BASELINE:
        # Unreachable while BASELINE is 0, since a count cannot go negative.
        # Kept because the baseline is what makes this check honest if the
        # number ever has to move again: a drop is only good news if the floor
        # drops with it, otherwise the next regression has a free pass.
        print()
        print(f"::error::Good news, handled badly: the count dropped to {n} but the")
        print(f"::error::baseline in {__file__} still says {BASELINE}.")
        print("::error::Lower BASELINE to the new count so the check keeps its teeth.")
        print("::error::Leaving it high would let the next regression back in silently.")
        return 1

    print()
    print("no file pins the generated config by relative path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
