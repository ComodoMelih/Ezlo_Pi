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

The two serial and BLE routes work on a hub straight out of the box. The third
does not: it is reached over the network, and a factory-fresh hub has no WiFi.
That prerequisite is a subsection of its own, immediately before it.

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

### First, if you want either network route: the hub has no WiFi

A factory-fresh hub is not on a network, and the only thing that says so is one
line on the serial log:

```
ERROR : WiFi Details not set !
```

([`ezlopi_main.c:257`](../ezlopi-main/ezlopi_main.c#L257).) Until that is dealt
with there is no local websocket to call and no cloud connection, so the route
below does not exist yet. The serial channel is the way in, with the
credentials at the **top level** exactly as `cmd:3` puts the configuration
there:

<!-- machine-checked: wifi-over-serial -->
```json
{
  "cmd": 2,
  "ssid": "your-network",
  "pass": "your-wifi-password"
}
```

Three things about this are worth knowing before you conclude it failed:

- **The password must be at least 8 characters.** A shorter one is refused
  outright, with no association attempted. The example above is checked against
  the firmware's own `EZPI_CORE_WIFI_PASS_CHAR_MIN_LEN`, so it cannot come to
  show a password this firmware would reject.
- **The answer can be ten seconds away.** The handler stores the credentials and
  then tries to associate `EZPI_CORE_WIFI_CONN_RETRY_ATTEMPT` times, waiting
  `EZPI_CORE_WIFI_CONN_ATTEMPT_INTERVAL` milliseconds each, and only replies
  after that loop finishes — 2 attempts and 5000 ms in this build. A read window
  shorter than that reports no answer for a hub that is still trying.
- **The reply carries two numbers and they mean different things.**
  `{"cmd":2,"status_write":1,"status":1}` is stored *and* associated.
  `status_write: 1` with `status: 0` is the interesting one: the credentials are
  in storage, and the access point was not reachable. Treating the pair as a
  single verdict loses that distinction, which is the difference between a typo
  and an AP out of range.

Once it is up, you do not need to hunt for the DHCP address: the hub advertises
itself over mDNS as `_ezlo._tcp` on
[`EZPI_SERV_WS_SERVER_PORT`](../ezlopi-user-config/EZLOPI_USER_CONFIG.h#L148),
17001 ([`ezlopi_core_mdns.c:590`](../ezlopi-core/ezlopi-core-mdns/ezlopi_core_mdns.c#L590)).

**One note on credentials in logs.** The firmware does not echo the password
back — the trace that used to print it is commented out at
[`ezlopi_service_uart.c:1290`](../ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L1290)
— but it *does* print the SSID on every failed attempt
([`:1326`](../ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L1326),
[`:1330`](../ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L1330)).
A boot log from a hub that could not associate carries the network name, which
is worth knowing before attaching one to a bug report.

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

## What is verified on hardware, and what is not

Split, because these two halves are now in different states and lumping them
together was how a real defect stayed hidden behind a green host suite.

**Verified on an ESP32 over serial `cmd:3`.** A board stores the document
above, and on the next boot the resolver reads all three keys from it: all six
`color_order` names are accepted, `"zzz"` and `"grbx"` are refused, and a
refused order leaves the good `pixel_count` in place rather than taking it down
with it. The refusal is reported as `sk6812: config not taken verbatim; using N
pixel(s)` on the serial log, so a jig can tell the two apart.

That took finding a defect that no host test could have caught. A device
configuration reaches the driver through `cJSON_ParseWithRef()`, whose strings
point into the stored payload and end at a quote rather than a NUL, so reading
`color_order` as a C string saw `grb"}]}` and **every colour order was refused
on a real board** while all 17,576 three-letter strings were classified
correctly on the host. If you are running firmware older than that fix, the
symptom is that warning line on every boot with a perfectly valid document.

**Verified on an ESP32 over serial `cmd:2`.** A board with no stored network
took the line above and answered `{"cmd":2,"status_write":1,"status":1}` —
stored *and* associated. On the next boot `ERROR : WiFi Details not set !` was
gone, the board printed a DHCP address on its own subnet, and the device list
still dumped, so storing credentials does not disturb the stored strip
configuration. The two status numbers were never seen to disagree, so
`status_write: 1` with `status: 0` is read from the handler, not observed.

**Not verified: the network routes themselves.** Nothing has ever connected to
this hub on 17001. The port number and the `_ezlo._tcp` advertisement above are
read from the source; no client has resolved that mDNS name or opened that
socket, and `hub.ezlopi.config.set` has never been called on real hardware by
any route. Getting WiFi up is as far as this has been taken.

**Not verified: anything that lights.** No board has driven a multi-pixel strip.
The actuation — RMT timing at length, `rgbw` producing a fourth byte per pixel,
the reset gap at the cap — has never run on real hardware, and neither has the
mapping: that `"rbg"` is *accepted* is measured, that it puts red where red goes
is not. The serial log reports accepted or refused and nothing more, so this
half needs eyes on a strip rather than a better test.

Treat the pixel counts and timings above as what the code computes, not as
measurements.

**The practical recipe, since the serial route tells you nothing.** Send the
`cmd:3` line, wait for `status_write: 1`, then reset the board — the
configuration is read in `__prepare()` at boot, so nothing you sent is in
effect until then. Send *after* the application is up, not straight after a
reset: the UART control channel does not exist in the bootloader, and a line
written too early is swallowed with no error, which looks exactly like a
refused document.

## How this file is checked

The four blocks marked `<!-- machine-checked: ... -->` are extracted from this
file by `tests/host/test_aledonboard.c`. The first is run through the real
`EZPI_util_devcfgval_check()` and `EZPI_aledcfg_from_json()`; the second is
what its resolution is compared against; the third is compared with the first.
The fourth, the WiFi line, is not a copy of anything here and is checked
against the firmware directly. The test fails if

- the example is not a configuration this firmware would store, or
- it does not resolve to the shape the second block claims, or
- the serial block's `cmd` is not `EZPI_UART_CMD_SET_CONFIG`, taken from the
  firmware's own enum rather than retyped here, or
- the serial block carries a `data` wrapper, or wraps a document that is not
  the worked example above, or
- the WiFi block's `cmd` is not `EZPI_UART_CMD_WIFI` — from that same enum, in
  which it is adjacent to `SET_CONFIG`, so a transposed 2 and 3 is caught — or
  it nests under `data`, or its `ssid` and `pass` are not under key names the
  firmware's own lookup would find — that lookup is case-insensitive, so `SSID`
  would work and `network` would not, and the test is deliberately no stricter
  than the firmware here — or
- the WiFi block's example password is shorter than
  `EZPI_CORE_WIFI_PASS_CHAR_MIN_LEN`, which the test reads as text out of
  `ezlopi-user-config/EZLOPI_USER_CONFIG.h` at runtime — it cannot `#include`
  that header on a host, and the host suite's stand-in for it would have made
  the check compare this document against a number typed into the test suite,
  or
- the WiFi block carries any member beyond those three, since the handler reads
  nothing else and a `dev_detail` pasted in beside the credentials would be
  accepted and silently ignored, or
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
