# Onboarding an addressable LED strip

How to onboard a WS2812 / SK6812 strip on EzloPi so that the whole strip
lights, and what this firmware will and will not do with the configuration you
send.

This describes the **device configuration** document — the one carrying
`chipset` and `dev_detail`. That is a different document from the hub
provisioning payload (`user_id`, certificates, `cloud_server`), which is
documented in [`provisioning-schema.md`](provisioning-schema.md). You need both,
and they go in by different routes.

## The device

The driver is `device-0009-other-RMT-SK6812`, registered as
`device-rgp_strip-RMT_SK6812` with **`id_item` 9**. It is already in the
onboarding list; nothing has to be enabled to reach it.

It drives the strip over RMT and exposes six items: `rgbcolor`, `dimmer`,
`switch`, `dimmer_up`, `dimmer_down`, `dimmer_stop`.

## The keys

A `dev_detail` entry needs these, or the configuration is refused before it is
stored, or stored and then silently skipped at boot:

| Key | JSON type | Meaning |
|---|---|---|
| `device_id` | **string** | Refused if absent or not a string. A number here is refused. |
| `id_item` | number, non-zero | `9` for this driver. An entry with `0` or no `id_item` is skipped at boot with no log line naming it, which is why it is refused up front. |
| `dev_name` | string | Shown in the app. |
| `dev_type` | number | `5`, meaning `EZLOPI_DEVICE_INTERFACE_PWM`. |
| `gpio1` | number | The data pin the strip's DIN is wired to. |

And these three describe the strip itself:

| Key | JSON type | Default if absent or malformed | Meaning |
|---|---|---|---|
| `pixel_count` | number, integer, 1..2048 | `1` | How many LEDs are on the strip. |
| `rgbw` | boolean | `false` | True for an SK6812-**RGBW**, which has a fourth white channel and so four bytes per pixel instead of three. |
| `color_order` | string | `"grb"` | Byte order on the wire. Any of `grb`, `rgb`, `brg`, `rbg`, `gbr`, `bgr`, case-insensitive. |

Notes on the three strip keys, because each is a place where a reasonable guess
is wrong:

- **A quoted count is refused, not parsed.** `"pixel_count": "60"` resolves to
  `1`, not `60`. That is what a web form sends when nobody converted the field,
  and silently accepting it would make the string and number forms behave
  differently in a way nothing reports.
- **A fractional count is refused, not truncated.** `2.5` resolves to `1`. A
  rounded count gives a strip that works at almost the right length, which is
  the hardest kind of wrong to notice.
- **An out-of-range count falls back to `1`, not to the cap.** `100000`
  resolves to `1`. A number that large is far likelier a typo or a unit
  confusion than a request for the longest strip this can drive, and quietly
  lighting 2048 pixels would hide it. `1` is conspicuous and recoverable.
- **One bad key does not discard the others.** A good `pixel_count` beside a
  nonsense `rgbw` keeps the count.
- **If the strip lights the wrong colours, `color_order` is the key to change.**
  All six permutations are honoured. A strip that shows green when you asked
  for red is wired `rgb` rather than `grb` — the part number does not settle
  this, since strips sold as WS2812B have shipped in more than one order. The
  order is named by the order itself for that reason, rather than by chip.
- **Omitting all three is fully supported and is the pre-existing behaviour**:
  one GRB pixel, no white channel. Every strip provisioned before these keys
  existed has exactly that configuration, and continues to behave the same.

## A worked example

A 60-pixel SK6812-RGBW strip with DIN on GPIO 18, on an esp32:

<!-- machine-checked: strip-config -->
```json
{
  "chipset": "ESP32",
  "dev_detail": [
    {
      "device_id": "1002e001",
      "id_item": 9,
      "dev_name": "Hallway strip",
      "dev_type": 5,
      "gpio1": 18,
      "pixel_count": 60,
      "rgbw": true,
      "color_order": "grb"
    }
  ]
}
```

That document resolves to this strip shape:

<!-- machine-checked: strip-config-resolves-to -->
```json
{
  "pixel_count": 60,
  "rgbw": true,
  "color_order": "grb"
}
```

Both blocks above are **read out of this file and checked by the host test
suite** — see *How this file is checked*. They are not illustrative.

`chipset` must match the chip you are sending to: `ESP32`, `ESP32S2`,
`ESP32S3` or `ESP32C3`. A configuration naming a different chip is refused
rather than adopted, deliberately: the pin numbers in it were chosen for other
hardware, so honouring it would instantiate devices on whatever GPIOs those
numbers happen to name here.

## Getting it onto the device

Three routes. All three validate the document, store it verbatim — unknown keys
are preserved, so a configuration carrying a key a newer app added is not
refused by an older hub — and then differ in what happens next. **One of them
reboots your hub.** The differences matter more than the similarity, so each is
stated rather than tabulated.

### Serial, `cmd:3`

The one worth using from a script or a jig: no base64, no chunking, no
authentication. Write one JSON line to the UART control channel (see
[section 5 of the README](../README.md#5-the-serial-control-channel) for the
framing) with the configuration document at the **top level**, beside `cmd`:

<!-- machine-checked: strip-config-over-serial -->
```json
{
  "cmd": 3,
  "chipset": "ESP32",
  "dev_detail": [
    {
      "device_id": "1002e001",
      "id_item": 9,
      "dev_name": "Hallway strip",
      "dev_type": 5,
      "gpio1": 18,
      "pixel_count": 60,
      "rgbw": true,
      "color_order": "grb"
    }
  ]
}
```

The hub answers `{"cmd":3,"status_write":1}`, or `status_write` `0` if the
document was refused. **It does not reboot and does not tell you that one is
needed** — see *What this firmware will not do with it*.

Two things here are easy to get wrong, and both are the kind of mistake that
looks like it worked:

- **`cmd:3`, not `cmd:7`.** `cmd:7` is `EZPI_UART_CMD_SET_PROV`, which takes the
  *hub provisioning* payload — serial number, certificates, `cloud_server`. It
  is a different document on a different handler, and it is the one
  [`provisioning-schema.md`](provisioning-schema.md#serial-cmd7) documents.
  Sending `dev_detail` to it does not configure a strip. The number in the block
  above is checked against `EZPI_UART_CMD_SET_CONFIG` itself by the host suite,
  so it cannot drift from the enum.
- **Top level, not nested under `data`.** The provisioning path wraps its
  payload as `{"cmd":7,"data":{…}}`. This one does not: the whole received line
  is what gets stored, `cmd` member included. A document nested under `data`
  parses, is stored, and produces a hub with no devices.

### BLE, the dynamic-config characteristic

Service `0x00E5`, characteristic **`0xE501`** — *not* the provisioning
characteristic `0xE301`. Both are listed in the GATT table in
[`provisioning-schema.md`](provisioning-schema.md#gatt-layout), and the chunk
envelope (`sequence`, `len`, `total_len`, base64, 400-byte chunks) is the same
as the one documented there for provisioning.

**This route restarts the hub about 1.5 seconds after a configuration it
accepted**, which is how the strip comes up without you doing anything else. A
refused document does not restart it. The characteristic takes writes from
anyone in radio range, with no pairing.

### `hub.ezlopi.config.set`, over the local websocket or the cloud

The configuration goes in `params.config` as an object. On success the result
carries `deviceCount` and `rebootRequired: true`; the hub does **not** restart
itself, so the caller decides when. This route additionally refuses an entry
whose `id_item` names a driver this image was not built with, which the other
two do not check.

**The whole document must serialise to under 4096 bytes.** It is stored in a
`0x1000`-byte field and written with `cJSON_PrintPreallocated()` into a 4 KB
buffer, which fails rather than truncates. The three strip keys add roughly 50
bytes per strip, so this bites on the number of devices, not on the length of
any one strip. Only the websocket route checks that bound before storing; the
other two let the storer refuse the write.

## What this firmware will not do with it

- **Changing `pixel_count` takes effect on the next boot, not immediately.**
  The configuration is read in `__prepare()`, which runs at boot, and the strip
  is initialised once behind a latch. Re-shaping a running strip needs
  `led_strip_free()` plus a re-init, not an assignment.

  Whether anything tells you so depends on which route you used, which is the
  real trap: `hub.ezlopi.config.set` answers `rebootRequired: true`, BLE just
  restarts the hub for you, and **serial `cmd:3` says nothing at all** — it
  answers `status_write: 1` and leaves a hub that has stored your new pixel
  count and is still driving the old one. A jig that sends a config and then
  reads the strip will see the previous shape.
- **The 2048-pixel cap is derived on paper, not observed.** A WS2812/SK6812 bit
  is 1.25 µs, so an RGBW pixel is 40 µs and 2048 pixels is 81.9 ms of unbroken
  transmission — about 12 frames per second before the ≥50 µs reset gap. The
  cap sits where the strip stops looking animated on one RMT channel. No board
  has been driven at it.

## Not verified on hardware

**No board has driven a multi-pixel strip.** The configuration parsing is
host-tested and the driver compiles clean on all four CI targets, c3 included,
but the actuation — RMT timing at length, `rgbw` producing a fourth byte per
pixel, the reset gap at the cap — has never run on real hardware. Four green
builds remove "it might not even build there"; they say nothing about whether
the strip lights.

Treat the pixel counts and timings above as what the code computes, not as
measurements.

## How this file is checked

The three blocks marked `<!-- machine-checked: ... -->` are extracted from this
file by `tests/host/test_aledonboard.c`. The first is run through the real
`EZPI_util_devcfgval_check()` and `EZPI_aledcfg_from_json()`; the second is
what its resolution is compared against; the third is compared with the first.
The test fails if

- the example is not a configuration this firmware would store, or
- it does not resolve to the shape the second block claims, or
- the serial block's `cmd` is not `EZPI_UART_CMD_SET_CONFIG`, taken from the
  firmware's own enum rather than retyped here, or
- the serial block carries a `data` wrapper, or wraps a document that is not
  the worked example above, or
- any block is missing, unparseable, or its marker cannot be found.

The serial block is therefore a second copy of the example, which would
normally be the thing that rots — it is safe here only because the test
compares the two and goes red when they disagree.

A documentation example that has drifted away from the code is worse than no
example, because it is read as authoritative. This one cannot drift without
`make -C tests/host` going red.

The two GATT UUIDs and the `rebootRequired` behaviour above are **not**
machine-checked; they were read from the source and are listed in the table
below so the next reader can do the same.

## Source

| Thing | Where |
|---|---|
| The resolver | [`ezlopi_util_aledcfg.c`](../ezlopi-util/ezlopi-util-aledcfg/ezlopi_util_aledcfg.c) |
| Key spellings | [`ezlopi_cloud_keywords.c`](../ezlopi-cloud/ezlopi-cloud-constants/ezlopi_cloud_keywords.c) |
| The driver | [`device_0009_other_RMT_SK6812.c`](../ezlopi-sensors-devices/device-0009-other-RMT-SK6812/device_0009_other_RMT_SK6812.c) |
| What a configuration must contain | [`ezlopi_util_devcfgval.h`](../ezlopi-util/ezlopi-util-devcfgval/ezlopi_util_devcfgval.h) |
| The 4 KB store | [`ezlopi_core_factory_info.c`](../ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c) |
| The serial command numbers | [`ezlopi_service_uart.h`](../ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.h) |
| The serial handler, and what it stores | [`ezlopi_service_uart.c`](../ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c) |
| The GATT UUIDs | [`ezlopi_service_ble.h`](../ezlopi-services/ezlopi-service-ble/ezlopi_service_ble.h) |
| The BLE route, and its restart | [`ezlopi_service_ble_dynamic_config.c`](../ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_dynamic_config.c) |
| `rebootRequired` | [`ezlopi_cloud_devcfg.c`](../ezlopi-cloud/ezlopi-cloud-devcfg/ezlopi_cloud_devcfg.c) |
