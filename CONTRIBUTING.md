# Contributing


Thank you for considering contributing to EzloPi! We value your interest and support.

Contributions to EzloPi can take various forms, such as testing newly released features, providing pull requests for new features, proof of concepts, language files, or fixes, and contributing missing documentation for features and devices in our documentation.

This document outlines guidelines for interacting with the issue tracker and pull requests within this repository to ensure a streamlined and effective collaboration process.

## Opening New Issues

1. Opening an issue signifies the existence of a problem in the code that requires attention from the project contributors.
2. When creating an issue, it is mandatory to complete the provided template. The requested information is crucial! Ignoring the template or providing insufficient information may result in the closure of the issue.
4. Issues concerning topics already covered in the documentation will be similarly closed.
5. Issues related to unmerged PRs will be closed. Any issues with a PR should be documented within the PR itself.
6. Issues accompanied by investigations revealing the root cause of the problem will be prioritized.
7. Duplicate issues will be closed.

## Triaging of Issues/PR's

1. Any project contributor can participate in the triaging process if they choose to do so.
2. Issues that need closure, either due to non-compliance with this policy or other reasons, should be closed by a contributor.
3. Accepted issues should be labeled appropriately.
4. Issues with the potential to impact functionality for many users should be considered severe.
5. Issues resulting from the SDK or chip should not be labeled severe, as there might be limited solutions. Use common sense when deciding, and document such issues in the Wiki for user reference.
6. Feature requests' viability and desirability should be discussed.
7. Feature requests or changes addressing specific/limited use cases, especially at the expense of increased code complexity, may be denied or may require redesign, generalization, or simplification.
8. Feature requests not accompanied by a PR:
   - May be closed immediately (denied).
   - May be closed after a predetermined period (left as a candidate for someone to pick up).

## Pull Requests

A Pull Request (PR) is the process through which code modifications are managed on GitHub.

The process is straightforward.

- Fork the EzloPi Repository git repository.
- Write/Change the code in your Fork for a new feature, bug fix, new sensor, optimization, etc.
- Header, Source and CMakeList files should accurately follow [this](ezlopi-src-template/Readme.md) guideline.
- Ensure tests pass — see [Checks your PR has to pass](#checks-your-pr-has-to-pass) below.
- Create a Pull Request against the default branch of this fork. (If you are
  contributing to [upstream](https://github.com/ezloteam/Ezlo_Pi) instead,
  target its **development** branch.)

1. Modify only relevant files. Check `git diff` before you push — in
   particular, a reformat-on-save editor will happily rewrite an entire file
   you only meant to change two lines of, and a diff like that is very hard to
   review.
2. `sdkconfig` is generated and git-ignored. Never commit it. If you need to
   change a build option permanently, change `sdkconfig.defaults.<target>`.

## Checks your PR has to pass

"Ensure tests pass" used to be the entire instruction here, and there was
nothing to run. There is now. Everything below is also run automatically by
[`.github/workflows/ci.yml`](.github/workflows/ci.yml) on every push, so you
can let CI tell you — but running it locally is faster.

### Host tests — no hardware or toolchain needed

```sh
make -C tests/host
```

This needs only a C compiler. It compiles the real firmware sources — no copies
— into seventeen binaries, every one of them covering something that parses or
validates input it did not produce: the helpers in
[`ezlopi-util/ezlopi-util-sanitize`](ezlopi-util/ezlopi-util-sanitize/), the
vendored cJSON extensions, the Improv state machine, the Home Assistant
discovery builder, the ZE08-CH2O, PMS5003, MB1013 and GY-GPS6MV2 parsers, the
UUID generator, the action-name table, the local-key, MQTT-config and
websocket-session helpers, the device-configuration recovery counter and
validator, and the `hub.mqtt.config.*` and `hub.ezlopi.config.*` API handlers.
This sentence used to say "eight areas" and was last accurate at eight
binaries. Everything is built under
AddressSanitizer and UndefinedBehaviorSanitizer, and under `-Wall -Wextra
-Werror` except where [`tests/host/Makefile`](tests/host/Makefile) documents a
scoped exemption for vendored or third-party code. Then it runs the assertions
in [`tests/host/`](tests/host/). It should end with:

```
=== build/test_sanitize ===
72 check(s), 0 failure(s)
=== build/test_cjext ===
62 check(s), 0 failure(s)
=== build/test_improv ===
2302 check(s), 0 failure(s)
=== build/test_ha ===
  swept 70564 combination(s): 56948 mapped, 13616 rejected
183 check(s), 0 failure(s)
=== build/test_ze08 ===
26 check(s), 0 failure(s)
=== build/test_pms5003 ===
92 check(s), 0 failure(s)
=== build/test_mb1013 ===
  swept 10000 four-digit frames: 0 unrecognised, 0 mismatched
50 check(s), 0 failure(s)
=== build/test_gps6mv2 ===
  swept 100 satellite counts after a 2-digit count: 0 wrong
146 check(s), 0 failure(s)
HOST TESTS PASSED
```

The per-binary counts will drift as tests are added; `HOST TESTS PASSED` on the
last line is the part that matters, and the `Makefile` only prints it when every
binary exited 0.

**If you touch any code that parses input arriving over BLE, UART or the
network, add tests here.** `ezlopi-util-sanitize` is deliberately
dependency-free — `idf_component_register` is called with an empty `REQUIRES` —
precisely so it can be compiled by a plain host compiler. Please keep it that
way; adding an ESP-IDF dependency to it makes the whole suite unbuildable
off-target.

There are two patterns to copy, and which one applies depends on the driver.

ZE08-CH2O needed no restructuring: its dependency on ESP-IDF is confined to type
declarations in one header, so `tests/host/stubs/` supplies the handful of
headers underneath that one and the *real* driver gets compiled as it stands.

PMS5003 is the other case, and the more common one. `pms5003.c` configures GPIOs
and creates an `esp_timer`, so it cannot be compiled on a host at all — but its
frame parser depends on nothing except the C library. The parser therefore moved
into `pms5003_parse.c`, with `pms5003.h` including `pms5003_parse.h` and never
the reverse, and only that file is compiled here. Note that
`tests/host/stubs/driver/gpio.h` deliberately declares *no functions*: a driver
that actually drives a pin is meant to fail to link rather than be stubbed
around, because the stub would then hide the dependency instead of forcing this
split. If you find yourself wanting to add function declarations to that stub,
the seam is what you actually want.

MB1013 is the same split, and it shows how little the parser usually needs once
the hardware plumbing is on the other side of a function call: its rule in the
`Makefile` passes no `-Istubs` at all, and `mb1013_parse.c` imports nothing from
libc or ESP-IDF — only three libgcc soft-float helpers, for the one division
that turns the raw reading into metres. Aim for that. When a parser takes a
buffer and a length and returns a value, the ESP-IDF surface it needs tends to
be zero.

GY-GPS6MV2 is a third case, and the cheapest of the three: look for it before
assuming you need to move any code. `GYGPS6MV2_parse_gpgga_mesg()` was already a
standalone function in a file whose only other includes are `<string.h>` and the
trace macros. The single thing keeping it off a host compiler was its *header*,
which declared the NMEA layout and one cloud-action prototype together, so
including it dragged in the device and item core, cJSON and the actions list —
for a prototype `gyGPS6MV2.c` never calls. Splitting the layout into
`gps6mv2_parse.h`, with `sensor_0053_UART_GYGPS6MV2.h` including it rather than
the reverse, took that file's header closure from 170 headers to 123, its
`ezlopi_core_*` headers from eight to two, and its cJSON headers to none —
measured with `ninja -C build -t deps`, which is also how to check your own
split did what you think. No function moved.

So before splitting a `.c` file, check whether the dependency is really in the
code or only in the header it includes. Two headers — one for the layout and the
parser, one for the cloud plumbing — is often the whole job.

The remaining UART driver is R307, and it is the counter-example that makes the
paragraph above worth actually doing rather than assuming the answer. Checked
the same way, its dependency is in the *code*: `R307_AS606.c` calls
`esp_timer_get_time()` 31 times, `vTaskDelay()` nine times and
`uart_write_bytes()` six times, from function bodies. No header move touches
any of that. The specific obstacle is `__Response_function()`, which interleaves
the confirmation-code `switch` with an `esp_timer_get_time()`/`vTaskDelay()`
loop polling for the reply — the waiting and the interpreting are the same
function, so it needs the PMS5003 treatment and not the GY-GPS6MV2 one.

So the check cuts both ways, and the two drivers were mis-estimated in opposite
directions before anyone ran it: GY-GPS6MV2 was assumed expensive and was a
header split, R307 was assumed cheap for the same reason and is the most
expensive of the five. `grep -c` on the function bodies is most of the
measurement.

Some code cannot reach this suite at all, and it is worth saying so plainly
rather than quietly leaving it uncovered. R307's `generate_packet()` is
`static`, in no header, in a translation unit that needs `uart_write_bytes()`,
`esp_timer` and `vTaskDelay` — so a bug fixed in it in this release has no test
here, and the CHANGELOG says so instead of implying coverage. If you are in that
position, a throwaway program that copies the old and new function side by side
and runs both under AddressSanitizer is worth far more than nothing: it is how
the boundaries in that entry were measured rather than argued, and it caught two
errors in the reasoning — the over-read starts two bytes later than expected,
because `data[64..65]` are still inside the allocation and so invisible to the
sanitiser, and a non-terminating loop could not be observed at all until the
packet was moved into an oversized slab, because ASan aborted on the over-read
first. Neither would have been noticed by reading the code. Delete the program
afterwards; record the numbers.

The opposite failure is worth the same warning, and it is more embarrassing:
code that needed no seam at all and still had no tests. `ezlopi-util-uuid` ties
MB1013 for the shortest build rule in `tests/host/Makefile` — one `-I`, its own
directory, no stubs — but MB1013 got there by being split, and this needed
nothing: `ezlopi_util_uuid.c` includes `<stdlib.h>`, `<string.h>` and
`<time.h>`, its header includes nothing, and the whole file compiles unmodified.
It could have been tested from the day it was written. When somebody finally did, `ezlopi_util_uuid_parse()` had
four defects, including a read past the end of its input buffer on any string
shorter than 32 hex digits, and a round trip through the component's own
`unparse()` that did not return the original UUID. So before reaching for the
dependency argument, check whether there is a dependency: `grep` the includes
first. The answer is sometimes that nobody has written the file.

That component is also a reminder to check *reachability* before writing
"fixed a bug" anywhere. Nothing in this repository calls any function in
`ezlopi-util-uuid`. This paragraph used to go on to say "it compiles into the
image as dead code", and that was an assumption nobody had checked: `nm` on
the linked esp32c3 ELF finds no `ezlopi_util_uuid_*` symbol, and a later change
to the component left all four images byte-identical in size. The linker
discards the whole thing. The defects were real and are now tested, but they
were not merely unreachable, they were absent — and the CHANGELOG says so in
those words.

Worth noting how that error survived: "compiles into the image" is the kind of
clause that sounds like a measurement and is actually a guess, and it was
repeated into three files before anything disproved it. If you write a claim
about the built artifact, run `nm` on the artifact.

**That sweep has already been run, so don't run it again.** The obvious way to
generalise the paragraph above is to ask the compiler which other files are in
the same position — `gcc -fsyntax-only -I<the file's own directory> <file>`,
with no stubs and no other include path. When that sweep was first run, 27 of
the files that passed had no tests. The honest result is that the list is
nearly empty of opportunity:

| what they turned out to be | count |
| --- | --- |
| tables of string literals (mostly `ezlopi-cloud-constants`) | 13 |
| a licence header and nothing else | 4 |
| a single `#include` of their own header | 3 |
| wrapped in `#if 0` from first line to last | 3 |
| only a header include guard, in a `.c` file | 1 |
| a `.c` file that is only `#include`s and defines nothing | 1 |
| inactive blocks of device provisioning credentials | 1 |
| actual executable logic | **1** |

The one file with logic in it was `ezlopi-core-actions`, and it did have a
defect. But the lesson is that "compiles with no stubs" selects for *emptiness*
far more strongly than for portability, and `ezlopi-util-uuid` was a genuine
outlier rather than the first of a series.

So where *is* the lever? This paragraph used to answer "a stub `sdkconfig.h`,
which is the first missing header for 69 of the 214 files that do not compile".
The first half is still true — it is the top blocker, 70 files as of this
writing — and the conclusion drawn from it was wrong.

The sweep is now a committed script,
[`tests/ci/host_reachable.py`](tests/ci/host_reachable.py), so every figure
below can be re-derived instead of trusted; a number that cannot be checked is
a number nobody can correct. It states its own file set, including the two
directories it deliberately leaves out, because the earlier totals could not be
compared against anything without knowing that.

It currently reports 36 of 255 first-party `.c` files compiling with nothing
but their own directory on the include path. Ten of the 36 now have host
tests — `ezlopi-core-actions`, `mb1013_parse`, and the `ezlopi-util` components
`-devcfg`, `-ha`, `-improv`, `-localkey`, `-mqttcfg`, `-sanitize`, `-uuid`,
`-wssession`. The remaining 26 are the same kinds of thing the table above
describes: fourteen of them are `ezlopi-cloud-constants` string tables.

Being in the 36 is not the same as being testable, in either direction. Two
components with substantial test files are not in it: `ezlopi-util-devcfgval`
and `ezlopi-cloud-devcfg`, both because they read a `cJSON` tree and so need
`cjext.h`, which is the fifth most common blocker at 14 files. The second of
the two is the clearer case — it is host-tested by moving the six calls that
need `<esp_partition.h>` and the HAL into a separate port file and faking them,
so the component as a whole is tested while one of its two `.c` files remains
unreachable by this measure and is counted as such. The 36 is a measure of how
little a file includes, which is why the rest of this section is about how
badly that correlates with anything.

| include path | of the 219 failures, now compile | …of those, define no symbols | net gain |
|---|---|---|---|
| plus an empty `sdkconfig.h` | 52 | **52** | **0** |
| plus a real generated `sdkconfig.h` (510 `#define`s) | 9 | **9** | **0** |

Those last two rows depend on *which* configuration, which is worth saying
because re-running the script will not reproduce them exactly. Against a
differently configured 504-`#define` header the second row reads 11 and 11, and
the blocker counts below shift by one or two — the conclusion, that the net
gain is zero either way, is what survives the difference.

Every file `sdkconfig.h` blocks is a file wrapped end to end in
`#ifdef CONFIG_SOMETHING`. An empty stub lets 52 through precisely because it
leaves their bodies switched *off*: each compiles to an object with no symbols
in it, and a test linked against one would pass while exercising nothing. Hand
them the real configuration and the count *falls* to 9, because switching the
bodies on makes them need ESP-IDF. After `sdkconfig.h` the blockers are
`ezlopi_core_cloud.h` (41), `ezlopi_util_trace.h` (31), `cjext.h` (22) and
`freertos/FreeRTOS.h` (22).

The real lever is the unglamorous one: stub those four, which is what
`tests/host/stubs/` already does for the handful of files that needed them.
Each is a judgement call about how much of ESP-IDF to reimplement, which is why
this suite has grown a file at a time instead of in a sweep.

A stub `sdkconfig.h` is still sometimes the right answer — see
`tests/host/stubs-mqtt-api/`, which exists to switch a subject *on* — but it
belongs to one test rather than to the shared stub directory, for the reason
that file documents.

That table is generated, not counted by hand, and the first version of it was
wrong in a way worth naming: it claimed 15 string tables by quietly folding in
three files that are each their own oddity — `ezlopi_cloud_thermostat_str.c`
contains nothing but a header's `#ifndef`/`#define`/`#endif`,
`ezlopi_cloud_constants.c` is eleven `#include`s that define nothing and expand
to 1081 lines of `extern` declarations, and `ezlopi_test_prov.c` is discussed
below. Eight buckets that sum to 27 beats five buckets that sum to 27 by
rounding.

Two traps in measuring this, both of which produced confident wrong answers
first:

- **Count what the compiler sees, not what the file says.** A first pass at
  ranking those 27 files by branch points stripped comments and counted
  `if`/`for`/`while`/`switch` in the text. It nominated `ezlopi_cloud_data.c`
  (9 branches) and `ezlopi_core_event_queue.c` (8) as the only two worth
  testing. Both are inside `#if 0` end to end — every branch counted was text
  the compiler never compiles, and the dead block is also why they appeared in
  the zero-stub list at all, since their `#include "freertos/FreeRTOS.h"` is
  inside it too. Run the file through `gcc -E -P` and measure the output.

- **A missing tool scores a pass.** Checking "does this symbol survive into the
  ELF?" with `xtensa-esp32-elf-nm ... | grep -c` reported `0` — the hoped-for
  answer — while `nm` was not on `PATH` at all and had exited 127 printing
  nothing. `grep -c` on empty input is `0`. Verify the tool exists and abort if
  it does not, and sanity-check the total symbol count before trusting a zero.
  Related: `cmd | sed '...'` followed by `echo $?` reports *sed's* status, which
  is how one of these scripts announced success for a check that could not
  succeed.

When you add a test, check that it would actually have caught the bug: break
the fix on purpose, confirm the suite goes red, then put it back. A test that
passes against the broken code is evidence of nothing.

A related trap, which the `ezlopi-core-actions` test was built to avoid:
when the data under test is generated from a list, don't check it against that
same list. The bug there was one wrong string in an X-macro table, and a test
comparing `ezlopi_action_names[]` to the table's own string column would have
confirmed the bug rather than finding it. Rebuild the expectation from the
other column — in that case from the macro's token with `#action` — so the two
halves can disagree.

Sweep inputs rather than picking one, and the uuid suite shows why in numbers.
Run its 256 round-trip patterns against the old parser and 240 fail — but 16
*pass*, because a nibble-level misalignment is invisible whenever the adjacent
nibbles happen to repeat. A single hard-coded UUID had about a one-in-sixteen
chance of turning that bug into a green run.

Two traps there, both of which this suite hit for real. First, if your deliberate
break stops the file *compiling* — deleting a bounds check often removes the
only use of the length parameter, and `-Werror=unused-parameter` then fails the
build — the compiler caught it and your test is still unproven. Make the
condition unreachable instead of deleting it. Second, if a break is *not* caught,
work out whether it is actually unobservable before assuming the test is weak:
MB1013's payload-scan bound can be widened with no effect, because the scan stops
at the first non-digit and the caller has already established the byte after the
payload is `'\r'`. That is a sound argument, but it rests on the terminator
check, which is why `test_terminator_is_required()` exists.

And sometimes a survivor is simply a hole, so do the work of deciding which.
GY-GPS6MV2 has one branch that is not reached by a comma — `'*'` opens the
checksum field by assigning `comma_count` a literal rather than incrementing it
— and a mutation changing that literal from 15 to 14 survived. The reason was
not subtle once looked at: field 14 is an empty `case`, the suite asserted every
other field's *value* but only the checksum's *termination*, and a field that is
never written is trivially terminated. One `check_str` closed it. The suite was
weak in exactly one place and the mutation found it, which is the entire point —
treat a survivor as a question, not as a result.

### Firmware build

```sh
idf.py set-target esp32   # or esp32s2 / esp32s3 / esp32c3
idf.py build
```

Use **ESP-IDF v4.4.6** exactly; the project does not build on v5.x. CI builds
all four targets, so a change that only compiles on one of them will be caught
even if you cannot build the others locally.

If you were thinking of sending a patch towards v5 or v6: please read
[docs/esp-idf-migration.md](docs/esp-idf-migration.md) first. It has the
measured error count broken down by root cause, and most of the work turns out
to be four or five mechanical sweeps rather than a per-file slog — so
uncoordinated per-file patches will conflict with each other for no gain.

### Repository hygiene

CI will fail your PR if:

- The old hard-coded `opentelemetry.ezlo.com` endpoint reappears anywhere.
- `CONFIG_EZPI_ENABLE_OPENTELEMETRY=y` is committed into any
  `sdkconfig.defaults*` file. Open-telemetry has no default endpoint, so
  enabling it by default produces a build that retries a connection forever.
- `sdkconfig.defaults` or any `sdkconfig.defaults.<target>` goes missing. The
  base file is load-bearing even though it is nearly empty: IDF only looks for
  `.<target>` variants of files already in `SDKCONFIG_DEFAULTS`, which starts
  out holding just `sdkconfig.defaults`. Remove it and every per-target file is
  ignored *silently*, and the build still succeeds with the wrong
  configuration.

### Things worth knowing before you debug something

- **Device instantiation is boot-only.** Devices are built once at startup
  (`EZPI_device_prepare()`), so a configuration written over BLE or UART only
  takes effect after a reboot. This is not a bug in your change.
- **Do not let anything convert line endings across the whole tree.** The repo
  ships precompiled archives such as
  `ezlopi-components/Bosch_BSEC2/precompiled_binaries/esp32/libalgobsec.a`.
  Stripping `0x0D` bytes from one of those corrupts the object code and the
  link fails with `error adding symbols: malformed archive`, which reads like a
  project bug but is not.
- **On Windows, set `core.autocrlf=false` in the clone before you do anything
  else.** Git for Windows ships `core.autocrlf = true` in its *system* config
  (`/etc/gitconfig` inside the install, not your `--global`), so checking out
  this repo with it rewrites the working tree with CRLF. Nothing appears wrong
  until a second Git disagrees: a clone used from both Git for Windows and WSL
  will show hundreds of files as modified to whichever one has `autocrlf`
  unset. In this tree that was 773 files whose only difference from `HEAD` was
  one `0x0D` per line.

  The hazard is not the noise, it is what the noise hides. Staging one of those
  files commits a whole-file line-ending change with your real edit buried
  inside it, and `git diff` on a 120-line file reports `@@ -1,121 +1,121 @@`
  either way. Fix it per clone:

  ```sh
  git config --local core.autocrlf false
  git config --local core.eol lf
  git checkout -- .          # only after confirming nothing real is pending
  ```

  Two traps if you go looking at this yourself. `git diff --name-only
  --ignore-cr-at-eol` still lists every CRLF-only file — the filter applies to
  `--numstat` and to the patch text, not to the decision about which names to
  print, so the name list looks like 777 real changes. And in `bash`,
  `grep -c $"\r"` is i18n quoting, not an escape: it counts lines containing the
  letter `r`. Use `tr -dc '\r' < file | wc -c`.

  A repo-wide `.gitattributes` would be the general fix, but it is not a safe
  one-line addition here: some blobs are already committed with CRLF — the
  vendored `bme280`, `lib8tion`, `color` and `led_strip` components among them —
  so `* text=auto eol=lf` renormalises them and produces exactly the tree-wide
  rewrite the bullet above warns about. Left alone deliberately.

### Reporting a security problem

Please do not open a public issue or PR for a security vulnerability. See
[SECURITY.md](SECURITY.md).

--------------------------------------

## Contributor License Agreement (CLA)

```
By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the EASL license; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the EASL license; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it) is maintained indefinitely
    and may be redistributed consistent with this project or the open
    source license(s) involved.
```

We appreciate your contributions and adherence to these guidelines, making the EzloPi project better for everyone.