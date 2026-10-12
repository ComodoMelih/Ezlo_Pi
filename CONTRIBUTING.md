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
— for five areas that handle untrusted input: the helpers in
[`ezlopi-util/ezlopi-util-sanitize`](ezlopi-util/ezlopi-util-sanitize/), the
vendored cJSON extensions, the Improv state machine, the Home Assistant
discovery builder and the ZE08-CH2O frame parser. Everything is built under
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
183 check(s), 0 failure(s)
=== build/test_ze08 ===
26 check(s), 0 failure(s)
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

Note what makes the ZE08 case the pattern to copy: its dependency on ESP-IDF is
confined to type declarations in one header, so `tests/host/stubs/` can supply
the handful of headers underneath it and the *real* driver gets compiled. The
other four UART drivers cannot join yet — they reach into cJSON, the device and
item core, FreeRTOS tasks and GPIO — which is the concrete argument for giving
them the same parser/hardware seam.

When you add a test, check that it would actually have caught the bug: break
the fix on purpose, confirm the suite goes red, then put it back. A test that
passes against the broken code is evidence of nothing.

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