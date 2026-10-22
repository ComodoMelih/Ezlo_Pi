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

And these four describe the strip itself:

| Key | JSON type | Default if absent or malformed | Meaning |
|---|---|---|---|
| `pixel_count` | number, integer, 1..2048 | `1` | How many LEDs are on the strip. |
| `rgbw` | boolean | `false` | True for an SK6812-**RGBW**, which has a fourth white channel and so four bytes per pixel instead of three. |
| `color_order` | string | `"grb"` | Byte order on the wire. Any of `grb`, `rgb`, `brg`, `rbg`, `gbr`, `bgr`, case-insensitive. |
| `led_chip` | string | `"sk6812"` | Which family's bit timings to clock the data out at. One of `sk6812`, `ws2812` (alias `ws2812b`), `apa106`, `sm16703`, case-insensitive. |

Notes on the four strip keys, because each is a place where a reasonable guess
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
- **If the strip lights the wrong colours, `color_order` is the key to change,
  and there is a procedure that finds the right value in three looks.** Store
  `"rgb"`, then command pure red, pure green and pure blue in turn and write
  down the colour the strip actually shows for each. **Those three colours, in
  that order, spell the value to store.** Under `"rgb"` the first byte carries
  red, the second green and the third blue, so whatever appears for commanded
  red names the channel the strip puts first, and so on for the other two.

  Worked on the bench reel: commanded red showed **blue**, green showed **red**
  and blue showed **green**, so the value is `brg` — and storing `brg` made
  every primary come out as itself. Do not stop after one colour: commanding
  red alone cannot tell `grb` from `brg`, because both put red in the byte
  `grb` fills with red, and that near-miss is recorded further down this
  document because it was made here.

  The part number does not settle this — strips sold as WS2812B have shipped in
  more than one order, and this reel was labelled SK6812 and wired BRG. The key
  is named by the order itself for that reason, rather than by chip.
- **`led_chip` is a different question from `color_order`, and setting one does
  not affect the other.** `color_order` is which of the three bytes goes first;
  `led_chip` is how long a `1` bit is held high. A WS2812B wants 1000 ns and an
  SK6812 wants 600 ns. This is worth stating because the driver's own per-chip
  table carries a colour order as well as timings, so the obvious fear — that
  selecting `apa106`, whose row says RGB, would flip your strip's byte order —
  is true of that library's documented behaviour and false here: this firmware
  packs the frame buffer itself and never uses that field. Your `color_order`
  stays authoritative whatever `led_chip` says.
- **A chip name that is nearly right is refused, not approximated.** `ws2813`
  and `ws2815` are real parts with timings close to WS2812's, and both resolve
  to the default. That is deliberate: close enough that a guess would look
  defensible, far enough that a wrong guess would be invisible. Only the four
  families the driver actually has are accepted, plus `ws2812b` as an alias for
  `ws2812`, because the `B` is a packaging revision rather than a different
  protocol and it is what is printed on the reel.
- **Wrong timings are the one mistake here with no obvious symptom.** A wrong
  `pixel_count` lights part of the strip; a wrong `color_order` lights it in the
  wrong colours; both are plain on sight. Wrong timings are inside the parts'
  tolerances often enough that the strip mostly works and occasionally shows one
  wrong pixel. If that is what you are seeing, `led_chip` is the key to check,
  and the boot log names the one it settled on.
- **Omitting all four is fully supported and is the pre-existing behaviour**:
  one GRB pixel, no white channel, SK6812 timings. Every strip provisioned
  before these keys existed has exactly that configuration, and continues to
  behave the same. For `led_chip` in particular the default is the *shipped*
  chip rather than the *common* one — WS2812B is the more popular part, so
  defaulting to it would have suited more new installs and silently re-timed
  every existing one.

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

**First you have to get in, and the socket is plain `ws://`.** Reaching for
`wss://` gets a handshake failure that looks exactly like a wrong key, so it is
worth saying: the hub calls `httpd_start()` and not `httpd_ssl_start()`
([`ezlopi_service_ws_server.c:810`](../ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L810)),
and there is no TLS on this port at all.

An unauthenticated connection may call exactly one method,
`hub.offline.login.ui`
([`:336`](../ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L336));
everything else is refused until it succeeds. Its params are `{user, token}`
([`ezlopi_core_offline_login.c:143`](../ezlopi-core/ezlopi-core-offline-login/ezlopi_core_offline_login.c#L143)),
where `token` is the hub's local API key. In a default build the `user` field is
**not** checked — `__user_id_accepted()` returns true unless
`CONFIG_EZPI_OFFLINE_LOGIN_VERIFY_USER_ID` is defined
([`:344`](../ezlopi-core/ezlopi-core-offline-login/ezlopi_core_offline_login.c#L344)) —
so the local key is the only credential that matters. Authorisation is recorded
**per connection**, against the socket, so every later call has to go down the
same one; reconnecting means logging in again.

Where the key comes from: the hub prints it in its own boot log, inside a boxed
banner, when `CONFIG_EZPI_LOCAL_KEY_ANNOUNCE` is set
([`ezlopi_core_local_key.c:376`](../ezlopi-core/ezlopi-core-local-key/ezlopi_core_local_key.c#L376),
[`Kconfig.projbuild:443`](../ezlopi-main/Kconfig.projbuild#L443)). That is a
convenience for bring-up and a disclosure the moment a boot log is shared —
treat a captured log as carrying a credential, because it does.

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
above, and on the next boot the resolver reads all three of the keys that
document carries: all six `color_order` names are accepted, `"zzz"` and
`"grbx"` are refused, and a refused order leaves the good `pixel_count` in
place rather than taking it down with it. The refusal is reported as
`sk6812: config not taken verbatim; using N pixel(s), rgbw=0, chip=sk6812` on
the serial log, so a jig can tell the two apart. `led_chip` was added later and
is not part of this measurement — see below.

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

**Verified: the websocket route, end to end to the driver.** A client on the
same subnet opened `ws://<hub>:17001`, logged in with `hub.offline.login.ui`
carrying `{user, token}` where the token is the hub's local API key, and got
`{"result":{},"error":null}`. `hub.info.get`, `hub.devices.list` and
`hub.items.list` all answered: the strip came back as one device with six items,
`reachable` and `ready`. `hub.item.value.set` was then called on the `dimmer`
and `rgbcolor` items, and the hub's own report changed to match — `dimmer` went
from 100 to 10 to 30, and a deliberately odd `{"red":12,"green":200,"blue":37}`
read back as `#0cc825`. That last one is a true round trip: the getter
re-derives the channels from the pixel buffer through the configured colour
order, so the value reached the buffer and came back out again.

**Two things that round trip does *not* establish**, both worth stating because
each is easy to over-read. First, `hub.item.value.set` discards the item
function's return value — `EZPI_items_set_value_v3` adds an empty result object
and reports nothing about what the driver did — so `"error":null` means *the
method was dispatched*, not *the strip was driven*. The evidence that the driver
ran came from the serial log alongside: one `item_id:` trace per call, and no
`sk6812: could not fill the strip` from the refused-fill branch. Second, the
colour round trip goes out through the order mapping and back in through the
same mapping, so it is symmetric: an inverted mapping would round trip just as
cleanly. It cannot be used as evidence the wire order is right.

**Verified: a 30-pixel strip lights. The order it was verified in was wrong,
and one primary is why.** A board with
`{"gpio1":16,"pixel_count":30,"rgbw":false,"color_order":"grb"}` stored lit
along its whole length on `__init()`'s boot fill, and then rendered **red** for
`rgbcolor = {"red":255,"green":0,"blue":0}` sent to `20ec6000` over the local
API with the dimmer at 10. The length result stands: RMT timing works at 30
pixels and the one-LED defect does not recur.

**The order conclusion drawn from it did not.** It read "the configured order is
the wire order for at least that permutation", and that does not follow. Under
`grb` the packer emits byte0=G, byte1=R, byte2=B
([ezlopi_util_aled.c:32](../ezlopi-util/ezlopi-util-aled/ezlopi_util_aled.c#L32)
maps `grb` to `{1,0,2}`), and a strip wired **BRG** maps byte1 to red as well.
Red coming out red is consistent with GRB *and* BRG, so the one measurement
taken could not distinguish the configured order from the one this reel
actually has.

All three primaries on that same board, config still `grb`, dimmer at 10:

| commanded | strip showed | therefore |
|---|---|---|
| `{"red":0,"green":255,"blue":0}` | **blue** | byte0 → blue |
| `{"red":255,"green":0,"blue":0}` | **red** | byte1 → red |
| `{"red":0,"green":0,"blue":255}` | **green** | byte2 → green |

A complete permutation: this reel is **BRG**, not GRB. The third row was
predicted from the first two before it was looked at, which is what separates a
measurement from three anecdotes.

`color_order` was then stored as `"brg"` over `cmd 3`, read back off the next
boot, taken verbatim, and green commanded again — **green**, where the same
command had given blue. The recipe at the top of this document was checked the
same way: `"rgb"` stored, commanded red predicted to show blue, and it did.
Two predictions, two hits.

**What this cost, and the general lesson.** Every signal said the strip was
fine. The config echoed back correctly, no fill was refused, the API returned
`"error":null`, and the earlier sweep showed all six order names *accepted*.
None of that is a statement about which order this reel wants. **Acceptance and
correctness were two different measurements and the first was allowed to stand
in for the second** — the same shape of mistake as the discarded fill result
recorded in the CHANGELOG, where the only thing that could have disagreed was
never consulted.

**Still not verified, and narrowed rather than closed.** One length is not a
range: `rgbw` producing a fourth byte per pixel is still unexercised, because
this strip is configured `rgbw: false` and the white channel stays unreachable
in practice; so is the reset gap at the cap. Of the six orders, three have now
been lit — `grb` and `rgb` as diagnostics, `brg` as the correct one — and the
other three, `rbg`, `gbr` and `bgr`, are accepted but unlit. Checking them
means storing each in turn
and rebooting, since the order is resolved once in `__prepare()`.

**A reset does not clear the strip, and a stale frame looks exactly like a
fault.** The LEDs hold their last frame as long as they have power, and
resetting or reflashing the ESP32 does not interrupt that. A strip left showing
red by an earlier experiment — or by whatever firmware the board shipped with —
still shows red while the hub boots, and goes on showing it if the data line is
broken. Before concluding anything from the colour on a strip, make the
firmware drive it and watch it change: `__init()` fills `{255,255,255}` at
brightness 255, so a board that is driving its strip at all turns it **white**
within a second or two of a reset. That one observation separates "not driven"
from "driven to the wrong colour", and they have completely different causes.

**Before suspecting the firmware when a strip is dark, check the hardware.**
This cost a day. Every API-level signal was green, the config echoed back
correctly, `led_strip_init`/fill/flush all returned `ESP_OK`, a four-way RMT
timing sweep found nothing and a 20-pin GPIO sweep found nothing — because the
fault was never in the firmware. Ask first: is there a DC power input separate
from USB, and is anything in it (on controllers of this class the strip's V+
comes from there, not from USB, so a USB-only board can have a dead output rail
and a perfect data line); which end of the strip is in the connector, since
they are directional; and is the strip 5 V or 12 V.

**Not verified on hardware: `led_chip` selecting anything but the default.**
Three things are measured and they are all host-side or compile-side: the
resolver accepts the five accepted names and refuses the near-misses (140
checks in `tests/host/test_aledcfg.c`, with the suite confirmed to fail against
a subject that defaults to WS2812 and against one that accepts `ws2812` as
`ws2812b`); the firmware builds; and omitting a family from the driver's
mapping switch is a build error rather than a silent fallback, measured by
deleting a case and rebuilding.

None of that is a strip lit at WS2812 timings. The bench strip **is** an
SK6812, and its stored config carries no `led_chip`, so it resolves to exactly
the timings it was already being driven at — re-flashing it is a regression
check and nothing more. Confirming the other three families needs either a
strip of that family or a scope on the data line, and neither has been done.
What *is* worth noting is the direction of the risk: the default is unchanged,
so this key can only be wrong for someone who sets it.

**Not verified: `hub.ezlopi.config.set`.** It has still never been called on real
hardware by any route. The route to it is now known to work, which is a
different claim.

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
