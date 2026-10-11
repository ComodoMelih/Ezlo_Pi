# Browser install page

A one-page flasher for the binaries this repository builds. Open it in Chrome or
Edge, plug a board in over USB, press the button. No ESP-IDF, no esptool, no
Python.

It is a static page driven by
[ESP Web Tools](https://esphome.github.io/esp-web-tools/), which talks to the
chip over the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API)
from inside the browser. Nothing is uploaded anywhere: the binary is fetched
from the page's own origin and written straight to the board.

## How it differs from config.ezlopi.com

[config.ezlopi.com](https://config.ezlopi.com/) flashes a binary Ezlo hosts, and
then walks you through describing your wiring. It is the right starting point
for most people and this page does not replace it.

This page flashes **the binaries built from this tree, by CI, from the commit it
was published from**. That is what makes it useful here:

- anyone running a fork or a patched tree gets a flashable image without
  installing a toolchain;
- what you flash corresponds to a commit you can read;
- it works for the three targets the hosted configurator does not offer.

## Status per target, honestly

ESP Web Tools reads the chip ID over serial and picks the matching build itself,
so there is one button rather than four. But "there is a build" is not "it has
been run":

| Target | Builds in CI | Flashed and booted by a human |
| --- | --- | --- |
| ESP32 | yes | yes |
| ESP32-S2 | yes | **no** |
| ESP32-S3 | yes | **no** |
| ESP32-C3 | yes | **no** |

The same caveat as [the README](../README.md#supported-hardware): for three of
the four targets, all that has been demonstrated is that the firmware links. The
page says this too, where someone about to flash will actually see it.

A **4 MB flash part is assumed** on every target — see
[the partition table](../ezlopi_partitions_v3_1_4_181_4mb.csv). On a 2 MB board
the flash will fail or the device will not boot.

## What the page does not do yet

It does not set up Wi-Fi in the browser. ESP Web Tools does that over
**Improv *Serial***, and this firmware currently implements Improv over **BLE**
only (`CONFIG_EZPI_BLE_ENABLE_IMPROV`, off by default). The two are the same
protocol over different transports and are not interchangeable.

That is why the manifest sets `"new_install_improv_wait_time": 0`. The ESP Web
Tools default is to wait 10 seconds after flashing for an Improv Serial
response; with no Improv Serial in the firmware that is 10 seconds of waiting
for a reply that cannot come, followed by a dead end. Setting it to `0`
disables Improv Serial detection, so the install finishes immediately and the
user is sent to the Ezlo app or the serial console, which is where provisioning
actually happens today.

**When Improv Serial lands, delete that key** (or raise it) and the page gains
in-browser Wi-Fi setup with no other change.

## Why the binary is a single merged image

A flashable EzloPi install is four images at four offsets — bootloader,
partition table, OTA data, application. ESP Web Tools will not accept that form
for an ESP-IDF v4 project; it wants one image written at offset 0. CI therefore
runs `esptool.py merge_bin` and publishes `merged-firmware.bin` per target.

The offsets come from `build/flash_args`, which the build generates, so they are
never restated by hand and cannot drift from the partition table. This also
matters because they are not the same on every chip: the bootloader sits at
`0x1000` on ESP32 and ESP32-S2 but at `0x0` on ESP32-S3 and ESP32-C3.

`tests/ci/check_merged_bin.py` then verifies that every one of those images is
byte-identical inside the merged file at its own offset, and that the merged
file ends exactly where the last one does. A wrong offset or a dropped input is
a successful `esptool` run, so the exit code alone is not evidence.

## Publishing it

The `install-page` CI job assembles the directory on every push and uploads it
as an artifact, so it is always built and always checked. **Deploying it to
GitHub Pages is opt-in and off until a maintainer turns it on**, because
deploying needs repository settings that a pull request cannot change, and a job
that fails on `master` for that reason would be worse than no page at all.

To turn it on:

1. **Settings → Pages → Build and deployment → Source: GitHub Actions.**
2. **Settings → Secrets and variables → Actions → Variables →** add
   `PUBLISH_INSTALL_PAGE` = `true`.

The next push to `master` publishes to `https://<owner>.github.io/<repo>/`.
Until both are done the `deploy-install-page` job is skipped and nothing else
changes.

## Running it locally

Web Serial needs a secure context, so `file://` is not enough — serve the
directory over `localhost`, which counts as secure:

```bash
# from a checkout, with the four merged images already in place
cd install && python3 -m http.server 8000
# then open http://localhost:8000/
```

To get the images, either download the `install-page` artifact from a CI run and
unpack it here, or build a target yourself and merge it:

```bash
idf.py set-target esp32 && idf.py build
(cd build && esptool.py --chip esp32 merge_bin -o merged-firmware.bin @flash_args)
python3 tests/ci/check_merged_bin.py build
mkdir -p install/esp32 && cp build/merged-firmware.bin install/esp32/
```

`manifest.json` as committed carries `"version": "__VERSION__"`. CI replaces it
with the commit it built from; `tests/ci/check_install_page.py` rejects an
assembled page that still has the placeholder, so the page can never be
published claiming a version it does not have.
