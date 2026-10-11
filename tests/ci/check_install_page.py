#!/usr/bin/env python3
"""Verify an assembled install/ directory before it can be published.

Usage: check_install_page.py <assembled-install-dir>

Why this exists
---------------
Every failure this page can have is silent. A manifest that points at a path
which does not exist renders a working-looking button that fails only once a
user has plugged a board in and clicked it. A chipFamily typo means the right
image is never offered for that chip, and the user is told their board is
unsupported. A leftover "__VERSION__" ships a page claiming a version it does
not have. None of that breaks a build, and none of it is visible without
flashing a real board of each kind.

So the page is checked the same way the firmware is: by asserting the things
that have to be true, and by being able to fail.

The checks
----------
1.  manifest.json parses, and has the keys ESP Web Tools needs.
2.  Every builds[].parts[].path exists, is plausibly an ESP image, and is
    flashed at offset 0 -- the only offset valid for a merged image.
3.  Every chipFamily is one ESP Web Tools actually knows, and matches the
    directory its image was taken from.
4.  No image in the directory is left unreferenced by the manifest. This is the
    check that catches a target being added to the CI matrix and not to the
    manifest: without it, the new target's image would be published and never
    offered to anyone.
5.  Neither the manifest nor the page still contains the version placeholder,
    so the stamping step cannot be skipped unnoticed.
"""
import json
import os
import sys

# The spelling ESP Web Tools expects, per its documentation, mapped from the
# idf.py set-target name used for the directory. Getting this wrong is the
# failure mode with no symptom: the tool compares the detected chip against
# these strings, and an unrecognised one simply never matches.
CHIP_FAMILY = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
}

PLACEHOLDER = "__VERSION__"
IMAGE_NAME = "merged-firmware.bin"

# An ESP-IDF image starts with 0xE9. In a merged image the first thing flashed
# is the bootloader, which sits at 0x1000 on ESP32 and ESP32-S2 but at 0x0 on
# ESP32-S3 and ESP32-C3, so the magic byte is at one of those two offsets and
# which one is not something this script should hard-code per target.
MAGIC_OFFSETS = (0x0, 0x1000)

problems = []


def problem(msg):
    problems.append(msg)


def main():
    if len(sys.argv) != 2:
        print("usage: check_install_page.py <assembled-install-dir>")
        return 2

    root = sys.argv[1]
    manifest_path = os.path.join(root, "manifest.json")
    index_path = os.path.join(root, "index.html")

    if not os.path.isfile(manifest_path):
        print("::error::%s does not exist" % manifest_path)
        return 1

    try:
        with open(manifest_path, encoding="utf-8") as fh:
            manifest = json.load(fh)
    except ValueError as exc:
        print("::error::%s is not valid JSON: %s" % (manifest_path, exc))
        return 1

    print("manifest: %s" % manifest_path)

    # --- 1. shape -----------------------------------------------------------
    for key in ("name", "builds"):
        if key not in manifest:
            problem("manifest has no %r key" % key)

    builds = manifest.get("builds")
    if not isinstance(builds, list) or not builds:
        problem("manifest 'builds' must be a non-empty list; a page with no "
                "builds offers nothing to anyone")
        print_problems()
        return 1 if problems else 0

    # --- 5a. version was stamped -------------------------------------------
    version = manifest.get("version", "")
    if PLACEHOLDER in str(version):
        problem("manifest version is still %r -- the stamping step did not run,"
                " and the page would tell users it is a build that does not "
                "exist" % PLACEHOLDER)
    else:
        print("version:  %s" % version)

    # Documented default is 10 seconds of waiting for an Improv Serial reply.
    # Not an error either way, but worth stating in the log which behaviour is
    # being published, because it is the difference between "install finishes"
    # and "install appears to hang".
    wait = manifest.get("new_install_improv_wait_time")
    if wait == 0:
        print("improv:   Improv Serial detection disabled (no Improv Serial in "
              "the firmware yet)")
    else:
        print("improv:   will wait %s s for an Improv Serial reply"
              % (wait if wait is not None else "10 (default)"))

    # --- 2 and 3. every build is usable ------------------------------------
    referenced = set()
    seen_families = set()

    for i, build in enumerate(builds):
        where = "builds[%d]" % i
        family = build.get("chipFamily")

        if family in seen_families:
            problem("%s repeats chipFamily %r; only the first would ever be "
                    "used" % (where, family))
        seen_families.add(family)

        if family not in CHIP_FAMILY.values():
            problem("%s has chipFamily %r, which is not one of %s. ESP Web "
                    "Tools matches the detected chip against this string, so an"
                    " unrecognised value means that chip is never offered a "
                    "build." % (where, family, sorted(CHIP_FAMILY.values())))

        parts = build.get("parts")
        if not isinstance(parts, list) or len(parts) != 1:
            problem("%s must have exactly one part: a merged image flashed at "
                    "offset 0. Separate images are not accepted for ESP-IDF v4 "
                    "projects." % where)
            continue

        part = parts[0]
        offset = part.get("offset")
        if offset != 0:
            problem("%s part offset is %r, expected 0. The merged image begins "
                    "at the start of flash." % (where, offset))

        rel = part.get("path")
        if not isinstance(rel, str) or not rel:
            problem("%s part has no usable 'path'" % where)
            continue

        referenced.add(rel)
        abs_path = os.path.join(root, rel)

        if not os.path.isfile(abs_path):
            problem("%s points at %r, which does not exist in the assembled "
                    "page. The install button would render and then fail after "
                    "the user plugged a board in." % (where, rel))
            continue

        # The directory an image came from has to agree with the chipFamily
        # claimed for it. Publishing an ESP32-C3 image as ESP32-S3 is a brick,
        # and the paths are the only record of which build is which.
        target_dir = rel.split("/")[0]
        expected = CHIP_FAMILY.get(target_dir)
        if expected is None:
            problem("%s image lives under %r, which is not a known target "
                    "directory (%s)" % (where, target_dir,
                                        ", ".join(sorted(CHIP_FAMILY))))
        elif expected != family:
            problem("%s claims chipFamily %r but its image came from the %r "
                    "build, which is %r. Flashing it would write the wrong "
                    "chip's firmware." % (where, family, target_dir, expected))

        size = os.path.getsize(abs_path)
        with open(abs_path, "rb") as fh:
            head = fh.read(MAGIC_OFFSETS[-1] + 1)

        if not any(len(head) > off and head[off] == 0xE9 for off in MAGIC_OFFSETS):
            problem("%s image %r has no ESP image magic (0xE9) at 0x0 or 0x1000"
                    ". It is not a flashable image -- a truncated download or "
                    "an error page would look like this." % (where, rel))
        else:
            print("  ok      %-10s %-34s %8d bytes" % (family, rel, size))

    # --- 4. nothing published but unreachable ------------------------------
    for target in sorted(CHIP_FAMILY):
        on_disk = os.path.join(root, target, IMAGE_NAME)
        rel = "%s/%s" % (target, IMAGE_NAME)
        if os.path.isfile(on_disk) and rel not in referenced:
            problem("%s was published but no build in the manifest references "
                    "it, so it can never be flashed. A target was most likely "
                    "added to the CI matrix without being added to the "
                    "manifest." % rel)

    # --- 5b. the page itself -----------------------------------------------
    if not os.path.isfile(index_path):
        problem("index.html is missing; there is no page to publish")
    else:
        with open(index_path, encoding="utf-8") as fh:
            page = fh.read()
        if PLACEHOLDER in page:
            problem("index.html still contains %r" % PLACEHOLDER)
        if "manifest.json" not in page:
            problem("index.html does not reference manifest.json, so the "
                    "install button has nothing to install")

    return print_problems()


def print_problems():
    if problems:
        for p in problems:
            print("::error::install page: %s" % p)
        print("::error::%d problem(s); refusing to publish the install page."
              % len(problems))
        return 1
    print("install page is consistent and every advertised image is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
