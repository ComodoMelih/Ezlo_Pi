# Changelog

All notable changes in this fork, relative to upstream
[ezloteam/Ezlo_Pi](https://github.com/ezloteam/Ezlo_Pi) at commit `f2a74e5`.

Each entry says what was wrong, not just what changed — several of these bugs
were invisible in normal use, and the reason they stayed hidden is usually the
most useful part.

Severity markers:

- **[sec]** — reachable from an unauthenticated BLE, UART, or local-network
  client. The local-network case was added to this list for the session-model
  entry below; before that every **[sec]** item here was BLE or UART, and the
  marker said so.
- **[bug]** — incorrect behaviour, not a security issue.
- **[feat]** — new capability. Off by default unless an entry says otherwise.
- **[infra]** — build, test, packaging or documentation.

---

## Unreleased

### Memory safety — [sec]

Most of these are reachable by anyone in BLE range, or anyone with physical
access to the UART header. No pairing or authentication is required to reach
the parsers involved: the GATT characteristics are declared
`ESP_GATT_PERM_WRITE` rather than `ESP_GATT_PERM_WRITE_ENCRYPTED`, and
`CONFIG_EZPI_BLE_ENABLE_PASSKEY` is unset in every shipped
`sdkconfig.defaults.*`. The serial provisioning command checks nothing at all —
it reads no `user_id` and its provisioned-state guard is disabled.

Two items sit instead on the cloud provisioning path — the `cJSON *` passed to
a `%s`, and the 512-byte stack over-read in `set_basic()`. That path is an
*outbound* HTTPS GET validated against the bundled CA, so triggering either
means controlling that response. They are listed here anyway because of what
they expose rather than how they are triggered: the first was a tree-wide
defect class, and the second wrote the over-read stack bytes into the
factory-info partition, from where anyone in BLE range can read them back out
of device-info without authenticating.

Evidence differs by item, so it is worth being explicit about which:

- The first four are covered by regression tests in [`tests/host/`](tests/host/).
- The BLE chunk-reassembly defects are covered by
  [`tests/ci/check_ble_chunk_clamp.py`](tests/ci/check_ble_chunk_clamp.py),
  which fails on the pre-fix revision of each file and passes on the current
  tree. The reassembly functions are `static` inside translation units that
  need the ESP-IDF GATT headers, so there is no host harness for them yet.
  That is a gap, and it is why the check pairs clamps and frees structurally
  rather than relying on anyone re-reading these files.
- The chunk accumulator is split between a primitive that *can* be host-tested
  and service code that cannot, and the evidence splits the same way.
  [`tests/host/test_ble_buffer.c`](tests/host/test_ble_buffer.c) compiles the
  real `ezlopi_core_ble_buffer.c` against two stub headers and covers the
  iterative free, the new `total_len()` and the `bool` return — 36 checks, each
  one confirmed to fail against the specific wrong version it guards. The free
  check is the best-evidenced: reverting the function to its recursive form
  makes it abort with an ASan `stack-overflow`, so the regression is
  demonstrated rather than asserted. Two of the mutations also showed that a
  wrong `total_len()` is a *heap* overflow inside `accumulate_to_start()`, not
  merely a wrong number. The two service-level bounds and the
  discard-on-new-connection logic have **no automated coverage**: those files
  pull in cjext, NVS, the factory-info partition, FreeRTOS tasks and the GATT
  profile. For them the only check is that all four targets build and link the
  new symbol, which catches a type error but not a wrong constant.
- The format-attribute defect is covered by the compiler itself: with the
  attribute in place and `-Werror=format` already on, reintroducing the bad
  call fails the build. That was verified by reintroducing it, not assumed.
- The `set_basic()` over-read is the best-evidenced item here. It was
  reproduced as a real out-of-bounds read rather than inferred: the same
  buffer layout and copy lengths under `-fsanitize=address` report
  `stack-buffer-overflow ... READ of size 126` from a 40-byte array, and the
  bounded version runs clean. A second harness confirms the fix stores
  byte-identical data for the two callers that were already correct, across
  eight cases including empty, exactly-full and over-long values — and was
  mutated to confirm it can actually detect a difference.
  [`tests/ci/check_finfo_bounded_store.py`](tests/ci/check_finfo_bounded_store.py)
  then guards the pattern, and was verified against the real pre-fix file from
  git, where it flags all twelve copies, not merely against its own samples.
- The two leaks — `get_provisioning_status()` and the serial `cmd:7` path —
  have **no automated coverage at all.** Both need `esp_partition`, so neither
  can run on the host. No structural rule would have caught the first either:
  the pre-fix function did call `ezlopi_free` on the right variable, just on
  only one of two paths, and catching that needs branch analysis rather than
  pattern matching. No check was added rather than one that would give false
  confidence. Both were found by reading, and the audits of sibling call sites
  that followed were also by reading.

None of these have been exercised on physical hardware; see Known gaps.

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

- **The same unbounded copy in the BLE *provisioning* assembler — the sibling
  that was missed.**
  `ezpi_base64_decode_provisioning_info()` in
  [`ezlopi_service_ble_provisioning.c`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c)
  is a near-line-for-line copy of the dynamic-config function in the bullet
  above. Only one of the two copies was fixed. The provisioning copy still did
  `memcpy(base64_buffer + pos, data, len)` with `len` taken straight from the
  chunk's JSON, where it has no enforced relation to the actual length of
  `"data"`. A single 517-byte GATT write of
  `{"total_len":1,"sequence":1,"len":100000,"data":"AA"}` satisfied the
  completion test, allocated a two-byte staging buffer and then copied 100000
  bytes out of a two-byte string into it — an over-read and an over-write in
  one statement. This is the provisioning characteristic, so it is reached
  before the payload's own `user_id` is looked at; the heap is already
  corrupted by the time anything checks who is talking. `CONFIG_EZPI_BLE_ENABLE`
  is `y` in `sdkconfig.defaults.esp32`, `.esp32c3` and `.esp32s3`, so this was
  in every shipped build for those chips — the Kconfig `default n` is
  overridden by all three.

- **`total_len` was unbounded, which would have defeated the clamp anyway.**
  Both assemblers size their staging buffer with `ezlopi_malloc(total_size + 1)`
  on a `uint32_t`, so a claimed `total_len` of `0xFFFFFFFF` wraps the argument
  to `0` while leaving `total_size - pos` at roughly 4GB of apparent room —
  the clamp would then be computed from a number that no longer describes the
  allocation and would permit the very overflow it exists to stop. Provisioning
  now rejects any `total_len` outside `1..EZPI_BLE_PROV_MAX_BASE64_LEN`, which
  is derived from the partition layout rather than picked: everything a
  provisioning payload can persist lands below
  `EZLOPI_FINFO_OFFSET_HUB_DATA` (`0x6000`), and base64 expands by 4/3, so 4/3
  of that region already exceeds any storable payload.

- **Dynamic-config had no such bound — the bullet above silently said so.**
  When provisioning was capped, the dynamic-config assembler was left with the
  bare `ezlopi_malloc(total_size + 1)`, so the wrap described above was still
  reachable on characteristic `0xE501`. It now rejects any `total_len` outside
  `1..EZPI_BLE_DYNCFG_MAX_BASE64_LEN`, derived the same way but from a tighter
  constraint: `EZPI_core_factory_info_v3_set_ezlopi_config` stores the config
  with `cJSON_PrintPreallocated` into a buffer of
  `EZLOPI_FINFO_LEN_EZLOPI_CONFIG_JSON` (`0x1000`) bytes, so a config larger
  than that cannot be stored at all and there is nothing to be gained by
  accepting its base64. The macro uses the *ceiling* form of the 4/3 expansion
  where provisioning's uses the floor form; at this size the two differ by four
  bytes, which is the difference between accepting and refusing a config that
  exactly fills the field.

- **The BLE chunk accumulator grew on unauthenticated input, and freeing it
  recursed once per node.** Listed as a Known gap in the previous entry; closed
  here. Both reachable services append every incoming chunk to a file-static
  linked list before parsing anything, and only free it when the client's own
  arithmetic — `((sequence - 1) * 400 + len) >= total_len` — says the transfer
  is done. A client that repeats one never-completing chunk, or simply stops one
  chunk short, kept appending. Three separate defects, and the second is the one
  worth the most attention:

  1. *Nothing bounded the list.* Both services now refuse a write that would
     push the bytes held past `EZPI_BLE_PROV_MAX_ACCUM_LEN` /
     `EZPI_BLE_DYNCFG_MAX_ACCUM_LEN`, discarding what has accumulated. These are
     deliberately **not** the base64 caps above: a node holds the whole JSON
     envelope (`{"len":400,"total_len":...,"sequence":...,"data":"..."}`), not
     the 400 bytes of payload inside it, so bounding the list at the payload cap
     would refuse an ordinary config about three-quarters of the way through.
     The accumulator caps are the payload cap divided into 400-byte chunks and
     multiplied back up by the 517-byte envelope — 7238 bytes for dynamic-config
     and roughly 42 KB for provisioning, which is large because that payload
     legitimately is (three PEM blobs at `0x1000` each).

  2. *The free recursed before freeing.* `EZPI_core_ble_buffer_free_buffer()`
     called itself on `->next` and only then released the current node, so peak
     stack depth was one frame per node — a quantity the client chose. The crash
     therefore landed at the moment the transfer *succeeded*, which presents as
     an unexplained reboot into an unprovisioned state rather than as anything
     pointing at BLE. Now iterative.

  3. *A failed append was indistinguishable from a successful one.*
     `EZPI_core_ble_buffer_add_to_buffer()` returned `void` and ignored a `NULL`
     from `create()`. The remaining chunks still arrive, the completion test is
     still satisfied, and what gets base64-decoded into the factory partition is
     a configuration with a hole in the middle. It now returns `bool` and both
     services abandon the transfer when it is `false`.

  Two deliberate deviations from what that Known gap predicted. It said the fix
  "needs a real app transfer to size it" — the bound is instead derived from the
  storage limit, which is a stronger argument than a measurement: a payload that
  cannot be stored need not be accumulated. And it called for "a free on
  disconnect"; what is implemented is a discard when a *new connection* writes,
  keyed off the `conn_generation` counter each service already maintains. A
  disconnect-time free would require `ezlopi-core-ble` to know about its
  services, inverting the dependency. More importantly, a cleanup that only runs
  on a connection event fails silently if the event does not arrive, so it
  carries none of the security weight here — the unconditional append-time bound
  in (1) does, and the generation check only prevents one client's partial
  transfer being concatenated onto the next client's.

  The two bounds and the generation check live in the service files, which cannot
  be host-compiled; see Known gaps for what that leaves unverified.

- **Flash overflow writing certificates and keys: one field could overwrite
  the next.** The four PEM setters in
  [`ezlopi_core_factory_info.c`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c)
  — CA certificate, SSL private key, public key and shared key — each took
  their length from `ezlopi_calculate_certificate_length()`, which returns the
  position of the PEM end marker in a client-supplied string and has no upper
  bound. That length went to a writer whose name promises otherwise:
  `__EZPI_core_factory_info_v3_set_4kb()` erases exactly `0x1000` bytes and
  then writes `len + 1`.

  The fields are contiguous inside one 32 KB partition, so the overflow had
  somewhere to go:

  | field | offset | what an over-long value runs into |
  |---|---|---|
  | `EZLOPI_CONFIG_JSON` | `0x1000` | 4 KB of unused space at `0x2000` |
  | `CA_CERTIFICATE` | `0x3000` | the SSL private key |
  | `SSL_PRIVATE_KEY` | `0x4000` | the shared key |
  | `SSL_SHARED_KEY` | `0x5000` | **hub data — serial number, Wi-Fi SSID and password, MAC** |

  The spilled bytes also landed in flash this call had *not* erased, and NOR
  flash can only clear bits on write, so the neighbouring field was corrupted
  rather than cleanly replaced. The write stays inside the partition, so
  `esp_partition_write` had no reason to refuse it.

  Reached from BLE provisioning
  ([`ezlopi_service_ble_provisioning.c:472`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L472)),
  which is unauthenticated in every shipped `sdkconfig.defaults`, and from
  UART `cmd` handling, which checks nothing. Both pass a JSON string straight
  through.

- **A malformed certificate destroyed the stored one.** The same helper
  returned `0` when the end marker was *absent*, and the setters passed that
  `0` to the writer — which duly erased the whole 4 KB field and stored an
  empty string. So supplying a certificate without its `-----END ... -----`
  trailer was not rejected; it wiped the real certificate. The non-zero guard
  on the way in (`cj_data->str_value_len`) did not help, because it tested the
  length *before* the recalculation replaced it with 0.

  Both are now decided by `EZPI_util_pem_store_len()` in
  `ezlopi-util-sanitize`, which bounds the value against the field it is going
  into and **refuses rather than truncates** — a truncated certificate is not a
  certificate, and storing a prefix would trade a clear failure for a device
  that fails to connect for no visible reason. A missing end marker refuses
  too, so the stored value is left alone. `set_4kb()` additionally refuses any
  write larger than the field as a second line of defence, since its name
  promised a bound that nothing enforced. `ezlopi_calculate_certificate_length()`
  is deleted rather than left available: it had external linkage but no
  declaration in any header, so nothing outside that file could call it, and
  leaving it invites the next person who needs a certificate length to reuse
  it.

  Covered by 26 host checks in `tests/host/test_sanitize.c` (72 → 98 in that
  binary), testing the boundary at 4094 / 4095 / 4096 bytes rather than at one
  arbitrary "big" value, and mutation-tested 4 of 4 caught (removing the bound,
  an off-by-one that forgets the terminator, falling back to `strlen` when the
  marker is missing, and truncating instead of refusing) against a control
  reporting 0. **Untested on hardware** — no board has been provisioned with an
  over-long certificate, and the flash-level consequence above is read from the
  layout constants and the ESP-IDF write semantics rather than observed.

  The arithmetic is host-tested, but the two things that make it load-bearing
  are not reachable from a host test, because
  `ezlopi_core_factory_info.c` needs `esp_partition` and cannot be
  host-compiled at all: that the setters actually call the bounded helper, and
  that the backstop inside `set_4kb()` still exists. Both are silent when
  undone — deleting the backstop leaves a function whose name promises a 4 KB
  write and whose body writes whatever it is handed, and all four images still
  build clean. So
  [`tests/ci/check_finfo_pem_bound.py`](tests/ci/check_finfo_pem_bound.py)
  enforces them textually, self-testing its own scanner against fifteen
  samples on every run — including the prose in that file, which names the
  deleted helper and would otherwise make the check fire on the fixed source.
  Verified against the pre-fix file, where it reports eleven findings and
  exits 1.

- **The device config write put one byte of uninitialised heap into flash.**
  Found while reading this write path end to end, because the local
  configuration work below adds a network-reachable caller to it.
  `set_4kb()` appends its own terminator: it fills a `len + 1` buffer with
  `snprintf`, which places the NUL at index `len`, and then writes `len + 1`
  bytes. So the count must *exclude* the terminator, and four of the five
  callers pass it that way. `EZPI_core_factory_info_v3_set_ezlopi_config()`
  passed `strlen(tmp_data) + 1`, which made the write one byte longer than
  `snprintf` had filled — so the last byte sent to flash was whatever the
  allocator happened to leave in that heap slot.

  The factory-info partition reads back over BLE device-info and the UART
  status query without authenticating, which is what turns a stray byte into a
  disclosure. It is one byte per config write and it sits after the NUL every
  reader scans for, so nothing malfunctioned and nothing was corrupted — which
  is precisely why it survived.

  Demonstrated rather than deduced: the same buffer arithmetic with the heap
  pre-filled with a sentinel shows the four-caller convention writing ten
  bytes with zero untouched, and the `+ 1` form writing eleven with one
  sentinel byte still in it. The same off-by-one also meant a config
  serialising to exactly 4095 bytes asked for a 4097-byte write into a
  4096-byte field, so it was the overflow above in miniature. Now passes
  `strlen(tmp_data)`, matching the other four, and the CI check above refuses
  a `+ 1` count on any call to that writer.

  Noted but deliberately *not* changed: `set_ssl_public_key()` writes to the
  `SSL_SHARED_KEY` offset, the same field `set_ssl_shared_key()` uses. There is
  no `SSL_PUBLIC_KEY` offset in the layout and the reader caches that field
  into a variable named `__pub_key`, so this firmware treats the two as one
  stored value. The visible consequence is that
  [`ezlopi_service_webprov.c`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c)
  calls `set_ssl_public_key()` and then `set_ssl_shared_key()`, so when the
  cloud supplies both the second overwrites the first. Fixing it means adding a
  field to the factory partition layout, which provisioning tooling outside
  this repository also writes, so it is recorded rather than changed.

- **Unauthenticated heap exhaustion: three leaked cJSON trees per BLE write.**
  `ezpi_provisioning_info_write_func()` parsed the envelope of every inbound
  chunk and never freed it; the reassembly loop above parsed and leaked one
  tree per chunk; and `ezpi_service_ble_serial_config_write()` in
  [`ezlopi_service_ble_device_info.c`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_device_info.c)
  leaked its whole tree on every write. Any client in range could exhaust the
  heap one write at a time without authenticating. The third of these was found
  by the CI check, not by hand — a careful manual read of the same two files
  had already missed it, because the write handler *does* call `cJSON_Delete`,
  just on a different variable. That is why the check pairs each parse with a
  free of the same variable rather than asking whether a free appears anywhere
  in the function.

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

- **257-byte heap leak per status query on every unprovisioned hub.**
  `EZPI_core_factory_info_v3_get_provisioning_status()`
  ([`ezlopi_core_factory_info.c:219`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L219))
  read the stored `cloud_server` URL and freed it **only inside the branch that
  found `https://` in it** — so the unprovisioned path leaked the buffer, and
  that is the path a factory-fresh device always takes.

  It leaks on blank flash rather than skipping it, which is what makes this
  reliable instead of theoretical: an erased region is all `0xFF` with no zero
  byte, so `__read_string()` does not return `NULL` for an unset field, it
  returns a non-NULL string of `0xFF` bytes. That fails the `strstr` and fell
  straight into the unfreeing `else`.

  `EZLOPI_FINFO_LEN_CLOUD_SERVER_URL` is `0x100`, so 257 bytes went per call,
  and the function is called on every BLE device-info read (the
  `provisioned_status` field) and every UART status query — neither
  authenticated. A client in radio range could poll an unprovisioned hub's heap
  away, which is precisely the state a hub is in *while being provisioned*.

  Now freed unconditionally, matching `get_cloud_server()` and
  `get_provisioning_server()` immediately below it, which both already got this
  right — the three functions are the same shape and only one had the free
  misplaced. Every other caller of `__read_string()` was audited at the same
  time: the rest either hand ownership to their caller or cache deliberately
  into the `__ca_cert`/`__pvt_key`/`__pub_key` statics, so this was the only
  one. Not covered by a host test — the function needs `esp_partition` — and
  not yet observed on hardware; see Known gaps.

- **512 bytes of stack read out of bounds and written into the factory-info
  partition.**
  `EZPI_core_factory_info_v3_set_basic()` takes a caller-filled
  `s_basic_factory_info_t` of `char *` members and writes each into a
  fixed-size region of flash. It did that with a `memset`/`memcpy` pair per
  field where **the count was the size of the destination field**, which has
  no relationship to the size of the buffer the caller actually passed:

  ```c
  memset(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_DEVICE_UUID, 0, EZLOPI_FINFO_LEN_DEVICE_UUID);
  memcpy(tmp_buffer + EZLOPI_FINFO_REL_OFFSET_DEVICE_UUID, cfg->device_uuid, EZLOPI_FINFO_LEN_DEVICE_UUID);
  ```

  Two of the three callers survived that only by coincidence — the BLE and
  serial paths declare their stack arrays at exactly those lengths and zero
  them first. `__provision_update()` in
  [`ezlopi_service_webprov.c`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c)
  does not. It declares `tmp_dev_uuid[40]`, `tmp_prov_uuid[40]`,
  `tmp_cloud_server[128]` and `tmp_provision_token[300]` against fields of
  `0x7E`, `0x7E`, `0x100` and `0x200`, so those four copies ran 86, 86, 128
  and 212 bytes past the ends of the arrays — 512 bytes of adjacent stack in
  total.

  The read is the smaller half of the problem. Those bytes were then written
  to the factory-info partition and are served straight back out: the BLE
  device-info characteristic and the UART status query both report
  `cloud_server`, the device UUID and the provisioning UUID, and neither
  requires authentication. Whatever was on that task's stack — and this is the
  task that handles the cloud socket — became readable by anyone in radio
  range.

  All thirteen string fields now go through one helper that bounds the copy
  with `strnlen()` of the source. For the two callers that were already
  correct this is byte-for-byte identical, which was checked rather than
  assumed: the destination is still zeroed first and their sources are
  zero-filled past the string, so the bytes no longer copied were zeros
  either way. Enlarging webprov's four buffers instead would have added 512
  bytes to that task's stack purely to keep copying padding the `memset`
  already wrote. The two remaining `memcpy`s in the function are the binary
  `id` and `config_version` fields, whose sources are exactly the right size.

  Found while writing [`docs/provisioning-schema.md`](docs/provisioning-schema.md)
  — specifically while checking what the field size limits in that document
  actually mean, which required reading how each field reaches flash.

- **The serial provisioning command leaked a struct on every call.**
  `ezpi_service_uart_process_provisioning_api()` in
  [`ezlopi_service_uart.c`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c)
  allocates an `s_basic_factory_info_t`, fills it, hands it to `set_basic()`
  and never frees it. The BLE sibling it was copied from does free it, two
  lines after the same three setter calls; the copy simply dropped the line.
  The same sibling-copy pattern as the BLE chunk overflow above — a function
  duplicated, then fixed in only one of its two homes.

  This path is reachable without any authentication whatsoever: `cmd:7` reads
  no `user_id`, and its provisioned-state check is `if (0)` with the real
  condition commented out beside it. So it can be driven in a loop by
  anything able to write to the UART. Now freed. The struct holds only
  pointers and scalars, so that is the whole leak; the strings it points at
  are the caller's stack.

  The struct is also now zeroed on allocation, matching the BLE sibling. That
  part is not a fix for a live defect — `set_basic()` reads neither of the two
  members the serial path left uninitialised, `version` and `user_id`, which
  was verified rather than assumed — but it means the day that function starts
  reading `user_id`, this path hands it `NULL` instead of a stale heap
  pointer.

- **A `cJSON *` passed to a `%s`, in every shipped image.**
  `__provision_update()` in
  [`ezlopi_service_webprov.c:710`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L710)
  logged `TRACE_E("key \"%s\" not found.", cj_root_prov_data)` — a `cJSON *`
  where a `char *` was expected. The logger started reading at the first byte
  of the `cJSON` struct, whose leading member is the `next` pointer, and ran
  on until it happened to meet a zero byte, printing whatever lay in between.
  The branch is reached whenever a provisioning response parses as JSON but
  carries neither `data` nor `error_code`.

  The reason it survived so long is the more useful finding: every `TRACE_*`
  expands into `trace_color_print()`, a variadic function that **had no
  `printf` format attribute**, so no format string anywhere in the tree was
  ever compared against its arguments. (The macro form that might have given
  the compiler a hint is behind an `#if 0`.) `CONFIG_EZPI_UTIL_TRACE_EN=y` in
  all four shipped `sdkconfig.defaults.*`, so the line was compiled in
  everywhere.

  `trace_color_print()` and `trace_otel()` now carry
  `__attribute__((format(printf, …)))`. Because `-Werror=format` was already
  enabled, that converts the whole class into build failures — and it is worth
  recording that across all four targets it found **exactly one** defect, the
  one above, so the attribute costs nothing to keep. Guarded by
  `tests/ci/check_trace_format_attr.py`, which exists for the case where the
  attribute itself gets deleted; the build would go green again and silently
  stop checking.

- **`free()` on a FreeRTOS tick count: one untagged `void *` holding four
  different kinds of thing, with one type-blind destructor.**
  `l_fields_v2_t::user_arg` in
  [`ezlopi_core_scenes_v2_type_declaration.h`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_v2_type_declaration.h)
  is scratch space that each when-method uses to remember its own state between
  evaluations. There are eleven stores into it across the scenes component, and
  they do not put the same kind of thing there:

  | what is stored | sites | is it an address? |
  |---|---|---|
  | `xTaskGetTickCount()` (`isInterval`) | 2 | no — a small integer |
  | a house-mode id (`isHouseModeChangedTo`/`From`) | 3 | no — a small integer |
  | `grp_state`, a `bool` (`isGroupState`) | 1 | no — 0 or 1 |
  | an owned struct from `ezlopi_malloc` | 3 | yes, and it owns it |
  | an owned `cJSON` tree | 2 | yes, and it owns the whole tree |

  `EZPI_scenes_delete_fields()` released all eleven the same way:
  `if (fields->user_arg) { free(fields->user_arg); }`. That is correct for
  exactly one of the five rows.

  The six integer stores are the serious half. A tick count is not an address,
  so the guard `if (user_arg)` passes — a non-zero integer is non-NULL — and
  then `free()` is handed a value the allocator never returned. On the host,
  under ASan, this aborts; on the device it corrupts the heap. Reaching it does
  not require anything exotic, because it is the ordinary teardown path:
  `hub.scenes.delete` → `EZPI_core_scenes_depopulate_by_id_v2()` →
  `EZPI_core_scenes_delete()` → `EZPI_core_scenes_delete_when_blocks()` →
  `EZPI_scenes_delete_fields()`. So deleting a scene whose `when` block used
  `isInterval`, `isGroupState` or either house-mode method called `free()` on an
  integer. `CONFIG_EZPI_SERV_ENABLE_MESHBOTS` is on in all four shipped
  configurations — confirmed by finding
  `EZPI_core_scenes_delete_field_user_arg` in all four linked ELFs, not by
  reading Kconfig — so this was in every image.

  The two `cJSON` stores are the quieter half: `free()` on a `cJSON *` releases
  the root node and nothing else, so every child node and every key string
  stayed allocated. A scene created and deleted repeatedly leaked a little each
  time.

  Release is now driven by a tag the writer sets. `user_arg` gained a companion
  `user_arg_type`, and the new `EZPI_core_scenes_delete_field_user_arg()`
  switches on it: `FIELD_USER_ARG_HEAP` → `ezlopi_free`,
  `FIELD_USER_ARG_CJSON` → `cJSON_Delete`, `FIELD_USER_ARG_NONE` → nothing.
  `NONE` is deliberately `0`, so a field that was zeroed at allocation and
  never written is never released; the unsafe behaviour now requires someone to
  ask for it explicitly. All eleven stores set the tag, including the six that
  store integers and therefore only need `NONE` — the slot is already zeroed
  there, so those six lines change no behaviour, but the point of a tag is that
  the writer states what it wrote locally instead of making the reader
  reconstruct a whole-program argument.

  The helper is exported rather than kept `static` because a when-method that
  wants to discard its own remembered state mid-evaluation needs the same
  release the destructor uses, and doing that by hand is how two of the three
  defects below arose.

- **Use-after-free: a `cJSON` child kept as remembered state after its parent
  was deleted.**
  `EZPI_scenes_operators_is_dictionary_changed_operations()` in
  [`ezlopi_core_scenes_operators.c`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_operators.c)
  stored `cj_value` into `user_arg` to compare against on the next evaluation.
  But `cj_value` is a *child* of `cj_item_value`, which the same function
  deletes before returning. The remembered pointer therefore aliased freed
  memory from the moment it was stored, and the next evaluation of the scene
  read through it. Now stores a `cJSON_Duplicate()` of the value, which the
  field owns, tagged `FIELD_USER_ARG_CJSON`; and when the store does not
  happen, the duplicate is deleted rather than leaked.

- **`free()` without clearing the pointer, so the next pass used it and the
  teardown freed it twice.**
  The sunstate when-method in
  [`ezlopi_core_scenes_when_methods_helper_functions.c`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c)
  discards its own state mid-evaluation. It called `free()` on `user_arg` and
  left the pointer in place, which is two defects in one line: the next
  evaluation found a non-NULL `user_arg` and read the freed struct through it,
  and the eventual `hub.scenes.delete` freed the same block a second time. Now
  calls `EZPI_core_scenes_delete_field_user_arg()`, which clears both the
  pointer and the tag, so a second release is a no-op.

- **A shadowed declaration that leaked a `cJSON` tree on every call and made
  the function it was in a no-op.**
  In `IS_deviceitem_group_value_check()` in the same file, a block built its
  object with `cJSON *cj_device_items_data = cJSON_CreateObject(...)` — a *new*
  declaration shadowing the function-scope variable of the same name. So the
  tree was unreachable the moment the block ended, and the outer variable it
  was meant to initialise stayed `NULL`. Every value comparison below it is
  guarded on that outer pointer, so the entire rest of the function was
  unreachable: the leak was the visible symptom, and the when-method silently
  evaluating nothing was the real cost. Now assigns to the outer variable.

  This one was found by building with `-Wshadow`, which is not otherwise
  enabled. The sweep found nine unique shadowed declarations; the other eight
  were each read afterwards and none changes behaviour, so this is the only one
  of the nine that was a defect. The per-site reasoning is under Known gaps,
  with the residual gap being that the flag still is not enabled.

  Found by the same reading pass: the tree is now also deleted on the path
  where it was built but not adopted into `user_arg`, which previously leaked
  it.

  **Evidence.** All four are covered by
  [`tests/host/test_scenefields.c`](tests/host/test_scenefields.c), which
  compiles the real `ezlopi_core_scenes_delete.c` — not a copy — against two
  small stubs, under `-fsanitize=address,undefined`. It is the first host test
  to reach into the scenes component.

  These tests are adjudicated mostly by the sanitizers rather than by
  assertions, which is the point: a `free()` of a non-heap value and a leak of
  `cJSON` children are both invisible to `assert`, so a version of this file
  that only asserted would have passed against the broken code. That is a claim
  about the tests, so it was checked rather than asserted — each of the eight
  tests was run individually against the pre-fix destructor spliced back in,
  and each behaved as its comment claims:

  | test | pre-fix | adjudicated by |
  |---|---|---|
  | scalar (tick count) not freed | fails | ASan, on the wild `free` |
  | bool not freed | fails | ASan, on the wild `free` |
  | heap block *is* freed | **passes** | — the one case the old code got right |
  | `cJSON` tree fully deleted | fails | LeakSanitizer at exit |
  | releasing twice is safe | fails | double free |
  | mixed list | fails | ASan, on the wild `free` |
  | `NULL` is a no-op | **passes** | — no-op either way |
  | via `delete_when_blocks()` | fails | ASan, on the wild `free` |

  Running them one at a time was necessary rather than tidy: run together, the
  first test aborts the process and the remaining seven never execute, so a
  single run proves only that *one* of them can fail. In particular the
  LeakSanitizer claim for the `cJSON` test is only visible in isolation.

  The last row matters most. The seven tests above it call the field destructor
  directly, so they would all still pass if the when-block destructor had been
  changed to call something else; that one joins the real `hub.scenes.delete`
  chain at `EZPI_core_scenes_delete_when_blocks()`, the last link that is
  reachable on a host — the two above it need the scene list and the meshbot
  task.

  The Makefile rule for this test asserts with `nm` that
  `ezlopi_core_scenes_delete.c` produced a non-empty object. That guard is
  there because the first attempt at this test reported a clean compile while
  having compiled nothing at all: every scenes header is wrapped end-to-end in
  `#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS`, so without that macro the
  translation unit preprocesses away and both the compile and the link succeed.
  The test now has its own stub directory so that a shared `sdkconfig.h` cannot
  switch that branch for unrelated tests.

  All four targets build clean, with the fixed destructor present in each ELF.
  Not exercised on physical hardware; see Known gaps.

### Scene triggers — [bug]

- **A type tag stored in a `union` with the value it describes, so storing the
  value destroyed the tag.**
  `s_item_expn_data_t` in
  [`ezlopi_core_scenes_when_methods_helper_functions.h`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.h)
  was declared `typedef union` over exactly two members: `e_value_type_t
  e_type`, commented "the data's nature", and `u_field_value_v2_t u_value`,
  commented "reutilizing the field structure to store extracted data". The
  field structure it meant to reuse is `s_field_value_t`, which holds those
  same two members as a **struct**. One keyword apart, and the two shapes are
  not equivalent in any way: in the struct the tag and the value have separate
  storage, in the union they are the same bytes.

  So every one of the nine writers did `sample_data.e_type = VALUE_TYPE_x;` and
  then immediately stored `sample_data.u_value.<member>` over the top of the
  tag it had just set. Measured rather than argued about, by compiling the real
  headers and reproducing that two-line sequence:

  | written as | read back as, while it was a union |
  |---|---|
  | `VALUE_TYPE_STRING` | not a valid tag (the pointer's low bytes) |
  | `VALUE_TYPE_BOOL`, value `true` | `VALUE_TYPE_NUMBER` |
  | `VALUE_TYPE_BOOL`, value `false` | `VALUE_TYPE_UNDEFINED` |
  | `VALUE_TYPE_NUMBER`, value `23.0` | `VALUE_TYPE_UNDEFINED` |
  | `VALUE_TYPE_NUMBER`, value `0.1` | not a valid tag |

  The tag never survived. `sizeof` confirms the cause: the union was 8 bytes
  where the struct is 16, because it put `u_value` at offset 0 on top of
  `e_type` instead of after it.

  Three places read that tag — `ezlopi_core_scenes_when_methods.c:471` and the
  two `switch` statements in `__replace_old_with_new_data_val()` and
  `____old_vs_new_extract_data()` — and none of them could take the arm it was
  written for. The `true` and `0.1` rows are the sharper half: those did not
  merely fall through to `default`, they selected the *wrong* arm, so a stored
  `bool` was read back out of `u_value.value_double`.

  What that cost: `__replace_old_with_new_data_val()` never copied the new
  value over the remembered one, and `____old_vs_new_extract_data()` — which
  returns 1 for "the value is unchanged" — always fell to `default` and
  returned 0. `isItemStateChanged` therefore answered "changed" on every single
  evaluation, regardless of the value. Now a `struct`.

- **The remembered item value's string was leaked, and the fix above is what
  made it possible to release it.**
  When the value `isItemStateChanged` remembers is a string, the
  `s_item_exp_data_t` it keeps owns a copy of it — two allocations behind the
  one `user_arg` pointer. `FIELD_USER_ARG_HEAP` releases a flat block, so it
  released the struct and left the string.

  Fixing that needs the destructor to ask the struct which of the two it is
  holding, which is exactly the tag the union was destroying: an
  earlier attempt at this arm was written and then deleted *because* reading
  `sample_data.e_type` was unsound while the type was a union — it would have
  decided whether to call `free()` on a pointer by inspecting the low bytes of
  a `double`, which is the same defect as the tick count in the entry above,
  one level down. With the tag intact the arm is sound, so there is now a
  `FIELD_USER_ARG_ITEM_EXP` that releases the inner string when the tag says
  `VALUE_TYPE_STRING` and the outer block always.

  **Evidence.** Three tests were added to
  [`tests/host/test_scenefields.c`](tests/host/test_scenefields.c), and each
  was checked against the specific wrong version it guards rather than against
  "the old code" generally — one targeted mutation at a time, since a mutation
  no test notices means the test is decoration:

  | mutation | caught by | adjudicated by |
  |---|---|---|
  | the type back to a `union` | tag-survives-its-value | `assert` |
  | the arm frees only the outer block | releases-inner-string | LeakSanitizer |
  | the arm frees `value_string` regardless of the tag | number-not-freed-as-a-pointer | ASan |

  The first of those is the one test in that file an `assert` is the right
  adjudicator for, because it is a claim about layout rather than about
  behaviour. The third exists because "free the string unconditionally" is the
  obvious wrong way to write this arm, and for a remembered number it is a
  `free()` of half a `double`.

  All four targets build clean. What is *not* evidenced is the end of the
  chain: which scenes behave differently now. See Known gaps, including why the
  risk is asymmetric.

### UART sensor dispatch — [sec]

- **The UART receive task invoked its callback with a NULL buffer on every
  non-data event.** In `ezlopi_hal_uart.c` the `upcall(buffer, data_len, ...)`
  call sits *outside* the event `switch`, so it also ran for `UART_BREAK`,
  `UART_BUFFER_FULL`, `UART_FIFO_OVF`, any unrecognised event type, a
  zero-length buffered read, and a failed `ezlopi_malloc` — in all of which
  `buffer` is NULL and `data_len` is 0.

  Three of the six registered callbacks dereference the buffer without testing
  it: `pms_read_upcall()` reads `buffer[0]` as its first statement,
  `ze08_ch2o_parse()` passes it straight to its checksum helper, and
  `__uart_gps6mv2_upcall()` `memcpy()`s 256 bytes out of it. So a break
  condition on the line — RX held low, which requires no cooperation from the
  attached device and no authentication — was enough to panic a hub with a
  PMS5003, ZE08-CH2O or GY-GPS6MV2 configured. The callback is now invoked only
  when there is data. `uart_read_bytes()` also returns `-1` on failure, which
  the old code widened into a huge `uint32_t`; the same guard covers that.

- **Heap overflow in the UART read.** The buffer was allocated with the length
  from `uart_get_buffered_data_len()` but filled with `event.size` bytes, and
  those are two different numbers — `event.size` is this event's count, while
  more bytes can arrive on the line between the two calls. Whenever `event.size`
  was the larger, `uart_read_bytes()` wrote past the end of the allocation. Now
  reads the smaller of the two.

  **Not covered by a host test.** `ezlopi-hal-uart` needs `driver/uart.h` and
  FreeRTOS, so it cannot compile in `tests/host/`, which deliberately builds
  only IDF-free sources. Writing a test against a *copy* of the dispatch loop
  was rejected for the reason given at the top of that Makefile — a copy
  drifts. Covering this needs the HAL seam tracked in
  [Known gaps](#known-gaps).

- **Every UART callback then read a fixed number of bytes regardless of how
  many arrived.** Fixing the HAL stopped the NULL dispatch but not this: the
  buffer handed up is a heap block sized to the bytes that actually arrived,
  and five of the six callbacks ignored `output_len` entirely. A frame split
  across two UART events — the ordinary case on a busy line, not a
  malformed-input case — was read off the end of that block every time.

  | Driver | Read | Available |
  |---|---|---|
  | ZE08-CH2O | 9 bytes | `output_len` |
  | PMS5003 | 32 bytes | `output_len` |
  | R307 fingerprint | 256 bytes | `output_len` (a reply is ≤ 12) |
  | GY-GPS6MV2 | 256 bytes | `output_len` |
  | MB1013 | `buffer[idx + 5]` with `idx < output_len` | 5 bytes short |
  | LD2410 | — | already correct |

  All five now require the bytes they read to be inside `output_len`. For a
  well-formed frame nothing changes; the bytes no longer read were never
  sensor data, they were whatever followed the allocation.

  Three smaller defects in the same paths: the R307 driver copied 64 bytes from
  `another_buffer + 6` out of a 64-byte array, over-reading 6 bytes of adjacent
  stack (the array is now sized `+ 6`); `__uart_gps6mv2_upcall()` never checked
  `buffer` for NULL at all, only `tmp_buffer`; and that same function called
  `strlen()` on a 256-byte `ezlopi_malloc` block it had just filled completely
  with NMEA text, which contains CR and LF but no NUL, so there was no
  terminator anywhere in it. The resulting over-long length was passed to
  `strncpy` as a byte count, and since the guard above it only reserves 255
  bytes of a 768-byte `gps_cir_buf` at an offset of up to 512, that wrote past
  the end of the buffer and into the rest of `GPS6MV2_t` — a write overflow,
  not only an over-read. The copy is now clamped and terminated.

- **The MB1013 clamp above was not sufficient, and the comment claiming it was
  is wrong.** Clamping the loop bounded which indices were *tested*; it did not
  bound the `atoi()` call inside the loop body:

  ```c
  atoi((const char *)&buffer[idx + 1]) / 10.0
  ```

  That reads a `char *` from a heap block containing no NUL anywhere. The
  comment added with the clamp argued it was safe because the payload "starts at
  a digit and stops at the first non-digit, and `buffer[idx + 5]` is known to be
  `'\r'`". Both halves are false. Nothing checked that the four payload bytes
  were digits, and `'\r'` is a **whitespace** character, which `strtol` — which
  `atoi` is — *skips* rather than stopping at. So a frame whose payload is
  whitespace made the conversion step over the frame's own terminator and keep
  walking off the end of the allocation.

  Confirmed, not reasoned: against exact-size heap blocks under
  AddressSanitizer, `"R"` + four spaces + `"\r"` gives
  `heap-buffer-overflow ... 0 bytes after 6-byte region` inside
  `StrtolFixAndCheck`. Tabs and newlines do the same. Well-formed four-digit
  frames were never affected, which is why this survived the clamp review;
  `"Rzzzz\r"` and `"R   -\r"` were also safe, because `strtol` does stop at a
  letter or a sign.

  Fixed by removing the class rather than the instance: the frame scan moved to
  a new `mb1013_parse.c` and reads exactly the four payload bytes, never calling
  a string function on unterminated memory. Two behaviour changes, both only on
  malformed input and both documented in `mb1013_parse.h`: a payload with no
  digit now yields *no* reading instead of a fabricated `0.0`, and the scan
  continues past it, so a junk frame no longer masks a good frame later in the
  same buffer. Every input the old code read correctly still reads the same —
  verified against `atoi()` on a NUL-terminated copy across all 10000
  four-digit frames, 0 unrecognised and 0 mismatched, with space-padded payloads
  and the `"12z3"` → `12` truncation point preserved deliberately.

- **The ZE08-CH2O checksum rejected 99.6% of valid frames, so that sensor has
  never reported a reading.** `ze08_ch2o_sensor_checksum()` was two nested
  loops with identical bounds and the same induction variable name, the inner
  shadowing the outer, with the accumulate line written twice in the body. It
  summed every byte fourteen times, computing `(~(14·S))+1` instead of
  `(~S)+1`. Those agree only when `13·S ≡ 0 (mod 256)`, and 13 is coprime to
  256, so only when `S` is itself a multiple of 256.

  Measured, not argued: sweeping all 65536 concentration/range pairs and
  installing the datasheet-correct checksum in each frame, 65280 of 65536 were
  rejected. `ze08_ch2o_parse()` gates every reading on that comparison, so
  `data->available` was essentially never set. Now a single loop.

  **This one is covered by a host test.** `tests/host/test_ze08.c` compiles the
  real `ze08_ch2o.c` — the only UART driver whose ESP-IDF dependency is
  confined to type declarations — by stubbing the five ESP-IDF headers
  underneath `ezlopi_hal_uart.h` rather than copying that header, so the real
  one is compiled and its callback signature cannot drift. The parser and its
  upcall are both `static`, so the test supplies `EZPI_hal_uart_init()` and
  keeps the pointer the driver registers; every case runs the real
  registration path. 26 checks, including a 65536-frame sweep and every short
  length from 1 to 8 fed through exact-size heap blocks so ASan can see the
  bounds.

  Six deliberate defects were reintroduced one at a time to confirm the suite
  goes red — the doubled checksum loop, the length guard disabled, both NULL
  guards disabled, the length guard turned into an equality test (the
  plausible wrong fix, which would drop coalesced reads), and an
  accept-everything checksum. All six were caught; the length-guard mutation
  reports as an ASan `heap-buffer-overflow`, which is what establishes the
  over-read was real rather than theoretical.

  The other four drivers could not join the suite when this was written: they
  pull in cJSON, the device/item core, FreeRTOS tasks and GPIO. That was the
  concrete argument for the parser seam tracked in
  [Known gaps](#known-gaps). Three of the four have since been brought in —
  PMS5003, MB1013 and GY-GPS6MV2, each under *Testing* below — leaving R307,
  whose dependency turned out to be genuinely in the code rather than only in a
  header. Of the four clamps that were not host-testable when this was written,
  two still are not: R307's and GY-GPS6MV2's remain verified only by an esp32
  build.

- **R307's transmit path had the MB1013 shape too: a clamp on one statement
  that the next three statements did not inherit.** Everything above is about
  bytes arriving; this one is about bytes leaving. `generate_packet()` clamped
  the `memcpy` into a 64-byte `data[]` and then let three other things keep
  using the unclamped `length`:

  ```c
  length = length + 2;
  txPacket->Packet_len[0] = (uint8_t)(length >> 8);     /* unclamped */
  txPacket->Packet_len[1] = (uint8_t)(length & 0xFF);
  length = length - 2;
  if (length < MAX_PACKET_LENGTH_VAL)
      memcpy(&(txPacket->data), Combined_data, ((int)length));
  else
      memcpy(&(txPacket->data), Combined_data, (MAX_PACKET_LENGTH_VAL - 2));
  ...
  for (uint8_t i = 0; i < (int)length; i++)             /* unclamped */
      sum += (uint16_t)(txPacket->data[i]);
  ```

  The three consequences, with the boundaries measured rather than reasoned —
  both versions of the function were run side by side under AddressSanitizer on
  a packet heap-allocated at exactly `sizeof(fingerprint_packet_t)`, which is
  what `SEND_PACKET()` does with `ezlopi_malloc()`:

  - The checksum loop summed `data[0 .. length-1]` while the copy had written
    only 62 of them. At `length` 64 to 66 it read uninitialised bytes of
    `data[]` and then `chk_sum`, both inside the allocation; from 67 up it read
    past the end of it — `heap-buffer-overflow READ of size 1 ... 0 bytes after
    75-byte region`. The threshold is 64 rather than 62 because the old test was
    `length < MAX_PACKET_LENGTH_VAL`, so 63 went down the `memcpy` branch and
    copied 63 bytes into a 64-byte array: in bounds, just inconsistent with the
    62 the other branch copied.
  - That loop's counter was a `uint8_t` compared against an `int`, so from
    `length` 256 up, `i` wrapped to 0 and **the loop never terminated**. 255
    stops one iteration short of wrapping and completes; 256 does not. ASan
    cannot show this, because the over-read aborts first — so it was isolated by
    placing the packet inside a 4096-byte slab, where every read the loop makes
    is in bounds and the loop bound is the only thing left under test. 62, 64
    and 255 complete; 256, 300 and 1000 are killed by `SIGALRM`.
  - `Packet_len` was written from the unclamped value, and `send_uart_packets()`
    computes its `uart_write_bytes()` count as `Packet_len - 2`. A `length` of
    255 declares `Packet_len` 257 and would have put 255 bytes starting at
    `data` onto the wire — 191 of them past the end of the packet. That is the
    worst of the three, being the only one that moves adjacent heap contents off
    the device.

  Fixed by clamping the parameter once, before anything derives from it, and
  widening the counter to `uint16_t`. Clamping `length` itself rather than one
  statement means the copy and the loop cannot drift apart again, which is the
  lesson MB1013 taught twice.

  **This is latent, and saying otherwise would be easy and wrong.** No input
  reaches it today. `generate_packet()` and `SEND_PACKET()` are both `static`
  and appear in no header — checked, not assumed — and all seventeen call sites
  pass `(uint16_t)(sizeof(Combined_data))` of a local array. Those seventeen
  were resolved to literal sizes: 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4, 5, 5, 5,
  5, 6. The largest is 6, against a clamp threshold of 62 and a non-termination
  threshold of 256. It is fixed anyway because the comment on the `Packet_len`
  field — *"should not exceed 256bytes"* — invites precisely the packet size
  that breaks it, so the first person to send a template or a notepad page
  through this function would have found all three at once.

  **There is no host test for this one.** `generate_packet()` is `static` in a
  translation unit that needs `uart_write_bytes()`, `esp_timer` and
  `vTaskDelay`, so `tests/host` cannot reach it, and R307 is not the cheap
  header split GY-GPS6MV2 turned out to be — see *Testing* below. The
  AddressSanitizer runs above are a throwaway probe over copies of both
  versions, not a committed test, and they are the only evidence that the
  boundaries are where this entry says they are. Verified on the pinned v4.4.6
  toolchain only to the extent a build can verify anything: the object
  compiles, the clamp precedes all three derivations, the two-branch `memcpy`
  and the `uint8_t` counter are both gone, no warning mentions `R307_AS606.c`,
  the whole-build warning count is unchanged at 144, and the image is the same
  size as before.

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

### Local discovery — [bug]

- **The hub advertised a service on a port with nothing listening on it.** The
  `_ezlo._tcp` mDNS record was added unconditionally, pointing at the port the
  local websocket server uses. That server only exists when
  `CONFIG_EZPI_LOCAL_WEBSOCKET_SERVER` is on: with it off, `ezlopi_main.c`
  calls `EZPI_service_ws_server_dummy()` instead of
  `EZPI_service_ws_server_start()`, and the dummy does nothing — so the IP
  event handler that would eventually reach `httpd_start()` is never
  registered, and the port has no listener at all.

  A hub built that way announced itself over mDNS and then refused every
  connection the advert attracted. That is precisely the configuration a Home
  Assistant user who only wants MQTT builds, and it is now a first-class CI
  target, so it is worth being honest about. The advert is behind the same
  `#if` as the server it points at.

  `mdns_init()` and the hostname are deliberately left *outside* the new guard:
  resolving `<hostname>.local` is useful in every build, and it is all this
  service can truthfully offer when there is no local API to point at. CI
  asserts `EZPI_init_mdns` survives in the MQTT-only build, so over-guarding
  would be caught too.

  **No `_http._tcp` record was added**, which was the original plan and turned
  out to be wrong. The server registers exactly one URI, `/`, with
  `is_websocket = true`. A plain `GET` is not rejected: `esp_http_server` only
  performs the websocket handshake once an `Upgrade: websocket` header has set
  `ws_handshake_detect`, and otherwise still invokes the handler — which takes
  its `HTTP_GET` branch, files the connection in the websocket client table and
  returns without writing a response body. So any client that followed such an
  advert would hang rather than render. The reasoning is recorded in the source
  so it does not get re-added.

- **The listening port was a bare `17001` in two files in two different
  components.** `ezlopi-core-mdns` advertised it and
  `ezlopi-service-ws-server` bound it, with nothing tying the two together —
  changing the server's port would have left mDNS advertising the old one with
  nothing behind it. It now has one definition in `EZLOPI_USER_CONFIG.h`, which
  both already include. It has to live there rather than in either component's
  header because `ezlopi-core-mdns` declares no service dependencies and core
  must not depend on a service.

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

- **The tree no longer hard-codes the build directory in its `sdkconfig.h`
  includes — all 83 occurrences are converted.**
  `#include "../../build/config/sdkconfig.h"` pins both the build directory's
  name and its position relative to the source file, so it breaks for any build
  directory other than `<project>/build` — including the perfectly ordinary
  `idf.py -B <dir>`, out-of-tree builds and most IDE integrations. ESP-IDF
  already puts the generated config directory on every component's include path,
  so the plain `#include "sdkconfig.h"` works; seven files, two of them
  upstream's, were already written that way and built fine.

  Done in one pass rather than file-by-file because the result is *verifiable*
  rather than merely plausible. The preprocessor is handed the same
  `sdkconfig.h` either way, and the conversion replaced one line with one line
  in every file — so not even the `__LINE__` values that the `TRACE_*` macros
  embed could shift. That is a falsifiable prediction, so it was tested instead
  of asserted: an esp32 image built before and after the change differs in
  exactly 69 bytes, all of them build metadata — the app descriptor's build
  *time* string, the ELF sha256 that covers it, and the trailing image
  checksum. With those masked the images are byte-identical;
  `.flash.text` (1,254,203 bytes), `.flash.rodata`, `.dram0.data` and
  `.iram0.text` compare identical section by section; and all 13,800 symbols
  match **including their addresses**. The masking was itself checked against a
  deliberately corrupted image, so the comparison can fail.

  Host tests cannot be affected and were confirmed not to be: `tests/host`
  compiles four leaf files, none of them among the 83, and the one that needs
  headers gets the stubs in `tests/host/stubs/`.

  Counting the occurrences correctly took more care than it looks, and the
  traps are recorded here because the checker that now guards this has to keep
  avoiding them:

  - **Matching a fixed `../` depth misses a file.** Almost every occurrence sat
    two directories deep, but `ezlopi-user-config/EZLOPI_USER_CONFIG.h:48` is
    one shallower and wrote `"../build/config/sdkconfig.h"`. Match on the path
    *suffix* at any depth.
  - **Mentioning the path is not including it.**
    `git grep -l 'build/config/sdkconfig.h' -- '*.c' '*.h'` reported 89, but six
    of those named the old form in a comment while deliberately not using it —
    four converted files explaining what they moved away from, and two host-test
    stubs describing what they stub out. The real number was 83.
  - **A regex block-comment stripper *undercounts*, and this one cost the most.**
    `re.sub(r"/\*.*?\*/", "", text, flags=re.S)` looks obviously correct and is
    not: banner comments like `//*******` contain the substring `/*`, so the
    regex treats them as opening a block comment and deletes everything through
    the next `*/` — swallowing the real `#include` underneath. That hid
    `ezlopi_core_scenes_cjson.c` and produced a confident 82 instead of 83, and
    it was only caught because the script's total was cross-checked against an
    independent `grep` and the two disagreed. The checker is a character scanner
    that knows `//` starts a line comment, and self-tests that exact case on
    every run.

  The payoff was then confirmed directly rather than inferred from the include
  paths: `idf.py -B <dir> build` succeeds on the converted tree and creates no
  `<project>/build` at all. Restoring the pinned form on that same tree makes
  the identical command fail with
  `fatal error: ../../build/config/sdkconfig.h: No such file or directory`,
  while it still builds fine into the default `./build` — so the failure is the
  include and nothing else.

  Regression-guarded by
  [`tests/ci/check_sdkconfig_includes.py`](tests/ci/check_sdkconfig_includes.py)
  in the `hygiene` job, whose baseline is now zero, and by the
  `firmware-outdir` job, which builds esp32 into a directory outside the
  project so the capability itself is tested and not just the include style.

- **The OTA error path logged a meaningless error code, and read the wrong
  struct field to get it.** `_http_event_handler` called
  `esp_http_client_get_errno(evt)`, passing the *event* struct where the
  function takes a *client handle*. The compiler warned
  (`-Wincompatible-pointer-types`) and was right.

  ESP-IDF dispatches events as `&client->event`, and it happens that
  `sizeof(esp_http_client_event_t)` equals
  `offsetof(struct esp_http_client, transport)` — both 28 bytes on this target,
  confirmed with `_Static_assert` against the real headers. So reading
  `->transport` through the mistyped pointer read the word immediately *after*
  the event member, which is `client->data_written_index`: a byte count, used
  as a transport pointer. The `NULL` check inside
  `esp_http_client_get_errno()` does not catch this, because the pointer it is
  handed is a perfectly valid non-`NULL` event struct.

  That count is `0` for the GET that `esp_https_ota()` issues, so in practice
  this logged `-1` rather than crashing. But it logged `-1` for *every* failure,
  which is exactly the diagnostic someone needs when they first point a hub at
  their own firmware server and the TLS handshake fails. Had the count been
  non-zero, the chain dereferences it as a pointer and writes through it. Now
  passes `evt->client`.

### OTA — [feat]

- **Which hosts a hub will accept firmware from is now a build-time choice, so
  firmware can be self-hosted.** Previously `.cert_pem = ca_mios` was the only
  trust source, and `ca_mios.h` is gitignored — so the only way to get an
  OTA-capable image was to hand-write a header containing Ezlo's CA chain, and
  the only host that chain accepts is Ezlo's own. The OTA *URL* was already
  arbitrary (`hub.firmware.update.start` takes any URL, and is reachable from
  the local websocket API as well as the cloud), so the pinned certificate was
  the single thing standing between this project and self-hosted updates.

  The new `EZPI_OTA_CA_SOURCE` choice has two options:

  - `EZPI_OTA_CA_HEADER` — the original pinned header. **Still the default, so
    the shipped configuration is byte-for-byte unchanged.** Pinning is the
    stricter option, and it is the only one that can verify a private or
    self-signed CA.
  - `EZPI_OTA_CA_BUNDLE` — verify against the root certificates ESP-IDF already
    embeds, the way a browser would. **No `ca_mios.h` is needed, so an
    OTA-capable image builds from a clean clone.** A server with an ordinary
    public certificate works, with the per-target caveat below.

  This is close to free: `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` is already in
  every shipped target's configuration, and `esp_crt_bundle.h` was already
  `#include`d in `ezlopi_core_ota.c` but unused. Measured on esp32 with OTA on:
  pinned `0x191b60`, bundle `0x191b10` — the bundle build is 80 bytes
  *smaller*, because it drops the embedded PEM string and starts using bytes
  the image was already carrying.

  The trade-off is stated in the Kconfig help rather than hidden: selecting the
  bundle widens trust from one chain to every CA in it.

  Both paths are covered by CI, and the two jobs are deliberately asymmetric.
  `firmware-ota` generates a placeholder `ca_mios.h`, so it can only ever prove
  the pinned path compiles; it also now asserts that the pinned header is still
  what the Kconfig *default* selects, so the wider-trust option cannot become
  the default by accident. `firmware-ota-bundle` builds with **no `ca_mios.h`
  anywhere in the tree** and asserts its absence first — that clean-clone build
  is the claim being made, so it is tested directly rather than inferred.

  The link-presence check is on the `ca_mios` symbol, not on
  `esp_crt_bundle_attach`. The latter looks like the obvious thing to assert and
  is useless: it is present in *both* configurations, so an assertion on it
  would pass either way. `ca_mios` genuinely discriminates, and both assertions
  were checked against the opposite build to confirm they fail when they should.

  > The reason `esp_crt_bundle_attach` is present either way was recorded here
  > as "mbedtls links it regardless". That is not right, and the real reason
  > matters more. The linker does drop it when nothing references it — what
  > references it is `ezlopi_core_http.c`, unconditionally, for the outbound
  > provision-sync call. Read out of the `.map` of real builds: on esp32
  > `esp_crt_bundle_attach` is at `0x4017f040` and the 14,746-byte blob at
  > `0x3f427d05`, pulled in by
  > `libezlopi-core-http.a(ezlopi_core_http.c.obj) (_binary_x509_crt_bundle_start)`;
  > same on esp32c3 with the 63,739-byte blob. So the bundle is already in
  > every shipped image whether OTA is on or off, which is what makes
  > `EZPI_OTA_CA_BUNDLE` cost nothing — and it is also why the symbol cannot
  > discriminate. Right conclusion, wrong mechanism, now checked.

- **Corrected: the bundle does not reach Let's Encrypt on esp32.** The entry
  above, and the Kconfig help it describes, both said a self-hosted firmware
  server behind "a GitHub release asset or anything behind Let's Encrypt" would
  work. The GitHub half is true on all four targets. The Let's Encrypt half is
  false on esp32, and the reason is drift in configuration this project
  inherited rather than anything it did.

  ESP-IDF ships two root sets, and the four committed `sdkconfig.defaults.*`
  files do not agree on which to use —
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` on esp32,
  `..._DEFAULT_FULL` on esp32c3, esp32s3 and esp32s2. Both lines come from the
  same upstream commit (`f2a74e5`), so this is drift and not a decision.
  Reading the bundle out of real builds rather than trusting the CSV:

  | Target | Root set | Roots | Bytes | ISRG Root X1 / X2 |
  |---|---|---|---|---|
  | esp32 | CMN | 36 | 14,746 | **absent** |
  | esp32c3 | FULL | 136 | 63,739 | present |

  Let's Encrypt has issued from ISRG Root X1 since the `DST Root CA X3`
  cross-sign ended in 2021. `DST Root CA X3` is *still in the CMN set*, which
  is the part that makes this worth writing down: the root is there, it is
  expired, and it signs nothing current — so "an ISRG root is in the bundle
  somewhere" and "grep found a Let's Encrypt root" are both the wrong question.
  The same opt-in configuration therefore verifies a Let's Encrypt firmware
  host on three targets and fails TLS on the fourth, with no diagnostic beyond
  a TLS error on a hub in the field.

  The shipped root sets are **left alone.** Aligning esp32 to `FULL` was built
  and measured rather than estimated — `0x19c3a0` against `0x190440`, so 48,992
  bytes, taking it from 19% free to 17%, which is almost exactly the
  48,993-byte difference between the two embedded blobs. Affordable, then, but
  it would also widen trust for `ezlopi_core_http.c`, which already uses the
  bundle for the outbound provision-sync call, and that is upstream's call
  rather than a side effect of a documentation fix.
  What changed is that the Kconfig help and README now state the per-target
  answer and what to do about it, instead of a sentence that was true on three
  targets out of four.

- **The per-target root sets are now a check, not a claim.**
  `tests/ci/check_ota_ca_bundle_roots.py` walks the bundle the build actually
  embedded — `gen_crt_bundle.py` writes a big-endian count, then per
  certificate a 2+2 byte length header, the subject-name DER and the key DER —
  and compares the subject CNs against the set the documentation promises for
  that variant.

  It has to be a check because the thing being asserted is the content of a
  file this repository does not own: ESP-IDF generates it from its own CSV and
  PEM, and bumping the IDF pin can change it. No build would notice. The bundle
  is generated, embedded and linked whatever is in it, and
  `esp_crt_bundle_attach()` returns `ESP_OK` either way.

  It fails in **both** directions, which is the part an earlier draft got
  wrong. A root the docs promise going missing is the obvious failure. A root
  the docs warn is absent turning up is also a failure, because then the esp32
  Let's Encrypt warning has quietly become a lie and self-hosters are being
  steered away from something that would have worked.

  Verified by mutation on real artifacts, not only on fixtures: presenting the
  real esp32 CMN bundle to the FULL expectations reports both ISRG roots
  missing; presenting the real esp32c3 FULL bundle to the CMN expectations
  reports both as unexpectedly present; and flipping one byte of a real bundle
  is rejected by the parser as `certificate 2 of 36 claims 33282+8752 bytes but
  only 14319 remain` rather than silently yielding a short list that would read
  as "roots are missing". The ten self-test cases cover the same ground plus an
  empty bundle, which must not pass as "nothing required is missing".

  It runs in the four-target `firmware` matrix rather than only in
  `firmware-ota-bundle`, because the bundle is in all four images regardless of
  OTA, so its contents are a property of every build.

- **OTA stays off by default, and the Kconfig help now says why.** The goal was
  to turn it on. Having checked what that would take, it should not be turned
  on, and leaving `default n` with the word "Enable OTA" as its entire help text
  made a deliberate position look like an oversight.

  Everything mechanical is already in place. The partition table carries `ota0`
  and `ota1` at `0x1EFC00` each plus `otadata` at `0x15000`; all four
  `sdkconfig.defaults.*` have `CONFIG_EZPI_ENABLE_WIFI=y` and
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`; and the OTA client itself was hardened
  in earlier work in this changelog. Measured on esp32 with
  `CONFIG_EZPI_ENABLE_OTA=y` and `CONFIG_EZPI_OTA_CA_BUNDLE=y`, built from a
  tree containing no `ca_mios.h`: `0x1923f0` against `0x190440` for the default
  build, so **8,112 bytes**, still 19% free. It builds, links, and the image
  contains `esp_https_ota` and no `ca_mios`.

  The blocker is that neither certificate source is defensible *as a default*:

  - `EZPI_OTA_CA_HEADER`, the current default, needs `ca_mios.h`. That file is
    in `.gitignore` — `git blame` puts the line in upstream commit `f2a74e5`,
    so it is upstream's decision, not this fork's. `default y` with this CA
    source would mean **a clean clone fails to build**, which is worse for a
    new user than OTA being off. Committing a CA header to work around it would
    reverse an upstream decision.
  - `EZPI_OTA_CA_BUNDLE` builds from a clean clone, but as the default it
    widens trust from one chain to every public root. The `firmware-ota` job
    already asserts the default has not moved off the pinned header, which is a
    guard an earlier session put there on purpose; silently flipping it would
    just be defeating that guard from the other side.

  So it remains opt-in, with the two lines to add written out in the help text
  and in README §6.1, and both paths built in CI. Worth noting what the choice
  does *not* buy either way: **no shipped configuration verifies the firmware
  image itself.** `CONFIG_SECURE_BOOT` and
  `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` are unset on all four targets
  (esp32c3 carries `CONFIG_SECURE_BOOT_SUPPORTS_RSA=y`, which is a chip
  capability, not a configuration), so an image is trusted on the strength of
  where it was downloaded from and nothing else — and what the pinned option
  pins is a CA root, not Ezlo's host, so an attacker holding any certificate
  from that one root is not excluded by it either.

  Both triggers were traced rather than assumed. `hub.firmware.update.start` is
  registered in `ezlopi_core_api_macros.h` behind `CONFIG_EZPI_ENABLE_OTA` and
  reachable from two places: Ezlo's cloud websocket, and the local websocket
  server — where `__message_upcall()` admits nothing but `hub.offline.login.ui`
  until `EZPI_core_offline_is_user_logged_in()`, whose token is compared
  against the stored local key. The local key is **write-only** across every
  external surface (grep of BLE and UART for `local_key` finds only the two
  provisioning writers, and the BLE device-info payload does not carry it), so
  there is no unauthenticated path to an update. The README and Kconfig help
  now record the signed-OTA option —
  `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, which makes
  `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` default on and needs no
  eFuses burned — as the answer for self-hosters, along with the reason it
  cannot be a default here: a hub built that way refuses Ezlo's images.

### Local API credentials — [sec]

- **The local API key was printed to the serial console on every login
  attempt.** `ezlopi_core_offline_login.c` contained
  `TRACE_D("password: %s", password_saved)`, where `password_saved` is the
  device's only local-API credential. Now removed.

  The reason this is listed as a defect rather than as debug noise is that a
  `TRACE_D` here is *not* compiled out or filtered in a shipped build, contrary
  to how it reads. These are EzloPi's own trace macros, not `ESP_LOG`, so
  `CONFIG_LOG_DEFAULT_LEVEL` does not apply to them. The severity was chased to
  the actual `printf`: `TRACE_D` is `trace_log_sev_debug`
  ([`ezlopi_util_trace.h:192`](ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h#L192)),
  which is severity 4; `serial_log_severity` defaults to
  `ENUM_EZLOPI_TRACE_SEVERITY_MAX`
  ([`ezlopi_core_log.c:86`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L86));
  the upcall is registered at `:255` and ends in `printf` at `:309`. The
  default build printed it.

- **The Wi-Fi password was printed every time the stored config was loaded.**
  `ezlopi_core_nvs.c:393` was
  `TRACE_D("Load wifi config:: ssid: %s, password: %s", wifi_info, &wifi_info[32])`.
  Now reports the SSID and the password's length only.

  This one is worth reading as a method note rather than as a bug. It was not
  found by reading the file; it was found by checking that the *first* fix had
  actually worked — searching the built `.bin` for the format string
  `password: %s` after removing the line above. The count came back 1 instead
  of 0, and this was the other caller. Grepping the source for the credential
  would not have found it, because the variable is called `wifi_info` and the
  password is an offset into it.

- **The provisioning token and local key were printed during provisioning.**
  Two `TRACE_S("...: %s", ...)` calls in `ezlopi_core_factory_info.c` logged
  the local key and the cloud provisioning token in full as they were written
  to flash. Both now report a length. The other five fields written in the same
  block are identifiers and URLs and are still logged in full.

- **The local key comparison was a prefix match.**
  `strncmp(password_saved, cj_token->valuestring, strlen(password_saved))`
  accepted any token *beginning* with the key, so appending anything to a valid
  key still authenticated. It also accepted everything when the stored key was
  empty, because `strlen("")` is 0 and `strncmp(a, b, 0)` is 0.

  Replaced by `EZPI_util_localkey_match()`, which requires equal lengths and
  equal bytes, refuses a stored key shorter than 8 characters, accepts `NULL`
  on either side, and runs the whole key without an early exit so its duration
  does not report how many leading bytes a guess got right. The length check
  ahead of that loop does leak the key length, which is fixed by whoever
  provisioned the device and is not secret.

  The empty-key case was not reachable in the shipped build, and the reason is
  worth recording because it was luck rather than a check:
  `EZPI_core_factory_info_v3_get_local_key()` tests `isprint(local_key[0])`,
  and erased flash reads as `0xFF`, for which `isprint` is false — so a blank
  region returned `NULL` and the caller's `NULL` guard caught it. The
  protection against "empty key accepts every token" was a printability test
  on one byte, in a different component, written for a different purpose.

- **The user-id verification was disabled with a `#warning`, and is now a
  documented build option rather than commented-out code.** Two copies of
  `#warning "Lomas: user-id verification disabled!"` and about fifteen lines of
  commented-out verification are replaced by `__user_id_accepted()`, behind
  `CONFIG_EZPI_OFFLINE_LOGIN_VERIFY_USER_ID`, **`default n`**.

  Default `n` is deliberate and is the conservative answer, not the lazy one.
  Three findings drove it. The commented-out code refuses the login outright
  when no user id is stored, which would lock a self-flashed device out of its
  own local API. No shipped build has ever enforced this check, so enabling it
  changes the behaviour of already-claimed devices against an app whose exact
  `user` value cannot be tested from this repository. And this repository has
  already answered the same question the same way once:
  [`ezlopi_service_ble_ble_auth.c:158`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_ble_auth.c#L158)
  keeps a prefix comparison for the BLE user id, with a note saying it is what
  the shipped app authenticates against. Enforcing strict equality here while
  that path deliberately does not would be incoherent.

### Local API sessions — [sec]

These four defects are one defect wearing four hats: nothing in the local
websocket server ever associated a request, a logout, or a broadcast with *the
connection it belonged to*. Each symptom below follows from that.

This was found while scoping a no-compile local configuration UI — the feature
the next section's groundwork is for. The UI would have been served from this
server and gated by this login, so the gate was checked before anything was
built on it. It did not hold.

- **One login authorised every connection on the network.** The whole hub
  shared `static bool logged_in = false`
  ([pre-fix `ezlopi_core_offline_login.c:73`](ezlopi-core/ezlopi-core-offline-login/ezlopi_core_offline_login.c)).
  The per-request gate in `__message_upcall()` (pre-fix `:302`) consulted that
  flag and nothing else, so it asked "has *anyone* logged in?" rather than "has
  *this caller* logged in?"

  The gate itself was in the right place and ran on every request, which is why
  this reads as safe on a quick pass. The flag behind it was global.

  Reach: the server binds `INADDR_ANY`
  ([`httpd_main.c:287`](https://github.com/espressif/esp-idf)) on port 17001
  ([`EZLOPI_USER_CONFIG.h:148`](ezlopi-user-config/EZLOPI_USER_CONFIG.h#L148)),
  plain `ws://`. So while a legitimate client was logged in, any other peer on
  the LAN could call any API method — device configuration, scenes, factory
  reset — without presenting the local key.

  **Honest severity.** This is not remotely reachable from outside the LAN, and
  it is not reachable from cold boot: the flag starts false, and until some
  legitimate client logs in the only method that passes the gate is
  `hub.offline.login.ui` itself, which still requires the key. It needs a
  logged-in client to stand on. On a hub in normal use — a phone app or local
  UI connected — that condition is the steady state, not an edge case.

- **Any unauthenticated peer could log everyone else out.** A websocket CLOSE
  frame reached `EZPI_core_offline_logout_perform()` (pre-fix `:436` and
  `:465`) with no login check, and that cleared the single global flag. Opening
  a socket and closing it immediately deauthorised every real client. A third
  site (pre-fix `:637`) did the same from the send-failure path, so one
  client's bad connection logged out the others.

- **Only one client could ever receive broadcasts.** The client list keyed and
  deduplicated on `http_handle`, which callers fed from `req->handle` — a field
  ESP-IDF documents as `/*!< Handle to server instance */`
  ([`esp_http_server.h:330`](https://github.com/espressif/esp-idf)). It is one
  value for the entire server, identical for every connection, so every client
  after the first looked like a duplicate and `clients_add()` short-circuited
  forever. A second client connected cleanly and then silently received
  nothing.

  The struct field was commented "HTTP connection handle", which is how this
  survived: the comment described what the code needed rather than what the
  field held. That comment is now corrected in place, since it was load-bearing.

- **A plain browser GET was treated as a completed handshake.** ESP-IDF skips
  its handshake block when there is no `Upgrade` header
  ([`httpd_uri.c:314`](https://github.com/espressif/esp-idf), gated on
  `aux->ws_handshake_detect`) but invokes the URI handler regardless at `:329`.
  The handler logged "Handshake done" and registered the socket anyway, so
  `curl http://hub:17001/` planted a non-websocket descriptor as the broadcast
  target. Now the handler asks `httpd_ws_get_fd_info()` instead of assuming,
  and a non-websocket GET gets a one-line plain-text explanation of the port.

**The fix is per-connection authorisation.** A new dependency-free component,
`ezlopi-util-wssession`, holds a fixed table of `{fd, authorised}` keyed on the
socket descriptor. ESP-IDF's `open_fn`/`close_fn` hooks create and destroy the
entry, so a session cannot outlive its connection — and `close_fn` is the right
hook rather than the CLOSE frame, because ESP-IDF documents it as running "for
all terminated sessions, including sessions where the socket was closed by the
network stack", which is the case a CLOSE frame misses. `open_fn` always sets
`authorised = false`, so a reused descriptor number never inherits the previous
owner's authority.

Two details worth recording because getting either wrong would have been worse
than the bug:

- **Overriding `close_fn` transfers responsibility for closing the socket.**
  [`httpd_sess.c:365`](https://github.com/espressif/esp-idf) is
  `if (hd->config.close_fn) close_fn(hd, fd); else close(fd);` — so the hook
  must call `close()` itself or leak a descriptor per connection. This was
  verified in the IDF source rather than assumed, because the two failure modes
  are a descriptor leak and a double close.

- **The request-to-connection association had to survive two tasks.**
  `EZPI_core_api_consume` is called from the cloud task
  ([`ezlopi_service_webprov.c:355`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L355))
  as well as the local one, so an ambient "current connection" global would be
  readable from the wrong task. The claim records the owning task handle and is
  honoured only when `xTaskGetCurrentTaskHandle()` matches, so a cross-task
  read fails closed and returns "no connection" rather than someone else's. No
  lock is needed because only the claiming task writes. The same hazard is
  already documented in this tree at
  [`ezlopi_core_ota.c:164`](ezlopi-core/ezlopi-core-ota/ezlopi_core_ota.c#L164).

**A use-after-free was fixed on the way through.**
`ezpi_ws_server_broadcast()` dereferenced `curr_client->next` after calling
`ezpi_ws_server_send()`, which can free that node via the fail-count eviction
above. Pre-existing, but it was close to unreachable while the list could only
hold one entry, and becomes reachable as soon as multiple clients work. The
loop now reads `next` before sending.

**One deliberate behaviour change, flagged because it is visible to the Ezlo
app.** `hub.offline.login.ui` dispatched over the *cloud* connection now
returns an error (`-32500`, `user.login.nolocalsession`) instead of succeeding.
This method authorises the connection it arrives on, and only the local
websocket server has one; dispatched from the cloud there is nothing to
authorise. It previously "succeeded" by setting the global flag — which is
precisely the bug above, so preserving that success is not an option. Logging
in over the local connection, which is what the app actually does, is
unchanged. Two simultaneous logged-in clients now work, where the old code
refused the second while admitting unauthenticated ones.

`EZPI_core_offline_logout_perform()` is left in place and is now correct if
called, but **nothing calls it** — all three former call sites were connection
teardown, which now goes through the close hook. Its documentation says so, to
keep it from being re-wired to the old pattern.

Costs +1136 bytes on both esp32 and esp32c3 (measured, identical on each).

**Untested on hardware.** No board has had two clients connected at once, and
the cloud-path error above has not been exercised against the real Ezlo app.
The session logic itself is covered by 185 host checks described under Testing.
What is unverified is the wiring: that ESP-IDF's hooks fire as documented on a
real board.

**What this does not fix.** The transport is still plain `ws://`, so anything
able to intercept or spoof TCP on the LAN is unaffected by any of this — a
session keyed on a descriptor authorises whoever holds that socket. Only TLS
would change that, and it is listed under Known gaps rather than claimed here.

### Device configuration — [bug]

- **A device configuration that will not parse no longer bricks the hub.**
  `EZPI_device_prepare()` reads the stored configuration once per boot, and when
  the parse failed it stamped this chip's name into the config, wrote it back
  and restarted — unconditionally, for every possible cause of failure.

  That is a correct repair for exactly one cause: a configuration saved without
  a `chipset` key, which is the migration it was written for. For the others it
  is an unrecoverable reboot loop, and the reason is in `cjext` rather than in
  the caller. `add_item_to_object()` ends in `add_item_to_array()`, which
  appends — there is no replace and no duplicate check — while
  `get_object_item()` walks from `object->child` forward and returns the
  **first** key whose name matches. So when a `chipset` key is already present
  and is itself why the parse failed, the stamp appends a second key that
  nothing will ever read. The next boot fails in the same place, appends a
  third, and restarts again.

  `ezlopi_device_parse_json_v3()` reports failure in four places, and only the
  first is repaired by stamping:

  | Cause | Line | Stamping helps? |
  |---|---|---|
  | No `chipset` key at all | `devices.c:977` | yes — this is the migration |
  | `chipset` present but not a string | `devices.c:971` | no — duplicate key, never read |
  | `chipset` names a different chip | `devices.c:964` | no — duplicate key, never read |
  | A device entry left `error` set | `devices.c:933` | no — irrelevant to the cause |

  The third row is the realistic one: move a config between an ESP32 and a C3,
  or have the app send the wrong chipset, and the board is finished.

  **Why it was worse than a loop.** The restart is issued from inside
  `EZPI_init()`, which [`ezlopi_main.c:139`](ezlopi-main/ezlopi_main.c#L139)
  calls before `EZPI_SERV_uart_init()` (line 143) and
  `EZPI_ble_service_init()` (line 150). During such a loop neither ever runs,
  nor does the websocket server, so the hub is unreachable over every interface
  it has — which removes the BLE factory reset too, the one documented
  recovery. `CONFIG_FREERTOS_HZ=1000` in all four committed
  `sdkconfig.defaults.*` files makes the `vTaskDelay(1000)` ahead of the
  restart one second, so the cycle also rewrote the configuration into flash
  about once a second for as long as the board had power. Recovery was a serial
  erase.

  The configuration that triggers it is app- or attacker-supplied: it arrives
  over BLE characteristic `0xE501` or over UART.

  **The fix is to restart only when restarting can change the answer**, which
  is only when the key being added was absent. Everything else finishes booting
  instead, so the services come up and the hub can be given a corrected config
  over BLE or the local API, or factory reset. A reachable hub is recoverable;
  a rebooting one is not. The legitimate migration is unchanged and still costs
  exactly one restart.

  A mismatched chipset is deliberately **not** rewritten to match the running
  chip. The pin numbers in such a config were chosen for different hardware, so
  adopting it silently would instantiate devices on whatever GPIOs those
  numbers happen to name here. Refusing it and staying reachable is the weaker
  action and the safer one.

  Whatever devices did instantiate are kept, and the enum is named
  `EZPI_DEVCFG_CONTINUE_BOOT` rather than "boot without devices" for that
  reason: the parse builds each device as it walks the list and reports only
  the last entry's outcome, so a fault in a late entry leaves the earlier
  devices working. The two chipset failures do give an empty list, because they
  return before the device loop is entered.

- **The decision is a tested component rather than an inline branch.** The
  three-way choice moved to `ezlopi-util-devcfg`, which has no `REQUIRES` for
  the same reason as `ezlopi-util-sanitize`, `-ha` and `-localkey`: the
  function it came from touches the factory-info partition, `cjext` and
  `esp_restart()`, so it could only be exercised by flashing a board and
  deliberately storing a broken configuration on it — and the failure under
  test takes away the interfaces you would watch it through.

  Reduced to three booleans, [`tests/host/test_devcfg.c`](tests/host/test_devcfg.c)
  enumerates all eight inputs and asserts the property that actually matters:
  **no input restarts more than once.** That is checked by construction rather
  than by my having reasoned about the cases. Termination is structural —
  `EZPI_DEVCFG_STAMP_AND_REBOOT` is returned only when `chipset_key_present` is
  false, and stamping makes it true, so the input that caused a restart cannot
  recur after it.

  The old rule is kept in the file as `legacy_action()` and a check fails if it
  ever *stops* looping, so the file demonstrates the defect rather than
  asserting it. Proven able to fail, on three mutations of the real
  implementation: reverting to the unconditional stamp reports 11 restarts on
  each of the four failing inputs (7 failures); never restarting breaks the
  migration (2 failures); and transposing the two boolean arguments — the exact
  wiring mistake the file names as being outside its reach — is caught with 4
  failures.

  The model is deliberately generous to the old code in the one way that
  matters: it assumes stamping always succeeds in making the key present,
  whereas the real `cjext` appends a duplicate the parser never sees. That can
  only make the old behaviour look better than it is, so a loop reported on the
  host is at least as bad in firmware.

- Also on that path: the store is no longer conditional on the diagnostic
  `cJSON_PrintUnformatted()` succeeding. It used to sit inside `if (tmp_str)`,
  so an allocation failure while printing silently skipped both the write and
  the restart. And the print buffer is now released with `ezlopi_free()`:
  `cjext` allocates it through `ezlopi_malloc`
  ([`cjext.c:1331`](ezlopi-components/cjext/cjext.c#L1331)) and resizes it with
  `ezlopi_realloc` ([line 1349](ezlopi-components/cjext/cjext.c#L1349)) before
  returning it, so the plain `free()` there was a mismatch. Equivalent in the shipped configurations,
  where `CONFIG_EZPI_HEAP_ENABLE` is `default n` and unset in all four
  `sdkconfig.defaults.*` files so `ezlopi_free` *is* `free()`; it is the
  opt-in heap-tracking build that was left with a stale entry.

  The expected chipset name also comes from one `__expected_chipset_name()`
  helper now, instead of the presence test and the stamped value each having
  their own `#if` chain. Two chains are what quietly stop agreeing when a
  target is added, and a target present in one but missing from the other gives
  a hub that stamps a name its own parser then rejects — the loop above, from
  the opposite direction.

  Costs 304 bytes on esp32 (`0x190570` against `0x190440`) and 272 on esp32c3
  (`0x1a22f0` against `0x1a21e0`), with the free-space percentage unchanged on
  both. **Untested on hardware:** no board has been made to store a config that
  fails to parse, so the loop is established by reading the code and the
  termination by the host test, not by watching a board recover.

### Device configuration — [feat]

- **There is now something that can say whether a device configuration is worth
  storing.** New component
  [`ezlopi-util-devcfgval`](ezlopi-util/ezlopi-util-devcfgval/), one function
  and a reason enum, with
  [`tests/host/test_devcfgval.c`](tests/host/test_devcfgval.c) — 211 checks.

  The two paths that already wrote a device configuration — the BLE
  *dynamic-config* characteristic and the UART command channel, not the BLE
  provisioning characteristic, which writes connection data and never touches
  the device config — checked nothing about what they were handed beyond "cJSON
  parsed it". (They now call this component; that came later and is its own
  entry below.) From
  [`ezpi_dynamic_config_write_func()`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_dynamic_config.c#L265),
  and the same shape in
  [`ezpi_service_uart_set_config()`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L1399):

  ```c
  cJSON *cjson_config = cJSON_Parse(__FUNCTION__, decoded_data);
  if (cjson_config)
  {
      if (EZPI_core_factory_info_v3_set_ezlopi_config(cjson_config))
  ```

  So `{"hello":"world"}` is accepted and written to flash. It is valid JSON and
  it is not a device configuration. Nothing complains at the time. On the next
  boot `ezlopi_device_parse_json_v3()` reports `EZPI_ERR_JSON_PARSE_FAILED`
  because there is no `"chipset"`, the recovery rule added above stamps the
  chipset name and restarts once, and the boot after that finds a chipset that
  matches and no `"dev_detail"` at all — which is not an error to the parser,
  so it reports success with an empty device list.

  What the user sees is a controller that works and has forgotten every sensor,
  with nothing connecting that to the configuration they sent some minutes
  earlier.

  The quietest of the five refused shapes is a device entry whose `id_item` is
  absent, not a number, or zero. The parser reads it through
  `CJSON_GET_VALUE_INT`, which writes `0` and only overwrites it for a
  `cJSON_Number`, and then looks for a driver only `if (0 != id_item)`. An
  entry without a usable one is skipped in silence — no error, no device, no
  log line naming the entry. That is the failure that cannot be diagnosed from
  outside the device, so it is refused before the write instead.

  A missing `device_id` is refused for a different reason, found while writing
  the justification for each rule down. The parser sets
  `error = EZPI_ERR_JSON_PARSE_FAILED` for it
  ([`ezlopi_core_devices.c:934`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L934))
  and then carries straight on: the entry is still instantiated if its
  `id_item` is usable, and `error` is reassigned on every iteration, so whether
  the configuration is reported as broken depends on whether a *later* entry
  overwrites it with `EZPI_SUCCESS`. A hub whose boot verdict turns on the
  ordering of the device list is not something to leave reachable from the
  network.

- **It is deliberately asymmetric, because the Ezlo app writes these.** Unknown
  members, extra members and anything this component has no opinion about are
  accepted. A configuration carrying a key added by a newer app must not be
  refused by an older hub — that failure breaks a hub that was working, which
  is worse than the one being fixed.

  The same reasoning covers a quirk that would have been easy to get wrong in
  the strict direction. `cJSON_GetObjectItem` is case insensitive and the boot
  parser uses it, so a configuration spelling the member `"ChipSet"` builds
  devices on a board today. The check therefore accepts it, and
  `test_member_lookup_matches_the_parser()` pins that. The chipset *value* is
  still compared case sensitively, because that is what the parser's `strncmp`
  does.

  A mismatched chipset is refused rather than adopted, which is the one place
  this is stricter than it needs to be on purpose: the pin numbers in an ESP32-S3
  configuration were chosen for an S3, so honouring it here would instantiate
  devices on whatever GPIOs those numbers happen to name on this part.

- **Why it is a separate component rather than a check inside the parser.**
  There is no dry run. `ezlopi_device_parse_json_v3()` is `static` and
  *instantiates devices as it walks* — it calls each driver's
  `EZLOPI_ACTION_PREPARE`. So it cannot be asked whether a configuration is
  acceptable; the only way to find out on hardware is to store it and reboot,
  and the answer arrives as an absence. `ezlopi_core_devices.c` also cannot be
  host-compiled at all: it reads the factory-info partition, drives the device
  table and calls `esp_restart()`.

  Separated out, it answers one question with no side effects and no
  allocation, and the host test puts a few hundred shapes through it in
  milliseconds. The checks hardest to get from a board are the ones that matter
  most: that the *first* bad entry in document order is the one named, that
  4095 serialised bytes fit a 4096-byte field and 4096 do not, and that a
  2000-entry list walks clean under ASan and UBSan.

  `test_the_tree_is_not_modified()` is the one worth copying. The callers hold
  a tree they are about to serialise and write, so a check that stamped a
  default or dropped a member would mean the thing stored is not the thing
  judged — and that difference would only surface on the next boot. Every shape
  in the table is serialised before and after and the two must be identical.

- **Mutation-tested 10 of 10 caught.** Not checking `id_item`; comparing
  `valuedouble` against `0.0` instead of the parser's `(int)` cast, which
  accepts `0.4` — a non-zero number that truncates to zero and builds nothing;
  not checking that `dev_detail` is an array; the size check off by one;
  reporting the last bad entry instead of the first; comparing the chipset as a
  prefix, so `ESP32S3XX` matches `ESP32S3`; returning `NULL` for a value
  outside the enum; two reasons sharing a token; not checking that an entry is
  an object; and rewriting `chipset` in place to be helpful, which
  `test_the_tree_is_not_modified()` catches twice. A control run with the
  component restored passes.

- **Nothing called it when it was written.** The method that does now —
  `hub.ezlopi.config.get`/`.set`, the no-recompile configuration path, in the
  entry immediately below — needed one thing this component deliberately does
  not do: say whether an `id_item` names a driver *this image was built with*.
  That depends on the device table in `ezlopi_core_devices_list.c`, which
  differs between builds and is not available here.

  As committed, nothing referenced it and the linker dropped it: the esp32
  image was `0x190c70`, **byte-identical in size** to the commit before, with
  the component compiled into `libezlopi-util-devcfgval.a` and no `devcfgval`
  symbol in the ELF. That is no longer true as of the next entry, which is
  where the size this component actually costs is recorded.

- **The two unauthenticated paths still bypassed it, and now do not — [sec].**
  The component was written because of what the BLE dynamic-config
  characteristic and the UART command channel accept, and then wired only into
  the websocket `.set` method that came with it. The two paths it was written
  for kept calling
  `EZPI_core_factory_info_v3_set_ezlopi_config()` directly, so the check
  existed, was tested, was linked into the image — and the most exposed writer
  in the firmware went straight past it. `{"hello":"world"}` over BLE still cost
  the user every configured sensor, by the sequence described further up this
  entry.

  Both now call `EZPI_util_devcfgval_check()` before the store and refuse on a
  non-`OK` reason: the UART path answers `EZPI_UART_CMD_STATUS_FAIL`, and the
  BLE path logs the reason token and leaves `reboot_to_apply` false, so a
  refused configuration no longer restarts the hub. This is marked **[sec]**
  rather than [bug] for the reason the rest of the BLE items are: that
  characteristic is `ESP_GATT_PERM_WRITE` and is reached with no pairing, so the
  write that wiped the device list needed only radio range.

  The check is deliberately **not** pushed down into
  `EZPI_core_factory_info_v3_set_ezlopi_config()` itself, which would have
  covered every caller at once and been the obvious move. Two of its callers are
  in the boot path —
  [`ezlopi_core_devices.c:666`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L666)
  and [line 720](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L720) —
  and the first is the chipset-stamping migration, which by construction writes
  back a configuration that has *just failed to parse*. A validator there would
  refuse the repair. That code runs from inside `EZPI_init()` before BLE, UART
  and the websocket start, where its own comment notes that restarting on a
  cause the stamp cannot fix "loops forever with no interface left to recover
  through" — so the blast radius of being wrong at that spot is an unrecoverable
  device, not a refused write. Validation belongs at the three entry points that
  accept a configuration from outside; the boot path's job is repair, and it
  keeps the ability to store a document a strict reader would reject.

  No new test. What changed is two call sites in files that cannot be
  host-compiled — the BLE service needs the GATT profile and the factory
  partition, the UART service needs the USB/driver headers — and the behaviour
  being invoked is already covered by `test_devcfgval.c`, which asserts
  `{"hello":"world"}` is refused with `EZPI_DEVCFGVAL_CHIPSET_MISSING`. The
  wiring itself is evidenced by a four-target build plus an `nm` check that
  `EZPI_util_devcfgval_check` is present in each ELF, since a call that failed
  to link against the component would otherwise leave both services writing
  unvalidated configurations while the build stayed green.

- **Not checked, and still listed under Known gaps:** whether the GPIOs a
  configuration names are valid for the target, free, or claimed twice. Looking
  into it turned up evidence that one of those three is the wrong thing to
  check; the Known gap now records what the pin keys and sentinels actually are
  rather than what it assumed.

- **The device configuration can now be read and replaced over the websocket
  API.** Two new methods, `hub.ezlopi.config.get` and
  `hub.ezlopi.config.set`, in new component
  [`ezlopi-cloud-devcfg`](ezlopi-cloud/ezlopi-cloud-devcfg/), with
  [`tests/host/test_devcfg_api.c`](tests/host/test_devcfg_api.c) — 322 checks,
  counting the third method's in the entry below.

  Before this, the only ways to tell the firmware which sensors exist and which
  GPIOs they sit on were the BLE dynamic-config characteristic and the UART
  command channel. Both need physical proximity, and neither is reachable from
  the websocket API that every other hub setting is changed through. A hub on a
  shelf could be asked its firmware version, its rooms, its scenes and its MQTT
  broker over the network, but not what devices it had been configured with,
  and could not be reconfigured at all. This is the piece "no-compile
  configuration" was missing: the device list becomes a thing you can read and
  replace over the same connection as everything else, which is what a local
  configuration UI needs in order to exist.

  **The write path refuses before it writes, and the reason is not the obvious
  one.** Storing a wrong configuration does not fail loudly — it fails on the
  next boot, silently, as devices that simply are not there.
  `ezlopi_device_parse_json_v3()` skips an entry whose `id_item` it does not
  recognise without logging anything, and treats an absent device list as
  success. So `.set` applies `EZPI_util_devcfgval_check()` and then the one
  check that function deliberately leaves to its caller: whether each `id_item`
  names a driver this image was built with. The order is fixed and is the point
  of the file — parse the envelope, serialise once and refuse if it does not
  fit, validate the tree, check every `id_item` against the device table, and
  only then write.

  Those last two refusals are reported under separate tokens on purpose. A
  configuration refused by the validator is malformed and every build will
  refuse it; one refused for an unknown `id_item` is well-formed and would work
  on a build that included that driver. The remedy is a different firmware
  image, not a different configuration, and a single "invalid config" would
  send someone editing JSON that was already correct. Errors also carry
  `badIndex` — the position in `dev_detail` of the entry at fault — because a
  thirty-device configuration refused without one leaves the user to find the
  bad entry by bisection.

  **It does not reboot.** The parser runs once, from `EZPI_device_prepare()`
  during `EZPI_init()` — one call site, which is why a stored configuration
  takes effect on the next boot and not before. `.set` answers
  `"rebootRequired": true` rather than restarting the hub out from under the
  caller. There is deliberately no `.reset` to match `hub.mqtt.config.reset`:
  resetting this field means a hub with no devices, which the existing
  factory-reset paths already reach, and a second unauthenticated route to
  "forget everything" is not an improvement.

- **The chipset name now has one definition instead of two.**
  `EZPI_core_device_expected_chipset_name()` is the de-`static`'d
  `__expected_chipset_name()` from
  [`ezlopi_core_devices.c`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c),
  exposed so `.set` can refuse a configuration naming the wrong chip using the
  same name the boot parser will compare against. A second `#if
  CONFIG_IDF_TARGET_*` chain in the cloud layer would have been fine the day it
  was written and would be exactly the drift the comment above that function
  already warns about — a target added in one place and forgotten in the other
  gives an API that refuses configurations the parser would accept, or accepts
  ones it will not.

- **Everything that cannot be host-tested is in one file.**
  `ezlopi_cloud_devcfg_port.c` is seven forwarding functions, because
  `ezlopi_core_factory_info.h` includes `<esp_partition.h>` and
  `ezlopi_core_devices_list.h` pulls in seven HAL headers, so nothing including
  them compiles on a host. The seam is drawn exactly where the host build stops
  working: the test compiles the real handler and the real validator and
  substitutes seven recording fakes, and the port has **no host coverage at all,
  by design**. It is short enough to read in full and has no branch that could
  be wrong in an interesting way.

  The property the test is built around is not "does it store a config" — that
  is one call — but the negative one: **a request that is refused must leave
  the stored configuration alone.** Every refusal case asserts that nothing was
  written.

  Twelve mutations of the write path were run, eleven caught: collapsing every
  validator token into one (18 failures), never checking that a driver exists
  (28), ignoring the validator verdict (71), handing the validator a limit one
  byte too generous (6), mis-measuring the document a byte short (2), treating a
  failed write as success (3), inverting the region-size fallback (6), accepting
  a non-object `params.config` (1), not requiring `params` (2), reporting
  `badIndex` on errors it does not apply to (7), and leaking the string `.get`
  reads (caught by LeakSanitizer, not by an assertion). A control run passes.
  The twelfth — changing the size clamp's `>` to `>=` — survives and is
  *provably equivalent*: at `serialised_len == limit` both forms yield `limit`,
  since the false branch casts a value already equal to it. The clamp exists
  only to stop a length above 4 GiB wrapping to a small number, so it has no
  behavioural boundary to detect; it is labelled an equivalent mutant in
  [`tests/host/mut_devcfg.py`](tests/host/mut_devcfg.py) rather than left
  looking like a gap.

  Two of those mutations changed the code under test rather than the test.
  Reporting `badIndex` on every error survived at first — the header promised
  it was *absent*, not `-1`, for refusals that are not about one entry, and
  nothing checked it, because the test's reader returned `-1` for both. It now
  asserts on the member's presence. And dropping the validator call produced a
  `SEGV` in `__every_id_item_has_a_driver` rather than failed assertions: that
  function documents its precondition correctly, but the cost of trusting it is
  a null dereference on an unauthenticated method, so it now checks
  `cJSON_IsNumber()` first and an entry with no usable `id_item` degrades to
  "no driver claims it". The same mutation now yields 71 clean failures instead
  of a crash. That guard is unreachable while the validator is correct and is
  **not covered by a test**, which the comment on it says plainly.

- **A `cjext` lookup defect was found and documented rather than fixed.**
  `cJSON_GetObjectItem()` passes the *stored* key's `str_key_len` to
  `case_insensitive_strcmp()` as the number of bytes it may compare
  ([`cjext.c:2271`](ezlopi-components/cjext/cjext.c#L2271)), and the two places
  that set that field disagree about whether it includes the NUL: the parser
  stores the inclusive length ([lines
  1897](ezlopi-components/cjext/cjext.c#L1897) and
  [2024](ezlopi-components/cjext/cjext.c#L2024)), while
  `add_item_to_object()` stores `strlen()` ([line
  2423](ezlopi-components/cjext/cjext.c#L2423)). Without the NUL in the budget
  the comparison cannot reject a requested name that is a *longer* extension of
  a stored key. Measured, on an object holding `config` then `configSize`:

  ```
  built  tree: cJSON_GetObjectItem(o, "configSize") -> the "config" node
  parsed tree: cJSON_GetObjectItem(o, "configSize") -> the "configSize" node
  ```

  `cJSON_GetObjectItemCaseSensitive()` is unaffected; it compares
  `max(strlen(name), str_key_len)` bytes ([line
  2263](ezlopi-components/cjext/cjext.c#L2263)).

  **It does not reach this feature's behaviour.** Requests always arrive
  parsed, so every lookup `.set` and the validator perform on input —
  `params`, `config`, `dev_detail`, `device_id`, `id_item` — runs under the
  inclusive convention and is correct; and the serialised response is correct
  JSON either way, since the ambiguity exists only inside `cjext`'s lookup. It
  surfaced because a *response* is a built tree, so two checks reading
  `configSize` out of one were getting the `config` node. The test reads
  responses with its own exact-match walk and
  `test_cjext_prefix_lookup_is_still_broken()` pins the behaviour from both
  sides, so the day `cjext` is fixed this says so instead of quietly changing
  meaning.

  Not fixed here because `str_key_len` also feeds the key printer ([line
  2126](ezlopi-components/cjext/cjext.c#L2126)): changing the convention
  changes how all 123 methods print, which belongs in its own commit with its
  own evidence rather than riding along with a new API.

- Costs **2,752 bytes** on esp32 (`0x191730` against `0x190c70`) — 1,764 in
  `.flash.text` and 984 in `.flash.rodata`, the remaining 4 being image
  padding. This is also where `ezlopi-util-devcfgval` stops being free: the
  entry above records it as byte-identical because nothing referenced it yet,
  and these two methods are what pull it in, which the baseline build confirms
  by containing no `devcfgval` or `CLOUD_devcfg` symbol at all. Both method
  names appear in the image strings and every `EZPI_CLOUD_devcfg_*` symbol is in
  the ELF — eight when this was written, ten once the entry below added a third
  method and its port function — so the methods are genuinely reachable rather
  than compiled and dropped. No new compiler warnings.

  The `cJSON_IsNumber()` guard described above costs 80 bytes in the subject's
  object file and does not change the image size at all — it lands inside
  existing section padding. Checked by compiling the file both ways rather than
  assumed from the unchanged total, because an unchanged total is exactly what
  a change that never got compiled also looks like.

  **Untested on hardware.** No board has written a configuration through these
  methods, read one back, or rebooted into one. The handler's logic is covered
  on a host; the seven port functions that actually touch the partition and the
  device table are not covered at all, and neither is the one thing that
  matters most — that a stored configuration is what the boot parser reads back
  on the next start.

- **A client can now ask what this image can be configured with.** A third
  method, `hub.ezlopi.config.drivers.list`, in the same component — and the one
  that makes the other two usable without a build tree to hand.

  `.set` refuses an `id_item` no driver claims, which is correct and completely
  unhelpful on its own: nothing told the caller which `id_items` *this* image
  was compiled with. The device table is assembled from 63 compile-time
  switches in
  [`ezlopi_core_device_macros.h`](ezlopi-core/ezlopi-core-devices/ezlopi_core_device_macros.h)
  — plain `#define`s, not Kconfig options, so they do not appear in
  `menuconfig` — and no two builds need agree. A UI with a hardcoded list would
  offer drivers that are not in the image and get refused one at a time.

  **What it reports is what `.set` accepts, by construction.** The list is read
  from the table itself, by the same walk and the same termination condition
  `driver_exists()` uses, rather than from a second list that could drift.
  `test_drivers_list_agrees_with_the_write_path()` checks both directions: every
  enumerated `id_item` is then fed to `.set` and must be accepted, and an
  `id_item` the table does not hold must be absent from the list *and* refused
  by `.set` — without that second half, a handler that just counted upwards from
  zero would pass the first.

  `count` is the length of the array that was actually sent, counted as entries
  are added rather than read from the table beforehand, so it cannot disagree
  with the array it describes. A list cut short by an allocation failure is
  reported as an error too, because a short list is indistinguishable from an
  image with fewer drivers and would make a client hide drivers the hub has.

  **Names lose the table's padding.** All 63 are padded to exactly 36
  characters for source alignment; the longest real name is 35. That padding is
  a property of the source file, not of the driver, so sending it would put a
  column of trailing spaces into any UI that displays names verbatim — 2,268
  bytes padded against 1,529 trimmed, so 739 bytes of nothing. A name too long
  to trim into the buffer is sent *unmodified* rather than truncated: a padded
  name looks untidy, a truncated one is unrecognisable and matches nothing.

  **No paging, and the reason is a measured size rather than an assumption.**
  The worst case a 63-row table can produce — every name the full 36 characters
  with nothing to trim — serialises to **3,869 bytes** against the 10,240-byte
  buffer every response shares, and
  `test_drivers_list_fits_the_transport()` asserts it stays under *half* of
  that, so the table has room to roughly double before this becomes a problem
  and the test says so while there is still room to act.

  The port function is **walked, not indexed**, which is the one thing in it
  that could have been wrong: the table's end is a sentinel row *inside* the
  array, so `list[index]` for an index past that row reads memory the table does
  not own — and "one past the end" is exactly what an enumerating caller asks
  for on its last call.

  61 new checks (the binary goes from 261 to 322) and nine further mutations,
  eight caught: sending the padding (7 failures), leaving one space (1),
  trimming one character too many (3), calling an unnamed driver `""` instead of
  null (1), truncating an over-long name (2), starting the walk at row one (26),
  reporting a `count` that is not the array's length (4), and spelling the key
  `idItem` so it cannot be copied straight into a `dev_detail` entry (14). The
  ninth — staying silent when the list was cut short — **survives, and is a gap
  rather than an equivalent mutant.** Every way into that branch is a `cjext`
  allocation failure, and `cjext` allocates through `ezlopi_malloc()`, which
  `tests/host/stubs/EZLOPI_USER_CONFIG.h` defines as plain `malloc()` for this
  binary and six others; making it fail on demand would change what those six
  are built against. The row is in
  [`mut_devcfg.py`](tests/host/mut_devcfg.py) labelled as untested rather than
  omitted, so the table does not look complete when it is not.

  Costs **624 bytes** on esp32 (`0x1919a0` against `0x191730`) — 456 in
  `.flash.text` and 168 in `.flash.rodata`, which account for the total exactly
  with nothing left over for padding. `EZPI_CLOUD_devcfg_drivers_list` and
  `EZPI_CLOUD_devcfg_port_driver_at` are in the ELF and absent from the baseline
  one, and the method name appears in the image strings of all four targets, so
  it is reachable rather than compiled and dropped. All four targets CI builds —
  esp32, esp32s2, esp32s3, esp32c3 — build with no warning in any touched file.

  **Untested on hardware,** on the same terms as the two methods above: no board
  has been asked for its driver list. The figure that would change on real
  hardware is the response size, and only if a build enabled a different set of
  switches than the 63 counted here.

- **Three of the 63 device switches did nothing, and a fourth turned off four
  drivers.** — [bug] In
  [`ezlopi_core_devices_list_modules.h`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices_list_modules.h)
  the rows for `digital-out relay`, `digital-out plug` and `digital-in generic`
  were each guarded by `DEVICE_0001_DIGITALOUT_GENERIC` — the switch belonging
  to the row above them — instead of their own
  `DEVICE_0002_DIGITALOUT_RELAY`, `DEVICE_0003_DIGITALOUT_PLUG` and
  `DEVICE_0004_DIGITALIN_GENERIC`.

  Neither consequence announced itself. Setting any of those three to `0` did
  nothing at all, and setting `DEVICE_0001_DIGITALOUT_GENERIC` to `0` removed
  four drivers instead of one. That is the same failure the method above exists
  to prevent — a build that quietly does not contain the drivers its author
  thinks it does — reached from the other end, which is why it turned up while
  that method was being written.

  Found by checking all 63, not just the suspicious ones: those three were
  **the only** switches in the table read nowhere outside their own definition
  file, and are now read. The defining `#define`s were already present and
  correct, so this is a wrong guard rather than a missing option — which is
  also why the compiler could not have warned: an undefined name in `#if`
  quietly evaluates to `0`, so a typo here removes a driver instead of failing
  the build.

  **No effect on any default image.** All 63 switches default to `1`, so this
  changes only what a build with one of those four set to `0` contains.
  Confirmed rather than argued: the baseline and worktree esp32 images differ by
  exactly the 624 bytes the new method costs, and the baseline reproduced the
  `0x191730` recorded in the entry above.

### Local operation — [feat]

- **A self-flashed device can now log in to its own local API.** This was the
  single thing standing between "local control exists in the architecture" and
  "local control works". Everything else was already there: the local websocket
  server exposes the same methods as the cloud path from the same table, scenes
  run locally, and nothing in the request path requires the cloud. That is 117
  methods in a default esp32 build — 123 in
  [`ezlopi_core_api_macros.h`](ezlopi-core/ezlopi-core-api/ezlopi_core_api_macros.h)
  less the six a default config compiles out: the three `hub.mqtt.config.*`
  (`CONFIG_EZPI_ENABLE_MQTT` unset), `cloud.firmware.info.get` and
  `hub.firmware.update.start` (`CONFIG_EZPI_ENABLE_OTA` unset), and
  `hub.protect.signal.list`. The derivation is spelled out because the bare
  number was wrong in two different ways before anyone checked it. When this
  entry was written the table held 117 entries, but a default build exposed
  **114** — `cloud.firmware.info.get`, `hub.firmware.update.start` and
  `hub.protect.signal.list` were already compiled out, so "exposes 117" counted
  rows in the table rather than methods a hub answers. Six methods have been
  added since (three `hub.mqtt.config.*`, three `hub.ezlopi.config.*`), which
  took the table to 123 and the exposed count to 116, and the third config
  method in the entry above takes it to 117 — the same number as the original
  claim, arrived at differently and by coincidence.

  But the API has exactly one credential. An unauthenticated client is allowed
  `hub.offline.login.ui` and nothing else — a real gate, at
  [`ezlopi_service_ws_server.c:292`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L292)
  — and that method compares the supplied token against the local key. The key
  had one source: the factory-info partition, written only by Ezlo cloud
  provisioning over BLE. On a board somebody flashed themselves that region has
  never been written, so it reads back as erased `0xFF`,
  `EZPI_core_factory_info_v3_get_local_key()` returns `NULL`, and the login
  fails. Every local method was implemented, reachable and working, and none of
  them could be called.

  `ezlopi-core-local-key` resolves the key from three sources in a fixed order:
  the factory partition, then an NVS entry, then — if there is neither — 32
  characters generated from `esp_fill_random()` and stored in NVS.
  **Factory-first is the compatibility guarantee**: a cloud-provisioned device
  uses exactly the key it used before, whatever is in NVS, so nothing about the
  Ezlo path changes. The two storage locations are separate, so a device that
  is later claimed through the app returns to the cloud key with no migration
  step.

  `esp_fill_random()` and not `rand()`. The repository's other random source,
  `ezlopi_util_uuid_generate_random()`, was `srand(time(NULL))` plus `rand()`,
  whose seed is guessable to within the second the device first booted — a key
  from that source would have carried roughly 20 bits of real entropy rather
  than 160. That function has since been changed to use `esp_fill_random()`
  too, so the contrast this paragraph drew no longer exists in the tree; it is
  left standing because it is the reason this function was written the way it
  was.

- **The generated key is printed once at startup**, behind
  `CONFIG_EZPI_LOCAL_KEY_ANNOUNCE` (`default y`). A generated credential nobody
  can read is not a feature, so this is a deliberate exposure replacing an
  accidental one, and it differs from the `TRACE_D` it replaces in three ways:
  only a *locally generated* key is ever printed (a cloud-provisioned key never
  is, since the Ezlo app already holds it and printing it would be a leak with
  no benefit); it happens once at boot rather than on every login attempt, so
  it is not in the log that gets pasted into a bug report; and it can be
  switched off, which is what anyone shipping a product on this firmware should
  do, installing their own key with `EZPI_core_local_key_set()`.

  It uses `printf` rather than `TRACE_*` on purpose. Every `TRACE_*` macro
  expands to nothing when `CONFIG_EZPI_UTIL_TRACE_EN` is off, and `TRACE_*`
  output is also subject to the runtime-settable, NVS-persisted
  `serial_log_severity` — either of which would leave a user holding a device
  with an unknowable key. With the option off, the firmware still reports that
  a key *exists* and withholds the value, so a missing banner is not mistaken
  for a missing key.

  Anyone with the serial port can read the key while this is on. On this
  hardware that threshold is already total, since serial access means the
  ability to reflash the board, so it is not a new exposure — but it is a real
  one, and the Kconfig entry says so.

- **`ezlopi-util-localkey`** holds the comparison, validation and generation,
  and has no `REQUIRES` at all so that `tests/host` can drive every case on a
  desktop. The entropy is passed in as an argument rather than read from
  `esp_fill_random()` inside, for the same reason. 58 host checks; see the
  Testing section for what they establish and how they were shown to fail.

  `EZPI_util_localkey_is_valid()` is deliberately a length-and-printability
  rule and **not** an alphabet test. A cloud-provisioned key is 64 mixed-case
  alphanumerics and contains the four characters generation omits (`l`, `o`,
  `0`, `1`); requiring the generation alphabet would reject every provisioned
  device. There is a host check asserting exactly that, so the next person to
  tidy `is_valid()` into an alphabet test gets a red suite instead of a field
  failure.

- **Not validated on hardware.** None of this has run on a board: not the
  first-boot generation, not the NVS persistence, not the announcement, not a
  login against a generated key. Three builds were verified — defaults, with
  user-id verification on, and with the announcement off — and all three
  compile and link with the new symbols present in the ELF. The absence of the
  credential format strings was checked against the built `.bin` rather than
  the source. That is the limit of what has been established.

### Provisioning — [feat]

- **Improv Wi-Fi over BLE, as an opt-in sixth GATT service.** Guarded by
  `CONFIG_EZPI_BLE_ENABLE_IMPROV`, which is `default n`: with it off, not one
  line of the new code is compiled and the advertisement is byte-identical to
  upstream's. Verified by building both ways — the feature costs 2,576 bytes
  when on, and `origin/master`'s advertisement struct was diffed field by field
  against the one this tree emits with the feature off.

  [Improv](https://www.improv-wifi.com/) is the protocol Home Assistant and ESP
  Web Tools speak, so a board can be put on Wi-Fi by a generic client instead of
  only by the Ezlo mobile app. The Ezlo path is untouched: the service is added
  *after* the existing five, so every handle the app relies on keeps the value
  it has always had.

  Provisioning is refused unless the hub is unclaimed, re-checked at the moment
  credentials arrive rather than only when the client connects. RPC frames are
  length- and checksum-validated before any byte of them is indexed, and the
  passphrase is logged by length only.

  Only the mandatory `0x01` (send Wi-Fi) command is implemented, and the
  capability bits advertise exactly that, so a conforming client never offers
  the optional commands and `UNKNOWN_RPC` is the correct answer to them.

- **The advertisement budget is now a build error rather than a silent
  truncation.** Improv's advertisement has to carry a 128-bit service UUID *and*
  6 bytes of service data, which together with the flags element comes to
  exactly 31 bytes — the whole legacy payload, with nothing spare.

  Nothing in the stack reports an overflow. `esp_ble_gap_config_adv_data()`
  returns `ESP_OK` either way, and `btm_ble_build_adv_data()` guards each element
  with a length test and simply stops emitting when it runs out of room. Service
  data is emitted last, so it is the first thing lost. The symptom on a device is
  "Improv clients do not offer provisioning", with nothing in the log.

  So there is a `_Static_assert` on the payload size, and a runtime check for
  the fields whose sizes are not constant expressions (device name, TX power,
  the interval pair, manufacturer data). The assert was mutation-tested: growing
  the service data by one byte makes the build fail with the intended message.
  Enabling the interval pair *and* Improv would need 37 bytes, which is why the
  Improv advertisement serves the device name from the scan response instead.

  Neither check can run unless something compiles this configuration, hence the
  `firmware-improv` CI job below.

### Home Assistant — [feat]

- **Devices wired to an EzloPi now appear in Home Assistant with no YAML, over
  MQTT Discovery.** Off by default: `CONFIG_EZPI_ENABLE_MQTT` plus a broker URI
  turns it on.

  This is an addition, not a replacement. **No existing line of core or service
  code changed to make it work.** State updates are observed through
  `EZPI_core_broadcast_method_add()`, the same registry the two websocket
  transports already use, and writes arrive through the same
  `EZPI_items_set_value_v3()` the cloud calls — so a driver cannot tell an MQTT
  write from a cloud one.

  Everything on the broadcast path uses `esp_mqtt_client_enqueue()` rather than
  publish. A registered broadcast method runs on the loop-service task while
  that task holds the single global serialization buffer, so anything slow there
  delays the Ezlo cloud broadcast queued behind it. Enqueue hands off to the
  MQTT task and returns; publish can block on the client.

  The client starts *before* `EZPI_core_wait_for_wifi_to_connect()`, on purpose.
  That function waits on an event bit only the Wi-Fi driver ever sets, so on an
  Ethernet-only board it never returns. esp-mqtt's own reconnect loop does not
  care which interface came up.

- **With both websocket transports off, every item update used to be dropped
  silently.** `EZPI_service_broadcast_init()` installs the queue that every
  broadcast method is fed from, but the `#if` around it named only the two
  websocket transports. So a local-only hub left the queue function `NULL`: it
  would have published its discovery payloads, then gone permanently quiet with
  nothing in the log to say why. `CONFIG_EZPI_ENABLE_MQTT` is now a term in that
  condition. Removing it again was verified to make the symbol vanish from
  `app_main`, so the CI job covering this can actually fail.

- **The MQTT topic layer is built and parsed separately from the service**,
  because it is pure string work and because every way it can go wrong is
  invisible on the device. A discovery config published to a slightly wrong
  topic is not a degraded version of the right one — it lands where nothing is
  subscribed and the entity simply never appears. So truncation is treated as
  failure and the buffer cleared, which means a caller that ignores the return
  value publishes nothing rather than publishing to a clipped topic.

  The object id is the reduced device name followed by the device and item ids
  as eight hex digits each. Fixed width is what lets the command topic be split
  by position instead of by searching for separators, which a device name
  containing underscores would defeat — user-supplied names routinely have them.
  The ids are budgeted *before* the name, so a buffer too small to hold
  everything loses name bytes and never id bytes: a clipped item id would
  address a *different* item, which presents as a device ignoring commands
  rather than as a bug.

  Parsing is deliberately strict, because a command topic arrives from the
  network and names an item about to be written. Anything that is not exactly
  `_<8 hex>_<8 hex>/set` is refused rather than guessed at, and a refusal zeroes
  the caller's ids instead of leaving the previous ones in place. The length is
  honoured rather than a NUL, because esp-mqtt hands over a pointer and a length
  into a larger buffer.

  Covered by 183 host checks under ASan/UBSan, up from 130: every buffer size
  from 1 to 64 through the object id builder with each result fed back through
  the real parser, and a round trip of 12 awkward real-world device names
  (empty, punctuation-only, non-ASCII, containing slashes, longer than the
  buffer). Mutation-tested 12 of 12 caught — which found a real gap, in that
  every wrong-leaf case also happened to be the wrong *length*, so the
  separator check refused them before the leaf check ever ran. Two four-byte
  wrong leaves (`/get` and `_set`) now isolate it.

- **The broker can now be configured on a running hub, with no rebuild**, through
  three new API methods: `hub.mqtt.config.get`, `.set` and `.reset`. The five
  `CONFIG_EZPI_MQTT_*` strings became fallback defaults; the live values live in
  NVS.

  This closes a gap that made the NVS layer decorative. The settings store had
  a read path and no writer: a symbol dump showed all five setters unreferenced
  and discarded by the linker, so the firmware could *read* a broker URI out of
  NVS that there was no way to put there. The store was shipped a release
  earlier than the thing that uses it.

  `set` takes any subset of the five fields and applies **all of it or none of
  it**. Every supplied field is validated before anything is written, so a typo
  in the URI cannot also wipe the username. This is deliberately not described
  as a transaction: a power cut between two NVS writes still leaves a partial
  apply. What the two passes buy is that a *rejected* request changes nothing,
  which is the case that happens to people; the power-cut case is survivable
  for a different reason, namely that every field is revalidated when read and
  a half-written one falls back to the compiled-in default.

  Over-long values are refused rather than truncated, which is why the tree's
  own `EZPI_core_cjson_get_value_string_by_copy()` is not used here. It
  truncates with `snprintf`, and a 200-character URI cut to 128 is still a
  *valid* URI — it would pass validation and be stored as a plausible address
  nobody typed. It also cannot distinguish an absent field from `""`, because
  it leaves the caller's buffer untouched in both cases, and `""` is meaningful
  for three of these five fields: an empty URI is how MQTT is switched off again
  without reflashing.

  `get` never returns the password, only a `passwordSet` flag — enough for a
  settings screen to tell "leave unchanged" apart from "clear". Authentication
  is the transport's: the local websocket server refuses every method but
  `hub.offline.login.ui` until a user is logged in, and these inherit that
  rather than adding a second check that could disagree with the real one.
  Everything reports `rebootRequired: true`, because the client is created once
  at startup and saying otherwise would imply the broker had already moved.

  With MQTT off the image is byte-for-byte the same size as before the change
  (0x1903f0), so the four committed default configurations pay nothing for any
  of this.

- **A password containing a space was refused, and nothing said it would be.**
  `EZPI_util_mqttcfg_cred_is_valid()` was sharing a helper with the URI path,
  which rejects spaces for good reasons of its own; the credential header has
  always said credentials have "no alphabet restriction" and are refused only
  for length and control characters. So the documentation was right and the
  code was wrong. Credentials now reject `0x00`-`0x1F` and `0x7F` only, and
  accept spaces and non-ASCII bytes — MQTT usernames are UTF-8 and passwords
  are binary, and the byte-counted topic buffers that justify narrowing the
  prefixes have nothing to do with a credential. The misleading helper is
  renamed after what it actually enforces.

  Found by the new host test using `"p@ss w0rd"` as its sample password, chosen
  because a space in a password is exactly what an over-eager validator
  refuses.

- **`cJSON`'s `str_value_len` is not the length of the string, and the two
  parsers in `cjext` disagree about it.** `cJSON_Parse` stores an allocation
  size — for an unescaped string, `strlen + 1`; `cJSON_ParseWithRef` stores the
  exact length; `cJSON_CreateString` stores `strlen`. Nothing in the firmware
  had noticed, because it is only ever used where an over-estimate is harmless:
  as a `"%.*s"` precision, as a length handed to `strncmp`, or as a buffer size,
  all of which stop at the first NUL.

  Code that does arithmetic on it does notice. The new config handler used it
  to bounds-check incoming fields, which rejected a value of exactly the
  maximum length as one byte too long and would have written its terminator one
  byte past a buffer sized for the maximum. The handler now determines the
  length the way `"%.*s"` does — up to `str_value_len` bytes, stopping at a NUL
  — which is correct for all three construction paths.

  The behaviour is now pinned by ten checks in `tests/host/test_cjext.c` and
  documented at `ezlopi_core_api.c`, next to the commented-out
  `cJSON_ParseWithRefWithLength()` line that someone will eventually be tempted
  to enable: a referenced string is terminated by its closing quote rather than
  a NUL, and `EZPI_STRNCMP_IF_EQUAL` is `strncmp` over the *longer* of the two
  lengths, so switching parsers would make method dispatch stop matching
  anything and read past each value into the payload.

- **Two things that read as accidental are correct, and are now commented as
  such.** A payload larger than the client's buffer arrives as several
  `MQTT_EVENT_DATA` events, and only the first carries a topic — continuation
  fragments have `topic_len == 0`. Those were already dropped, but by a guard
  that looks like an ordinary argument check, so "fixing" it to accept
  `topic_len == 0` would parse a fragment as a topic.

  The device-list walk in the announce path takes no lock. That matches every
  other caller of `EZPI_core_device_get_head()` — `ezlopi-core-devices` exposes
  no lock to take — and adding one here alone would not address the dozen
  unsynchronised walks in `ezlopi-cloud`. It belongs in the core, for all of
  them at once. See [Known gaps](#known-gaps).

### UUID utility — [bug]

**None of this is reachable.** Nothing in the repository calls any function in
`ezlopi-util-uuid` — not `parse`, not `unparse`, not `generate_random`, not
`copy`, not the `uuid_generate` macro. That is not just a `grep` result: the
component's object file defines all four as global text symbols, and the linker
discards every one of them, so `nm` on `Ezlo_Pi_v4x.elf` matches
`ezlopi_util_uuid` zero times and the image is byte-identical before and after
this change at `0x18fca0`. The code is fixed because it is a published utility
header in an open-source project and the next person to call it would have
inherited all of the below — not because anything was failing on a device. Read
the rest of this entry with that in front of it.

- **`ezlopi_util_uuid_parse()` read past the end of its input.** The loop's only
  bounds were `j < 16` and `i < 36`, and the body read `in[i + 1]`. Nothing
  consulted the terminator, so any string shorter than 32 hex digits was walked
  past its NUL. Parsing a 9-byte heap buffer holding `"550e8400"` gives
  AddressSanitizer `heap-buffer-overflow READ of size 1 ... 0 bytes after
  9-byte region`; an 8-character string *literal* gives
  `global-buffer-overflow ... 0 bytes after global ... of size 9`, so it is
  visible on either storage class. This is the one that would have mattered: a
  UUID arriving from a cloud payload or a scene definition is attacker-
  influenced in both content and length.

- **It did not round-trip through its own `unparse()`.** `parse()` advanced `i`
  by two on the iteration that saw a `'-'`. A hyphen is one character, so that
  consumed the hyphen *and* the first digit after it, and every byte from there
  on was read one nibble out of step. Feeding `unparse()`'s own output
  `550e8400-e29b-41d4-a716-446655440000` straight back in returned
  `550e840029ff41d471ff446655440000` — wrong from byte `[4]`, which is exactly
  where the first hyphen sits. The two halves of the component disagreed with
  each other. `unparse()` was and is correct; only `parse()` was changed.

- **Uppercase hex did not parse.** `__hex2dec()` compared against a
  lowercase-only table with `==`, so `'A'` through `'F'` returned `-1`. RFC 4122
  §3 requires a reader to accept either case. Confirmed to be an independent
  defect rather than a knock-on of the hyphen bug by parsing uppercase input
  with the hyphens already removed, which was still wrong — worth stating
  because the obvious assumption was that one bug explained both symptoms.

- **Undefined behaviour on malformed input.** The `-1` from `__hex2dec()` went
  straight into `(x << 4) | y` unchecked. Left-shifting a negative value is
  undefined, and UndefinedBehaviorSanitizer reports `left shift of negative
  value -1`; the result landed as `0xff`, so garbage input produced
  plausible-looking bytes rather than any kind of error.

  Malformed, short or `NULL` input now yields the nil UUID. **The signature was
  deliberately left as `void`**, so a caller still cannot distinguish a rejected
  string from a successful parse of
  `00000000-0000-0000-0000-000000000000`. Returning a `bool` would have been
  source-compatible — every call site would be a statement ignoring it — but
  there are no call sites to be compatible with, and an API change did not
  belong in the same commit as the parsing fix. The limitation is documented in
  the header instead of being left for someone to discover.

- **Also in this component, and also not bugs:** the `hex` lookup table was
  `const unsigned char hex[16]` initialised from a 16-character literal, which
  legally drops the NUL (C99 §6.7.8p14) and would trip gcc 15's
  `-Wunterminated-string-initialization`; it is `static const unsigned char
  hex[]` now, static because nothing outside the file referred to it.
  `ezlopi_util_uuid_copy()` compared an `int` against `sizeof`.
  `ezlopi_util_uuid_unparse()` writes `out[36]` unconditionally and never
  documented that it needs 37 bytes — there is now an `EZLOPI_UUID_STR_SIZE`
  for that.

- **`ezlopi_util_uuid_generate_random()` was not random.** It called
  `srand(time(NULL))` once and then `rand()` four times. Both halves were wrong
  on this hardware. `time(NULL)` before SNTP has synchronised is a small count
  from the epoch, so the seed is not merely guessable to within the second a
  device booted — it is frequently the *same* small value on every device of a
  batch. And `rand()` is a deterministic PRNG, so recovering that one seed
  yields every UUID the device will ever emit. A version-4 UUID is specified to
  carry 122 random bits; this produced on the order of twenty an attacker could
  not already predict.

  Now one `esp_fill_random()`, the hardware RNG, which
  `ezlopi_core_local_key.c` already uses for the same reason. Filling the
  buffer in one call also removes the old loop's second, smaller defect:
  `rand()` returns at most `RAND_MAX`, which is `0x7fffffff` in newlib, so the
  top bit of each four-byte group was always zero and four of the 128 bits were
  not random at all.

  `RAND_LENGTH`, which described how many bytes came out of each `rand()` call,
  is gone from the public header along with the loop it belonged to.

  What this does **not** promise: `esp_random()` is a true RNG when Wi-Fi or
  Bluetooth is up, and otherwise carries the prerequisites in the "Random
  Number Generation" section of the ESP-IDF guide. Neither this function nor
  its caller checks that the radio is on.

  The earlier version of this entry said the fix was "a seam, not a one-liner"
  because an ESP-IDF dependency would cost this component its host tests. That
  was right, and the seam is `tests/host/stubs-uuid/esp_random.h` — a verbatim
  redeclaration of the real header's two functions, with
  `esp_fill_random()` implemented in `stub_random.c` as a counter. An
  `#ifdef ESP_PLATFORM` inside the component would have been smaller and would
  have meant the firmware and the test compiled two different functions, which
  is the one thing that would have made the test stop being evidence.

  The counter is also what makes the new assertion possible. Randomness is not
  checkable in a unit test, but *provenance* is: with a predictable source, the
  test asserts that all sixteen bytes of the UUID came from `esp_fill_random()`
  in order, and that only the two nibbles RFC 4122 §4.4 requires were
  overwritten afterwards. That matters because the old loop's byte count
  depended on a macro — had `RAND_LENGTH` been 3 or 5, four bytes or one byte
  of every UUID would have been uninitialised stack, and every assertion the
  suite had at the time would still have passed.

  | mutation | caught by |
  |---|---|
  | back to `srand()` + `rand()` | "the RNG is consulted exactly once", and four more |
  | fill 12 of the 16 bytes | "exactly 16 bytes are drawn from the RNG" |
  | `out[6] = 0x40` instead of `(out[6] & 0x0f) \| 0x40` | "byte 6 keeps its random low nibble" — while "version nibble is 4", the assertion that existed before, still passes |

  The third row is the one that justifies the test: it is a wrong version the
  suite could not previously see.

### Action name table — [bug]

**Also not reachable, and for the same mechanical reason.** All three call sites
of `EZPI_core_actions_to_string()` in the tree are commented out. The object
file defines both `EZPI_core_actions_to_string` and `ezlopi_action_names`, and
neither name appears among the 13802 symbols of the linked `Ezlo_Pi_v4x.elf`;
the image stays at `0x18fca0` and the corrected string literal is absent from
`Ezlo_Pi_v4x.bin` entirely. The `e_ezlopi_actions_t` *enum*, in contrast, is
load-bearing — 63 driver files in `ezlopi-sensors-devices` reference one of its
enumerators — so the live half of this component was never affected.

- **Two distinct actions stringified to one name.**
  `ezlopi_core_actions_list.h` read
  `EZLOPI_ACTION(HUB_UPDATE, "EZLOPI_ACTION_HUB_GET_ITEM")`, so
  `EZPI_core_actions_to_string(EZLOPI_ACTION_HUB_UPDATE)` returned
  `"EZLOPI_ACTION_HUB_GET_ITEM"`. Every other one of the sixteen entries spells
  its own identifier. In a trace that is a wrong answer wearing the shape of a
  right one — the kind of defect that survives code review precisely because
  the line looks like its neighbours.

  Fixed in the table. The test that covers it does **not** read the table's
  string column: it rebuilds each expected name from the macro's first argument
  with the preprocessor's stringify operator, so the table is checked against
  the token it is named after rather than against itself. A test written the
  obvious way would have agreed with the bug. Confirmed by running the new test
  against the unfixed header first, where it fails two checks — the per-entry
  comparison and an independent all-strings-distinct check — and passes the
  other 142.

- **The range guard has an upper bound and no lower bound, and that is left
  alone.** `if (EZLOPI_ACTION_MAX > action)` would index `ezlopi_action_names[]`
  out of bounds for any negative argument. It is safe today, but not because
  the function checks: every enumerator is non-negative, so the compiler gives
  the enum an unsigned type and `-1` converts to a value far above `MAX`. That
  is a property of the type rather than of the code, so instead of adding a
  redundant branch the test asserts the property at compile time and fails the
  build if a future list makes the enum signed. gcc proved the point while the
  test was being written: a first draft wrote the assertion as a runtime
  `(e_ezlopi_actions_t)-1 < 0` and `-Wtype-limits` rejected it as always false,
  which under this suite's `-Werror` is a build failure.

- **How it was found, and why there is no third one of these.** The UUID
  defects above came from noticing a component that needed no test seam and had
  no tests, so that observation was generalised: ask `gcc -fsyntax-only` which
  first-party `.c` files compile with nothing but their own directory on the
  include path. 32 of 246 do, and 27 of those had no tests. That list is much
  less interesting than it sounds. Four files are a licence header and nothing
  else, three are a single `#include`, three more — `ezlopi_cloud_data.c`,
  `ezlopi_core_event_queue.c` and `ezlopi_core_ethernet.c` — are wrapped in
  `#if 0` from first line to last, and fifteen are tables of string literals.
  `ezlopi-core-actions` is the only one of the 27 containing executable logic.
  "Compiles with no stubs" selects far more strongly for emptiness than for
  portability, so this seam is now exhausted; the next lever is a stub
  `sdkconfig.h`, which is the first missing header for 69 of the remaining
  files. Recorded in [`CONTRIBUTING.md`](CONTRIBUTING.md) so the sweep is not
  repeated from scratch.

### Dead null checks — [bug]

- **31 checks that compared an array's own address against NULL.** Across seven
  files the code tested things like `if (handler->raw_data_reg)`,
  `curr_field->name ? strlen(curr_field->name) + 1 : 0` and
  `NULL != user_notification_node->user_id`, where the operand is a `char[]` or
  `uint8_t[]` *member* of the struct, not a pointer stored in it. Its address is
  the struct's address plus a fixed offset, so the test can only be false for
  one absurd pointer value, and the error branch behind it was unreachable.
  These are the reason the project cannot compile under GCC 14, which promotes
  `-Waddress` to an error.

  Removed rather than rewritten. The tempting "fix" is to replace the dead test
  with the one the author presumably meant (`name[0] != '\0'`), but that is a
  behaviour change — fields with an empty name would start taking a different
  path — and there is no hub to test the new behaviour against. Making the code
  say what it already does is the bug fix; what it *should* do is a separate
  question, and each deferred one is recorded in a comment at the site.

  Two of these deserve singling out. `gxhtc3_compute_values()` has therefore
  always returned `true` unconditionally, so its one caller learns nothing by
  testing the result; a real check needs a validity flag the struct does not
  carry. And in the scene-status broadcast, an empty `user_id` still emits an
  empty string into the `notifications` array exactly as before — whether it
  should be skipped instead depends on what the hub expects in that field.

  Measured on the 4.4.6 toolchain this project ships with, by building the tree
  with and without the change and comparing images. The result is
  behaviour-identical but *not* byte-identical, and the difference is worth
  recording because it contradicts the obvious assumption that dead code costs
  nothing:

  - 30 of the 31 sites were genuinely free — GCC had already folded them.
    Proven by construction: a build with all 31 removals except the `gxhtc3` one
    is byte-identical to the baseline.
  - The 31st was not. For `gxhtc3_compute_values` GCC emitted a real runtime
    test (`addi.n a9, a2, 6` / `movi.n a8, 0` / `bne`), so the function shrank
    by 14 bytes. The branch was still dead, and instructively so: it fired only
    when `handler == 0xFFFFFFFA` and *not* when `handler` was NULL, since
    `0 + 6` is non-zero. It never guarded the value worth guarding.
  - That 14 bytes moved `.flash.text` by 12, which shifted 2,143 pointer words
    across `.flash.rodata`, `.dram0.data` and `.iram0.text` by exactly −12.
    Of 12,581 symbols, exactly one changed size and none were added or removed.

### Build and configuration — [infra]

- **The enum-to-`int *` assumption under `CJSON_GET_VALUE_INT` is now
  compile-time enforced.** The build emits 87 `-Wincompatible-pointer-types`
  warnings from
  [`ezlopi_core_cjson_macros.h`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_cjson_macros.h),
  all the same shape: a device driver passes an enum-typed struct field to
  `CJSON_GET_VALUE_INT`, which forwards its address as an `int *`, so
  `EZPI_core_cjson_get_value_int()` writes an `int` through a pointer to an
  enum.

  **This is not a live defect**, and it is worth being precise about why
  rather than filing it as a scare. On this toolchain with this project's
  flags an enum is exactly int-sized and int-aligned, so the write is 4 bytes
  into a 4-byte field. That was checked by asking the compiler, and the same
  probe fails under `-fshort-enums`, which is what establishes it was
  measuring something; there is no `-fshort-enums` anywhere in the generated
  `compile_commands.json`.

  The safety is an ABI accident, though, not a property of the code. Narrow
  the enums and all 87 sites begin writing 4 bytes into fields as small as
  one, corrupting adjacent struct members — silently, in drivers, at
  configuration-parse time.

  The 87 call sites are **deliberately not changed**: editing that many lines
  across the device drivers to fix no present defect is exactly the churn most
  likely to disturb working Ezlo behaviour. Instead the assumption they rest
  on is stated as two `_Static_assert`s next to the macro, so if it ever stops
  holding the build stops with a message naming the problem, instead of the
  firmware misbehaving in the field. Verified by appending `-fshort-enums` to
  the real project build and confirming the assert fires with that message,
  then removing it; esp32 and esp32c3 (RISC-V, a different ABI) both build
  clean with binary sizes unchanged, since the asserts cost nothing.

- **`REQUIRES tinyusb` was unconditional while every use of it was not.**
  `ezlopi_service_uart.c` wraps all of its USB-CDC code — includes and calls
  alike — in `#if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32C3)`,
  because neither chip has a USB-OTG peripheral. The component's `REQUIRES`
  asked for `tinyusb` regardless of target.

  On IDF 4.4.6 this is harmless by accident, which is why it survived: IDF's
  `tinyusb` component is `visible if USB_OTG_SUPPORTED` and registers no sources
  on esp32, so the dependency resolves to an empty stub. It stops being harmless
  on IDF 5.x, where `tinyusb` is not an IDF component at all and configure fails
  with `Failed to resolve component 'tinyusb'`. The registry cannot stand in
  either: `espressif/esp_tinyusb` depends on `espressif/tinyusb`, which supports
  only chips that have the peripheral — so on esp32 this is not a component to
  rename but a dependency that should never have been requested. The `REQUIRES`
  now mirrors the `#if` in the source.

- **A hub could not be built without the websocket client at all.**
  `EZPI_service_webprov_is_connected()` is declared inside an
  `#if CONFIG_EZPI_WEBSOCKET_CLIENT` block in `ezlopi_service_webprov.h`, but
  `ezlopi_service_uart.c` called it unguarded. Turning the websocket client off
  therefore failed the build outright on
  `-Werror=implicit-function-declaration`, which made a local-only hub
  impossible to compile — the exact configuration the MQTT work needs. No build
  in CI had the option off, so nothing caught it.

  With the websocket client on, the generated code is unchanged. With it off
  there is no cloud client, so there is no cloud connection to report and the
  field is reported as false.

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

- **New host test suite, [`tests/host/`](tests/host/)**: 4540 assertions across
  seventeen binaries, no framework, no hardware. `make -C tests/host` builds and runs
  the lot from scratch in about five seconds; running the already-built binaries
  takes a tenth of a second, so there is no reason not to run it on every edit. Built under
  AddressSanitizer and UndefinedBehaviorSanitizer, and under `-Wall -Wextra
  -Werror` except for two scoped exemptions the `Makefile` documents at the
  point of use (vendored `cjext.c`, and one stale `#warning` in `ze08_ch2o.c`).
  The sanitizers are the point: the defects being regression-tested are buffer
  overruns, and an overrun that happens to land in unused stack passes a plain
  assertion. Buffer tests also bracket the destination with a canary to catch a
  write that lands just past the end.

- **The suite was verified to have teeth.** Six mutations were introduced into
  the sanitize helpers — one per fixed defect — and each was confirmed to make
  the suite fail, by assertion or by ASan. A green suite that would also pass
  against the original buggy code is worse than no suite, because it is
  evidence of nothing.

- **New CI check for the BLE chunk-reassembly bug class,
  [`tests/ci/check_ble_chunk_clamp.py`](tests/ci/check_ble_chunk_clamp.py).**
  This one exists because of how its bug was found. There were two copies of
  the chunk assembler, one pasted from the other; the clamp was added to one
  and the other kept an unauthenticated remote heap overflow across a release.
  Fixing the second copy does nothing about a third, and the two files are far
  enough apart that no reviewer would connect them. So the rule is structural
  rather than per-site: in the BLE sources, a function that reads a chunk
  length out of a JSON envelope and then calls `memcpy` must also call
  `EZPI_util_clamp_copy_len`, and every parsed cJSON tree must be freed through
  the variable it was parsed into.

  It earned its place immediately — the lifetime rule found a third leak, in
  `ezpi_service_ble_serial_config_write()`, in a file that had just been read
  through by hand looking for exactly that.

  Like the other two `tests/ci/` checks it self-tests on every run, and for the
  same reason: the first draft of the lifetime rule asked only whether a
  function called `cJSON_Delete` anywhere, which the provisioning write handler
  satisfied while leaking — it parsed two trees and freed one. That shape is
  now a fixture (`SELF_TEST['one_of_two']`) asserted to report exactly one
  leak, alongside the unclamped-`memcpy` shape. Verified end to end by running
  the finished check against the pre-fix revision of both files, where it
  reports all four defects at the right functions, and against the current
  tree, where it reports none.

- **New CI check that `TRACE_*` stays compiler-checked,
  [`tests/ci/check_trace_format_attr.py`](tests/ci/check_trace_format_attr.py).**
  The underlying fix is a one-line `__attribute__((format(printf, …)))` on
  `trace_color_print()` and `trace_otel()`, and since `-Werror=format` was
  already enabled, the *build* is now the check — a `TRACE_*` whose arguments
  disagree with its format string cannot compile. Verified in both directions
  by reintroducing the `cJSON *`-to-`%s` call and confirming the build fails
  with the expected diagnostic, then removing it and confirming four clean
  target builds with zero format diagnostics.

  So this script guards something narrower and easier to lose: the attribute
  itself. Delete it in a refactor and the build goes green while every format
  string in the tree silently stops being checked — the same failure shape as
  the pasted BLE assembler above, where the fix was right and the thing that
  kept it right was missing.

  Its own first draft was wrong in an instructive way. The header carries a
  commented-out historical prototype of `trace_color_print()` above the live
  declaration, and the check matched the comment — reporting the attribute
  absent while it sat two lines below. The self-test passed throughout, because
  the fixtures had no comments in them. Comment stripping was added and so was
  `SELF_TEST['commented_decoy']`, which reproduces the real header's shape. It
  is also run against the pre-fix header from git, where it reports both
  declarations unguarded.

- **New CI check that factory-info field copies stay bounded by their source,
  [`tests/ci/check_finfo_bounded_store.py`](tests/ci/check_finfo_bounded_store.py).**
  Guards the `set_basic()` over-read described under **Memory safety** above:
  no `memcpy` in `ezlopi_core_factory_info.c` may take its source from an
  `s_basic_factory_info_t` member and its count from an `EZLOPI_FINFO_LEN_*`
  constant. Address-of sources are exempt, since `&cfg->id` really is as long
  as its field.

  The reason this needs a script rather than a compiler flag is that there is
  nothing for a compiler to object to. `set_basic()` received a pointer and a
  byte count; that the count came from the destination's size is a fact about
  the program, not about the statement. The four images build clean with the
  defect present, and the only visible symptom is extra bytes in a flash
  region nobody diffs.

  Verified against the real pre-fix file recovered from git, where it flags
  all twelve string copies and correctly exempts the two binary ones — not
  merely against its own fixtures. That distinction is the lesson from
  `check_trace_format_attr.py` below, whose self-test passed while its scan
  was wrong, and it is why `SELF_TEST['commented_decoy']` exists here too: the
  helper's own documentation quotes the old pattern in prose directly above
  the code, and a scanner that reads comments would flag the explanation of
  the fix as the bug.

  The defect itself was separately reproduced under `-fsanitize=address`
  rather than argued from the source, and the claim that the fix preserves
  stored bytes was tested across eight cases with a harness that was mutated
  to prove it can report a mismatch.

- **New CI check that the embedded CA bundle holds the roots the docs promise,
  [`tests/ci/check_ota_ca_bundle_roots.py`](tests/ci/check_ota_ca_bundle_roots.py).**
  Runs on all four targets in the `firmware` matrix. Full reasoning under
  **OTA — [feat]** above; the short version is that it polices a documentation
  claim about a file ESP-IDF generates, so an IDF pin bump can falsify it with
  nothing in any build going red. It fails in both directions — a promised root
  going missing, and a root the docs warn is absent appearing — and was
  verified by mutating real bundles, not just its own ten fixtures.

- **`ezlopi-util-uuid` joined the suite** — `tests/host/test_uuid.c`, 437
  checks. It tied MB1013 for the shortest build rule in the `Makefile` — one
  `-I`, its own directory, no stubs — but it reached that point differently,
  and the difference was the point: MB1013 needed a parser split, and this
  needed nothing. The whole translation unit compiled unmodified against the
  system compiler. Every other entry in this suite needed either a split or a
  shim; this one needed neither and had simply never been tested. (It takes one
  stub directory now — `stubs-uuid`, for `esp_random.h` — which is the price of
  the RNG fix above and is argued for there.) It could have
  been covered from the day it was written. `ezlopi_util_uuid_parse()` turned
  out to have four defects, described under **UUID utility — [bug]** above, one
  of which ASan reports as a buffer overflow — so the cost of leaving a dependency-free file untested was
  four real defects sitting in a published header. Checking the includes before
  reaching for the dependency argument is now the first paragraph of that
  section in `CONTRIBUTING.md`.

  **This one was mutation-tested against real history rather than an invented
  break.** The suite was compiled against the pre-fix file taken straight from
  the previous commit (`git show HEAD:...`), which fails it loudly: the
  canonical round trip fails, UBSan reports `left shift of negative value -1`,
  and ASan aborts the run on `global-buffer-overflow`. One number from that run
  is worth keeping — of the 256 swept round-trip patterns, 240 failed and **16
  passed**. A nibble-level misalignment is invisible whenever the adjacent
  nibbles repeat, so a single hard-coded UUID had roughly a one-in-sixteen
  chance of turning this bug into a green run. That is the argument for
  sweeping inputs instead of picking one, in numbers rather than in principle.

- **`ezlopi-util-devcfg` joined the suite** — `tests/host/test_devcfg.c`, 18
  checks, covering the boot-loop fix described under **Device configuration —
  [bug]** above. The interesting part is not the count but that the property is
  a *bound* rather than a case list: all eight combinations of the three
  booleans are enumerated and none may restart more than once. A case list
  would have been me re-deriving the same reasoning that produced the bug.

  It is also the second file here, after `test_localkey.c`, to keep the
  superseded behaviour alongside the new one and fail if the old rule *stops*
  misbehaving. That shape has now earned its place twice: it turns "I believe
  this was broken" into something a reader can run.

- **`ezlopi-util-wssession` joined the suite** —
  [`tests/host/test_wssession.c`](tests/host/test_wssession.c), 185 checks,
  covering the per-connection authorisation described under **Local API
  sessions — [sec]** above. This is the clearest case so far for why the
  extract-to-a-dependency-free-component rule is worth the trouble: the defects
  it covers are *invisible to a single-client test*. They need two connections
  driven independently, which is awkward on a board and trivial on a host.

  The property under test is not an example, it is a quantifier: **no
  connection inherits another's authority.** So the test enumerates every
  ordered pair from `{0, 1, 3, 4, 7, 11, 12, 29}` and asserts that authorising
  one never authorises the other, rather than picking a pair and reasoning
  about the rest. Descriptor `0` is in that set deliberately — a table relying
  on static zero-initialisation would claim fd 0 as an authorised session, and
  only a test that tries fd 0 finds that.

  Descriptor *reuse* is covered the same way, in both orderings — orderly close
  then reopen, and reopen with no close at all. That case cannot be tested on
  hardware with any confidence, because it depends on the OS handing back a
  number it just took away.

  It is the third file here, after `test_localkey.c` and `test_devcfg.c`, to
  keep the superseded behaviour alongside the new one and fail if the old rule
  stops misbehaving. Here that shape pays for itself more directly than in
  either earlier case: the old model was a single global `bool`, which is ten
  lines to reimplement, and the *same* two scenario functions run against both
  models through a function-pointer struct. So the claim "a stranger was
  admitted" is not an assertion in prose — it is a test that passes against the
  old code and fails against the new, in the same file, on every run.

  Mutation-tested 4 of 4 caught, against an unmutated control that reports 0:

  | mutation | failures |
  |---|---|
  | `is_authorised()` returns true if *any* session is authorised — the original defect, reintroduced | 69 |
  | `open()` does not clear `authorised` when it reuses an entry | 1 |
  | `authorise()` marks every live session instead of the one named | 56 |
  | a full table returns success instead of failing closed | 1 |

  The two mutations caught by a single check each are the interesting ones. A
  count of 1 is not a weak result here — both are narrow behaviours that
  exactly one property can distinguish, and the point of recording the number
  is that it is not 0.

- **`ezlopi-core-actions` joined the suite** — `tests/host/test_actions.c`, 144
  checks, and the last file the zero-stub sweep had to offer. It found the
  `HUB_UPDATE` string defect described under **Action name table — [bug]**
  above.

  The design decision worth recording is what the test compares against. The
  obvious implementation walks `ezlopi_action_names[]` and checks each entry
  against the string in `ezlopi_core_actions_list.h` — which is the table
  checking itself, and would have passed happily on the bug. Instead the test
  includes the same X-macro list and rebuilds every expected name from the
  macro's *token* with `"EZLOPI_ACTION_" #action`, so the table is measured
  against the identifier it claims to name. Verified by building it against
  the unfixed header, where exactly two checks fail and 142 pass.

  One check in it is deliberately a compile-time assertion rather than a
  runtime one, because the thing being asserted is a property of a type: that
  `e_ezlopi_actions_t` is unsigned, which is the only reason the function's
  single-sided range guard is not an out-of-bounds read. A runtime version
  cannot fail — and in fact cannot compile, since `-Wtype-limits` plus
  `-Werror` rejects an always-false unsigned `< 0` comparison.

- **`ezlopi-util-localkey` is the first component in the suite written in order
  to be testable, rather than found to be testable** —
  `tests/host/test_localkey.c`, 58 checks. The local key is the entire access
  control of the offline API, and the check it performs lived inline in
  `ezlopi_core_offline_login.c` — a file that cannot be host-compiled because
  it pulls in cjext, NVS and the factory-info partition. So the comparison, the
  validation and the generation were moved somewhere with no dependencies at
  all, and the entropy is passed in as an argument rather than read from
  `esp_fill_random()` inside. Both choices exist only to make these checks
  possible.

  **The test demonstrates the defect instead of asserting it.** It keeps a
  verbatim copy of the old expression as `legacy_match()` and checks that the
  old code *accepted* a key with junk appended, and accepted every token
  against an empty key. Those checks fail if the premise is wrong, so the file
  is the evidence for the fix rather than a restatement of it. Running it
  prints the "legacy accepted" lines.

  **Four mutations, all caught.** The real implementation was reverted to the
  old `strncmp` prefix match in a scratch copy: 7 failures. Reverted with a
  `NULL` guard added: the same 7 — and without the guard it *segfaults* under
  ASan in `strlen`, which is the concrete demonstration that the old code was
  only safe because its caller checked for `NULL` first. Transposing two
  characters of the generation alphabet: caught by the known-vector check.
  Removing one character from the alphabet, so that 32 no longer divides 256
  and generation becomes biased: **caught at compile time** by a negative-array
  `typedef`, because a biased generator produces perfectly plausible keys and
  so cannot be noticed by reading the output.

  One check is worth naming because it protects a compatibility property rather
  than a correctness one: `is_valid()` is asserted to accept `l`, `o`, `0` and
  `1`, which generation never emits. A cloud-provisioned key contains them, so
  anyone who "tidies" `is_valid()` into an alphabet test would lock out every
  provisioned device — and now gets a red suite instead. The test vector for
  that is the real 64-character provisioned key left in a commented-out line in
  `ezlopi_core_factory_info.c`, rather than an invented string, so the question
  is settled against a value that actually shipped.

  Not covered, and recorded rather than implied: that the comparison is
  *measurably* constant-time (the structural guarantee is no data-dependent
  branch; timing it on a host under ASan would measure the host's scheduler),
  where the entropy comes from, whether the key persists across a reboot, and
  the complete absence of rate limiting on login attempts.

- **The PMS5003 frame parser was split out so it could be tested at all** —
  `tests/host/test_pms5003.c`, 92 checks. This is the first driver brought into
  the suite by restructuring it rather than by already being separable, and it
  is the pattern the remaining three UART drivers need.

  `pms5003.c` calls `gpio_config`, `gpio_set_level` and `esp_timer_create`, so
  it cannot be compiled off-target. The obvious move — adding those functions to
  `tests/host/stubs/driver/gpio.h` — was deliberately not taken: that stub
  declares *no* functions precisely so a hardware-touching driver fails to link
  instead of appearing to work, and widening it would have hidden the
  dependency rather than removed it. Instead `pms_read_upcall` and the UART
  upcall moved verbatim into a new `pms5003_parse.c`, with `pms5003.h`
  including `pms5003_parse.h` and never the reverse. The new translation unit's
  entire set of undefined symbols is `memcpy` and `memset`, which is what makes
  it host-compilable; the component's `CMakeLists.txt` needed no change because
  it already globs `*.c`.

  Nothing about on-target behaviour changed: both function bodies were moved
  without edit, including the `ESP_LOGE` on a failed checksum (`TRACE_E` would
  have been tidier but compiles to nothing when tracing is off, which would have
  been a silent behaviour change). Verified on the pinned v4.4.6 toolchain that
  `pms5003_parse.c.obj` is actually produced, that `pms5003.c.obj` imports
  `EZPI_pms5003_uart_upcall` from it, and that the pre-split static name is
  absent from the ELF — a globbed source that silently stopped being compiled
  would otherwise have left a green build with no parser in the image.

  **This converts one of the four previously untestable clamps into a tested
  one.** The suite covers all 13 decoded fields, a 65536-value sweep of
  `pm25_standard`, every short-frame length from 1 to 31 through exact-size heap
  allocations so ASan can see the over-read, and the exact-minimum 32-byte case
  that a `<=` off-by-one would reject. Six mutations were confirmed to make it
  fail. The first attempt at the clamp mutation was thrown away for being
  worthless: deleting the length check also deletes the only use of
  `output_len`, so `-Werror=unused-parameter` failed the build and the
  *compiler*, not the test, was doing the catching. Rewritten to leave the
  parameter referenced and the condition unreachable, it now aborts under
  AddressSanitizer, which is the actual claim being made.

- **The MB1013 parser was split out the same way, and doing it found a live
  bug** — `tests/host/test_mb1013.c`, 50 checks. The split was undertaken to
  make an existing clamp testable; re-reading the comment justifying that clamp
  while writing the tests is what exposed the unterminated `atoi()` over-read
  described above. That is the argument for this work in one sentence: the clamp
  had been reviewed and documented, and the hole was still there until something
  executed it.

  `mb1013_parse.c` is the cleanest of the three driver rules in the `Makefile`:
  no `-Istubs` at all, and its complete set of undefined symbols is `__divdf3`,
  `__floatunsidf` and `__truncdfsf2` — libgcc soft-float, for the one `/ 10.0`.
  Nothing from libc, nothing from ESP-IDF. Verified on the pinned v4.4.6
  toolchain that the object is produced, that the driver imports
  `EZPI_mb1013_parse_range` from it, that the symbol reaches the ELF, and that
  no MB1013 object imports `atoi` or `strtol` any more.

  The tests allocate every frame at its exact length, so the byte after the
  terminator is poisoned and a one-byte over-read aborts rather than landing
  harmlessly in a scratch array — without that, the original defect passes. They
  sweep all 10000 real frames against `atoi()` on a terminated copy as an
  independent oracle, slide a frame through every offset 0..31, reject every
  length 0..5 while accepting exactly 6, and cover whitespace payloads at
  several offsets.

  **Eight of eight mutations caught**, with two worth recording. The one that
  matters most — restoring the original `atoi()` — was initially caught by the
  *compiler*, because the mutation orphaned the payload helper and
  `-Werror=unused-function` fired; it was rewritten to keep the call and discard
  only its result, and now aborts under ASan. Separately, widening the payload
  scan bound is **not** caught, and that is correct: it is an equivalent mutant,
  because the scan stops at the first non-digit and the caller has already
  established that the following byte is `'\r'`. It is still run and reported,
  counted in neither direction, because the reasoning depends on the terminator
  check — and noticing that dependency is what added
  `test_terminator_is_required()`, which had been missing.

  **This converts a second of the four previously untestable clamps into a
  tested one**, leaving R307 and GY-GPS6MV2.

- **The GY-GPS6MV2 GPGGA parser was brought in by splitting a header, not code
  — and it reported a GPS fix the device did not have** —
  `tests/host/test_gps6mv2.c`, 146 checks.

  **The defect.** Each of the 13 parsed GPGGA fields is filled by writing single
  bytes at an absolute index derived from the field's own start — `FIELD[diff -
  1] = c` — so nothing in the fill path ever shortens a field. The `memset` that
  was supposed to handle that ran only when the incoming field was *empty*:

  ```c
  if (dummy_container[start_index + 1] == ',')
  { // applies only once
      memset(&FIELD, 0, sizeof(FIELD));
      break;
  }
  ```

  So a non-empty value shorter than the previous sentence's inherited the
  previous value's tail. Measured, not reasoned: feeding `12` satellites and
  then `9` reads `Satellites_used` back as `"92"`; `1300.8` then `999.5` reads
  as `999.58`; `-41.3` then `-9.3` reads as `-9.33`.

  **Why that matters more than a wrong number on a dashboard.** The driver
  computes `GPS_FIX = ((total_sat > 2) && (gps_quality != 0))` from
  `atoi(Satellites_used)`. A retained digit can only make the count *larger*, so
  the error is not symmetric — it biases towards claiming a fix. After a good fix
  on 12 satellites, a drop to 1 satellite parses as 11 and **the device keeps
  reporting a valid GPS fix with one satellite in view**. A fresh handle fed the
  identical sentence reports no fix, which isolates retained state rather than
  the threshold as the cause. And because `prev_GPS_FIX` broadcasts only on
  change, the false fix is never corrected — it just persists.

  **The fix** replaces the empty-field test with a clear at the start of every
  field: `if ((diff = (i - start_index)) == 0)`. That holds exactly once per
  field, at the delimiter character itself, where the `diff > 0` fill guard
  already has nothing to write — so each field is cleared once and then filled,
  and an empty field still ends up cleared because nothing follows the delimiter.

  It also removes a second defect for free. `dummy_container[start_index + 1]`
  is, for a sentence ending in `','` or `'*'`, index `len` of a `len + 1`-byte
  `ezlopi_malloc` filled by `strncpy(dst, src, len)` — which writes no
  terminator when `strlen(src) == len`. The old guard read a byte that was never
  initialised. It is in bounds, so neither AddressSanitizer nor
  UndefinedBehaviorSanitizer says anything; it is gone regardless.

  **The split was a header move, and that is worth recording because the
  opposite was assumed.** The entry under known gaps below used to say this
  driver needed its parsing lifted out of the cloud reporting it was interleaved
  with. That is true of `__uart_gps6mv2_upcall()` in the registration file, but
  not of `GYGPS6MV2_parse_gpgga_mesg()`, which was already standalone in a file
  whose only other includes are `<string.h>` and the trace macros. The one thing
  keeping it off a host compiler was `sensor_0053_UART_GYGPS6MV2.h`, which
  declared the NMEA layout and a single cloud-action prototype together. Moving
  the layout to `gps6mv2_parse.h` — with the sensor header including it, so
  nothing downstream changed — took `gyGPS6MV2.c` from 170 headers to 123, from
  eight `ezlopi_core_*` headers to two, and from two cJSON headers to none,
  measured with `ninja -t deps`. No function moved. `GPS6MV2_t` is typedef'd in
  exactly one header, which is the thing that actually has to hold for a header
  split: two definitions would build green and disagree about field offsets.

  **The tests were confirmed red first** — 10 failures against the unfixed
  parser — then green after. They cover all 16 fields of a well-formed sentence,
  every field shrinking, the real power-up sentence `$GPGGA,,,,,,0,00,99.99,,,,,,*48`,
  every truncated prefix of a sentence, 50 repetitions of an identical sentence,
  a sweep of satellite counts 0..99 each following a two-digit count, and the
  `GPS_FIX` computation copied verbatim from the driver. Nine of nine mutations
  caught, including a full revert to the pre-fix parser taken straight from git
  rather than synthesised. One mutation initially survived and was a genuine
  hole, not an equivalent mutant: the suite asserted every field's value except
  the checksum's, of which it asserted only termination — and a field that is
  never written is trivially terminated.

  **Also asserted, and not a defect:** the five `atoi()` calls on these fields
  are *not* the MB1013 unterminated-`atoi()` bug. `GPS6MV2_t` is memset at
  allocation and every fill guard stops at least one byte short of its array, so
  the last byte of each is never written. That is a property of the guards rather
  than anything enforced, which is why `test_fields_stay_terminated()` checks the
  final byte of all 14 character arrays across nine hostile inputs on both fresh
  and reused handles, instead of the argument being left in a comment. MB1013 is
  the reason: it had this exact shape of argument in a comment, and the argument
  was wrong.

  Verified on the pinned v4.4.6 toolchain: both GPS objects compiled, the driver
  still imports the parser and the parser still defines it, one definition in the
  ELF, no warnings from any GPS file, and the whole-build warning count unchanged
  at 144. The image is 80 bytes *smaller* than before the fix.

  **This does *not* convert the GY-GPS6MV2 clamp into a tested one, and it would
  be easy to claim otherwise.** The clamp added in this release is in
  `__uart_gps6mv2_upcall()` in the registration file — the 255-byte copy and the
  bounded `strlen` into `gps_cir_buf` — and that function is still outside the
  suite, for the header-entanglement reason given under known gaps. What is now
  tested is the GPGGA field parser downstream of it. Two of the four clamps
  remain verified only by an esp32 build: R307's and this one.

- **R307 was examined for both of the patterns found above and has neither.**
  Two negative results, recorded because the next person to look will otherwise
  re-derive them, and because the GY-GPS6MV2 entry above is a standing warning
  that an assumption about a driver's dependencies can be wrong by a whole
  category.

  *It is not the cheap header split.* The hope after GY-GPS6MV2 was that R307
  would also turn out to be blocked only by its header. It is not:
  `R307_AS606.c` calls `esp_timer_get_time()` 31 times, `vTaskDelay()` nine
  times and `uart_write_bytes()` six times, in the function bodies rather than
  in declarations. Those are real code dependencies and no header move removes
  them. `__Response_function()` is the specific obstacle — it interleaves the
  confirmation-code `switch` with an `esp_timer_get_time()`/`vTaskDelay()`
  polling loop, so the parsing and the waiting are the same loop. That is the
  entanglement the earlier known-gaps entry predicted for GY-GPS6MV2, where it
  was wrong; here it is accurate.

  *It does not have the GY-GPS6MV2 stale-buffer defect.* The reason that one
  mattered was state surviving between parses, so every R307 receive buffer was
  checked for the same shape. All seventeen `memset(recieved_buffer, ...)` calls
  pair one-to-one with the seventeen `SEND_PACKET()` calls, and in every pair
  the clear comes first — the merged file order is `CS` seventeen times with no
  two clears or two sends adjacent. That ordering is load-bearing rather than
  incidental, for a reason that only becomes clear once you look at what reads
  the buffer: `__Response_function()` does not fill it. It *polls* it, spinning
  on `recieved_buffer[0] == FINGERPRINT_PID_ACKPACKET` with a
  `vTaskDelay(50ms)`, because the actual fill happens asynchronously in
  `__uart_0066_fingerprint_upcall()` on the UART task. The clear before each
  send is what stops the poll returning immediately on the *previous* reply's
  ACK byte.

  Its receive-side bounds are sound too, though that is a narrower claim than
  the receive path being sound. `recieved_buffer` is one array, sized
  `MAX_PACKET_LENGTH_VAL` (64); the single copy that fills it is clamped with
  `(_copy_size < MAX_PACKET_LENGTH_VAL) ? _copy_size : MAX_PACKET_LENGTH_VAL`;
  its source `another_buffer` is sized `MAX_PACKET_LENGTH_VAL + 6` so that
  reading 64 bytes from `another_buffer + 6` stays in bounds; the furthest
  constant index read anywhere is `[21]`; and `ReadSysPara`'s jump from `[11]`
  to `[16]` — the only non-consecutive step in that function — correctly skips
  the four-byte device address the datasheet puts there rather than being an
  off-by-four.

  **One thing found and deliberately not fixed, recorded because it is not
  obvious and is not a bounds problem.** The poll above tests only byte `[0]`,
  and the fill is a single `memcpy` running on a different task on a different
  core. `ACK_OK` is `0x00` and the buffer was just memset to zero, so a poller
  that observes `[0] == 0x07` after the copy has written the first byte but
  before it reaches the fourth will read `[3]` as `0x00` and report a failed
  command as a success. The window is a few instructions wide inside a ~12-byte
  `memcpy`, and nothing here establishes that it has ever been hit — it is
  stated as a mechanism, not as an observed failure. It is left alone because
  the fix is a sequencing or handshake change in the upcall rather than a bound,
  and that belongs with the parser seam below, not in a clamp commit.

  What the examination did find and fix is on the transmit side, under *UART
  sensor dispatch* above.

- **New CI workflow, [`.github/workflows/ci.yml`](.github/workflows/ci.yml).**
  Nothing in this repository was checked automatically before.

  | Job | What it does |
  | --- | --- |
  | `host-tests` | `make -C tests/host` under ASan + UBSan |
  | `hygiene` | no hard-coded Ezlo telemetry endpoint; telemetry not enabled in committed defaults; base and per-target `sdkconfig.defaults` all present; JSON string destinations initialised; no pinned `sdkconfig.h` includes |
  | `firmware` | builds all four targets in `espressif/idf:v4.4.6`, asserts the generated config was actually inherited from the per-target defaults, merges and verifies a flashable image, uploads `bin`/`elf`/`map`/`sdkconfig` |
  | `firmware-outdir` | builds esp32 with `idf.py -B /tmp/...`, outside the workspace, and asserts the image landed there and that **no `./build` was created at all** — so the tree cannot regress to a build directory it only builds in by default |
  | `firmware-ota` | builds esp32 with `CONFIG_EZPI_ENABLE_OTA=y`, which no committed default sets, and asserts the symbol really got enabled, that the pinned header is still the default CA source, and that `ca_mios` reached the ELF |
  | `firmware-ota-bundle` | builds esp32 with OTA on and `CONFIG_EZPI_OTA_CA_BUNDLE=y`, from a tree with **no `ca_mios.h`**, and asserts the header is absent, `ca_mios` is *not* in the ELF, and `esp_https_ota` still is |
  | `firmware-improv` | builds esp32 with `CONFIG_EZPI_BLE_ENABLE_IMPROV=y` and asserts both that the symbol got enabled and that the service is in the ELF |
  | `firmware-mqtt` | builds esp32 with `CONFIG_EZPI_ENABLE_MQTT=y` and **both websocket transports off** — the local-only Home Assistant configuration — and asserts the broadcast queue is still installed and that no mDNS service is advertised on a port with no listener |
  | `install-page` | assembles and verifies [`install/`](install/) from the firmware artifacts |
  | `deploy-install-page` | publishes it, once a maintainer opts in |

- **Several jobs exist only to compile code the other builds skip.**
  `ezlopi_core_ota.c` is wrapped in `#ifdef CONFIG_EZPI_ENABLE_OTA`, and the
  Improv service and advertisement sit behind `CONFIG_EZPI_BLE_ENABLE_IMPROV`.
  Neither symbol is set in any committed `sdkconfig.defaults.*`, so the four
  target builds compile *none* of either and a green run said nothing about
  them. Each job turns one feature on for one target.

  They assert afterwards that the symbol was actually enabled, because the
  first attempt at this used `-DSDKCONFIG_DEFAULTS`, which builds perfectly and
  leaves the symbol unset — the job passed while compiling exactly as little as
  the others. The config fragment is now appended to the per-target defaults
  file, the same mechanism the four firmware jobs already use, so it cannot
  diverge from them.

  `firmware-improv` goes one step further and greps the ELF for
  `EZPI_ble_service_improv_init`. A set Kconfig symbol only proves the
  configuration took; the symbol proves the file behind it was compiled and
  linked rather than preprocessed away to nothing, which is precisely how the
  whole of `ezlopi_core_ota.c` used to escape CI.

- **That ELF check then failed on its own `SIGPIPE`, which is worth recording.**
  `shell: bash` makes Actions run a step as
  `bash --noprofile --norc -eo pipefail`. Under `pipefail`,
  `nm build/*.elf | grep -q <symbol>` reports *failure on success*: `nm` sorts
  by symbol name, so the match is found almost immediately, `grep -q` exits at
  once, and `nm` dies writing the other ~13,800 symbols into a closed pipe.
  `pipefail` takes that 141 as the pipeline's status.

  Reproduced against the real ELF — exit 141 with `pipefail`, 0 without — which
  is also why the same check passed when run by hand. The symbol table is now
  written to a file first. The rewritten step was re-run under the same shell
  flags and confirmed to exit 0 for the real symbol and 1 for a deliberately
  misspelled one, so it is known to be able to fail and not merely to pass.

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

- **The suite reached `ezlopi-cloud` for the first time**, with
  `tests/host/test_mqtt_api.c` (96 checks) covering the three
  `hub.mqtt.config.*` handlers. That was only possible because the new file
  depends on nothing from ESP-IDF; the API layer as a whole still does not
  compile on a host.

  Two things about it are worth copying rather than the test itself. The first
  check in the file asserts the handlers are *present*: the subject is wrapped
  end to end in `#ifdef CONFIG_EZPI_ENABLE_MQTT`, so if the stub `sdkconfig.h`
  stopped defining it the translation unit would compile to nothing, the link
  would still succeed against the test's own fakes, and every check would pass
  while testing an empty file. Second, that stub lives in its own
  `tests/host/stubs-mqtt-api/` rather than the shared `stubs/`, because seven of
  the seventeen binaries put `stubs/` on their include path — `test_cjext`,
  `test_ze08`, `test_pms5003`, `test_gps6mv2`, `test_devcfgval`,
  `test_devcfg_api` and this one — and a shared `sdkconfig.h` would quietly
  decide the configuration of six tests that have nothing to do with MQTT,
  three of them drivers full of `#ifdef`s.

  Mutation-tested 6 of 6 caught — storing before validating, truncating instead
  of refusing, returning the password, treating `""` as absent, ignoring a
  failed write, and running against an empty subject. The better evidence is
  that it found two real defects the first time it ran, both described above:
  the `str_value_len` off-by-one and the credential validator refusing spaces.

- **The trace stub had never type-checked a format argument.** `EZPI_TRACE_NOOP`
  in `tests/host/stubs/ezlopi_util_trace.h` was `((void)sizeof(__VA_ARGS__))`,
  which works for one argument and turns `TRACE_W("%s", x)` into a comma
  expression that `-Werror=unused-value` rejects. Its own comment claimed the
  arguments were still checked so that a typo in a traced expression stayed a
  compile error; that was half true, and nothing noticed for as long as no
  host-compiled file logged anything with a format argument. It is now a
  variadic `static inline` no-op: unused, it is discarded; used, every argument
  is still named and still has to exist. Deliberately not routed to `printf()`,
  which would add format checking and simultaneously start failing the build on
  format mismatches in driver files no test currently reaches — a change worth
  making on purpose rather than as a side effect.

### Packaging — [infra]

- **The uploaded firmware artifacts could not flash a blank board.** The
  firmware jobs uploaded `build/*.bin`, a *non-recursive* glob. It matched the
  application image and `ota_data_initial.bin`, and silently missed
  `build/bootloader/bootloader.bin` and
  `build/partition_table/partition-table.bin`, because both live in
  subdirectories. The artifact was therefore enough to update a board already
  running EzloPi and not enough to bring up a new one — and nothing anywhere
  said so, because a partial set of images is not an error until someone tries
  to flash it.

  CI now also runs `esptool.py merge_bin` and publishes one
  `merged-firmware.bin` per target, written at offset 0.

  The offsets are not restated anywhere. `esptool` reads them from
  `build/flash_args`, which the build generates, so the merge cannot drift from
  the partition table — and it must not, because the bootloader is at `0x1000`
  on ESP32 and ESP32-S2 but at `0x0` on ESP32-S3 and ESP32-C3. A merge step
  with offsets written into it would have quietly produced an unbootable image
  for half the targets.

- **New check, [`tests/ci/check_merged_bin.py`](tests/ci/check_merged_bin.py).**
  `esptool` exiting 0 is not evidence here: it is being asked to copy bytes to
  offsets, and the mistakes worth catching — a wrong offset, a missing input —
  are both *successful* `esptool` runs. The check asserts every image listed in
  `flash_args` is byte-identical inside the merged file at its own offset, and
  that the merged file ends exactly where the last one does, which is what
  catches a dropped trailing image.

  Tested both ways against a real build: it passes on a correct merge and fails
  on a single flipped byte, on one appended byte, on truncation, and on a
  missing file.

- **New browser install page, [`install/`](install/).** A static page driven by
  [ESP Web Tools](https://esphome.github.io/esp-web-tools/): open it in Chrome
  or Edge, plug a board in over USB, press one button. No ESP-IDF, no esptool,
  no Python. The chip identifies itself over serial, so one button covers all
  four targets. It is the merged image above that makes this possible — ESP Web
  Tools will not accept a set of separate images for an ESP-IDF v4 project.

  The page states per target whether anyone has actually flashed and booted it,
  because for three of the four the honest answer is no, and that belongs where
  someone about to flash will see it rather than only in a README.

  It deliberately sets `"new_install_improv_wait_time": 0`. ESP Web Tools
  provisions Wi-Fi over Improv **Serial**, and this firmware implements Improv
  over **BLE** only — same protocol, different transport, not interchangeable.
  The default behaviour would be to wait 10 seconds after flashing for a reply
  that cannot come. Removing that one line is all the page needs once Improv
  Serial exists.

- **New check,
  [`tests/ci/check_install_page.py`](tests/ci/check_install_page.py).** Every
  way this page can break is silent. A manifest pointing at a path that does
  not exist still renders a working-looking button, and fails only once a user
  has plugged in a board and clicked it. A `chipFamily` typo means that chip is
  never offered a build and its owner is told the board is unsupported. So the
  check refuses to publish unless every advertised image exists, carries an ESP
  image magic byte, is flashed at offset 0, and has a `chipFamily` matching the
  build directory it came from; it also rejects an image that *no* build
  references, which is what would happen if a target were added to the CI
  matrix and not to the manifest.

  Tested both ways: it passes on a correctly assembled page and fails on a
  missing image, a mismatched `chipFamily`, a duplicate `chipFamily`, an
  unreferenced image, a non-image file, and an unstamped version placeholder.
  The CI step that assembles the page was itself tested by extracting the
  `run` body out of the workflow and executing it against simulated artifacts,
  rather than against a retyped copy of it.

- **Publishing is opt-in and off.** Deploying to GitHub Pages needs repository
  settings that a pull request cannot change, and a job that goes red on
  `master` purely because Pages was never enabled would be worse than no page.
  The page is assembled and checked on every push regardless, so it cannot rot
  while this is off. Enabling it is two steps, both in
  [`install/README.md`](install/README.md).

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
- **Documented the provisioning JSON schema**, in
  [docs/provisioning-schema.md](docs/provisioning-schema.md), so a hub can be
  configured from something other than the Ezlo mobile app. Linked from README
  section 5.1.

  The headline finding is that there are **three provisioning paths and no two
  share a schema** — BLE `0xE301`, serial `cmd:7`, and the hub's own outbound
  call to Ezlo's provision-sync. They differ in payload shape (flat versus
  nested under `data`), in key names, and in whether they authenticate at all.
  The document gives per-path key tables with the buffer size behind each
  field, the BLE chunk framing, and a security-properties section.

  Three traps get their own treatment, because each one compiles, runs, stores
  nothing and reports success: `provisioning_uuid` versus
  `uuid_provisioning`, `signing_ca_certificate` versus `ca_cert`, and
  `device_type_ezlopi` versus `ezlopi_device_type`. All three pairs genuinely
  exist as separate declared constants. Every key named in the document was
  checked against its declaration in `ezlopi_cloud_keywords.c` rather than
  inferred from the identifier.

  It also records what cannot be done rather than leaving readers to find out:
  the provisioning status characteristic `0xE302` is never registered and
  `0xE301` is write-only, because `EZPI_SERV_BLE_ENABLE_READ_PROV` and
  `EZPI_SERV_BLE_ENABLE_STAT_PROV` are `#define`d and then unconditionally
  `#undef`'d in `EZLOPI_USER_CONFIG.h`. Configuration can only be read back
  from device-info on `0xE401`. And "provisioned" means one thing only: the
  stored `cloud_server` contains the substring `https://`.

  Writing it was not a neutral exercise. Checking the claims against the
  source produced two live defects in the code being described — the
  `set_basic()` stack over-read and the serial-path leak, both under **Memory
  safety** above — and caught three errors in the document's own first draft
  before it was committed: it told readers to poll `0xE302`, which does not
  exist in any shipped build; it conflated buffer size with usable string
  length, when `snprintf` costs a byte to the terminator; and it named the CA
  certificate key as `ca_cert`, which is the read-back name from a function
  that is compiled out, not the name any write path accepts.

- **Measured what migrating off ESP-IDF v4.4.6 would actually cost**, in
  [docs/esp-idf-migration.md](docs/esp-idf-migration.md). The README said the
  tree "does not build on v5.x" without saying why or how much, which is not
  something a contributor can plan against. v5.5.5 and v6.0.3 were installed
  alongside v4.4.6 and the tree pushed through each: 276 errors across 73 files
  on v5.5.5, 208 across 20 on v6.0.3, with every error classified into
  mutually exclusive root-cause buckets. Four findings worth having before
  anyone starts:
  - **v6.0.3's lower number is the build dying earlier, not less work.** 185 of
    its 208 errors are one message, `driver/uart.h: No such file`, because
    v5.5.5's `driver` component still re-exports `esp_driver_uart` "for
    backward compatibility" (its own comment) and v6.0.3 dropped that list.
    Same for the legacy ADC API, which v5.5.5 still ships under
    `esp_adc/deprecated/` and v6.0.3 deleted. Hence the recommendation to go to
    5.5.5 first rather than straight to 6.x.
  - **Most of the count is a handful of sweeps, not a per-file slog.** 78
    errors are one toolchain typedef (`uint32_t` is `unsigned int` on the
    v4.4.6 compiler and `long unsigned int` on 14.2.0); 90 are one macro,
    `CJSON_GET_VALUE_INT`, taking the address of an enum lvalue; 58 are
    `portTICK_RATE_MS`. Counting errors overstates the work — e.g. four
    `pms5003.c` errors vanish by adding one `#include`.
  - **The `CJSON_GET_VALUE_INT` cluster is not a live memory bug**, despite
    being an `int *` write through a pointer to something else. All three enums
    involved are 4 bytes on the target, confirmed by a `_Static_assert`
    compiled with the actual target compiler, with a `-fshort-enums`
    counterfactual to show the assert can fail.
  - **One capability is lost either way:** `hall_sensor_read()` exists in
    v4.4.6 and in neither candidate, so `sensor-0018-other-internal-hall-effect`
    has no replacement API. Also `CONFIG_SDK_TOOLPREFIX`, which
    `ezlopi_cloud_info.c` reports to the cloud as `architecture`, no longer
    exists. Both need a decision rather than a fix.

  Also recorded: zero of this project's own `CONFIG_EZPI_*` keys are dropped by
  either version, which is the one class of breakage a compiler cannot catch;
  and the pin is v4.4.6 while the branch ended at v4.4.8, so two patch releases
  of fixes are available without touching a line of source.
- Added this file and [SECURITY.md](SECURITY.md).

---

## Known gaps

Listed because a changelog that only mentions what was finished is misleading.

- **`cJSON_GetObjectItem()` can return the wrong member, on trees this firmware
  builds itself.** Diagnosed in full in the device-configuration entry above.
  In one line: the key-length field means two different things depending on
  whether a tree was parsed or built, and the case-insensitive lookup needs the
  parsed convention to reject a requested name that extends a stored key. On a
  built tree, looking up `configSize` can return `config`.

  Not fixed, and the reason is the size of the blast radius rather than the
  size of the fix. The one-line change — make `add_item_to_object()` store the
  inclusive length — also changes what the key printer at
  [`cjext.c:2126`](ezlopi-components/cjext/cjext.c#L2126) is handed, and every
  response from all 123 API methods goes through that printer. It needs its own
  commit, its own host test for the printer, and a board, none of which belong
  inside a feature change.

  What is known: requests arrive parsed, so input validation across the
  firmware is unaffected, and serialised output is correct either way. The
  exposure is in-firmware code that looks up a longer member name against a
  built tree holding a shorter prefix of it.

  **Now audited.** The sweep is described in the entry below. Result: two call
  sites in the whole tree are genuinely exposed, both are masked, and both are
  unreachable on shipped hardware. The bug is still worth fixing, but it is not
  currently costing a correct response anywhere.

- **The prefix-lookup sweep: one latent collision is left, in the scene
  block-data scale list.** This entry records the sweep the item above used to
  say had not been done, because its residue is a gap rather than a fix.

  The first thing the sweep needed was ground truth, not more reading of
  `cjext.c` — a first pass at reading `parse_string()` got the convention
  backwards, which would have made the bug universal rather than build-only.
  So the convention was measured instead, by a 17-check C probe built under
  ASan/UBSan that reports `str_key_len` beside `strlen()` for the same key
  reached both ways, and that identifies a returned child by its *index* via
  pointer identity so a correct-by-accident value cannot pass. Measured: a
  parsed `"config"` stores 7, a built `"config"` stores 6. Seven of the
  seventeen checks disagree with a correct lookup, and all seven are on built
  trees. The probe also showed the exposure is wider than sibling pairs: an
  object holding `{"config":1}` *alone* returns that member for a `configSize`
  lookup, because the comparison stops before the mismatch.

  With the convention established, the sweep was mechanical:

  - **40 colliding names.** Of the key names this firmware looks up, the pairs
    where one is a strict case-insensitive prefix of another.
  - **127 call sites** look up one of those 40 names.
  - **25 of the 127 search an object this firmware built**, found by an awk
    pass that, per function body, intersects the locals assigned from
    `cJSON_CreateObject`/`CreateArray`/`CreateRaw`/`Duplicate`/
    `AddObjectToObject`/`AddArrayToObject` with the locals used as the search
    object. The first version of that scan counted only `Create*` and so missed
    two sites; `AddObjectToObject` returns a freshly built child and matters
    just as much. Everything else searches a parsed tree, including
    `cJSON_ParseWithRef`, which was checked and does use the inclusive
    convention — parsed trees cannot misfire regardless of the name.
  - **23 of those 25 are immune by construction.** They search an item-value
    object, and every such object in the firmware is built by the single
    chokepoint
    [`ezlopi_core_valueformatter.c`](ezlopi-core/ezlopi-core-valueformatter/ezlopi_core_valueformatter.c),
    which adds only `value` and `valueFormatted`. `value` is the shortest key
    present, so nothing can be a strict prefix of it — confirmed mechanically
    rather than by eye, by grepping for the only four names that could be
    (`ezlopi_v_str`, `ezlopi_va_str`, `ezlopi_val_str`, `ezlopi_valu_str`):
    zero matches.
  - **2 sites are genuinely exposed**, both in
    [`ezlopi_core_scenes_operators.c`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_operators.c)
    at lines 1102 and 1148, and both look up `valueType` on a
    freshly-built item-value object. Because that object never contains a
    `valueType` key, the lookup resolves to the `value` item instead. Both are
    masked: the result goes to `cJSON_GetStringValue()`, which returns `NULL`
    for anything that is not a string, into a `NULL`-safe `STR_OP_COMP`
    comparison against `"dictionary"`. For every shipped driver the outcome is
    therefore identical to the outcome of a correct lookup. They are also dead:
    no shipped driver has a dictionary-valued item.

  What is left, and why this is a gap rather than a closed question:
  `ezpi_value_scales_list()` at
  [`ezlopi_cloud_scenes_block_data_list.c:268`](ezlopi-cloud/ezlopi-cloud-scenes/ezlopi_cloud_scenes_block_data_list.c#L268)
  keys a built object by the runtime `value_type` of an item and then looks
  that key back up. A prefix pair among the value-type names would silently
  merge two types' scale lists into one. Counted: **zero prefix pairs among the
  26 value types shipped drivers actually use**, so it is correct today. It is
  one driver away from not being — a `value_type_int` already in the enum
  alongside a new `value_type_int64`, or `volume` alongside `volume_flow`,
  would do it, and the symptom would be a wrong scale list in a scene editor
  rather than a crash. There is no test that would catch it.

  One claim the sweep settled rather than left open: the item-id keys of
  `cj_device_items_data` cannot collide, because every id is rendered `%08x`
  and so every key is exactly 8 characters, and no fixed-width set can contain
  a strict prefix pair.

- **Nobody knows which `isItemStateChanged` scenes change behaviour now that
  the type tag survives.** The `union`→`struct` fix in the scene-triggers entry
  above switched on three `switch` statements that had never once taken the arm
  they were written for, so a when-method that has been reporting "changed" on
  every evaluation since it was written should now report it only on an actual
  change. That is the intended behaviour and the tests pin the layout it
  depends on, but the end of the chain — which scenes fire, and how often —
  runs through the meshbot task and the item cache and was not traced. It needs
  a board with a scene using `isItemStateChanged` and an observable `then`
  block.

  The direction of the risk is worth stating, because it is not symmetric: the
  old behaviour was to fire *too often*, so the change should make scenes
  quieter rather than noisier. A scene that looked like it worked because it
  re-triggered constantly will now trigger properly, and to someone watching,
  "properly" and "stopped working" look the same for as long as the condition
  does not change.

- **`-Wshadow` is not enabled in the build, so nothing stops a ninth shadowed
  declaration from appearing.** The eight that the sweep left are no longer
  untriaged — each was read individually and none changes behaviour — but that
  result is a snapshot, not a guard. Enabling the flag would mean either
  silencing eight warnings nobody intends to act on or renaming across eight
  files for no behavioural gain, and the flag also fires inside ESP-IDF
  headers, so it cannot simply be added with `-Werror`.

  The triage is recorded here so it does not have to be redone. The sweep that
  found the `cj_device_items_data` defect above found nine; that one was shown
  to change behaviour and was fixed. For the remaining eight, what makes each
  one safe is specific, and in every case it is that the shadowed outer
  variable is either already spent or holds the identical value:

  | site | variable | why it is benign |
  |---|---|---|
  | `ezlopi_cloud_items.c:124` | `device_id` | a `uint32_t` shadows the `cJSON *` loop variable, but the pointer is consumed on the line above and `cJSON_ArrayForEach`'s increment runs in the enclosing scope, so iteration is unaffected |
  | `ezlopi_cloud_scenes.c:875` | `new_scene_id` | the inner sits in the `else` of `if (new_scene_id && …->valuestring)`, so the outer is known falsy there and is never read again |
  | `ezlopi_core_broadcast.c:241` | `curr_node` | the outer was walked to `NULL` by the duplicate-check loop above; the inner re-seeds from `__method_head`, which is what a fresh walk needs |
  | `ezlopi_core_scenes_cjson.c:797` | `value_type_name` | outer and inner are both initialised from `EZPI_core_scenes_get_scene_value_type_name(curr_field->value_type)` with `curr_field` unchanged between them, so the values are identical |
  | `ezlopi_core_scenes_operators.c:1059` | `item_id` | the outer id was already resolved into `item_left`; the inner resolves the right-hand item. Correct, just identically named |
  | `gxhtc3.c:224` | `i` | the inner is the 8-bit shift counter of a CRC8; the outer byte index is not referenced inside it |
  | `sensor_0037_pms5003_sensor.c:138` | `pms_object` | the inner re-reads the same `item->user_arg` the outer already holds |
  | `ezlopi_service_ws_server.c:337` | `cj_method` | the outer is `NULL` from its declaration until line 361 reassigns it, and line 368 is its only read, so the inner is redundant rather than load-bearing |

  Worth saying plainly, because the first pass over this list got it wrong:
  `scenes_cjson.c:797` looks like the dangerous shape — an inner declaration
  initialised from a call — and is in fact the safest of the eight, because the
  outer is initialised from the very same call. The shape is not the evidence;
  what the outer holds at the point of the shadow is.

- **The seven functions in `ezlopi_cloud_devcfg_port.c` have no automated
  coverage of any kind.** They are the only part of the device-configuration
  feature that touches the partition and the compiled-in device table, which is
  precisely why they cannot be host-tested and why they were isolated into
  their own file. The handler above them is covered by 322 host assertions and
  21 mutations; these seven are covered by reading them. The specific
  untested claim that matters most is that a configuration stored by
  `hub.ezlopi.config.set` is byte-for-byte what `ezlopi_device_parse_json_v3()`
  reads on the next boot.

- **The GPIOs a device configuration names are not checked before it is
  stored.** `ezlopi-util-devcfgval` above refuses the shapes that cannot
  produce a device at all; it has no opinion on the pin numbers inside a well
  formed entry.

  The first draft of this entry claimed the drivers do not check either, and
  that was wrong. 49 driver files call `GPIO_IS_VALID_GPIO` or
  `GPIO_IS_VALID_OUTPUT_GPIO` before configuring a pin, and 35 of the 37
  `gpio_config()` call sites in `ezlopi-sensors-devices` test the return. A
  pin that does not exist on the target is caught at init by most drivers,
  which fail with `EZPI_ERR_INIT_DEVICE_FAILED`.

  One real hole is left, and a second one this entry used to claim turned out
  not to exist:

  - **Pins that are "valid" but already in use by the system.** On esp32
    `SOC_GPIO_VALID_GPIO_MASK` excludes only 24 and 28–31
    (`soc/esp32/include/soc/soc_caps.h:131`), so GPIOs 6–11 — the SPI flash —
    pass `GPIO_IS_VALID_GPIO` and `gpio_config()` succeeds on them. Nothing in
    this tree knows which pins the build has claimed for the flash, the console
    UART, or a configured I²C or SPI bus, and the last two depend on
    `sdkconfig`.

  - **"Two devices claiming the same pin" was the wrong check, and an earlier
    version of this entry had it as a hole to close.** Two devices sharing a pin
    is how I²C works: every sensor on a bus shares `gpio_sda` and `gpio_scl` and
    is distinguished by `slave_addr`. The in-tree example configuration at
    [`ezlopi_core_factory_info.h:225`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.h#L225)
    is exactly that — a BME280 and a TSL2561 both on `gpio_sda:21, gpio_scl:22`,
    `slave_addr` 118 and 57 — though that one is commented out, so the stronger
    evidence is the live HAL:
    [`EZPI_hal_i2c_master_init()`](ezlopi-hal/ezlopi-hal-i2c/ezlopi_hal_i2c_master.c#L91)
    guards installation on `NULL == i2c_master_conf_ptr[channel]` precisely so
    that the second device on a channel reuses the bus the first one brought up,
    and reads and writes then select the device by `->address`. Sharing is
    designed in. A pre-store check that refused repeated pin numbers would
    reject that document, so it would have broken the most ordinary
    multi-sensor setup there is while reporting that it was protecting the user.
    What is left of the original concern is narrower and needs per-bus
    knowledge, not a uniqueness test: two devices claiming the same pin *in
    incompatible modes*, or on the same I²C channel with *different* SDA/SCL
    pins, which that guard would silently resolve in favour of whichever device
    initialised first.

  Two things a future attempt needs, both of which cost more than they look:

  - **The pin keys are not one key.** The parser reads fifteen:
    `gpio`, `gpio_in`, `gpio_out`, `gpio_scl`, `gpio_sda`, `gpio_tx`, `gpio_rx`
    and `gpio1`–`gpio8`
    ([`ezlopi_cloud_keywords.c:254`](ezlopi-cloud/ezlopi-cloud-constants/ezlopi_cloud_keywords.c#L254)
    onward). A checker that handles `gpio` and stops has covered a minority of
    the device types.

  - **"Unused pin" has at least four encodings, and they disagree.** `-1`,
    `255`, `""` — an empty *string* where a number is expected — and `0`
    alongside `"is_ip":false`. The first two appear together as separate tests
    in the same driver
    ([`device_0001_digitalOut_generic.c:272`](ezlopi-sensors-devices/device-0001-digitalOut-generic/device_0001_digitalOut_generic.c#L272)
    onward, which checks `255` for the output pin and then `-1` *and* `255` for
    the input pin); the last two appear in the in-tree example configurations at
    `ezlopi_core_factory_info.h:226` and `:247`. So a validator cannot ask "is
    this a real pin number" without first deciding which of the four spellings
    of absent it accepts, and refusing any of them breaks configurations that
    work today.

  For the reserved-pin half, the sources exist and are build-derived rather
  than remembered: `soc/spi_pins.h` per target, and
  `esp_rom_efuse_get_flash_gpio_info()` from `esp_rom_efuse.h`, which reports
  the flash pins the running chip was actually strapped for. Both were confirmed
  present in this IDF. That is the shape a correct check would take — read the
  pins out of the build and the efuses, never from a table typed from memory.

  The intent to check at the HAL level is visible and disabled: the
  `GPIO_IS_VALID_OUTPUT_GPIO` and `GPIO_IS_VALID_GPIO` tests at
  [`ezlopi_hal_gpio.c:76`](ezlopi-hal/ezlopi-hal-gpio/ezlopi_hal_gpio.c#L76)
  and [line 96](ezlopi-hal/ezlopi-hal-gpio/ezlopi_hal_gpio.c#L96) are commented
  out.

  Still deferred, and the correction above is the reason rather than an aside:
  the first attempt at this gap named three things to check and one of them
  would have refused a normal two-sensor I²C configuration. The cost of being
  wrong here is refusing setups that work today, which is worse than the gap
  itself, and getting it right means per-target and per-`sdkconfig` answers plus
  a decision on four spellings of "no pin". That is a piece of work in its own
  right rather than a few lines on the end of this one.

- **The BLE accumulator bounds are not tested, and nothing is freed on
  disconnect.** The accumulator itself is now bounded and freed iteratively (see
  Memory safety above), so this is what is left of that gap rather than the gap
  itself.

  The bounds and the generation check are in the two service files, and those
  cannot be host-compiled — cjext, NVS, the factory partition, FreeRTOS and the
  GATT profile. So `EZPI_BLE_PROV_MAX_ACCUM_LEN` and
  `EZPI_BLE_DYNCFG_MAX_ACCUM_LEN` are checked by reading the arithmetic and by
  the four-target build, which proves the expressions compile and the symbols
  link but says nothing about whether the numbers are right. The specific risk
  is not a too-small cap being unsafe but a too-small cap **refusing a real
  provisioning transfer from the phone app**, which would be worse than the bug
  it fixes. The derivation says it cannot happen — a payload that exceeds the
  cap could not have been stored anyway — but that reasoning has not been run
  against an actual app transfer.

  `ESP_GATTS_DISCONNECT_EVT` in
  [`ezlopi_core_ble_gatt.c`](ezlopi-core/ezlopi-core-ble/ezlopi_core_ble_gatt.c)
  still only sets `disconnected_service->connected = false`, so a partial
  transfer survives the disconnect and is held until the next client writes.
  That is now a bounded quantity rather than an unbounded one, which is why it
  was left: freeing at disconnect means core calling into its services, and the
  cheap version of that — a free-callback registry in
  `ezlopi_core_ble_buffer.c` — adds a dependency inversion for a few kilobytes.

  The generation check also rests on an assumption that has not been verified on
  hardware: that `ESP_GATTS_CONNECT_EVT` is delivered to every registered
  `gatts_if`, not only to the one whose characteristic the client first touches.
  If it is not, a second client's write would be treated as a continuation of
  the first's transfer — it would then be caught by the length bound rather than
  by the generation counter, and would fail as a refused transfer instead of as
  concatenated data, which is the right way round for it to be wrong.

  Two other characteristics use the accumulator — Wi-Fi and Improv — and are
  untouched by this change, with no bound of their own. They were left alone
  because each frees the list unconditionally at the end of every write
  ([`ezlopi_service_ble_wifi.c:272`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_wifi.c#L272),
  [`ezlopi_service_ble_improv.c:470`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_improv.c#L470)),
  so nothing persists across writes to grow. That is a property of those two
  handlers rather than a guarantee, and it is the thing to re-check if either is
  ever changed to hold a transfer open across writes. (The previous version of
  this gap also listed device-info as a user of the accumulator. It is not —
  `ezlopi_service_ble_device_info.c` includes the header and never calls into
  it.)

- **Three sets of real-looking device provisioning credentials are committed in
  `ezlopi-services/ezlopi-service-webprov/ezlopi_test_prov.c`, and this change
  does not remove them.** Each block holds an EC private key, a public key, a
  provisioning token, a device UUID, a cloud UUID, a certificate and a CA
  certificate, keyed to three specific device serials (`105200073`,
  `105200122`, `105200134`).

  **None of it is compiled.** Three blocks are inside `#if 0` and a fourth is
  commented out with `//`, which was verified three independent ways rather
  than by reading: no `test_*` symbol is defined in the preprocessed output, no
  `test_ssl_pvt_key` / `test_prov_token` / `test_id_str` symbol appears in
  `Ezlo_Pi_v4x.elf`, and none of the three unique base64 prefixes of the key
  bodies appears anywhere in `Ezlo_Pi_v4x.bin`. (The single
  `-----BEGIN PRIVATE KEY-----` that *is* in the image comes from mbedTLS's
  `pkparse.c`, which carries the PEM delimiters it matches against — it is a
  parser table, not a key. That one needed checking rather than assuming,
  because it looked exactly like the bad outcome.)

  Not compiled is not the same as not published. The material is in the
  repository and in its git history, so deleting the lines would not retract
  it. Remediation is a decision for whoever controls those devices and that
  cloud account — the keys should be treated as disclosed and revoked, and only
  then is removing the file a tidy-up rather than a false reassurance. Flagged
  here rather than quietly edited.

- **Only `esp32` has been run on hardware.** All four targets now compile
  cleanly, but compiling is not running. `esp32s2`, `esp32s3` and `esp32c3`
  have never been flashed or booted, so for those three "supported" means "the
  firmware links", nothing more.
- **Hardware verification is partial even on `esp32`.** The firmware has not
  been reflashed since this refactor, and the device-add path through chunked
  `0xE501` config has not been re-tested on hardware.
- **OTA is untested on hardware and off by default on every target.** It can now
  be *built* from a clean clone — selecting `EZPI_OTA_CA_BUNDLE` needs no
  `ca_mios.h`, and CI builds that configuration on every push — but compiling is
  not updating. No board has fetched an image over either trust path, so neither
  the pinned nor the bundle verification has been watched succeed or fail against
  a real server.
- **The MQTT and Home Assistant path has never run against a real broker.** It
  builds in three configurations in CI and the topic layer is covered by 183
  host checks, but no broker has been connected, no entity has appeared in a
  real Home Assistant, and no MQTT command has switched a real relay. The topic
  strings being correct in a unit test is not the same as Home Assistant
  recognising them.
- **No board has announced itself to a real resolver.** The mDNS advert is
  asserted present or absent in the ELF by CI, which catches the wrong *code*
  being compiled, but nothing has watched a client discover a hub — or fail to.
  Separately, the advertised hostname is `ezlopi_<serial4>`, and an underscore
  in a `.local` host label is questionable under the relevant RFCs. It is left
  alone deliberately: no resolver has been observed rejecting it, and changing
  an advertised name risks breaking Ezlo's own clients.
- **Device-list walks are unsynchronised, across the whole tree.**
  `ezlopi-core-devices` exposes no lock, so every caller of
  `EZPI_core_device_get_head()` iterates without one — about a dozen in
  `ezlopi-cloud` plus the MQTT announce path added here. Fixing it means adding
  a lock to the core and taking it in all of them at once; doing it in one new
  caller alone would look like a fix without being one.
- **One latent free-of-a-literal in the mDNS path, unreachable in every shipped
  configuration.** When a factory-info getter returns `NULL` the free routine
  can call `ezlopi_free()` on the string literal `"null"`. It is only reachable
  with `CONFIG_EZPI_MDNS_ENABLE_EXTRA_SERVICES` on, which is off in all four
  committed defaults. The same routine also never frees the list nodes
  themselves and never `NULL`s `ezlopi_mdns_service_cntx`. Recorded rather than
  changed, because the fix is unverifiable without a configuration nothing
  currently builds.
- **The PSK is still sent to the cloud as `key`** in `hub.network.get`. Only
  the local trace is masked. See the README privacy note.
- **The local API is plain `ws://`, and the per-connection session fix does not
  change that.** The server listens on `INADDR_ANY:17001` with no TLS, so the
  local key crosses the LAN in cleartext on every login and anything able to
  intercept or spoof TCP on that network can take over an authorised socket.
  Sessions are keyed on the socket descriptor, which authorises whoever holds
  the socket — correct against *other* connections, and no defence at all
  against someone on the wire of the same one.

  This is recorded as a gap rather than fixed because the fix is `wss://` with
  a certificate, and a hub with no routable name and no CA has nowhere to get
  one that a client would accept; self-signed means teaching every client to
  skip verification, which is not obviously an improvement. It is called out
  because the session work above could easily be read as having closed the
  local-network hole in general, and it has not: it closed cross-connection
  authority, which was the part that let a passive peer in without touching
  the network.
- **The install page has never been used to flash a board.** It is assembled
  and checked on every CI run, and the images it serves are verified
  byte-for-byte against the build outputs, but no one has opened it in a
  browser and driven it end to end against hardware.
- **No UART sensor has been connected since the dispatch and clamp fixes.**
  None of these parts is on hand. The ZE08-CH2O path is covered by 26 host
  checks under ASan, proven able to fail by six mutations, but "tested" there
  means "against frames this repo constructs from the datasheet rule", not
  against a real sensor; its checksum fix means that sensor should start
  reporting readings for the first time, and *should* is the operative word.
  The PMS5003 and MB1013 clamps now have 92 and 50 host checks behind them on
  the same terms. The R307 and GY-GPS6MV2 clamps are still verified only by an
  esp32 build: GY-GPS6MV2 gained 146 host checks in this release, but they cover
  the GPGGA field parser, not `__uart_gps6mv2_upcall()` where that driver's clamp
  actually sits. R307 is in the same position twice over — besides its receive
  clamp, the `generate_packet()` transmit fix in this release has no host test
  either, only a throwaway AddressSanitizer probe over copies of both versions
  of the function. MB1013 is the reason that distinction is worth taking seriously
  rather than treating a reviewed clamp as settled: its clamp was reviewed,
  documented and wrong, and only writing the tests showed it.
- **The R307 parser still cannot be host-tested, and the reason recorded here
  for GY-GPS6MV2 turned out to be wrong about GY-GPS6MV2.** ZE08-CH2O is in the
  suite because its ESP-IDF dependency happens to be confined to type
  declarations, PMS5003 and MB1013 because their parsers were moved into their
  own translation units, and GY-GPS6MV2 because one *header* was split in two.

  This entry previously claimed both remaining drivers were hard for the same
  reason: that their UART upcalls — `__uart_0066_fingerprint_upcall` and
  `__uart_gps6mv2_upcall` — live in the `sensor_00XX_*.c` registration file
  alongside `ezlopi_core_cloud.h` and the device value-updated path, so the
  parsing would have to be lifted out of the cloud reporting it is interleaved
  with before any file could move. That is still true of those two upcalls. It
  was not true of the thing actually worth testing in the GPS driver:
  `GYGPS6MV2_parse_gpgga_mesg()` in `gyGPS6MV2.c` was already standalone, and
  only its header was entangled. The lesson is to check whether a dependency
  lives in the code or in the header before costing the work — the estimate here
  was wrong by a whole category.

  This entry also guessed that R307 "may well be the same shape, since
  `R307_AS606.c` is likewise nearly dependency-free". That has now been checked
  and the guess was wrong in the other direction. `R307_AS606.c` calls
  `esp_timer_get_time()` 31 times, `vTaskDelay()` nine times and
  `uart_write_bytes()` six times, from function bodies — real code dependencies
  that no header move touches. The specific obstacle is
  `__Response_function()`, which interleaves the confirmation-code `switch` with
  an `esp_timer_get_time()`/`vTaskDelay()` loop polling for the reply: the
  waiting and the interpreting are one function. So R307 needs the PMS5003
  treatment (move the logic behind a seam) and not the GY-GPS6MV2 one (move a
  header), and it is the most expensive of the five rather than the cheapest.
  Two wrong estimates in a row, in opposite directions, from the same habit of
  costing this work by looking at the file rather than at what it calls.

  What has *not* been brought in for either driver is the upcall itself:
  `__uart_gps6mv2_upcall`, its `gps_cir_buf` write-overflow path (clamped, never
  host-tested) and the `strstr`/`strchr` navigation of that buffer remain
  outside the suite, as does `__uart_0066_fingerprint_upcall` and the
  poll-versus-fill race noted under *Testing* above.
- **RESOLVED — GY-GPS6MV2's five `atoi()` calls are not the MB1013 defect.**
  This entry previously flagged them as unchecked, on the reasoning that a clamp
  around a string function is not the same as bounding it. They were checked.
  `GPS6MV2_t` is memset at allocation and every GPGGA fill guard stops at least
  one byte short of its array, so the last byte of each is never written and the
  fields are NUL-terminated by construction. Kept here as a resolved item rather
  than deleted, because the audit that produced this negative result is also what
  exposed the stale-tail defect fixed above — the same code, a different bug.
  `test_fields_stay_terminated()` now asserts the termination rather than leaving
  it to this paragraph.
- **The ESP-IDF migration is measured, not attempted.** v4.4.6 is still the
  only supported version. Every number in
  [docs/esp-idf-migration.md](docs/esp-idf-migration.md) comes from a compile:
  nothing was linked, nothing was run, nothing was flashed, and only the
  `esp32` target was surveyed. A clean compile on v5.5.5 would still leave the
  link stage, the image size and all runtime behaviour unknown. The error
  counts are also a floor rather than a total, because a translation unit that
  fails on a header never reaches its body — `portTICK_RATE_MS` reports 20
  errors against 58 real uses.
- **The repo still cannot declare a dependency outside the ESP-IDF tree.**
  There is no `idf_component.yml` anywhere, which is why `mdns` and
  `esp_websocket_client` — both moved to the component registry in v5 — are a
  blocker rather than a one-line fix. Adding the manifest is a prerequisite for
  any version bump, not a separate feature.
