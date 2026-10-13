#!/usr/bin/env python3
"""Which first-party .c files can the system compiler even look at?

This is the measuring tool behind two claims in CONTRIBUTING.md: how many
first-party sources compile on a host with nothing but their own directory on
the include path, and what the next most common blocker is after that.

It lives in the tree because both numbers were quoted in documentation from a
throwaway script, and one of them turned out to be wrong. A figure that cannot
be re-derived is a figure nobody can correct.

Three modes:

  --baseline    each file with only its own directory on the include path
  --sdkconfig   the same, plus a directory holding an empty sdkconfig.h
  --blockers    tally of the first missing header across all failures

"compiles" here means `gcc -fsyntax-only` succeeds. That is a low bar on
purpose: it is the bar a host test has to clear before anything else can be
discussed. The --sdkconfig mode additionally compiles each newly-reachable file
to an object and reports whether it defines any symbols at all, because a file
whose entire body sits inside `#ifdef CONFIG_SOMETHING` compiles perfectly and
emits nothing, and counting it as "now testable" is how the first version of
this measurement went wrong.
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

# Vendored third-party code is not ours to test and is excluded, as are the
# host tests themselves and any build output.
EXCLUDE_DIRS = {
    "build",
    "ezlopi-components",
    ".git",
    "__pycache__",
}

# Every top-level directory holding first-party .c files, with two
# deliberate omissions that have to be stated or the totals here cannot be
# compared with anything:
#
#   ezlopi-components/   vendored third-party code, not ours to test
#   ezlopi-src-template/ a skeleton for new components; no CMakeLists names it,
#                        so it is not compiled into any image
#
# ezlopi-lua-modules IS included. It is first-party and it is built.
FIRST_PARTY_PREFIXES = (
    "ezlopi-cloud",
    "ezlopi-core",
    "ezlopi-hal",
    "ezlopi-lua-modules",
    "ezlopi-main",
    "ezlopi-sensors-devices",
    "ezlopi-services",
    "ezlopi-util",
)

MISSING_HEADER = re.compile(r"fatal error: ([^:]+): No such file or directory")


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", ".."))


def sources(root):
    found = []

    for prefix in FIRST_PARTY_PREFIXES:
        top = os.path.join(root, prefix)

        if not os.path.isdir(top):
            continue

        for dirpath, dirnames, filenames in os.walk(top):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]

            for name in sorted(filenames):
                if name.endswith(".c"):
                    found.append(os.path.join(dirpath, name))

    return sorted(found)


def syntax_only(path, extra_includes):
    cmd = ["gcc", "-fsyntax-only", "-std=c99", "-I", os.path.dirname(path)]

    for inc in extra_includes:
        cmd += ["-I", inc]

    cmd.append(path)

    done = subprocess.run(cmd, capture_output=True, text=True)

    return done.returncode == 0, done.stderr


def defines_nothing(path, extra_includes, workdir):
    """True if the file compiles to an object that defines no symbols.

    A file wrapped end to end in an #ifdef that is not defined is the usual
    cause. Such a file is not host-testable in any useful sense -- a test
    linked against it would pass while exercising nothing.
    """
    obj = os.path.join(workdir, "probe.o")
    cmd = ["gcc", "-c", "-std=c99", "-I", os.path.dirname(path)]

    for inc in extra_includes:
        cmd += ["-I", inc]

    cmd += ["-o", obj, path]

    if subprocess.run(cmd, capture_output=True, text=True).returncode != 0:
        return None

    done = subprocess.run(["nm", "--defined-only", obj],
                          capture_output=True, text=True)

    return done.stdout.strip() == ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--sdkconfig", action="store_true")
    parser.add_argument("--blockers", action="store_true")
    parser.add_argument("--real-sdkconfig", metavar="PATH",
                        help="a generated build/config/sdkconfig.h to use "
                             "instead of an empty stub, which is the only way "
                             "to see what the #ifdef'd-out bodies really need")
    args = parser.parse_args()

    if not (args.baseline or args.sdkconfig or args.blockers):
        args.baseline = True
        args.blockers = True

    root = repo_root()
    files = sources(root)

    print(f"{len(files)} first-party .c files under {root}")

    base_ok = []
    base_fail = []
    blockers = collections.Counter()

    for path in files:
        ok, err = syntax_only(path, [])

        if ok:
            base_ok.append(path)
        else:
            base_fail.append(path)
            found = MISSING_HEADER.search(err)
            blockers[found.group(1) if found else "(not a missing header)"] += 1

    if args.baseline:
        print(f"\nbaseline: {len(base_ok)} compile, {len(base_fail)} do not")

    if args.blockers:
        print("\nfirst missing header, most common first:")

        for header, count in blockers.most_common(10):
            print(f"  {count:4d}  {header}")

    if args.sdkconfig:
        with tempfile.TemporaryDirectory() as tmp:
            stub = os.path.join(tmp, "stub")
            os.makedirs(stub)

            with open(os.path.join(stub, "sdkconfig.h"), "w") as handle:
                handle.write("/* deliberately empty */\n")

            now_ok = []

            for path in base_fail:
                ok, _ = syntax_only(path, [stub])

                if ok:
                    now_ok.append(path)

            empty = [p for p in now_ok if defines_nothing(p, [stub], tmp)]

            print(f"\nwith an empty stub sdkconfig.h: {len(now_ok)} of the "
                  f"{len(base_fail)} previously-failing files compile")
            print(f"  of those, {len(empty)} define no symbols at all "
                  f"-- they are entirely inside an #ifdef")
            print(f"  leaving {len(now_ok) - len(empty)} that actually gained "
                  f"anything")

    if args.real_sdkconfig:
        with tempfile.TemporaryDirectory() as tmp:
            stub = os.path.join(tmp, "stub")
            os.makedirs(stub)

            with open(args.real_sdkconfig) as src:
                body = src.read()

            with open(os.path.join(stub, "sdkconfig.h"), "w") as handle:
                handle.write(body)

            defines = body.count("#define")
            now_ok = []
            blockers = collections.Counter()

            for path in base_fail:
                ok, err = syntax_only(path, [stub])

                if ok:
                    now_ok.append(path)
                else:
                    found = MISSING_HEADER.search(err)
                    blockers[found.group(1) if found
                             else "(not a missing header)"] += 1

            empty = [p for p in now_ok if defines_nothing(p, [stub], tmp)]

            print(f"\nwith a real sdkconfig.h ({defines} #defines): "
                  f"{len(now_ok)} of the {len(base_fail)} compile")
            print(f"  of those, {len(empty)} define no symbols at all")
            print(f"  leaving {len(now_ok) - len(empty)} that gained anything")
            print("  what the rest now ask for first:")

            for header, count in blockers.most_common(5):
                print(f"    {count:4d}  {header}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
