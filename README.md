[![N|Ezlo-Pi](https://www.ezlopi.com/wp-content/uploads/2022/07/Logo.svg)](https://www.ezlopi.com/)

<picture>
  <img src="ezlopi-doc/ezlopi_cover.png"  align="center" >
</picture>

# EzloPi

EzloPi is firmware for ESP32-family boards, developed by Ezlo Innovation. It
turns a bare ESP32 into a configurable IoT controller: you describe the sensors
and actuators wired to the board, and the firmware exposes them to the EzloPi
cloud and mobile app.

Upstream project: [ezloteam/Ezlo_Pi](https://github.com/ezloteam/Ezlo_Pi) ·
[ezlopi.com](https://www.ezlopi.com/)

> **About this fork.** This tree carries bug fixes and hardening on top of
> upstream, and build/test infrastructure that upstream does not have. See
> [CHANGELOG.md](CHANGELOG.md) for everything that changed and why, and
> [SECURITY.md](SECURITY.md) for the security-relevant parts. Several of the
> fixes are for memory-safety defects reachable from an unauthenticated BLE or
> UART client.

## Licensing — please read

Despite the word "open-source" appearing in earlier versions of this README,
**this project is source-available, not open source.** It is licensed under the
Ezlo Available Source License (EASL) — see [LICENCE.txt](LICENCE.txt). The EASL
does not meet the [Open Source Definition](https://opensource.org/osd), because:

- §1.1(e) makes your rights "conditional on You not charging any fees", which
  fails OSD #1 (free redistribution).
- §1.1(b) forbids use "in connection with any product that is distributed or
  otherwise made available by any third party", which fails OSD #6 (no
  discrimination against fields of endeavor).
- §2.1 reserves all ownership to Ezlo, and §2.2 forbids removing proprietary
  notices.

Practically: you can read, build, modify and run this for free, including
commercially within your own organisation, but you cannot sell it or ship it
inside a product you distribute. Copyright is held by Ezlo Innovation, so
neither this fork nor any other can relicense it. If you need different terms,
talk to Ezlo.

## Supported hardware

| Target | Builds | Run on hardware | App size |
| --- | --- | --- | --- |
| ESP32 | yes | yes | 0x18f790 (19% free) |
| ESP32-S2 | yes | **no** | 0x1444a0 (35% free) |
| ESP32-S3 | yes | **no** | 0x1931b0 (19% free) |
| ESP32-C3 | yes | **no** | 0x1a1640 (16% free) |

"Builds" means a clean `idf.py set-target <t> && idf.py build` from a fresh
checkout, verified for all four. **Only ESP32 has been flashed and booted** —
for the other three, treat "supported" as "the firmware links", because that is
all that has been demonstrated.

Until recently ESP32-S2 did not build at all: it was the only target with
`CONFIG_EZPI_ENABLE_OTA=y`, and the OTA code needs a `ca_mios.h` that is
gitignored and so absent from the repository. Nobody noticed because nobody was
building S2. See the [CHANGELOG](CHANGELOG.md#ota--bug).

A 4 MB flash part is assumed throughout — see
[the partition table](ezlopi_partitions_v3_1_4_181_4mb.csv), which allocates
two ~2 MB OTA app slots and leaves no room to shrink.

## Supported peripheral classes

GPIO · analog input · digital input · PWM · UART · 1-Wire · I²C · SPI, plus a
long tail of specific sensors under
[`ezlopi-sensors-devices/`](ezlopi-sensors-devices).

# 1. Getting started without building anything

If you only want to use a board rather than modify the firmware, use the
[web-based configurator](https://config.ezlopi.com/). It flashes a prebuilt
binary over USB from the browser and walks through describing your wiring.

![EzloPi web configurator](ezlopi-doc/ezlopi_web_flasher.png)

Worked examples live at [ezlopi.com/examples](https://www.ezlopi.com/examples/):

1. [AC lamp with a relay](https://www.ezlopi.com/examples/relay-circuitry-and-lamp-circuit-setup/)
2. [AC lamp with a relay and a momentary switch](https://www.ezlopi.com/examples/ezlopi-ac-lamp-setup-with-relay-and-a-momentary-switch/)
3. [I²C MPU6050](https://www.ezlopi.com/examples/ezlopi-i2c-mpu6050-example/)
4. [HC-SR04 ultrasonic sensor](https://www.ezlopi.com/examples/hcsr04-ultrasonic-sensor-example/)
5. [HRLV MaxSonar-EZ MB1013 over UART](https://www.ezlopi.com/examples/hrlv-maxsonar-ez-mb1013-sonar-sensor-uart-example/)
6. [LDR digital module](https://www.ezlopi.com/examples/ldr-digital-module-example/)
7. [Dimmable LED](https://www.ezlopi.com/examples/ezlopi-dimmable-led-example/)
8. [TTP223B touch sensor](https://www.ezlopi.com/examples/ttp223b-touch-sensor-example/)
9. [Custom device/sensor integration guide](https://www.ezlopi.com/examples/ezlopi-v2-x-custom-device-sensorintegration-guide/)

# 2. Building the firmware

## 2.1 Prerequisites

**ESP-IDF v4.4.6 exactly.** Not v5.x — this tree uses v4.4 APIs and will not
compile against v5. Follow the
[v4.4.6 getting-started guide](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32/get-started/index.html),
or use the Docker image CI uses, `espressif/idf:v4.4.6`.

Verify what you have before filing a build issue:

```bash
idf.py --version
```

## 2.2 Clone

```bash
git clone https://github.com/ezloteam/Ezlo_Pi.git Ezlo_Pi
cd Ezlo_Pi
```

There are no git submodules, so `--recursive` does nothing here. (Earlier
instructions included it, which led people to believe a failed submodule step
explained their build problem.)

## 2.3 Build

```bash
idf.py set-target esp32       # or esp32s2 / esp32s3 / esp32c3
idf.py build
```

That is the whole configuration step. `set-target` generates `sdkconfig` from
[`sdkconfig.defaults`](sdkconfig.defaults) plus the matching
`sdkconfig.defaults.<target>` file, which ESP-IDF layers on automatically.

Do not edit `sdkconfig` directly and expect it to survive — it is generated and
git-ignored. To change a setting permanently, use `idf.py menuconfig` to find
the symbol, then add it to the appropriate `sdkconfig.defaults.*` file.

<details>
<summary>If you have used this project before: the old instructions changed</summary>

The previous README told you to copy `sdkconfig.<chip>.4mb` over `sdkconfig` by
hand. Those files are now named `sdkconfig.defaults.<target>` and are applied
automatically. The hand-copy step was a frequent source of trouble: `sdkconfig`
is generated, so a build would silently rewrite your copy, and switching chips
without re-copying produced a binary for the previous target.

</details>

## 2.4 Flash and monitor

```bash
idf.py -p <port> -b 460800 flash monitor
```

`<port>` is `/dev/ttyUSB0`-ish on Linux, `/dev/cu.usbserial-*` on macOS,
`COM3`-ish on Windows. Exit the monitor with `Ctrl-]`.

### Flashing without erasing device identity

`idf.py flash` writes only the bootloader, partition table, OTA data and app.
It leaves `nvs` (your configured devices, Wi-Fi credentials) and `prov_data`
(the per-device serial number, MAC and cloud certificates) intact. That is
what you want almost always.

**`idf.py erase-flash` destroys `prov_data`.** The certificates and serial
number in it are provisioned per board and are not recoverable from this
repository. Do not run it on a board you care about.

# 3. Testing

## 3.1 Host tests — no hardware needed

```bash
make -C tests/host
```

These compile the real firmware sources with your system compiler under
AddressSanitizer and UndefinedBehaviorSanitizer. Only code with no ESP-IDF
dependency can be tested this way, which is why
[`ezlopi-util-sanitize`](ezlopi-util/ezlopi-util-sanitize) deliberately
declares no `REQUIRES`.

Every case in [`tests/host/test_sanitize.c`](tests/host/test_sanitize.c)
corresponds to a defect that actually shipped. Please add a case here when you
fix anything in this area — and check that it fails before your fix.

## 3.2 CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push and
pull request:

- **host-tests** — the suite above.
- **firmware** — a clean `set-target` + `build` for all four targets, so
  breakage specific to a chip nobody is currently using still gets caught.
- **hygiene** — guards against regressions that are easy to reintroduce and
  hard to notice: a hard-coded telemetry endpoint, telemetry enabled by
  default, or a missing `sdkconfig.defaults` (which would make every
  per-target config file be ignored *silently*, producing a build that
  succeeds with the wrong configuration).

# 4. How the firmware is laid out

Components are plain ESP-IDF components. The root
[`CMakeLists.txt`](CMakeLists.txt) globs every immediate subdirectory of the
directories below and adds it as a component, so a new component is picked up
with no registration step — but note the flip side: **a directory without a
`CMakeLists.txt` is skipped silently**, with no warning, so a typo in a new
component's filename looks like code that compiles but never runs.

| Directory | What lives there |
| --- | --- |
| `ezlopi-main/` | app entry point and `Kconfig.projbuild` (all `EZPI_*` options) |
| `ezlopi-core/` | device model, BLE GATT, Wi-Fi, NVS, factory info, reset, OTA |
| `ezlopi-services/` | long-running tasks: BLE provisioning, web provisioning, UART control, telemetry |
| `ezlopi-cloud/` | handlers for individual cloud RPC methods |
| `ezlopi-hal/` | thin wrappers over IDF GPIO/ADC/PWM/I²C/SPI/1-Wire/UART |
| `ezlopi-sensors-devices/` | one component per supported sensor or actuator |
| `ezlopi-util/` | logging, versioning, UUID, untrusted-input helpers |
| `ezlopi-lua-modules/` | Lua scripting bindings |
| `ezlopi-components/` | vendored third-party code, including precompiled blobs |
| `ezlopi-src-template/` | the skeleton to copy when adding a device — [guide](ezlopi-src-template/Readme.md) |
| `tests/host/` | host-compiled tests |

Two behaviours are worth knowing before you debug anything:

- **Devices are instantiated only at boot.** The device list is parsed once
  during startup, and there is no runtime re-parse. Writing a new
  configuration therefore has no visible effect until the board reboots, which
  the firmware does for you after a successful config write.
- **`ezlopi-components/` contains precompiled binaries** (for example
  `Bosch_BSEC2/precompiled_binaries/*/libalgobsec.a`). If you script anything
  that rewrites files in this tree, exclude binaries — passing one through a
  line-ending conversion corrupts it and the link fails with "malformed
  archive", which does not look like the cause.

# 5. The serial control channel

The firmware accepts newline-free JSON commands on **UART0 at 115200 baud** —
the same port as the console. This is how the web configurator talks to the
board. It is unauthenticated: anything with physical access to the USB port has
full control.

| `cmd` | Meaning |
| --- | --- |
| 0 | reset — see the warning below |
| 1 | device info |
| 2 | Wi-Fi credentials / status |
| 3 | set device configuration |
| 4 | get device configuration |
| 5 | UART configuration |
| 6 | log configuration |
| 7 | set provisioning data |

> ### ⚠ `{"cmd":0,"sub_cmd":0}` is a destructive factory restore
>
> It erases **both** NVS and the factory-info partition, taking the board's
> serial number, MAC and cloud certificates with it
> (`EZPI_core_reset_factory_restore`, [ezlopi_core_reset.c:97](ezlopi-core/ezlopi-core-reset/ezlopi_core_reset.c#L97)).
> Those are provisioned per board and are not in this repository, so the board
> cannot be restored afterwards.
>
> **To simply reboot, use `{"cmd":0,"sub_cmd":1}`.** The two differ by one
> character and the destructive one is `0`, the value you get by leaving the
> field at its default.

Commands may be pipelined — `{"cmd":4}{"cmd":1}` in a single write is answered
twice. (Upstream silently dropped everything after the first object, because
`cJSON_Parse` stops at the end of the first value and still reports success.)

# 6. Troubleshooting

**`idf.py build` fails with errors about unknown IDF functions.** You are on
ESP-IDF v5.x. This tree needs v4.4.6; see §2.1.

**Link fails: `libalgobsec.a: error adding symbols: malformed archive`.** A
precompiled blob in `ezlopi-components/` has been corrupted, almost always by a
tool that rewrote line endings across the whole tree. Restore it with
`git checkout -- ezlopi-components/`.

**Build fails with `ca_mios.h is missing`, or `ca_mios.h: No such file or
directory`.** You have `CONFIG_EZPI_ENABLE_OTA` enabled. That header holds the
CA chain for the OTA server, is listed in `.gitignore`, and is therefore not in
this repository — you have to supply your own:

```c
/* ezlopi-core/ezlopi-core-ota/ca_mios.h */
static const char ca_mios[] = "-----BEGIN CERTIFICATE-----\n...";
```

Otherwise leave OTA off, which is the default on all four targets. OTA is
untested in this fork for exactly this reason.

**The board boots but nothing you configured appears.** Devices are only
instantiated at boot (§4). Power-cycle and re-check.

**Configuration writes seem to be ignored over BLE.** The board reboots to
apply them. If it reboots *before* acknowledging the write, the client sees a
timeout and assumes failure — that was a real bug, fixed in this fork; see the
CHANGELOG.

**Serial console is flooded with websocket reconnect failures.** Open-telemetry
is enabled without an endpoint. It is off by default here; if you turned it on,
set `CONFIG_EZPI_OPENTELEMETRY_CLOUD_ENDPOINT` to your own collector.

**Switching targets doesn't take effect.** `idf.py set-target` regenerates
`sdkconfig`; if you hand-edited it, your edits are gone, and if you have a
stale `build/` try `idf.py fullclean`.

# 7. Privacy note

`hub.network.get` reports the configured Wi-Fi network's pre-shared key to the
cloud in cleartext-over-TLS, as the `key` member
([ezlopi_cloud_network.c:174](ezlopi-cloud/ezlopi-cloud-network/ezlopi_cloud_network.c#L174)).
This fork does **not** change that, because the cloud and mobile app depend on
the field. It is documented here so the behaviour is not a surprise. The PSK is
redacted from local trace output, which previously printed it to the serial
console in the clear.

# 8. Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Note the CLA: contributions are
submitted under the EASL.

Pull requests target the **development** branch, and CI must be green.
