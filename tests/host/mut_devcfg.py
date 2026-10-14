#!/usr/bin/env python3
"""Does test_devcfg_api.c actually have teeth?

A passing test suite proves nothing on its own: a check that cannot fail is not
a check. This breaks ezlopi_cloud_devcfg.c one deliberate way at a time,
rebuilds, and reports how many assertions notice. A mutation that nothing
notices is a gap in the test, not a quirk of the mutation.

Run from tests/host:

    python3 mut_devcfg.py

It edits the subject in place and restores it from a copy in /tmp afterwards,
including when a mutation fails to build. Check `git diff` if it is interrupted.

Reading the output:

  - the control row must be "0 failures", or the whole run is meaningless
  - every other row should show failures, OR be marked an equivalent mutant
  - BUILD-FAILED means the mutation did not compile, which is NOT a caught
    mutation -- it means the mutation was badly chosen and tested nothing. The
    region-size row below is written the way it is for exactly this reason.
  - a crash (sanitizer or SEGV) counts as caught, but an assertion is better:
    it says what broke rather than just that something did.

One row is expected to survive and is labelled. See its comment.
"""

import subprocess, shutil

SUBJ = "../../ezlopi-cloud/ezlopi-cloud-devcfg/ezlopi_cloud_devcfg.c"
ORIG = "/tmp/devcfg_subject.orig"
shutil.copyfile(SUBJ, ORIG)

MUTS = [
    ("control (no change)", None, None),
    ("collapse every validator token into one",
     'EZPI_util_devcfgval_token(reason),\n                                        bad_index);',
     '"devcfg.bad",\n                                        bad_index);'),
    ("never check that a driver exists",
     'false ==\n                             __every_id_item_has_a_driver(cj_config,\n                                                          &bad_index)',
     'false && __every_id_item_has_a_driver(cj_config, &bad_index)'),
    ("ignore the validator verdict",
     "if (EZPI_DEVCFGVAL_OK != reason)",
     "if (false && (EZPI_DEVCFGVAL_OK != reason))"),
    # EQUIVALENT MUTANT, kept to document why it is not a gap. When
    # serialised_len == limit both forms yield `limit`: the true branch gives
    # limit, the false branch gives (uint32_t)serialised_len, which is limit.
    # The clamp exists only to stop a >4GiB length wrapping to a small number,
    # so it has no behavioural boundary to detect. Expect 0 failures.
    ("off-by-one in the size clamp [equivalent mutant]",
     "(serialised_len > (size_t)limit)",
     "(serialised_len >= (size_t)limit)"),
    # The real size decision: what limit the validator is told to enforce.
    ("hand the validator a limit one byte too generous",
     "measured, limit, &bad_index);",
     "measured, limit + 1u, &bad_index);"),
    ("mis-measure the document as one byte shorter than it is",
     "? limit\n                            : (uint32_t)serialised_len;",
     "? limit\n                            : (uint32_t)(serialised_len - 1u);"),
    ("treat a failed write as success",
     "else if (false == EZPI_CLOUD_devcfg_port_write(cj_config))",
     "else if ((EZPI_CLOUD_devcfg_port_write(cj_config), false))"),
    # Swapped rather than replaced: dropping `region` entirely trips
    # -Werror=unused-variable and the mutant never builds, which would read as
    # "caught" without any check having run.
    ("invert the region-size fallback",
     "(0u != region) ? region : DEVCFG_FALLBACK_LIMIT;",
     "(0u != region) ? DEVCFG_FALLBACK_LIMIT : region;"),
    ("accept a non-object params.config",
     "if (false == cJSON_IsObject(cj_config))",
     "if (false && (false == cJSON_IsObject(cj_config)))"),
    ("stop requiring params",
     "if (NULL == cj_params)",
     "if (false && (NULL == cj_params))"),
    ("report badIndex on every error, including the ones it does not fit",
     "if (bad_index >= 0)",
     "if (bad_index >= -1)"),
    ("leak the string that get reads",
     "EZPI_CLOUD_devcfg_port_free(stored);",
     "(void)0;"),
]

HOST = "cd /mnt/c/Users/admin/ezlopi_audit/Ezlo_Pi/tests/host && "


def run(cmd):
    return subprocess.run(["bash", "-lc", HOST + cmd],
                          capture_output=True, text=True)


results = []
for name, old, new in MUTS:
    shutil.copyfile(ORIG, SUBJ)
    if old is not None:
        s = open(SUBJ, newline="").read()
        if s.count(old) != 1:
            results.append((name, "PATCH-FAILED (%d matches)" % s.count(old)))
            continue
        open(SUBJ, "w", newline="").write(s.replace(old, new))

    b = run("rm -f build/test_devcfg_api build/devcfg_api_subject.o && "
            "make build/test_devcfg_api 2>&1")
    if b.returncode != 0:
        results.append((name, "BUILD-FAILED"))
        continue

    r = run("./build/test_devcfg_api 2>&1")
    tail = [l for l in r.stdout.strip().splitlines() if "checks," in l]
    if tail:
        verdict = tail[-1]
    elif r.returncode != 0:
        verdict = "CRASHED rc=%d (sanitizer or abort)" % r.returncode
    else:
        verdict = "NO-SUMMARY rc=%d" % r.returncode
    results.append((name, verdict))

shutil.copyfile(ORIG, SUBJ)

print()
print("%-56s %s" % ("MUTATION", "RESULT"))
print("-" * 92)
for n, r in results:
    print("%-56s %s" % (n, r))
