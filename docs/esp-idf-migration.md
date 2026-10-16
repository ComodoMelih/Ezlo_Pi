# Migrating off ESP-IDF v4.4.6

**Status: not started.** v4.4.6 remains the only supported version — see
[README §2.1](../README.md#21-prerequisites). This file exists because
"does not build on v5.x" is an assertion, and a migration plan built on an
assertion is a guess. Everything below was measured against real ESP-IDF
trees; where something was not measured, it says so.

v4.4 reached end of life on 2024-07-02 (Espressif advisory AR2024-008): no
further bug or security fixes on that branch. So this is maintenance debt with
a clock on it, and it also blocks Matter: as of 2026-10, esp-matter's README
recommends ESP-IDF v6.0.2, and nothing in that project targets v4.4. Whether
Matter would work on an older v5.x was not tested here.

One cheap thing first, unrelated to any of the below: the pin is **v4.4.6**,
but the branch ended at **v4.4.8**. Two patch releases of fixes are available
without changing a line of source. That has not been tried here and is not a
recommendation yet — it is the smallest experiment available, and it should be
run before the larger one.

---

## 1. How the numbers below were produced

Two ESP-IDF versions were installed side by side with the shipping v4.4.6 and
the tree was pushed through each one:

| | version | toolchain |
|---|---|---|
| current | v4.4.6 | `xtensa-esp32-elf-gcc` 8.4.0 |
| candidate | v5.5.5 | `xtensa-esp-elf-gcc` 14.2.0 |
| candidate | v6.0.3 | `xtensa-esp-elf-gcc` 14.2.0 |

Method, per version: copy the worktree, add an `idf_component.yml` for the
components that left ESP-IDF, loop `idf.py set-target esp32` while resolving
one unresolvable component name per iteration, then `ninja -k 0` so the build
continues past failures instead of stopping at the first one, then classify
every `error:` line.

Measured at commit `cecf5da`, target `esp32`.

**The error counts are a floor, not a total.** A translation unit that fails
on a header never reaches its body, and a component that fails to configure
blocks its dependents. The clearest example: the v5.5.5 build reports 20
errors for `portTICK_RATE_MS`, but the tree contains 58 uses of it across 27
files. Treat every count below as "at least".

An earlier run of the same survey, taken at `73c5290`, reported 307 errors
across 76 files. The difference is `258ad4f`, which removed 31 null checks on
arrays that can never be null; under GCC 14 each was a `-Werror=address`
failure. That class now measures 0, and the two runs differ by exactly
31 errors and 3 files — a second, independent check on that commit, which was
otherwise verified by disassembly.

---

## 2. Recommendation: v5.5.5 first, v6.x as a separate hop

| | v5.5.5 | v6.0.3 |
|---|---|---|
| registry components to declare | 2 | 3 |
| component renames to configure | 1 | 1 |
| error lines | 276 | 208 |
| distinct failing files | 73 | 20 |
| sdkconfig keys silently dropped | 34 | 50 |
| ...of which are this project's own | 0 | 0 |

v6.0.3's smaller numbers are not progress — they are the build dying earlier.
185 of its 208 errors are a single message, `driver/uart.h: No such file or
directory`, because `EZLOPI_USER_CONFIG.h` includes that header and nearly
everything includes `EZLOPI_USER_CONFIG.h`. The 20 "failing files" are the
files reached before the collapse.

Two measured facts make the staged hop the cheaper route, and both are
compatibility shims that exist in v5.5.5 and are gone in v6.0.3:

**`driver/uart.h`.** In v4.4.6 it lives in the `driver` component. In both
v5.5.5 and v6.0.3 it has moved to `esp_driver_uart`. But v5.5.5's
`components/driver/CMakeLists.txt` still carries, in its own words, a public
dependency on the `esp_driver_foo` components "for backward compatibility",
`esp_driver_uart` among them — so anything that requires `driver` still
resolves the header. v6.0.3 dropped that list entirely. Hence 21 errors versus
185.

**The legacy ADC API.** v5.5.5 still ships `esp_adc_cal.h` and
`esp_adc_cal_types_legacy.h` in `components/esp_adc/deprecated/include/`;
only the *component name* changed, from `esp_adc_cal` to `esp_adc`. v6.0.3 has
no `deprecated/` directory and no `driver/adc.h` at all, so the legacy API must
be rewritten against `adc_oneshot`/`adc_cali`.

That rewrite is smaller than it sounds, and for a good reason: 25 files consume
the ADC through `ezlopi-hal/ezlopi-hal-adc`, and only two touch the legacy
ESP-IDF API directly — `ezlopi_hal_adc.c` and the hall-effect sensor, which
loses its hardware support on v5 regardless (§5). So the v6 ADC rewrite is
effectively one file. This is the HAL seam paying for itself, and the argument
for extending the same seam to UART.

---

## 3. Stage 0 — build system (prerequisite, no source changes)

The repo has **no `idf_component.yml` anywhere**, so it cannot express a
dependency on anything outside the ESP-IDF tree. Three components it uses were
moved out of ESP-IDF into the component registry, at different versions:

| component | v4.4.6 | v5.5.5 | v6.0.3 | action |
|---|---|---|---|---|
| `mdns` | bundled | registry | registry | declare `espressif/mdns` |
| `esp_websocket_client` | bundled | registry | registry | declare `espressif/esp_websocket_client` |
| `mqtt` | bundled | bundled | registry | declare `espressif/mqtt` for v6 only |
| `esp_adc_cal` | bundled | renamed `esp_adc` | gone | rename in 2 `CMakeLists.txt` |
| `tinyusb` | bundled | `esp_tinyusb` | `esp_tinyusb` | done in `54b68e8` |

Two traps found the hard way:

- **`led_strip` must not be declared.** It looks like a registry component and
  there is one by that name, but this repo vendors its own at
  `ezlopi-components/led_strip`. Declaring it makes the registry copy shadow
  the vendored one.
- **Configure reports only the first unresolvable component**, so resolving
  them costs one full configure run each. The survey script loops instead.

Adding the manifest is the work tracked as "external components mechanism",
and it is a prerequisite for any IDF bump rather than a separate feature.

---

## 4. Stage 1 — source, grouped by root cause

276 errors across 73 files on v5.5.5. Grouped into mutually exclusive buckets,
because "276 errors" is not a plan and the buckets have very different costs:

| | root cause | errors | files |
|---|---|---|---|
| A1 | `uint32_t` changed underlying type: printf formats | 76 | 26 |
| A2 | `uint32_t` changed underlying type: `uint32_t*` vs `size_t*` | 2 | 1 |
| B1 | `int*` written through an enum lvalue, via `CJSON_GET_VALUE_INT` | 87 | 45 |
| B2 | same, via `EZPI_core_nvs_read_*` | 3 | 2 |
| C | header moved or removed | 24 | 4 headers |
| D1 | `portTICK_RATE_MS` renamed | 20 | 27 (see below) |
| D2 | other renamed/removed API, or an include that used to arrive for free | 42 | — |
| E | remainder, needs reading one at a time | 22 | 10 |

The "files" column is where a bucket's errors were *reported*, which for B1 is
the macro's own header; there it counts the distinct call-site files instead.

### A — one typedef, 78 errors

Not an ESP-IDF API change at all; a toolchain change, so it will happen on any
hop. On the v4.4.6 toolchain `uint32_t` is `unsigned int`; on the 14.2.0
toolchain it is `long unsigned int`. `size_t` stays `unsigned int` in both.

That single difference causes `%d`/`%u` on a `uint32_t` to become a hard error
(76 sites, 26 files — mechanical, use `PRIu32`), and makes `uint32_t *` no
longer compatible with `size_t *`, which is the whole of A2:
`uart_get_buffered_data_len()` calls that pass `&(uint32_t)` where the
signature has said `size_t *` in every version including v4.4.6. Two errors
were reported; the tree has five such call sites, in `ezlopi_hal_uart.c` and
`ezlopi_service_uart.c`.

### B — 90 errors, one macro

```c
#define CJSON_GET_VALUE_INT(cj_root, item_name_str, item_val) \
    EZPI_core_cjson_get_value_int(cj_root, item_name_str, &item_val)
```

The callee takes `int *` and writes through it unconditionally. The tree has
116 uses of this macro across 51 files; 87 of them, in 45 files, fail. All 87
pass the address of an enum lvalue, in three types:
`e_ezlopi_device_interface_type_t` (64), `e_ezlopi_gpio_logic_invert_t` (15),
`e_ezlopi_gpio_value_t` (8). The other uses pass a genuine `int` and are fine,
so this is not a sweep of all 116.

**This is a compile blocker, not a live memory bug.** All three enums have
small non-negative enumerators, the build does not use `-fshort-enums`, and a
`_Static_assert` compiled with the actual target compiler confirms such enums
are 4 bytes, the same as `int` — so the write is size-correct today. (The same
assert fails when `-fshort-enums` is added, which is what makes it evidence
rather than a tautology.) It is type-safety debt that GCC 8 only warned about
and GCC 14 refuses; the fix is a typed accessor or an explicit temporary at
each site, not a hunt for corruption.

B2 is the same shape through a different helper, in `ezlopi_core_log.c` and
`hilink_presence_sensor_setting.c`.

### C — 24 errors, four headers, two different situations

- `driver/uart.h` (21) and `esp_netif_ip_addr.h` (1) **still exist** in v5.5.5.
  They are missing because v4.x put `driver` and friends in the common requires
  list, so a component could include the header without declaring it and
  compile by accident. These are missing `REQUIRES`, not missing API.
- `mbedtls/config.h` and `mbedtls/certs.h` (1 each) are **genuinely gone** in
  mbedTLS 3.x: `ezlopi_core_http.c` and `ezlopi_main.c`.

### D — one rename, plus 42 errors that only look like renames

`portTICK_RATE_MS` → `portTICK_PERIOD_MS` is 58 uses in 27 files, purely
mechanical. The rest look alike in the error log but are three different
problems, and it is worth knowing which, because only the last group needs a
code change decision. Each symbol below was searched for in both IDF trees.

*Includes that used to arrive transitively and now have to be asked for.* The
symbol still exists and is unchanged; only the header that used to drag it in
has stopped doing so:

- `time_t` in 5 headers — needs `<time.h>`.
- `esp_timer_get_time` / `esp_timer_create` / `ets_delay_us` / `ets_printf`
  across 9 files.
- `ESP_MAC_WIFI_STA` and `ESP_MAC_BT`. Both still live in
  `esp_hw_support/include/esp_mac.h` in v5.5.5, exactly as in v4.4.6 — but
  v4.4.6's `esp_system.h` includes `esp_mac.h` on line 30 and v5.5.5's does
  not. Add `#include "esp_mac.h"`.

*One config default flipped.* `xTaskHandle` and `xSemaphoreHandle` are the
pre-v8 FreeRTOS spellings, still defined in v5.5.5 at
`FreeRTOS-Kernel/include/freertos/FreeRTOS.h` — but behind
`configENABLE_BACKWARD_COMPATIBILITY`. v4.4.6 had no Kconfig option for it and
hardcoded the fallback to `1`; v5.5.5 drives it from
`CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY`, which is `default n`. So there
are two fixes: set that one key, or rename the uses to `TaskHandle_t` and
`SemaphoreHandle_t`. Prefer the rename — the key is a deprecation crutch and
keeping it set only defers this.

*Genuinely gone, replacement needed:*

- `ADC_ATTEN_11db` → `ADC_ATTEN_DB_11`. Was in v4.4.6's `driver/adc.h`; zero
  hits anywhere in the v5.5.5 tree.
- `TCPIP_ADAPTER_IF_STA` → the `esp_netif` equivalent. Zero hits in v5.5.5.
- `GPIO_PIN_INTR_DISABLE` → `GPIO_INTR_DISABLE` from `hal/gpio_types.h`. This
  one is target-specific and easy to misread: v5.5.5 still ships
  `esp_rom/esp32/include/esp32/rom/gpio.h`, and other targets' copies of that
  header still define the whole `GPIO_PIN_INTR_*` enum — but the **esp32** copy
  defines none of them, where v4.4.6's did (lines 57-62).
- `mbedtls_sha1_ret` → `mbedtls_sha1`. Technically still present in v5.5.5, in
  `mbedtls/compat-2.x.h` — but nothing in mbedTLS or IDF includes that header,
  so it is unreachable without naming it explicitly. Treat it as removed.

### E — the remaining 22

Listed in full, because a bucket named "miscellaneous" is where migrations go
to die. Ten files, one of which is an ESP-IDF header:

- **`TaskHandle_t` and `QueueHandle_t` stored in an `int`** (6 errors) in
  `ezlopi_core_buffer.c` and `ezlopi_service_webprov.c`. Works only because
  pointers are 32 bits on these parts.
- **`esp_chip_info_t.full_revision`** (2) — field changed in v5;
  `ezlopi_service_ble_device_info.c` and `ezlopi_service_uart.c`.
- `-Werror=char-subscripts` (3), `mbedtls_base64_encode` arg 3 (1 — **fixed**;
  the `olen` arguments in `ezlopi_service_ble_provisioning.c` and
  `ezlopi_service_ble_dynamic_config.c` are `size_t` now, and the output
  buffers they describe were wrongly sized besides, so see the CHANGELOG entry
  rather than treating this as a cast),
  `-Werror=array-bounds` on a `char[0]` in `ezlopi_cloud_constants.h` (1),
  `-Werror=stringop-overread` on a `strlen` in
  `ezlopi_core_scenes_when_methods_helper_functions.c` (1), and one
  `xQueueSemaphoreTake` error reported inside ESP-IDF's own `semphr.h` but
  caused by a cast at our call site.
- The four `pms5003.c` errors are a cascade, not a defect. They read as
  "field name not in record or union initializer", but the designated
  initialiser is correct; `esp_timer_create_args_t` is simply an unknown type
  there because the `esp_timer.h` include is missing. Fixing the include in
  bucket D2 removes all four. Worth stating because this is what most of a
  migration's error count actually is, and why counting errors instead of
  causes overstates the work.

v6.0.3 adds two of its own, both trivial and both worth fixing now regardless
of version: a `-Werror=header-guard` mismatch in
`ezlopi_cloud_value_type_str.h`, and a 17-char initialiser for a 16-char array
in `ezlopi_util_uuid.c` (`-Werror=unterminated-string-initialization`).

---

## 5. Capability and behaviour losses — these need a decision, not a fix

**The ESP32 internal hall sensor is gone.** `hall_sensor_read()` ships in
v4.4.6's `driver/adc.h` and exists in neither v5.5.5 nor v6.0.3; Espressif
removed it. `sensor-0018-other-internal-hall-effect` is registered
unconditionally in `ezlopi_core_devices_list_modules.h`, so it is in every
build, though its body is already `#ifdef CONFIG_IDF_TARGET_ESP32` — the part
only ever existed on the original ESP32. There is no replacement API. The
device type has to be removed from the list or stubbed; either way it is a
user-visible capability loss and the only one found.

**`CONFIG_SDK_TOOLPREFIX` no longer exists.** `ezlopi_cloud_info.c` reports it
to the cloud as the device's `architecture` field. The key is among the 34
v5.5.5 drops, so that field needs a new source rather than a mechanical fix.

**mbedTLS 3.x** removed `mbedtls/config.h` and `mbedtls/certs.h`; those are
include changes. `mbedtls_sha1_ret()` is listed here only to rule it out: it is
a pure rename to `mbedtls_sha1()`, with the same `int` return. The `_ret`
suffix existed in 2.x to distinguish the error-returning variant from a
`void`-returning one; 3.x deleted the `void` version and took the plain name.
`compat-2.x.h` states the equivalence itself, as a one-line
`#define mbedtls_sha1_ret mbedtls_sha1`. Mechanical substitution is safe.

---

## 6. sdkconfig

Of 511 keys requested across `sdkconfig.defaults` and
`sdkconfig.defaults.esp32`, v5.5.5 does not recognise 34 and v6.0.3 does not
recognise 50. **In both cases, zero of them are this project's own
`CONFIG_EZPI_*` keys** — the whole of `ezlopi-user-config`'s Kconfig survives
intact. The drops are all ESP-IDF legacy: renamed Bluetooth controller keys,
`ESP32_DPORT_WORKAROUND`, `ESPTOOLPY_*` monitor-baud keys, the TLS 1.0/1.1
mbedTLS switches, `ESP_NETIF_TCPIP_ADAPTER_COMPATIBLE_LAYER` (which is why
`tcpip_adapter_set_hostname` disappears), and `TOOLPREFIX`/`SDK_TOOLPREFIX`.

A dropped key is silently ignored, not an error, so this is the one part of the
migration that cannot be caught by the compiler. It was checked by diffing the
requested keys against the generated `sdkconfig`.

---

## 7. What this does not tell you

- **Nothing was linked and nothing was run.** Every number here is from a
  compile. A clean compile on v5.5.5 would still leave the entire link stage,
  the image size, and all runtime behaviour unmeasured.
- **Nothing was tried on hardware**, on any version.
- Only the `esp32` target was surveyed. `esp32s2`, `esp32s3` and `esp32c3` will
  differ at least in the ADC and USB paths.
- Deprecation warnings were not counted. They are the *next* migration's
  errors.

## 8. Reproducing this

The survey is three scripts, none of which modify the worktree — they rsync it
to a scratch directory first. They are not in the repo because they hardcode
local ESP-IDF install paths; the method is in §1 and is about forty lines of
shell. The one part worth keeping is the loop in Stage 1: configure reports a
single unresolvable component per run, so resolving them one at a time by hand
costs a full configure each, and a mapping table plus a loop turns that into
one run.
