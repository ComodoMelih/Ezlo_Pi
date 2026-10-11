# Changelog

All notable changes in this fork, relative to upstream
[ezloteam/Ezlo_Pi](https://github.com/ezloteam/Ezlo_Pi) at commit `f2a74e5`.

Each entry says what was wrong, not just what changed — several of these bugs
were invisible in normal use, and the reason they stayed hidden is usually the
most useful part.

Severity markers:

- **[sec]** — reachable from an unauthenticated BLE or UART client.
- **[bug]** — incorrect behaviour, not a security issue.
- **[infra]** — build, test, packaging or documentation.

---

## Unreleased

### Memory safety — [sec]

All four of these are reachable by anyone in BLE range, or anyone with physical
access to the UART header. No pairing or authentication is required to reach
the parsers involved. All four are covered by regression tests in
[`tests/host/`](tests/host/).

- **Stack buffer overflow parsing a device `chipset` string.**
  `ezlopi_core_devices.c` copied an attacker-supplied `chipset` value into a
  10-byte stack buffer using the length from the JSON document. A chipset
  string of 10 bytes or more overran the buffer; an exactly-10-byte one left it
  unterminated, so the `strncmp` immediately after read off the end of it. The
  configuration being parsed arrives over BLE characteristic `0xE501` or over
  UART `cmd:3`, so the length is fully controlled by the client. Now clamped
  and always NUL-terminated via `EZPI_util_copy_str_bounded`.

- **Unbounded copy in the chunked BLE config assembler.**
  `ezlopi_service_ble_dynamic_config.c` trusted the `len` field of each
  `0xE501` chunk when copying into the reassembly buffer, rather than the space
  actually remaining. A client that declared a larger `len` than it sent, or
  that kept sending chunks past `total_len`, wrote past the end of the buffer.
  Now bounded by `EZPI_util_clamp_copy_len`, which takes the remaining
  destination space into account.

- **Double free in the UART reset handler.**
  `ezlopi_service_uart.c` called `cJSON_Delete` on `cj_sub_cmd`, which was
  borrowed from `cj_root` via `cJSON_GetObjectItem` and therefore still linked
  into that tree. `cj_root` was then left holding a freed child, and the caller
  deleted `cj_root` in turn. This went unnoticed because both *handled*
  sub-commands reboot the device before reaching that line — only an
  unrecognised `sub_cmd` hit it, which is exactly what a fuzzer or a mistyped
  command sends.

- **Plaintext Wi-Fi PSK written to the serial console.**
  `hub.network.get` reports the Wi-Fi password as `key`
  ([`ezlopi_cloud_network.c:174`](ezlopi-cloud/ezlopi-cloud-network/ezlopi_cloud_network.c#L174)),
  and `ezlopi_service_webprov.c` traced every outgoing cloud payload verbatim,
  so the PSK appeared in plaintext in the log. The trace copy is now masked by
  `EZPI_util_redact_secrets_in_place`. **The payload sent to the cloud is
  deliberately unchanged** — the mobile app and cloud appear to depend on that
  field, and changing the wire format is out of scope for a hardening pass. See
  the privacy note in the README.

### BLE provisioning — [bug]

The BLE provisioning flow worked only if everything succeeded on the first
attempt. These are the reasons it appeared to hang rather than report a
failure.

- **The Wi-Fi status (`0xE102`) and error (`0xE103`) characteristics were never
  created.** They had been declared in the header since the service was
  written, but nothing registered them with the GATT table. A client therefore
  had no way at all to learn whether a credential write had succeeded — the
  only outcome it could observe was the connection eventually working or not.
  Both are now registered, each with a CCCD, and notify on state change.

- **Failed and rejected provisioning attempts were silent.** Credential
  handling now has an explicit state machine (`idle`, `connecting`,
  `connected`, `failed`, `rejected`). An empty, malformed or incomplete
  credential write, or one carrying an unauthorized `user_id`, previously fell
  through without a reply and the client waited forever; it now reports
  `rejected`. A genuine association failure reports `failed` along with the
  Wi-Fi disconnect reason.

- **A disconnect from the *previous* network was reported as a failure of the
  new one.** Joining a new network tears down the old association first, and
  that teardown raised `WIFI_EVENT_STA_DISCONNECTED`. Suppressed via
  `sg_prov_ignore_disconnect`. Relatedly, a drop on an already-provisioned
  device is no longer reported as a provisioning error — only disconnects while
  a credential write is in flight count.

- **Notification subscriptions leaked across connections.** A CCCD write is
  per-connection, but the enabled flag was global, so a reconnecting client
  inherited the previous client's subscription and the stack was asked to
  notify a connection that no longer existed. Subscriptions are now tagged with
  a connection generation counter and only honoured for the connection that
  made them.

- **The device rebooted before acknowledging the config write that asked it
  to.** `EZPI_core_ble_gatt_call_write_by_handle` invokes the write upcall and
  *then* sends the ATT response, so calling `esp_restart()` inside the upcall
  swallowed the acknowledgement. The phone saw a write timeout on a write that
  had in fact succeeded. The reboot is now deferred ~1500 ms by a dedicated
  task, with an inline fallback if the task cannot be created.

- **The first chunk of a chunked config was always rejected.** The `sequence`
  field is 1-based on the wire but was compared against a 0-based counter.

- **Oversized notifications emitted truncated, invalid JSON.** If a status or
  error document exceeded the negotiated MTU, it was cut at the MTU boundary.
  It now falls back to a single-byte state code and lets the client read the
  characteristic for the detail.

### BLE transport — [bug]

- **Advertising was over the 31-byte legacy payload limit.** The advertised
  service UUID list was built dynamically from every registered service, which
  outgrew the advertising packet; some phones then saw no name, or no
  advertisement at all. Replaced with a curated static list advertising only
  `0x00E1` (Wi-Fi) and `0x00E3` (Provisioning), and the device name moved to
  the scan response. This removed `ezlopi_ble_setup_service_uuid()` and a large
  `#if 0` block.

- **Reads of unregistered handles were never answered**, leaving the client to
  a 30-second ATT timeout. Unhandled handles now get an immediate
  `ESP_GATT_READ_NOT_PERMIT` / `ESP_GATT_WRITE_NOT_PERMIT`.

- **Read responses leaked uninitialised stack.** The `esp_gatt_rsp_t` was not
  cleared before the read upcall, so a handler that set a short length left the
  remaining ~600 bytes of the response buffer as whatever happened to be on the
  stack. Now `memset` to zero first.

- **The device name was never advertised.** The call to
  `EZPI_core_ble_gap_config_scan_rsp_data()` was missing, so the scan response
  was never configured.

- **Notifications were sent to a disconnected peer.** Added NULL and
  `connected` guards in `EZPI_core_ble_gatts_characteristic_notify`, backed by
  new `connected` / `conn_generation` fields on `s_gatt_service_t` latched in
  `ESP_GATTS_CONNECT_EVT`. The connect handler also had to move out of a
  pairing-dependent `#if` block, and `g_mtu_size` is now reset to
  `ESP_GATT_DEF_BLE_MTU_SIZE` on disconnect instead of carrying the previous
  peer's negotiated value into the next connection.

### BLE device info — [bug]

- **Per-chunk memory leak.** The device-info document is re-rendered for every
  chunk, but the buffer was freed only on the last one, leaking a whole copy of
  the document per intermediate chunk.
- **Restored a NULL guard that had been commented out**, which turned an
  allocation failure into a panic in the middle of an ATT read.
- **The trace reported the gateway address as the station IP.** `sta_ip_str` is
  now kept separate from `ip_str`.

### UART control channel — [bug]

- **Every command after the first in a buffer was silently dropped.** The UART
  task hands over everything that accumulated in the driver's RX buffer, so two
  commands sent back to back arrive as one buffer, e.g.
  `{"cmd":4}{"cmd":3,...}`. `cJSON_Parse` stops after the first value and still
  reports success, so the rest disappeared with no error to the sender and
  nothing in the log. The buffer is now walked with
  `EZPI_util_json_value_end` and each object dispatched in turn — handlers that
  re-parse the raw text get only their own object.

### Open-telemetry — [bug]

- **Every build shipped pointing at an Ezlo internal host.**
  `CONFIG_EZPI_OPENTELEMETRY_CLOUD_ENDPOINT` defaulted to
  `wss://ot.review-staging-op-...opentelemetry.ezlo.com/multi`, which nobody
  outside Ezlo can reach. The default is now empty and you must supply your own
  collector. A CI check fails the build if the hard-coded endpoint reappears
  anywhere in the tree.

- **Enabling telemetry without an endpoint flooded the console.** The otel task
  retries `EZPI_core_websocket_client_init` in an uncapped loop, so with no URL
  it reconnect-failed forever. `ezlopi_service_otel_init` now returns early if
  no endpoint is configured. A second CI check fails the build if
  `CONFIG_EZPI_ENABLE_OPENTELEMETRY=y` is committed into any
  `sdkconfig.defaults*`.

### OTA — [bug]

- **`esp32s2` did not build at all from a clean clone.** `CONFIG_EZPI_ENABLE_OTA=y`
  was set in `sdkconfig.defaults.esp32s2` and in no other target's defaults.
  The OTA source is wrapped in `#ifdef CONFIG_EZPI_ENABLE_OTA`, so only the s2
  build compiled it — and it needs `ca_mios.h`, which is listed in
  `.gitignore` and is therefore not in the repository at all. The build failed
  with `ca_mios.h: No such file or directory`. OTA is now off by default on s2
  too, matching the other three targets.

  This is the defect that justified the CI firmware matrix: nobody was building
  esp32s2, so a target that could not compile *at all* sat in the tree
  unnoticed. Note the underlying problem is not specific to s2 — enabling OTA
  on **any** target breaks a clean clone the same way.

- **Enabling OTA now fails with an explanation instead of a missing header.** A
  `__has_include` guard reports that `ca_mios.h` must be supplied by hand, what
  it has to contain, and that the alternative is to leave OTA disabled.
  Previously the error pointed at a line in the middle of a function body and
  gave no hint that a file was expected to be provided out-of-band.

- **`#include "ca_mios.h"` was inside a function body**, between statements.
  The header defines a variable, so at block scope it became a per-call local.
  Moved to the file's include block, where it has static storage as intended.

- **`#include "../../build/config/sdkconfig.h"` hard-coded the build
  directory.** It pinned both the directory's name and its position relative to
  the source file, so it broke for any build directory other than
  `<project>/build` — including `idf.py -B <dir>`. ESP-IDF already puts the
  generated config directory on every component's include path, so this is now
  just `#include "sdkconfig.h"`.

### Build and configuration — [infra]

- **Switching chips no longer requires copying a file by hand.** The four
  `sdkconfig.<chip>.4mb` files are renamed to
  `sdkconfig.defaults.<idf_target>`, which ESP-IDF layers on top of
  `sdkconfig.defaults` automatically. The whole build is now:

  ```sh
  idf.py set-target esp32   # or esp32s2 / esp32s3 / esp32c3
  idf.py build
  ```

  The previous instruction was to copy `sdkconfig.<chip>.4mb` over `sdkconfig`
  — but `sdkconfig` is generated and git-ignored, so a build could quietly
  rewrite your copy, and switching chips without remembering to re-copy
  produced a binary configured for the wrong target.

  Verified rather than assumed: the generated `sdkconfig` from a clean
  out-of-tree build is byte-identical to the previously hand-maintained one,
  and the resulting binary differs only in the 83 bytes of the ESP-IDF app
  descriptor that hold the build timestamp and ELF hash.

- **Added a near-empty `sdkconfig.defaults`, which is load-bearing.** IDF only
  looks for `sdkconfig.defaults.<IDF_TARGET>` variants of entries already in
  `SDKCONFIG_DEFAULTS`, and that list starts out containing `sdkconfig.defaults`
  and nothing else. Delete the base file and *every* per-target file is ignored
  silently — the build still succeeds, with the wrong configuration. A CI check
  asserts the base file and all four per-target files exist.

- **Removed a dead `execute_process` from the root `CMakeLists.txt`.** It
  invoked `update_device_list.sh` with a `DEPENDS` on `xyz.txt`. Neither file
  has ever been in the repository, so on every clean checkout this ran a
  command that did not exist. It was invisible for two reasons:
  `execute_process` without `RESULT_VARIABLE` or `COMMAND_ERROR_IS_FATAL`
  discards the exit status, and `execute_process` has no `DEPENDS` option at
  all, so that argument was ignored too.

- **`tests/host/Makefile` was excluded by `.gitignore`.** The CMake section had
  a bare, unanchored `Makefile` pattern, which matched at every depth. The CI
  host-test job is `make -C tests/host`, so on a fresh clone it failed with "no
  makefile found". Un-ignored explicitly; CMake's own generated Makefiles live
  under `build/`, which is already excluded.

- **Removed `ezlopi-core/ezlopi-core-wsc/`** (839 lines). The directory held a
  single `.c` file with no header and no `CMakeLists.txt`. The root
  `CMakeLists.txt` globs component directories and silently skips any without a
  `CMakeLists.txt`, so this was never compiled, and nothing anywhere else in
  the tree referenced it.

- **Ignore editor atomic-save leftovers** (`**/*.tmp`, `**/*.tmp.*`).

### Testing — [infra]

- **New `ezlopi-util-sanitize` component** holding the untrusted-input helpers
  (`EZPI_util_json_value_end`, `EZPI_util_redact_secrets_in_place`,
  `EZPI_util_clamp_copy_len`, `EZPI_util_copy_str_bounded`). It is deliberately
  dependency-free — `idf_component_register` is called with an empty `REQUIRES`
  — so the firmware and the host tests compile *the same source*, rather than
  the tests exercising a copy that can drift.

- **New host test suite, [`tests/host/`](tests/host/)**: 72 assertions, no
  framework, no hardware, runs in about a second via `make -C tests/host`.
  Built with `-Wall -Wextra -Werror` under AddressSanitizer and
  UndefinedBehaviorSanitizer. The sanitizers are the point: the defects being
  regression-tested are buffer overruns, and an overrun that happens to land in
  unused stack passes a plain assertion. Buffer tests also bracket the
  destination with a canary to catch a write that lands just past the end.

- **The suite was verified to have teeth.** Six mutations were introduced into
  the sanitize helpers — one per fixed defect — and each was confirmed to make
  the suite fail, by assertion or by ASan. A green suite that would also pass
  against the original buggy code is worse than no suite, because it is
  evidence of nothing.

- **New CI workflow, [`.github/workflows/ci.yml`](.github/workflows/ci.yml).**
  Nothing in this repository was checked automatically before. Three jobs:

  | Job | What it does |
  | --- | --- |
  | `host-tests` | `make -C tests/host` under ASan + UBSan |
  | `hygiene` | no hard-coded Ezlo telemetry endpoint; telemetry not enabled in committed defaults; base and per-target `sdkconfig.defaults` all present |
  | `firmware` | builds all four targets in `espressif/idf:v4.4.6`, asserts the generated config was actually inherited from the per-target defaults, uploads `bin`/`elf`/`map`/`sdkconfig` |

  All four hygiene checks were tested both ways — each was confirmed to fail
  when its regression is reintroduced, not merely to pass today.

  The telemetry-endpoint check excludes markdown. Without that, documenting
  the removal of the hard-coded endpoint — naming the host in this CHANGELOG
  and in CONTRIBUTING — tripped the very check that enforces its removal. The
  narrowed check was then re-tested by planting the endpoint back into
  `Kconfig.projbuild` to confirm it still fails for code and config.

  The firmware job exists because three of the four supported chips were not
  being built by anyone routinely, so target-specific breakage could sit in the
  tree indefinitely. `fail-fast` is off so one chip failing does not hide the
  state of the other three.

### Documentation — [infra]

- **README rewritten.** Honest licensing section (EASL is source-available,
  **not** open source, and cannot be relicensed — the copyright is Ezlo
  Innovation's); real build and flash steps; a per-target table stating which
  targets have actually been tested and on what; the privacy note about the PSK
  in `hub.network.get`; and a troubleshooting section covering the traps below.
- **Pinned the ESP-IDF version.** The README claimed v4.4.4 in one place and
  v4.4.6 in another; `.github/workflows/ci.yml` is now the authoritative
  answer, and it is v4.4.6.
- **Dropped the misleading `git clone --recursive`.** There is no `.gitmodules`
  in this repository, so `--recursive` does nothing.
- Documented two traps that read like project bugs but are not: device
  instantiation is **boot-only** (`ezlopi_core_ezlopi.c:134` →
  `EZPI_device_prepare()`; there is no runtime re-parse, so a config written
  over BLE takes effect only after the reboot), and the repo ships precompiled
  archives (`Bosch_BSEC2/.../libalgobsec.a`) which any line-ending conversion
  will corrupt into `error adding symbols: malformed archive`.
- Documented the serial control channel command table, including a warning that
  `{"cmd":0,"sub_cmd":0}` is a **full factory restore** — it erases the
  factory-info partition and NVS, so the serial number, MAC and certificates
  are unrecoverable — whereas `{"cmd":0,"sub_cmd":1}` is a plain reboot. They
  differ by one character and the destructive one is `0`.
- Added this file and [SECURITY.md](SECURITY.md).

---

## Known gaps

Listed because a changelog that only mentions what was finished is misleading.

- **Only `esp32` has been run on hardware.** All four targets now compile
  cleanly, but compiling is not running. `esp32s2`, `esp32s3` and `esp32c3`
  have never been flashed or booted, so for those three "supported" means "the
  firmware links", nothing more.
- **Hardware verification is partial even on `esp32`.** The firmware has not
  been reflashed since this refactor, and the device-add path through chunked
  `0xE501` config has not been re-tested on hardware.
- **OTA is untested and off by default on every target.** It cannot be built
  without a `ca_mios.h` that this repository does not contain, so nothing here
  exercises it.
- **The PSK is still sent to the cloud as `key`** in `hub.network.get`. Only
  the local trace is masked. See the README privacy note.
