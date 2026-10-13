# Provisioning schema

How to configure an EzloPi hub from something other than the Ezlo mobile app.

Everything below was read out of the source rather than inferred from field
names, because the JSON keys and the C identifiers do not always agree. The
device type is reported as `device_type_ezlopi`; the cloud URL is
`cloud_server` on the two inbound paths but `coordinator_url` in the cloud
response; and the provisioning UUID is `uuid_provisioning` over BLE but
`provisioning_uuid` over the serial line — two different constants whose
values differ by word order, which is easy to misread. File and line
references are given so each claim can be checked.

Where behaviour is surprising it is called out rather than smoothed over. Read
[Security properties](#security-properties) before exposing any of this.

---

## The three provisioning paths

A hub can be configured three ways, and **no two of them share a schema.**
This is the single most important thing on this page: a payload that works
over BLE will not work over the serial line, and vice versa.

| | BLE provisioning | Serial `cmd:7` | Cloud provision-sync |
|---|---|---|---|
| Direction | **Inbound** — a client writes to the hub | **Inbound** — written to the UART | **Outbound** — the hub GETs from Ezlo |
| Transport | GATT write, chunked, base64 | plain JSON, one write | HTTPS GET, TLS with the stored CA |
| Payload shape | flat object | nested under `data` | nested under `data` |
| Authentication | `user_id` string match (see below) | **none at all** | `provision_token` in the query string |
| Who can reach it | anyone in radio range | anyone with physical UART access | whoever controls the cloud response |
| Entry point | `ezpi_provisioning_info_write_func()` | `ezpi_service_uart_process_provisioning_api()` | `__provision_update()` |
| Source | `ezlopi-service-ble/ezlopi_service_ble_provisioning.c` | `ezlopi-service-uart/ezlopi_service_uart.c:570` | `ezlopi-service-webprov/ezlopi_service_webprov.c:601` |

Three key names differ between the two inbound paths, so they are worth
stating side by side:

| Value | BLE key | Serial `cmd:7` key |
|---|---|---|
| provisioning UUID | `uuid_provisioning` | `provisioning_uuid` |
| wrapper | *(none — flat)* | `data` |
| owner id | `user_id` *(required)* | *(not read)* |

If you are building a tool to provision a hub on a local network, **BLE or the
serial line are the two real options.** The HTTP path is not a server on the
device; there is no endpoint to POST to. It is the hub calling out to
`https://ezlopiesp32.up.mios.com/provision-sync` and applying what comes back,
and it is documented here only so the three schemas are not confused.

---

## BLE provisioning

### GATT layout

All UUIDs are 16-bit (`ESP_UUID_LEN_16`), defined in
[`ezlopi_service_ble.h`](../ezlopi-services/ezlopi-service-ble/ezlopi_service_ble.h).

| Service | UUID | Characteristic | UUID | Access in shipped builds |
|---|---|---|---|---|
| Provisioning | `0x00E3` | config | `0xE301` | **write only** |
| Provisioning | `0x00E3` | status | `0xE302` | **not present** |
| Dynamic config | `0x00E5` | device config | `0xE501` | write |
| Device info | `0x00E4` | info | `0xE401` | read |
| Wi-Fi | `0x00E1` | credentials | `0xE101` | write |

Write the provisioning payload to **`0xE301`**.

The provisioning service is more limited than the header suggests, and this
trips people up. `ezlopi_service_ble_provisioning.c` can build `0xE301` as
read+write+notify+indicate, and can register a status characteristic `0xE302`,
but both sit behind `EZPI_SERV_BLE_ENABLE_READ_PROV` and
`EZPI_SERV_BLE_ENABLE_STAT_PROV` — and both of those are `#define`d and then
**unconditionally `#undef`'d** lower down in
[`EZLOPI_USER_CONFIG.h`](../ezlopi-user-config/EZLOPI_USER_CONFIG.h) (lines
153-154, then 210-211, under "Disable user configs here").

So in every shipped build:

- `0xE301` is `ESP_GATT_PERM_WRITE` with only `PROP_BIT_WRITE`. **You cannot
  read your configuration back from it**, and there are no notifications.
- `0xE302` **is never registered at all.** Do not wait on it.

Use `0xE401` on the device-info service to read state back.

### Chunk framing

A provisioning payload is far larger than one GATT write, so it is split. Each
write carries this envelope:

```json
{ "total_len": 4096, "sequence": 1, "len": 500, "data": "<base64>" }
```

| Key | Type | Meaning |
|---|---|---|
| `total_len` | number | total base64 **characters** across all chunks |
| `sequence` | number | 1-based chunk index |
| `len` | number | base64 **characters** in this chunk |
| `data` | string | this chunk of the base64 text |

Three things matter and are easy to get wrong:

- **`total_len` and `len` count base64 characters, not decoded bytes.** The
  staging buffer holds the base64 *text*; the decode afterwards produces about
  3/4 as many bytes.
- **`total_len` must be 1..32768.** A payload claiming more is refused
  outright. The cap is derived: everything a provisioning write can persist
  lands below `EZLOPI_FINFO_OFFSET_HUB_DATA` (`0x6000`), and base64 expands by
  4/3, so `(0x6000 / 3) * 4 = 32768`.
- **`len` is validated against the actual `data` string and the remaining
  buffer space, and truncated to whichever is smaller.** It is not trusted.
  Chunks whose lengths do not sum to `total_len` will produce a short, and
  therefore almost certainly unparseable, payload rather than anything worse.
  See the clamp note in [`CHANGELOG.md`](../CHANGELOG.md); this used to be an
  unauthenticated heap overflow.

Reassembly concatenates the `data` members in arrival order, base64-decodes the
result, and parses it as the JSON object described next.

> The decode return value is not checked. A payload that is not valid base64
> yields an empty string, which then fails to parse and is rejected — correct
> behaviour, but by accident of the buffer being zeroed first rather than by
> design.

### Configuration payload

The decoded JSON is a flat object. Only `user_id` is required.

The length column is the **buffer size**. The string fields are filled with
`snprintf(dst, sizeof(dst), ...)`, so the most characters you can actually
store is one fewer — 63 for a 0x40 field — with the last byte taken by the NUL
terminator.

| Key | JSON type | Stored as | Buffer size |
|---|---|---|---|
| `user_id` | string | NVS `user_id` | 32 |
| `serial` | number | `id`, uint32 | — |
| `version` | number | `config_version`, uint16 | — |
| `device_name` | string | factory info | 0x40 (64) |
| `manufacturer_name` | string | factory info | 0x40 (64) |
| `brand` | string | factory info | 0x40 (64) |
| `model_number` | string | factory info | 0x40 (64) |
| `uuid` | string | factory info | 0x7E (126) |
| `uuid_provisioning` | string | factory info | 0x7E (126) |
| `mac` | string | factory info | 0x40 (64) |
| `provision_server` | string | factory info | 0x100 (256) |
| `cloud_server` | string | factory info | 0x100 (256) |
| `provision_token` | string | factory info | 0x200 (512) |
| `local_key` | string | factory info | 0x100 (256) |
| `signing_ca_certificate` | string (PEM) | factory info | 0x1000 (4096) |
| `ssl_private_key` | string (PEM) | factory info | 0x1000 (4096) |
| `ssl_shared_key` | string (PEM) | factory info | 0x1000 (4096) |

> **The CA certificate key is `signing_ca_certificate`, not `ca_cert`.** This
> is worth stating because the header declares both
> (`ezlopi_signing_ca_certificate_str` and `ezlopi_ca_cert_str`) and the
> shorter one looks like the obvious choice. All three *write* paths read
> `signing_ca_certificate`. `ca_cert` appears exactly once in the tree, in the
> read-back JSON built by `ezpi_provisioning_info_jsonify()` — which sits
> inside `#ifdef EZPI_SERV_BLE_ENABLE_READ_PROV` and is therefore not compiled
> into any shipped build. Sending `ca_cert` is silently ignored.

The three PEM fields are the exception: they are handed to the factory-info
setters as cJSON items rather than copied through a stack buffer, so `0x1000`
there is the size of the flash region, not an `snprintf` bound.

Notes:

- **Over-long values are silently truncated, not rejected.** The string fields
  are copied with `snprintf` into fixed buffers, so a 300-character
  `device_name` is stored as its first 63 characters and the write still
  reports success. Check lengths yourself before sending.
- **`device_type` is not settable.** The code that read it is commented out and
  the field is set to `NULL`. `EZPI_core_factory_info_v3_get_device_type()`
  returns the constant `"ezlopi_generic"`. Note the read direction reports it
  under the key `device_type_ezlopi`, not `device_type`.
- The three PEM fields are handled as JSON items rather than copied strings, so
  embedded newlines in the PEM are preserved as normal JSON `\n` escapes.
- **`user_id` is required and must be non-empty.** A payload that omits it, or
  sends `""`, is rejected with no change to the device.
- **Omitted keys are safe to omit.** Every destination buffer is zeroed before
  the extraction, so an absent key leaves an empty string rather than stale
  data. A minimal payload is just `{"user_id": "..."}`, which claims the hub
  without configuring anything else.

### Checking the result

Read **`0xE401`** on the device-info service. It returns a JSON object that
includes a `provisioned_status` boolean. This is the only option in a shipped
build — `0xE301` is write-only and `0xE302` is not registered, as described
above.

**"Provisioned" means exactly one thing: the stored `cloud_server` string
contains the substring `https://`.** It is not a flag and not a checksum —
see `EZPI_core_factory_info_v3_get_provisioning_status()` in
[`ezlopi_core_factory_info.c:215`](../ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L215).
So a hub given a `cloud_server` of `http://...` reports itself unprovisioned
no matter what else succeeded.

---

## Serial `cmd:7`

The simplest path, and the one worth using from a script or a jig: no base64,
no chunking, no authentication. Write one JSON object to the UART (see
[section 5 of the README](../README.md#5-the-serial-control-channel) for the
framing of the control channel in general) and the hub answers
`{"cmd":7,"status":1}`.

Unlike BLE, the payload is **nested under `data`**:

```json
{
  "cmd": 7,
  "data": {
    "serial": 105200073,
    "version": 2,
    "device_name": "EzloPi_Hub",
    "provisioning_uuid": "...",
    "cloud_server": "https://...",
    "signing_ca_certificate": "-----BEGIN CERTIFICATE-----\n..."
  }
}
```

| Key | JSON type | Stored as | Buffer size |
|---|---|---|---|
| `serial` | number | `id`, uint32 | — |
| `version` | number | `config_version`, uint16 | — |
| `device_name` | string | factory info | 0x40 (64) |
| `manufacturer_name` | string | factory info | 0x40 (64) |
| `brand` | string | factory info | 0x40 (64) |
| `model_number` | string | factory info | 0x40 (64) |
| `uuid` | string | factory info | 0x7E (126) |
| `provisioning_uuid` | string | factory info | 0x7E (126) |
| `mac` | string | factory info | 0x40 (64) |
| `provision_server` | string | factory info | 0x100 (256) |
| `cloud_server` | string | factory info | 0x100 (256) |
| `provision_token` | string | factory info | 0x200 (512) |
| `local_key` | string | factory info | 0x100 (256) |
| `signing_ca_certificate` | string (PEM) | factory info | 0x1000 (4096) |
| `ssl_private_key` | string (PEM) | factory info | 0x1000 (4096) |
| `ssl_shared_key` | string (PEM) | factory info | 0x1000 (4096) |

Differences from the BLE schema that will bite you:

- **The wrapper.** BLE takes a flat object; this takes `{"cmd":7,"data":{...}}`.
  A flat payload here parses fine and then does nothing, because `data` is
  absent and the function answers `status:0`.
- **`provisioning_uuid`, not `uuid_provisioning`.** The two paths use
  separately declared constants whose string values differ in word order.
  Getting this wrong stores an empty provisioning UUID and still reports
  success.
- **No `user_id`, and none is read.** There is no ownership check on this
  path at all.
- **No `device_type`** here either; it is set to `NULL`, the same as on the
  other two paths.
- Over-long values are truncated exactly as on the BLE path, and omitted keys
  are safe to omit — every destination buffer is zeroed first.

**The provisioned-state guard on this path is disabled.** The code reads:

```c
// if (EZPI_core_factory_info_v3_get_provisioning_status())
if (0)
```

so an already-provisioned hub will accept a `cmd:7` and overwrite its
configuration. That is upstream's code, left as-is: the commented-out line
shows the intent was to refuse, but enabling it would change behaviour that
Ezlo's own tooling may rely on. Treat it as "this path always applies", not as
a guarantee that it checks.

---

## Cloud provision-sync

Included for completeness. This is **not** an endpoint on the hub.

The hub issues:

```
GET https://ezlopiesp32.up.mios.com/provision-sync?token=<provision_token>&version=<config_version>
```

and only if `ssl_private_key`, `ssl_shared_key`, `ca_cert` and
`provision_token` are all already stored. Responses are handled as:

- **200** with a `data` object — applied, then the hub reboots.
- **200** with an `error_code` key — treated as "nothing to do".
- **304** — config unchanged.
- neither `data` nor `error_code` — retried, up to 5 times.

The `data` object uses **different key names from the BLE schema**:

| Key | Stored as |
|---|---|
| `id` | `id` (parsed from string with `strtoul`) |
| `uuid` | device UUID |
| `cloud_uuid` | provisioning UUID |
| `version` | `config_version` |
| `coordinator_url` | `cloud_server` |
| `provision_token` | `provision_token` |
| `ssl_private_key` | SSL private key |
| `ssl_public_key` | SSL public key |
| `ssl_shared_key` | SSL shared key |
| `signing_ca_certificate` | CA certificate |

Note `coordinator_url` → `cloud_server` and `signing_ca_certificate` →
`ca_cert`: the same two values have different names on the two paths. This
response also cannot set `brand`, `device_name`, `device_type`, `manufacturer`
or `model_number` — they are explicitly set to `NULL`.

---

## Security properties

These are properties of the shipped firmware, established by reading it. They
are stated plainly because anyone writing a provisioning tool needs them, and
because some of them are deliberate compatibility decisions that are not
going to change.

**The provisioning characteristic is unauthenticated at the transport layer.**
It is declared `ESP_GATT_PERM_WRITE`, not `ESP_GATT_PERM_WRITE_ENCRYPTED`, and
`CONFIG_EZPI_BLE_ENABLE_PASSKEY` is unset in every shipped
`sdkconfig.defaults.*`. No pairing stands between a client in radio range and
the parser. `CONFIG_EZPI_BLE_ENABLE=y` in the esp32, esp32c3 and esp32s3
defaults (esp32s2 has no BLE radio).

**The serial path has no authentication of any kind.** `cmd:7` reads no
`user_id` and checks no ownership, and its provisioned-state guard is disabled
(`if (0)`), so anything that can write to the UART can overwrite a hub's
entire cloud configuration — including `cloud_server`, `provision_token` and
the CA certificate — whether or not the hub is already claimed. On a board
whose USB serial port is exposed, that is the weakest link on this page. This
is upstream behaviour and is left alone deliberately; the control channel is
treated as physically trusted.

**`user_id` is matched as a prefix, not compared for equality.** The check is
`strncmp(stored_user_id, user_id, strlen(stored_user_id))`
([`ezlopi_service_ble_ble_auth.c`](../ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_ble_auth.c)),
and the length comes from the *stored* id — so a hub claimed by `"123"`
accepts `"1234567"`. This is intentional and carries an in-code comment saying
so: it is the comparison the shipped Ezlo app and cloud already authenticate
against, and tightening it unilaterally would lock existing hubs out.

**First claim wins.** If no `user_id` is stored yet, the offered one is written
to NVS and the write succeeds. An unclaimed hub belongs to whoever reaches it
first. The empty-`user_id` case is closed at storage time, so a hub cannot be
claimed by an absent or empty id.

**A factory reset can be triggered over BLE without authentication**, and it
does clear ownership. Characteristic `0xE203` calls
`EZPI_core_reset_factory_restore()`, which erases both the factory-info
partition and NVS (`nvs_flash_erase()`), so the stored `user_id` goes with it.
Both reset characteristics are registered at `ESP_GATT_PERM_WRITE`.

Note that `EZPI_core_factory_info_v3_factory_reset()` on its own erases only
the factory-info partition and leaves NVS — including `user_id` — intact.
A reset through that function alone would wipe the configuration while
leaving the hub claimed.

**Nothing here is end-to-end authenticated or integrity-protected.** The
payload is plaintext over an unencrypted GATT link. Treat BLE provisioning as
something to do on a trusted premises, not over the air in a shared space.

---

## Verifying against the source

| Claim | Where |
|---|---|
| wire key names | `ezlopi-cloud/ezlopi-cloud-constants/ezlopi_cloud_keywords.c` |
| chunk framing, `total_len` cap, clamp | `ezlopi_service_ble_provisioning.c` |
| BLE field extraction and buffer sizes | `ezlopi_service_ble_provisioning.c:345-475` |
| `ca_cert` only in the read-back path | `ezlopi_service_ble_provisioning.c:752-785` |
| serial `cmd:7` schema, disabled guard | `ezlopi_service_uart.c:570-660` |
| buffer size constants | `ezlopi_core_factory_info.h:119-142` |
| how fields are written to flash | `EZPI_core_factory_info_v3_set_basic()` |
| UUIDs | `ezlopi_service_ble.h:104-270` |
| characteristic permissions as built | `ezlopi_service_ble_provisioning.c:215-234` |
| `READ_PROV`/`STAT_PROV` undef'd | `EZLOPI_USER_CONFIG.h:153-154, 210-211` |
| `user_id` prefix match, first-claim-wins | `ezlopi_service_ble_ble_auth.c` |
| "provisioned" definition | `ezlopi_core_factory_info.c` |
| cloud response handling | `ezlopi_service_webprov.c:601` |
| factory reset scope | `ezlopi_core_reset.c:97`, `ezlopi_core_nvs.c:237` |

The key names are declared as `const char *ezlopi_<name>_str = "<name>"`.
Every key named on this page was checked against its declaration rather than
inferred from the identifier, and for all of them the string and the
identifier do match. The traps are not mismatched identifiers but
*near-identical pairs that both exist*:

| | and |
|---|---|
| `ezlopi_provisioning_uuid_str` → `"provisioning_uuid"` | `ezlopi_uuid_provisioning_str` → `"uuid_provisioning"` |
| `ezlopi_signing_ca_certificate_str` → `"signing_ca_certificate"` | `ezlopi_ca_cert_str` → `"ca_cert"` |
| `ezlopi_device_type_ezlopi_str` → `"device_type_ezlopi"` | `ezlopi_ezlopi_device_type_str` → `"ezlopi_device_type"` |

Picking the wrong member of a pair compiles, runs, stores nothing, and reports
success. Do not assume the pattern holds for keys not listed here — check the
declaration, and check whether a second similarly-named constant exists.

## Not yet verified on hardware

None of the above has been exercised against a physical board in this
repository's history. The key names, buffer sizes, payload shapes and the
compiled-out characteristics were each read from the source and checked
against their declarations; the four target images build clean; and the BLE
clamping and rejection behaviour is covered by
`tests/ci/check_ble_chunk_clamp.py`. But the end-to-end flow — a third-party
tool writing `0xE301` or `cmd:7` and the hub coming up configured and talking
to a cloud — has not been observed once.

Writing this document was not a neutral exercise: checking its claims against
the source turned up two live defects in the code it describes (a 512-byte
stack over-read that was being written into the factory-info partition, and a
leak on the serial path), both now fixed, plus three errors in the document's
own first draft. Treat the parts marked as verified as verified-by-reading,
not verified-by-running. See **Known gaps** in
[`CHANGELOG.md`](../CHANGELOG.md).
