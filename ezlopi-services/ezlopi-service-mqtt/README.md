# MQTT / Home Assistant discovery

Publishes this hub's items to an MQTT broker using Home Assistant's MQTT
Discovery, so devices wired to an EzloPi show up in Home Assistant without any
YAML. Off by default; enable `CONFIG_EZPI_ENABLE_MQTT` and set a broker URI.

This is an addition, never a replacement. The hub still talks to the Ezlo cloud
exactly as before.

## Configuration

    idf.py menuconfig  ->  EzloPi Configuration  ->  MQTT

| Option | Default | Notes |
|---|---|---|
| `EZPI_ENABLE_MQTT` | off | Nothing below matters until this is on. |
| `EZPI_MQTT_BROKER_URI` | `""` | e.g. `mqtt://192.168.1.50:1883`. Empty means the service logs and does not start. |
| `EZPI_MQTT_USERNAME` / `_PASSWORD` | `""` | Sent only when the username is non-empty. |
| `EZPI_MQTT_TOPIC_PREFIX` | `ezlopi` | State and command topics live under this. |
| `EZPI_MQTT_DISCOVERY_PREFIX` | `homeassistant` | Must match Home Assistant's MQTT integration setting. |

### Configuring it without a rebuild

The five settings above are **fallback defaults, not the live configuration**.
Each is stored in NVS and reachable over the local websocket API, so a shipped
image can be pointed at a site's broker without a toolchain:

| Method | Params | Result |
|---|---|---|
| `hub.mqtt.config.get` | none | `uri`, `username`, `topicPrefix`, `discoveryPrefix`, `passwordSet` |
| `hub.mqtt.config.set` | any subset of `uri`, `username`, `password`, `topicPrefix`, `discoveryPrefix` | `rebootRequired: true` |
| `hub.mqtt.config.reset` | none | `rebootRequired: true` |

A Kconfig value is used only when NVS holds nothing for that setting, or holds
something that no longer passes validation. `reset` clears all five, which
returns the hub to whatever was compiled in.

Things worth knowing before building a UI on this:

- **`get` never returns the password.** It returns `passwordSet`, which is what
  a settings screen actually needs: it distinguishes "leave the existing
  password alone" from "clear it".
- **`set` takes a subset and applies all of it or none of it.** Every supplied
  field is validated before anything is written, so a typo in the URI cannot
  also wipe the username. It is not a transaction — a power cut between two NVS
  writes still leaves a partial apply — but a *rejected* request changes
  nothing, which is the case that happens to people. A partially written
  setting is revalidated when read and falls back to the compiled-in default.
- **Over-long values are refused, not truncated.** A 200-character URI
  shortened to 128 is still a valid URI, and would have been stored as an
  address nobody typed.
- **`""` is meaningful for three fields.** An empty `uri` is how MQTT is
  switched off again without reflashing; empty `username`/`password` mean an
  anonymous broker. The two prefixes cannot be empty. An absent field and an
  empty one are therefore different things, and are treated as such.
- **A password may contain spaces** and non-ASCII bytes; a topic prefix may
  not, and may never contain `+` or `#`.
- **Nothing takes effect until the next boot.** The client is created once at
  startup, so both `set` and `reset` report `rebootRequired: true` rather than
  implying the broker has already changed.
- **Runtime configuration needs `CONFIG_EZPI_LOCAL_WEBSOCKET_SERVER`**, because
  the method table it lives in only exists when a websocket transport is
  enabled. With MQTT on and both websocket options off — the shape the
  `firmware-mqtt` CI job builds — Kconfig is the only way in.
- Authentication is the transport's, not these methods': the local websocket
  server refuses every method except `hub.offline.login.ui` until a user is
  logged in, so these inherit that and deliberately do not add a second check.

Host tests for the three methods are in `tests/host/test_mqtt_api.c` (96
checks), and for the validators in `tests/host/test_mqttcfg.c` (106 checks).
None of it has yet been exercised against a real broker on hardware; see "What
has been verified" below.

## Topics

The node id is `ezlopi_` plus the low three bytes of the Wi-Fi station MAC. It
is deliberately **not** the Ezlo serial: `EZPI_core_factory_info_v3_get_id()`
returns 0 until the hub has been provisioned against the Ezlo cloud, and a
board flashed from this repository and pointed at a local broker never is.

    homeassistant/<component>/<node>/<object>/config   discovery (retained)
    ezlopi/<node>/<object>/state                       state (retained)
    ezlopi/<node>/<object>/set                         commands, writable items only
    ezlopi/<node>/status                               online / offline (LWT, retained)

`<object>` is the reduced device name plus the device and item ids as eight hex
digits each. The ids are what the command topic is parsed back into, so they
are never truncated; if a name is too long for the buffer it is the *name* that
loses its tail.

Changing `EZPI_MQTT_TOPIC_PREFIX` after entities exist orphans them, because
`unique_id` is derived from the object id and Home Assistant keys its registry
on that. Old retained messages also stay on the broker under the old prefix
until something clears them.

## What is published

The mapping from an EzloPi item to a Home Assistant entity lives in
`ezlopi-util/ezlopi-util-ha`, which has no ESP-IDF dependencies so it can be
swept on a desktop (`tests/host/test_ha.c` — 183 checks over 70,564 real item
combinations). That separation is not tidiness: a wrong mapping is invisible on
the device, because Home Assistant simply drops a malformed discovery payload
and the entity never appears, with no error anywhere to explain why.

Items map to four platforms: `sensor`, `binary_sensor`, `switch` and `number`.
One Home Assistant *device* is created per EzloPi device, which is what makes a
DHT22's temperature and humidity appear as two entities grouped under one
device rather than as two unrelated things.

Items with `show == false` are skipped — an item the hub does not put in its
own UI is not something to put on a dashboard either.

## Known gaps

These are real and deliberate, not oversights:

- **RGB items are not published at all.** `value_type` of `rgb` maps to
  `EZPI_HA_COMPONENT_NONE`. Doing this properly means the `light` platform with
  an `rgb_command_topic` and a JSON schema, and grouping the on/off and
  brightness items of the same device into that single light. Published as a
  `number` instead, a colour would be unusable.
- **A dimmable lamp appears as a `switch` plus a separate `number`,** not as one
  `light` with brightness. Same reason: grouping several items into one entity
  needs a layer above per-item mapping, which does not exist yet.
- **`dimmer_up` / `dimmer_down` / `dimmer_stop` are skipped.** They are
  momentary commands, not state, and Home Assistant has no entity that means
  "press and hold".
- **`array`, `token` and `fingerprint_action` value types are skipped.**
- **TLS is not wired up.** `mqtts://` needs `CONFIG_MQTT_TRANSPORT_SSL`, which
  the committed `sdkconfig.defaults*` files currently turn off, and a CA bundle.
  Until then the broker password travels in plaintext and is also compiled into
  the image, so anyone who can read the firmware can read it.
- **On an Ethernet-only board the broadcast pump never starts.** This service
  itself is fine — it is started before the Wi-Fi gate on purpose (see below) —
  but `EZPI_service_broadcast_init()` sits after
  `EZPI_core_wait_for_wifi_to_connect(portMAX_DELAY)` in `ezlopi_main.c`, and
  that waits on an event bit only the Wi-Fi driver ever sets
  (`ezlopi_core_wifi.c:843` is its only setter; `ezlopi-core-ethernet/` never
  touches the event group). That is a pre-existing defect in the boot sequence,
  not something this service introduced, and fixing it means reordering a path
  the Ezlo cloud transports also depend on.

## Design notes

**Nothing in the core was modified.** State updates are observed through
`EZPI_core_broadcast_method_add()`, the same registry the two websocket
transports already use. The service touches zero existing lines of core code.

**Everything on the broadcast path uses `esp_mqtt_client_enqueue()`.** A
registered broadcast method runs on the loop-service task *while that task
holds the single global serialization buffer*, so anything slow here delays the
Ezlo cloud broadcast queued behind it. `enqueue` hands the message to the MQTT
client's own task and returns without touching the network; `publish` can wait
on the client.

**The client starts unconditionally and retries.** It is deliberately not gated
on `EZPI_core_wait_for_wifi_to_connect()` — see the Ethernet note above.
esp-mqtt's own reconnect loop is indifferent to which interface came up, which
makes that the one gate that cannot be wrong.

**Commands reuse the cloud's own entry point.** An inbound `/set` is turned into
the same `hub.item.value.set` request shape the cloud sends and handed to
`EZPI_items_set_value_v3()`, so drivers see no difference between a cloud write
and an MQTT one. A payload that is not entirely a number is refused rather than
half-parsed, and no state is echoed back: the driver's own value-changed path
broadcasts the result, so a failed relay shows as unchanged instead of as on.

**Discovery is re-announced on every reconnect.** A broker restarted without
persistence has forgotten every retained config, and the hub is the only thing
that can tell it again.

## What has been verified, and what has not

Built and link-checked for esp32 in three configurations: MQTT off, MQTT on
alongside both websocket transports, and MQTT on with both websocket transports
off. The last one is why `ezlopi_main.c`'s broadcast-init condition includes
`CONFIG_EZPI_ENABLE_MQTT`: removing just that term was confirmed to make
`EZPI_service_broadcast_init()` disappear from `app_main`, which would leave
`__broadcast_queue_func` NULL and silently drop every item update.

With the option off, the image was compared byte by byte against one built from
the commit before this service existed. The only differences were the app
descriptor's build metadata, the image's trailing checksum, and eleven bytes
holding `__LINE__` values that `TRACE_*` embeds in `ezlopi_main.c`, which moved
only because the init call added sixteen lines above `__blinky`. No generated
code differs.

The mapping and topic layers are host-tested under ASan/UBSan and were
mutation-tested at 12/12 caught.

The runtime configuration added later was built for four configurations, and
with MQTT off the image is byte-identical in size to the pre-change baseline
(0x1903f0 either way) — the new component costs the default builds nothing. A
symbol dump was the check that mattered: before the API methods existed, all
five NVS setters were unreferenced and the linker discarded them, so the hub
could *read* a broker URI it had no way to write. All five link now.

The three methods are host-tested (`tests/host/test_mqtt_api.c`, 96 checks
under ASan/UBSan) and mutation-tested at 6/6 caught: storing before validating,
truncating instead of refusing, returning the password, treating `""` as
absent, and ignoring a failed write are each caught by the group that claims to
cover them. The test also found two real bugs on its first run, which is the
better argument for it: a value of exactly the maximum length was rejected
because cJSON's `str_value_len` counts the terminator, and a password
containing a space was refused because the credential check was sharing a
helper with the URI path.

**None of this has run on hardware.** No broker has been connected, no entity
has appeared in a real Home Assistant, no command has switched a real relay,
and no `hub.mqtt.config.set` has been sent to a live websocket. The
compile-time and host-level evidence above is exactly that, and nothing more.
