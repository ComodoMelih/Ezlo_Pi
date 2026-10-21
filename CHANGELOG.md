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

A note on one kind of reference, because it looks like a path in this tree and
is not. Several entries below attribute a measurement to a script named
`probe/something` — `probe/hurestore.sh`, `probe/bomut.py`. Those live in the
audit working directory, a sibling of this checkout, and are deliberately not
committed: each one is a throwaway written to take a single measurement, and
they hardcode absolute `/mnt/c/...` paths and a WSL invocation that is specific
to the machine this work was done on. They are named anyway, so that a number
quoted in prose says which run produced it rather than appearing from nowhere.
Anything meant to be re-run by someone else is in [`tests/ci/`](tests/ci/) or
[`tests/host/`](tests/host/) and is wired into `make test` or the gate set.

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

- **Four drivers asked for one allocation to be freed up to six times, and
  neither build could say so.** `l_ezlopi_item_t` carries a `void *user_arg`
  and a `bool is_user_arg_unique`
  ([`ezlopi_core_devices.h:110-129`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.h#L110-L129)).
  The bool means *this item frees `user_arg`*, and it is read in exactly one
  place — `ezlopi_device_free_item()`, which recurses over **every** item of a
  device:

  ```c
  if (NULL != (items->user_arg) && (true == items->is_user_arg_unique))
  {
      ezlopi_free(__FUNCTION__, items->user_arg);
      items->user_arg = NULL;
  }
  ```

  The `= NULL` afterwards protects nothing, because the duplicate is a
  *different* item still holding the same address.

  Nothing documented the field, and its name reads as "`user_arg` belongs to
  one device". Four drivers were written to that reading — they handed a single
  `ezlopi_malloc` to every item of one device and flagged all of them:

  | driver | items on the device | allocations | items flagged as owner |
  |---|---|---|---|
  | [`device_0009_other_RMT_SK6812.c`](ezlopi-sensors-devices/device-0009-other-RMT-SK6812/device_0009_other_RMT_SK6812.c) | 6 | 1 | 6 |
  | [`device_0022_PWM_dimmable_lamp.c`](ezlopi-sensors-devices/device-0022-PWM-dimmable-lamp/device_0022_PWM_dimmable_lamp.c) | 5 | 1 | 5 |
  | [`device_0038_other_RGB.c`](ezlopi-sensors-devices/device-0038-other-RGB/device_0038_other_RGB.c) | 3 | 1 | 3 |
  | [`sensor_0012_I2C_BME280.c`](ezlopi-sensors-devices/sensor-0012-I2C-BME280/sensor_0012_I2C_BME280.c) | 2 (+1 on a child) | 1 | 2 |

  SK6812 said so in a comment: `/*linked to only one device so 'TRUE'*/`.
  BME280 is the one that shows the misreading cleanly — its *pressure* item
  sits on a separate child device and was correctly `false`, so the rule was
  understood across devices and simply not applied within one.

  **Neither of the two heap configurations reports it, in opposite ways.**
  `CONFIG_EZPI_HEAP_ENABLE` is unset in `sdkconfig` and in all four
  `sdkconfig.defaults.*`, which makes `ezlopi_free` a `NULL`-checked `free()`
  ([`EZLOPI_USER_CONFIG.h:108`](ezlopi-user-config/EZLOPI_USER_CONFIG.h#L108))
  — so the shipped image performs a real double free. Switch heap tracing on
  and `EZPI_core_util_heap_free` walks its node list for a match with
  `freed == 0`, finds none for the second call, and returns having done nothing
  ([`ezlopi_core_heap.c`](ezlopi-core/ezlopi-core-heap/ezlopi_core_heap.c)) — so
  the instrumented build absorbs it silently. It corrupted the configuration
  that ships and hid in the one you would debug with.

  Reaching it needs no cloud traffic and no attacker.
  `__EZPI_initialize_devices_v3()` frees any device whose item fails to
  initialise — see *A device that fails to initialise no longer disappears
  without saying so* below — so a dimmable lamp configured on a pin the
  hardware refuses is enough. `hub.device.remove` and a factory reset reach the
  same free.

  Three things changed:

  - **The five drivers now name exactly one owner.** A fifth,
    [`sensor_0029_I2C_GXHTC3.c`](ezlopi-sensors-devices/sensor-0029-I2C-GXHTC3/sensor_0029_I2C_GXHTC3.c#L361),
    had the mirror-image bug: `item_temperature->is_user_arg_unique = false`
    where `item_humdity` was meant. That cleared the only owner and left the
    humidity item at its `memset` default, so on the normal path — both items
    present — *nothing* freed the allocation. The same line also dereferenced
    `item_temperature` from outside the `if` that had tested it for `NULL`. The
    old code carried the author's own uncertainty on that line as a comment:
    `// affected if 'value_pts' is already freed?`

    For RGB and GXHTC3 the owner is now picked where the code knows which items
    it actually got, rather than asserted in a per-item helper that cannot see
    its siblings. The 2-axis joystick had two owner assignments naming the
    *same* item, which was redundant rather than wrong; the uninformative one is
    gone, so that no driver in the tree has two.

    One hole in GXHTC3 is **left open deliberately**: its explicit cleanup tests
    `(NULL == item_temperature) && (NULL == child_device_hum)`, so if the child
    *device* is created but its item is not, neither item exists to own the
    allocation and nothing frees it. That needs `add_item_to_device` to fail,
    which is an out-of-memory path, and the condition cannot be tightened to
    name the humidity item without restructuring the function — the item
    variable is scoped inside the block that creates it. It is a leak on a path
    that is already failing to allocate, which is a worse trade to fix badly
    than to leave recorded.

  - **`is_user_arg_unique` is now documented at its declaration**, including
    where the flag belongs when the arg is shared between a parent device and
    its children: on the parent's item, because the child can be removed on its
    own.

  - **`ezlopi_device_free_single()` collapses duplicate ownership before
    freeing anything.** This is containment, not the fix: it turns a future
    miscount into a leak instead of heap corruption. It deliberately does not
    invent an owner where there is none, so flagging *none* still leaks — which
    is the half a static check cannot enforce, see below.

  [`tests/ci/check_user_arg_owner.py`](tests/ci/check_user_arg_owner.py) keeps
  it fixed: at most one owner per driver `.c`. That is a proxy, and the check
  says so — it cannot see which device an item belongs to, so it cannot
  distinguish two owners of two allocations (fine) from two owners of one (a
  double free), and it requires the stronger property. No file needs two.

  **The check was verified failing before it was trusted.** Run against the
  tree as committed *before* these fixes, with nothing changed but the check
  dropped in, it names exactly the four drivers above (plus the joystick's
  redundant pair) and exits 1. It also fails if `ezlopi_device_free_item()`
  stops honouring the flag — which would leave every driver compiling and every
  shared arg leaking — or if the collapse loop is removed.

  **Its comment-stripper reported line numbers that did not exist.**
  `strip_comments()` dropped the newlines inside `/* */` blocks, so the text it
  handed to `owners_in()` was shorter than the file and every line number after
  the first multi-line comment was low by however many lines those comments
  spanned. The counts were unaffected, which is why the gate still passed and
  failed in the right places and why this survived: the only wrong output was
  the part a maintainer actually follows. Pointed at a driver with a long
  licence header it named lines 607-612 for assignments really at 680-685 — a
  73-line error landing in an unrelated function.

  The five existing fixtures all passed throughout, because every one of them
  was a single line and none contained a comment spanning two. **Six fixtures
  now assert the position rather than the count** — an owner after a four-line
  block comment, after two of them, between two, after a `//` line, and after a
  continued string literal — and three of those six fail against the old
  stripper, which makes the self-test refuse to run the gate at all. That is
  the property worth having: a line number this check prints is a line number
  in the file.

  Found by cross-checking one row of its output against `grep -n` while
  measuring a different tree, not by reading the function.

  The leak half is **not** enforced, and that is deliberate: 12 driver files
  assign `user_arg` and name no owner, and they are mostly right to.
  [`sensor_0026_ADC_LDR.c:268`](ezlopi-sensors-devices/sensor-0026-ADC-LDR/sensor_0026_ADC_LDR.c#L268)
  and
  [`sensor_0061_digitalIn_reed_switch.c:292`](ezlopi-sensors-devices/sensor-0061-digitalIn-reed-switch/sensor_0061_digitalIn_reed_switch.c#L292)
  store a **string literal** there as a last-reported-value slot, where
  flagging an owner would hand a literal to `free()`; and
  `l_ezlopi_device_settings_v3_t` has its own `user_arg` with no ownership flag
  at all, which is what the `*_settings.c` files in that set are using. So
  "assigns `user_arg`, therefore must name an owner" is false 12 times over,
  and a check asserting it would be wrong rather than strict. Whether any of
  those 12 is a genuine leak is not yet established either way.

  **Not verified:** that a device whose item fails on real hardware takes the
  fixed path on a board. The repair is on the free path, and nothing has been
  flashed.

- **Ownership of an mDNS TXT value was decided by comparing the string, which
  worked for the one literal somebody thought of.** The mDNS advert builds a
  list of TXT records, each node holding an `mdns_txt_item_t` whose `value` is
  sometimes a heap string this code allocated and sometimes a string literal.
  The free routine told them apart like this
  ([`ezlopi_core_mdns.c`](ezlopi-core/ezlopi-core-mdns/ezlopi_core_mdns.c)):

  ```c
  if (mdns_context_head->mdns_context->value && 0 != strncmp(mdns_context_head->mdns_context->value, "ezlopi_generic", 14))
  {
      ezlopi_free(__FUNCTION__, (char *)mdns_context_head->mdns_context->value);
  }
  ```

  `"ezlopi_generic"` is what `EZPI_core_factory_info_v3_get_device_type()`
  returns — it is a `return "ezlopi_generic";` with no allocation behind it —
  so the comparison is a literal being recognised by name. There is a second
  literal in the same list. Every other node assigns its value as

  ```c
  service_cntx_name->value = (NULL == EZPI_core_factory_info_v3_get_name() ? "null" : EZPI_core_factory_info_v3_get_name());
  ```

  and a getter that returns `NULL` therefore puts `"null"` in the node, which
  the comparison does not recognise and the routine passes to `free()`.

  That is the same mistake as the `user_arg` entry above in a different place:
  **whether a pointer must be freed is a property of how it was obtained, not
  of what it contains**, and inferring it after the fact gets the cases nobody
  enumerated. The node now carries a `value_is_heap` flag set where the pointer
  is obtained, and the flag is documented at its declaration.

  Three more defects were in the same lines:

  - **Each getter was called twice** — once to test for `NULL`, once to use —
    and `__EZPI_core_factory_info_v3_read_string()` reads the partition into a
    fresh `ezlopi_malloc` on every call. So the first call's string was
    allocated, tested and dropped unreferenced: one leaked allocation per node,
    per boot. Fixed by holding the result in a local, which is also what makes
    the ownership flag derivable rather than guessed.

  - **The list nodes were never freed** — only the `mdns_txt_item_t` each node
    points at. Two nodes in the committed configurations, six with the extra
    services.

  - **`ezlopi_mdns_service_cntx` was never cleared**, so the static head
    outlived the contents it described. It is now cleared *before* the walk
    rather than after, so there is no window in which the head names freed
    memory.

  The free routine is reached from the **success** path, not only from retry
  exhaustion, so the node leak and the double-call leak ran on every boot that
  got as far as advertising. Only the free-of-a-literal needed
  `CONFIG_EZPI_MDNS_ENABLE_EXTRA_SERVICES`. The Known-gaps entry that had
  deferred this said the whole thing was unreachable in every shipped
  configuration; that is corrected where it stands.

  **Verified by compiling both configurations, because one of them compiles
  nothing.** `CONFIG_EZPI_MDNS_ENABLE_EXTRA_SERVICES` is `default n` in Kconfig
  and `is not set` in all four `sdkconfig.defaults.*`, so the four
  extra-service blocks — four of the six edited — are built by no configuration
  in the repository. A green four-target run would have said nothing about
  them.

  The four committed configurations, against the sizes measured at the previous
  commit, and then `esp32` with all five symbols forced on, which is the only
  way to compile the other four blocks:

  | target | `Ezlo_Pi_v4x.bin` | was | delta |
  | --- | --- | --- | --- |
  | esp32 | 1,683,856 | 1,683,856 | 0 |
  | esp32s2 | 1,374,192 | 1,374,192 | 0 |
  | esp32s3 | 1,698,720 | 1,698,720 | 0 |
  | esp32c3 | 1,757,104 | 1,757,120 | −16 |
  | esp32 + extra services | 1,684,208 | — | — |

  All five `rc=0`, and zero warnings naming `ezlopi_core_mdns.c` or its header
  once the file's two pre-existing `#warning` directives are excluded — they are
  `-Wcpp`, they predate this change, and counting them would have masked a real
  warning behind a nonzero baseline. **The 16 bytes off esp32c3 are not
  attributed to any one edit.** Three of the changes are compiled by the
  committed configurations and all three move code: the `device_type` getter
  going from two calls to one, the dead `"null"` arm dropped from the serial
  node, and the rewritten free routine, which is longer than what it replaced
  because it now frees the nodes as well as the items. None was measured in
  isolation. Every size in the table is a multiple of 16, so a net change smaller
  than the padding quantum would show as zero — which is the likeliest reason
  the three Xtensa targets did not move, and a reason not to read their `0`
  as "no code changed". Size is the instrument here rather than a hash because
  `CONFIG_APP_REPRODUCIBLE_BUILD` is off, so identical sizes are not a claim
  that the images are byte-identical either.

  The extra-services pass asserts that the five symbols actually reached the
  generated `sdkconfig.h`, because a configuration that silently failed to apply
  would have produced the same `rc=0`.

  **A CI job now builds that configuration**, so these four blocks stop being
  code that nothing compiles — `firmware-mdns-extra`, appending
  [`tests/ci/sdkconfig.mdns_extra_services`](tests/ci/sdkconfig.mdns_extra_services)
  to `sdkconfig.defaults.esp32`. It asserts each of the five symbols in both
  `sdkconfig` *and* `build/config/sdkconfig.h`, since those can disagree and it
  is the header the `#ifdef`s are resolved against. Unlike the OTA, Improv and
  MQTT jobs it carries **no `nm` assertion**, deliberately: every symbol those
  blocks reference is already in every image — `get_manufacturer` alone has six
  other call sites — so an `nm` check would pass on the plain `esp32` build too,
  and a check that cannot fail is worse than none because it reads as evidence.
  The job's own mechanism was verified by running its assertion twice: it passes
  with the fragment appended and fails with it absent.

  **Not verified:** any of it on hardware. The `"null"` free needs a factory
  partition with an unset field *and* the extra services enabled, and neither
  the leak nor the fix has been observed on a board — the argument that the
  values are heap is from reading `__EZPI_core_factory_info_v3_read_string()`,
  not from watching a heap.

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

  This one was found by building with `-Wshadow`, which at the time was not
  enabled. The sweep found nine unique shadowed declarations; the other eight
  were each read afterwards and none changes behaviour, so this is the only one
  of the nine that was a defect. All nine have since been dealt with and the
  flag is now on, as an error, under *Build and configuration* below — and
  this defect is of the class the enabled spelling catches, a block-scope
  declaration hiding a function-scope one, so the guard that now exists would
  have stopped it.

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

- **A `strlen()` on address `0x1`, because the guard in front of it tested the
  wrong one of two type fields — and a `!= NULL` test cannot catch it.**
  [`ISSUNSTATE_eval_range`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L662)
  read `field_value.u_value.value_string` behind a test on
  `curr_field->value_type` plus a null check, then handed the result to
  [`ISSUNSTATE_check_mdrn`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L718),
  which calls
  [`strlen`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L723)
  on it. Both halves of that guard look right and neither is.

  `l_fields_v2_t` carries two type-ish members, and they are not the same
  question. `field_value.e_type` is the union discriminant: it is set by
  [`EZPI_scenes_populate_fields_get_value`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_populate.c#L494)
  from the *actual JSON type* of the `value` member, on the line next to the
  write into the union. `value_type` is the declared schema type: it is set by
  [`EZPI_scenes_populate_assign_field`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_populate.c#L633)
  from the `"type"` *string* in the scene JSON. Nothing cross-checks the two, so
  `{"type":"token","value":true}` is an accepted field whose `value_type` says
  TOKEN and whose `e_type` says BOOL. Testing `value_type` therefore tells you
  what the field was *labelled*, not which union member is live.

  The null check is the part worth dwelling on, because it is the reason this
  survived a reading pass. A field is `memset` to zero at allocation and
  `u_value.value_bool = true` writes one byte of
  [`u_field_value_v2_t`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_v2_type_declaration.h#L129),
  so `value_string` reads back as `0x1` — non-null, so `NULL !=` passes, and
  `strlen` then walks address 1. A number is the same shape with the low word of
  the double standing in for the pointer. The question was never whether the
  pointer is null; it is whether it is a pointer.

  That is not reasoning that deserves to be taken on trust, so it is measured:
  the first new check in
  [`tests/host/test_scenefields.c`](tests/host/test_scenefields.c) performs
  exactly the write the `cJSON_True` arm performs and prints the value it reads
  back out of the other member.

  **Reachable by anyone who can create or edit a scene**, which is the part that
  makes it a [sec] item rather than a tidiness one. The isSunState dispatcher
  selects a handler by field *name* and never looks at the type at all —
  [`EZPI_STRNCMP_IF_EQUAL`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods.c#L581)
  against a six-entry name table — so a field named `range` reaches this
  function whatever its declared or actual type, and the handler's own
  `value_type` test was the only thing in the way.

  **The fix is to read the discriminant, which is what the rest of the component
  already did.** The serializer does it for this exact schema type — the TOKEN
  arm of `__cjson_add_fields` dispatches on
  [`e_type`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_cjson.c#L761) and
  *warns* rather than reading the union when it is neither a string nor a cJSON.
  So does the destructor:
  [`EZPI_core_scenes_delete_field_value`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_delete.c#L248)
  switches on `e_type` to decide what the union holds. A TOKEN carrying a
  non-string is a state two other readers already treat as legitimate; this one
  was the deviation. It is also the second site of this shape fixed on this
  branch — the sibling
  [`ISSUNSTATE_get_offs_tmval`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L586)
  was corrected under *Scene HTTP requests* — which is why the remainder is
  recorded as a class under *Known gaps* rather than as one more line item.

  **De-unioning the value was considered and rejected on a measured cost.**
  Making `u_field_value_v2_t` a `struct` would end this defect class at the
  layout level rather than one reader at a time, and it was tempting because
  this tree already contains an "a struct, not a union" fix — the
  `s_item_expn_data_t` item in the next section. That precedent does not apply:
  there the tag and the value shared storage, so storing the value destroyed the
  tag. `s_field_value_t` is already the correct tagged-union shape; the tag was
  present and simply not read. And the cost is real — compiled both ways,
  `sizeof(u_field_value_v2_t)` goes 8 → 48 and `sizeof(l_fields_v2_t)` 88 → 128,
  so 40 bytes per field on every field of every block of every scene, to fix
  readers that should be reading the tag they already have.

  **No diagnostic is emitted on the mismatch arm, deliberately.** This function
  runs about once a second, so a trace here would be precisely the log flood the
  retry backoff under *Scene HTTP requests* was written to stop. The sibling
  `ISSUNSTATE_get_offs_tmval` *does* trace its equivalent mismatch and can,
  because its once-per-day gate rate-limits it; there is no equivalent gate
  here. The cost is that a mistyped `range` field now makes the scene quietly
  not fire instead of crashing — better, but not good, and listed under *Known
  gaps* with the place a once-per-scene check belongs.

  **Evidence, and what it does and does not cover.** The three new checks are
  layout claims about the union, not tests of the guard: this file has no host
  harness — it needs NVS, HTTP, a clock and five sibling scene modules — so the
  host suite covers the *premise* and the build sweep covers the wiring, and
  neither substitutes for the other. `test_scenefields` goes 18 → 24 checks, 0
  failures, clean under `-Werror` with ASan and UBSan.

  The control is the layout itself, which is the only way to show these checks
  are not tautologies: with `union` changed to `struct` the bool check fails
  with `a stored \`true\` reads back as value_string (nil)`, and restoring the
  union returns it to 24 passing. A third check is the opposite control — a
  token that really does hold a string still passes the discriminant test — so
  the first two cannot be satisfied by a guard that rejects everything.

  Four targets cross-built against a `git archive HEAD` baseline, 0 errors on
  either side: esp32 +16 bytes of image, esp32s2 +16, esp32s3 +16, esp32c3 +16.
  Both controls held constant on every target, which matters more than usual
  here: the whole translation unit sits behind
  `CONFIG_EZPI_SERV_ENABLE_MESHBOTS`, so a config drift would make a build that
  compiled none of this indistinguishable from a successful one. The one warning
  reported in this file on each side of each target is an upstream
  `#warning "need to optimize these switch-case"` directive, not a diagnostic
  about code; it is identical before and after.

  **One thing the sweep taught about the sweeps.** `.flash.text` is not a clean
  measure of a change in this tree, and the build numbers in this document
  should be read accordingly. The sweep was run twice over code that was
  byte-identical between runs — only a comment had grown — and `.flash.text`
  reported `+12/+12/+12/+10` the first time and `+16/+16/+16/+12` the second.
  The cause is that every `TRACE_*` macro expands to a call carrying `__LINE__`
  as an integer argument
  ([`ezlopi_util_trace.h`](ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h#L172)),
  and 71 of this file's 89 trace calls sit below the edit, so adding comment
  lines re-encodes 71 immediates and a few of them change instruction width.
  The image size was `+16` on all four targets in *both* runs, so that is the
  number quoted; the section attribution is not stable enough to quote.

  Untested on hardware. No board has evaluated an isSunState scene.

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

- **"20 minutes before sunrise" fired forty minutes after it, and "30 seconds
  after" fired six hours later.** The offset half of an isSunState trigger —
  [`__issunstate_add_offs`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L2010)
  — had seven defects in it. Its companion entry under *Scene HTTP requests*
  fixed the half that fetches the suntime; this is the half that moves it, and
  until now it moved it to the wrong place for every offset anybody would
  actually type.

  Each one was established by compiling the shipped function **verbatim** and
  running it, not by reading it. Every number below is from that probe.

  **The parser could only read the three-field form.** It parsed `hms_interval`
  by finding colons with `strtoul`, and its two fallback branches — the ones
  that give `"30"` and `"5:30"` their meanings — both sat behind
  `if (NULL != ptr1)`. `strtoul` *always* stores through a non-NULL `endptr`,
  including the no-conversion case, where it stores the input pointer back. So
  the test never failed, the branches never ran, and the first field was always
  hours. Measured against a 00:00:00 suntime in `after` mode, so that the
  result *is* the offset:

  | offset | meant | the shipped code produced |
  |---|---|---|
  | `"30"` | 00:00:30 | **06:00:00** |
  | `"5:30"` | 00:05:30 | **05:30:00** |
  | `"12:34:56"` | 12:34:56 | 12:34:56 — correct |

  Only the shape that names all three fields worked. `"30"` became thirty
  hours, which the function's single wrap guard reduced to six.

  **The arithmetic had no borrow or carry.** It reduced hours, minutes and
  seconds as three independent fields, so a minute that needed to borrow from
  the hour borrowed from nothing:

  | | wanted | got |
  |---|---|---|
  | `0:20:00` *before* a 06:10:00 sunrise | 05:50:00 | **06:50:00** |
  | `0:20:00` *after* a 06:50:00 sunset | 07:10:00 | **06:10:00** |
  | `0:00:30` *before* 06:10:10 | 06:09:40 | **06:10:40** |
  | `1:30:00` *before* 06:10:00 | 04:40:00 | **05:40:00** |
  | `1:00:00` *before* 06:10:00 | 05:10:00 | 05:10:00 — correct |

  The second row is the one worth staring at: *after* moved the trigger
  earlier. Both directions were wrong, and wrong in the opposite direction to
  the one requested, whenever the offset's minutes exceeded the suntime's —
  which is most of the offsets a person would set. Only a whole number of
  hours, needing no borrow, came out right.

  **Nothing was range-checked, and `strtoul` accepts a leading minus.** An
  out-of-range result is not a loud failure here. The moment is compared to the
  wall clock *for equality* by
  [`ISSUNSTATE_check_mdrn`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L718),
  so a trigger at 81:39:39 is not an error — it is a scene that sits in the
  list, reports nothing, and never fires:

  | offset, against a 06:00:00 suntime | the shipped code produced |
  |---|---|
  | `"-1"`, `after` | **05:00:00** — a negative offset silently inverted the direction |
  | `"99:99:99"`, `after` | **81:39:39** |
  | `"500:00:00"`, `before` | **−470:00:00** |

  **It read uninitialised stack.** The offset was copied into a `char[10]` with
  `snprintf`, which writes the string and one terminator; index `[9]` was then
  set by hand. For any offset shorter than nine characters the bytes between
  are never written, and `strtoul(ptr1 + 1)` reads straight into them. With the
  stack primed with a known byte, `"5"` against a 00:00:00 suntime produced
  **05:7777717:00** — the minutes came out of the gap, not the offset.

  **And it read one byte past the end of that array.** Nine digits put `ptr1`
  on `time_diff[9]`, so `ptr1 + 1` is `time_diff[10]`. `"123456789"` produced
  **123456765:08:00**.

  **The same array also truncated silently**, which is the third defect that
  one `char[10]` carried. `snprintf(time_diff, 10, "%s", tm_offs_val)` keeps
  nine characters and a terminator, so a longer offset lost its tail with no
  return-value check and no trace — and truncation is the failure mode that
  turns a string the parser would have refused into one it accepts, since what
  is cut off is the end, where the trailing-character check would have looked.
  This one is read off the code rather than probed: unlike the two above it
  needs no primed stack to see.

  That last one is the most useful result in this entry, because **the
  sanitizers do not catch it.** Under `-fsanitize=address,undefined` the probe
  read that byte and exited 0 with no report. Rather than assume why, a second
  probe read the identical byte two ways: read directly it is a reported
  `stack-buffer-overflow ... READ of size 1 ... <== Memory access at offset 106
  overflows this variable`; read through `strtoul` it is silent. ASan
  instruments the code it compiles, and libc is neither compiled with it nor
  interposed for `strtoul`. So this defect was invisible to the one tool most
  likely to be pointed at it — which is a fair part of the answer to how it
  survived in shipped firmware.

  **Two more were in the caller, not the parser.**
  [`ISSUNSTATE_get_offs_tmval`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L586)
  used "is `defined_moment` all-zero?" as its do-this-once test, and that
  sentinel cannot do either job it was given. It cannot mean *already done*,
  because 00:00:00 is a time a real offset produces, so a scene whose moment
  landed on midnight recomputed it on every single evaluation. And it cannot
  mean *do it again tomorrow*, because nothing ever put the zeroes back — in
  the whole translation unit the only writes to `defined_moment` were the ones
  inside `__issunstate_add_offs` itself, reached only through that same
  all-zero test. The moment was therefore
  computed on the first evaluation after boot and kept for as long as the hub
  stayed up, while the suntime underneath it was refreshed daily. **An
  isSunState scene tracked the sun for one day and then became a fixed-time
  scene**, drifting a minute or two per day — in the direction that looks like
  a hub with a slightly wrong clock rather than a hub with a bug.

  The other was a union read without its tag. The caller tested
  `NULL != value_string` before using it, which is not the question: `e_type`
  is the discriminant, and `value_string` and `cj_value` are the same bytes. It
  matters here specifically, because
  [`EZLOPI_VALUE_TYPE_HMS_INTERVAL`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_populate.c#L578)
  is named explicitly in the `cJSON_Array` arm of field population, which
  stores
  [`VALUE_TYPE_CJSON`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_populate.c#L581).
  An offset arriving as an array or an object is a `cJSON *`, and
  `NULL != value_string` passes for it — every member of that union is nonzero
  when it is set — so a node address went to `snprintf("%s")`.

  **Read as a chain, every one of these hid the next.** It did not matter that
  the offset was unrange-checked, because the field widths were wrong anyway.
  It did not matter that the field widths were wrong, because the arithmetic
  could not carry. It did not matter that the arithmetic could not carry,
  because the answer was computed once and then frozen. And none of it
  surfaced, because the failure mode throughout is a scene that fires at a
  plausible-looking wrong time or does not fire at all — never a crash, never a
  log line.

  **The fix moves both calculations out to where they can be run on a
  development machine**, next to the three functions the companion entry moved
  there:
  [`EZPI_core_suntime_parse_offset`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L291)
  and
  [`EZPI_core_suntime_shift`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L382).
  The parser counts its fields and lets the count choose their meaning, which
  is the rule the shipped code was reaching for; the shift works in
  seconds-since-midnight, so there is no borrow to get wrong — a single
  `((t % 86400) + 86400) % 86400` handles both directions and the midnight wrap,
  and C's sign rule for `%` is why the addition is in the middle. Both
  range-check and both reject rather than salvage. `__issunstate_add_offs` is
  now a `bool`: parse, apply the sign, shift, and write the three outputs
  together only on success.

  The caller keeps two new fields instead of the sentinel,
  [`moment_tm_day`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.h#L143)
  and
  [`moment_valid`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.h#L144),
  so the moment is computed once per day and once per failure. Storing the
  *day* rather than a plain done-flag is what makes it correct in either field
  order — the dispatch loop walks the scene's own field list, so the offset
  field can be visited before the sunrise field, and comparing against
  `curr_tm_day`, which only advances once a suntime has actually been read,
  means the moment is never computed from yesterday's suntime. And both places
  that compare the moment to the clock —
  [`ISSUNSTATE_eval_range`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L662)
  and
  [`ISSUNSTATE_check_flag_result`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L763)
  — now require `moment_valid` first, because the struct's own zeroes read as
  00:00:00, a time the clock reaches every night.

  **Evidence.**
  [`tests/host/test_suntime.c`](tests/host/test_suntime.c) grew from **109**
  checks to **206**, at **0** failures. The load-bearing one is a sweep over
  **all 86400** seconds of the day: each is stepped forward and back one second
  and compared against an independent reference, and then round-tripped through
  **18** offsets. **0** mismatches. The reference is deliberately *not* the
  subject's method — it is an explicit three-field carry chain, the shape the
  shipped code was trying to be, written so that it actually carries. Two
  different methods agreeing at every second of the day is worth more than one
  method restating itself.

  **22** mutations of the two new functions were run. The control row — no
  mutation — sits at **206** checks and **0** failures both before the sweep
  and after the restore, which is the row that makes the 22 mean anything:
  without it, "21 detected" and "the harness fails on everything" print the
  same. **21** were detected and **1** survived.

  **Three** of the 21 were caught by a sanitizer and by nothing else, and that
  is the methodological finding worth recording: UBSan recovers by default, so
  a mutant can trip a runtime error and still print a tally. These three
  printed no tally at all — inverting the colon test, dropping the input NULL
  guard, and dropping the output NULL guard each killed the binary outright. A
  sweep that scored only the failure count would have filed all three as
  survivors. Counting a sanitizer report as a detection in its own right is
  what caught them.

  The survivor is an **equivalent** mutant and is recorded as one rather than
  chased. Growing `int field[3]` to `field[4]` changes no behaviour, because
  the array size is not what limits the field count — `while (count < 3)` is,
  so `count` only ever takes 0, 1 or 2 and the fourth slot is unreachable
  storage. That was measured rather than reasoned: the four-field rejections
  `"1:2:3:4"` and `"20:00:00:00"` are among the **9** checks that fail when the
  *trailing-character* check is removed instead, which is where the rejection
  actually comes from. The mutant stays in the list so the claim is re-measured
  next time rather than remembered.

  **Ten** checks assert in the direction of the bug, each number taken from the
  probe above rather than from the new implementation: `"30"` is 30 seconds and
  not 30 hours, `"5:30"` is 330 seconds, twenty minutes before a 06:10:00
  sunrise is *not* 06:50:00 and *is* 05:50:00, `"99:99:99"`, `"-1"` and
  `"500:00:00"` are refused with the output left untouched, and `"5"` is five
  seconds. Reinstating any of the shipped answers fails the suite. The
  uninitialised-stack read is the one defect with no assertion of its own, and
  the test says so at the site rather than leaving the gap implicit: there is
  nothing observable from outside the function to assert on, and the only
  defence is that the string is now accounted for in full or refused.

  All four targets build and link clean — esp32, esp32s2, esp32s3 and esp32c3 —
  and all three `EZPI_core_suntime_*` symbols are present in every one of the
  four images. On its own that proves only that *something* compiled, so the
  sweep was run twice: once against this tree and once against a `git archive`
  of the committed tree, eight builds in total. Both new symbols are **absent**
  from all four baseline images and **present** in all four fixed ones. The
  absence half is the half that carries weight here, because the whole scenes
  translation unit sits behind `CONFIG_EZPI_SERV_ENABLE_MESHBOTS`: with
  meshbots off these functions compile to nothing, the image still links, and
  the build still reports OK. A guard that stopped reaching the file is now
  caught by the symbol check rather than quietly shipping none of this.

  | target | image delta | `.flash.text` | `.flash.rodata` |
  |---|---|---|---|
  | esp32 | +480 | +336 | +152 |
  | esp32s2 | +464 | +308 | +144 |
  | esp32s3 | +448 | +296 | +152 |
  | esp32c3 | +320 | +168 | +160 |

  The two loadable sections sum to +488, +452, +448 and +328, so the image
  delta lands 8 bytes below that sum on esp32, 12 above on esp32s2, exactly on
  it on esp32s3 and 8 below on esp32c3 — segment alignment padding, and the
  same shape of residual the earlier suntime change reported. Reported rather
  than rounded: a reconciliation forced to balance exactly stops being a check.
  The all-sections total the sweep script printed is *not* this figure and
  should not be read as it — that line sums debug sections too, and on esp32c3
  it comes out negative against a positive image delta for exactly that reason.

  The full host suite is **27** binaries and **5758** checks at **0** failures.

  **One thing here is assumed rather than verified, and it is flagged in Known
  gaps:** the wire shape of `hms_interval`. There is no sample payload anywhere
  in this tree, so whether the controller sends `"0:20:00"` as a JSON string or
  an object is undetermined. The `e_type` gate makes either safe — the
  non-string case now traces what it got and refuses, rather than reading a
  cJSON node as a string — but which branch a real controller takes is
  untested. Nothing in this entry has been run on hardware.

- **No `isSunState` scene condition could ever fire. The function that decides
  deciphered its own input incorrectly and returned 0 for every one of its 512
  possible inputs.**
  [`ISSUNSTATE_check_flag_result`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c)
  accumulates a bitmask describing which scene fields were present and which
  matched, then `switch`es on it against eight case labels. Bit 4
  (`MASK_TIME_FLAG`) is set while the time condition is being evaluated and is
  consumed by the condition guarding the switch — but it was never cleared
  before the switch ran. So `flag_check` always arrived with 0x10 set, none of
  the eight labels include that bit, every input fell through to `default`, and
  `default` returned false. The fix is one line: clear the bit once it has been
  used.

  This is the "looks like a working product" failure mode in its purest form.
  A sunrise/sunset scene saved from the app, listed by the hub, and reported as
  enabled simply never ran. Nothing logged, nothing errored, and the only
  symptom was a scene that stayed quiet — indistinguishable from a scene whose
  conditions were genuinely not met, which is exactly what a user debugging it
  would assume.

  **Evidence, and the reason there is any.** The containing translation unit
  was previously believed un-hostable, which is why this sat behind no test.
  That belief was wrong: it needed a `freertos/task.h` stub and three warning
  suppressions, and nothing else. New
  [`tests/host/test_issun.c`](tests/host/test_issun.c) is **29** checks at
  **0** failures, built against the real translation unit rather than a copy.
  The load-bearing check sweeps **all 512** `(flag_check, clock-matches)` pairs
  and asserts the firing set is exactly the 16 derived independently from the
  case labels.

  **18** mutants were run against the subject (`probe/eti5.sh`,
  `probe/eti10.sh`): **14 killed, 4 survived**, with the subject restored
  byte-identical and green afterwards. Two results carry the weight:

  - **Deleting the mask-clear — which reproduces the code exactly as it
    shipped — is killed, with 11 of the 29 checks failing** and the sweep
    printing `0 of 512 pairs fire`. That is the measurement behind the claim
    at the top of this entry: not an inference from reading the switch, but
    the shipped function enumerated over its whole input space.
  - **Dropping the union-discriminant clause added in `0b72341` is killed by
    an AddressSanitizer SEGV on address 0x1**, from `strlen()` reading a small
    integer out of the union as a pointer. That closes a gap recorded against
    that commit — *"Mutating the layout fails a check; deleting the guard fails
    nothing"* — which is now false, and the entry above has been corrected.

  The 4 survivors are recorded rather than chased. Two were predicted: every
  case on this path declares its field type as `TOKEN` and carries a real
  string, so the schema-type and NULL clauses are unreachable from any input a
  scene can produce. One is a genuine coverage gap — no test calls
  `ISSUNSTATE_get_suntime`, so a mutant setting the wrong bit in it survives,
  and that is listed in [Known gaps](#known-gaps). The fourth is an
  **equivalent** mutant: removing the NULL check on the range word changes
  nothing, because `EZPI_STRNCMP_IF_EQUAL` NULL-checks both its arguments
  itself and the length ternary guards the `strlen`. I had predicted that one
  would be killed and was wrong, for the ordinary reason — I had not read the
  macro.

  **Untested on hardware.** No board has evaluated an `isSunState` condition
  before or after this change. The 512-pair sweep is a host loop over the real
  function; whether a hub now fires a sunrise scene is not something this
  establishes, and the clock and suntime inputs it depends on are stubbed here.

  Two things found while making the file hostable are recorded as gaps rather
  than fixed, to keep this commit to one change: ten `-Wsign-compare` sites in
  the `WHEN_function_for_opr/repeat/follow/pulse` timing family, comparing a
  `uint32_t` tick count against an `int` threshold (none of them on the
  `isSunState` path — I mis-measured this once and said so), and a wrinkle at
  `flag_check` 0x89, where a range is present and the moment is exact. Both are
  in [Known gaps](#known-gaps). The suppressions are `-Wno-error=cpp` rather
  than `-Wno-cpp`, deliberately, so upstream's `#warning` about the switch
  ladder keeps printing.

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

- **A hilink presence sensor that stopped answering rebooted the hub once a
  second, and would have gone on republishing its last reading if the panic
  were merely deleted.** `sensor_0067_hilink_presence_sensor.c` wrapped six
  calls in `ESP_ERROR_CHECK`. This project builds with
  `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y` and no `NDEBUG`, so that
  macro is the panicking variant: a non-zero result is a reboot, not a log line.

  The worst of the six is in `__notify()`, the `EZLOPI_ACTION_NOTIFY_1000_MS`
  handler, which `__device_loop()` in
  [ezlopi_core_ezlopi.c](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c)
  drives on a 1-second timer:
  `ESP_ERROR_CHECK(ld2410_get_data(&present_hilink_data))` at the pre-fix line
  178. Line numbers in this entry are the *pre-fix* ones and are deliberately
  not linked — the lines they name no longer exist, so a link would resolve to
  whatever now occupies that offset. A loose Dupont wire or a brown-out on the radar's 5 V
  rail therefore took the whole board down, every second, for as long as the
  fault lasted — every other configured device with it. That needs no
  authentication and no cooperation from anything but the sensor.

  **Deleting the macro alone would have been worse than the reboot, and that is
  why the read now also skips the body.** `present_hilink_data` is a copy of the
  stored reading taken immediately above the call, and `ld2410_get_data()`
  leaves the caller's buffer untouched when it cannot assemble a frame. So on a
  failed read the two structs are equal, the change-detection helper finds
  nothing to broadcast — which *looks* correct — but `sec_count` keeps advancing
  toward the 90-tick forced update, which copies the stale copy back over the
  stored reading and broadcasts all three items. The hub would have gone on
  publishing a reading from before the sensor died, with a fresh timestamp,
  indefinitely. A sensor that is silently stale is harder to notice than one
  that stops reporting, which is why the fix is `if (ESP_OK == ...)` around the
  whole body rather than a dropped macro.

  The other five, and what each one did:

  | Pre-fix line | Wrapped call | Returns | Reached by |
  |---|---|---|---|
  | 178 | `ld2410_get_data()` | `esp_err_t` | the 1-second notify timer |
  | 243 | `__add_array_to_object()` | `ezlopi_error_t` | nothing — see below |
  | 266 | `__add_value_to_cjson()` | `ezlopi_error_t` | a cloud `hub.item.value.get` |
  | 281 | `__add_value_to_cjson()` | `ezlopi_error_t` | the scene engine and the local API |
  | 305 | `HILINK_presence_sensor_apply_settings()` | plain `int`, 1 on failure | boot, after the UART handshake |
  | 306 | `ld2410_get_data()` | `esp_err_t` | boot, after the UART handshake |

  Only two of the six were even passing an `esp_err_t`. Three returned
  `ezlopi_error_t`, whose failure value `EZPI_FAILED` is `-1`, and one returned
  a plain `int` that is `1` on failure — so `ESP_ERROR_CHECK` was printing those
  as though they were ESP error names on its way to panicking. The two getter
  sites also discarded the result and then wrote `ret = EZPI_SUCCESS`
  unconditionally, which only ever mattered on the path the panic made
  unreachable; both now return the real answer.

  **One of the six is unreachable and is not claimed otherwise.** Line 243's
  `__add_array_to_object()` returns `EZPI_FAILED` only on a NULL `cj_params` or
  a NULL array; `cj_params` is already tested by both callers, and the array is
  a file-scope `static const char *[]`. It is fixed because it is the same
  mistake as the other two in its family, not because the fix is observable.

  **A seventh defect, found while reading rather than from the macro search.**
  `__add_value_to_cjson()` opened by casting `item->user_arg` into a local and
  *then* wrote `if (item && cj_params && hilink_data)`. The guard's own first
  condition was already too late — `item` had been dereferenced on the line
  above the test written to protect it. Latent today: no caller in the tree
  passes a NULL item. Fixed anyway, because a guard that consumes what it is
  about to validate is wrong on its own terms, and because this is the same
  shape as the self-contradictory guard in this sensor's settings file.
  Reproduced before fixing: the pre-fix file, driven with a NULL item, is an
  AddressSanitizer `SEGV on unknown address 0x000000000078`.

  **Covered by a new host test, and every fix proved by re-breaking it.**
  [`tests/host/test_hilink0067.c`](tests/host/test_hilink0067.c) compiles the
  real driver and drives its single exported dispatcher: 14 cases, 41 checks.
  The seven `ezlopi-cloud-constants` translation units are *linked rather than
  stubbed*, which is load-bearing — `__add_value_to_cjson()` selects its branch
  by comparing `item_name` **by pointer** against the keyword globals, so a
  stub holding a `strdup`'d equal string would take no branch at all and every
  value assertion would have passed against an empty object.

  Reachability was established before any fix was written. Against the unfixed
  file the binary dies with `SIGABRT` at the first site; to see the rest, a
  throwaway build replaced the abort with a recorder, and the 14 cases reach
  lines 305, 306, 178, 266 and 281 — five of the six, the sixth being the
  unreachable 243 — and then segfault on the seventh defect. Each fix was then
  reverted one at a time in a scratch copy: six of the seven variants die
  (`exit=134`, or ASan `SEGV` for the guard) and the seventh — the one that
  drops the macro but leaves the body running, i.e. what the obvious fix would
  have produced — fails four checks including *"nothing was published from a
  reading that never arrived"*. The unmodified file passes 41 of 41.

  Verified on the pinned v4.4.6 toolchain for **all four targets**, because
  `int32_t` is `int` on xtensa and `long int` on riscv32 and this file traffics
  in error-code types:

  ```
  esp32     build=0  subject-compiled=1  subject-diagnostics=1  image=0x19cf10
  esp32s2   build=0  subject-compiled=1  subject-diagnostics=1  image=0x1512b0
  esp32s3   build=0  subject-compiled=1  subject-diagnostics=1  image=0x1a0a50
  esp32c3   build=0  subject-compiled=1  subject-diagnostics=1  image=0x1aedb0
  ```

  The one diagnostic is the same on all four and is pre-existing: the
  `#warning "NABIN ; Need to remove this global var"` the file already carried.
  The host test downgrades that with `-Wno-error=cpp` rather than suppressing it
  with `-Wno-cpp`, so it still prints. Every image is *smaller* than at
  `e4ff1ec` — esp32 `0x19d070` → `0x19cf10`, 352 bytes — which is the six
  removed macro expansions and their `__FILE__` strings, and is the only
  independent confirmation that all six really were compiled in. Only `esp32`
  has ever been run on a device; no hilink presence sensor is attached to this
  board, so the radar in the tests is a stub that counts calls and returns what
  it is told to, and the abort is a model of a panic rather than a panic.

  **What this does not fix.** A reported init failure is still ignored one layer
  up — the core's gate is `< 0` and this file returns `+0x200` — and
  `__device_loop()` still discards the notify return for every driver in the
  tree. Both are recorded under [Known gaps](#known-gaps).

### I²C sensor bus — [bug]

- **The second I²C sensor in a configuration never initialised.**
  [`EZPI_hal_i2c_master_init()`](ezlopi-hal/ezlopi-hal-i2c/ezlopi_hal_i2c_master.c#L90)
  installs the IDF driver only the first time a channel is used, which is
  correct: I²C is a bus, every sensor on it shares SDA and SCL, and the device
  is chosen by address at transfer time. But `ret = EZPI_SUCCESS` was assigned
  *inside* that guard, so a device arriving on an already-running channel fell
  out of the function with the initial `EZPI_ERR_HAL_INIT_FAILED`:

  ```c
  if (NULL == i2c_master_conf_ptr[i2c_master_conf->channel])
  {
      ...
      i2c_master_conf_ptr[i2c_master_conf->channel] = i2c_master_conf;
      ret = EZPI_SUCCESS;
  }
  /* no else: a device sharing the bus returned failure */
  ```

  Eight of the nine callers are shaped
  `if (EZPI_SUCCESS == EZPI_hal_i2c_master_init(...))` and abandon the sensor on
  anything else, logging *"I2C init failed"* to a serial console nobody is
  watching. And this is not a narrow case: **every I²C driver in the tree
  hardcodes `I2C_NUM_0`** rather than reading a channel from the configuration,
  so "a second device on the channel" means any second I²C sensor at all,
  whatever pins it names.

  Concretely, the configuration the project itself documents at
  [`ezlopi_core_factory_info.h:225`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.h#L225)
  — a BME280 and a TSL2561 on GPIO 21/22, addresses 118 and 57 — could only
  ever have produced one working sensor. Two sensors on one bus is the ordinary
  way to use I²C.

  The fix adds the missing `else`: if the channel is already up, agree that it
  is the bus this device asked for and report success. The agreement is a
  comparison of SDA and SCL, not an unconditional yes, because the two are
  different bugs. One channel cannot drive two pin pairs, so a device naming
  different pins is refused with a log naming both pairs — previously it would
  have been handed the first device's pins and sat in the device list never
  responding. Clock speed is not compared: every driver hardcodes 100 kHz, so it
  cannot currently disagree, and refusing on it would add a failure mode without
  removing one.

  Evidenced by [`tests/host/test_i2c_shared_bus.c`](tests/host/test_i2c_shared_bus.c)
  — 30 checks, and the first HAL compiled in the host suite. It asserts on the
  `i2c_driver_install()` **call count** as well as the return value, because
  returning success while installing twice would be a different defect with an
  equally green test: the real `i2c_driver_install()` fails on an installed
  port and `ESP_ERROR_CHECK` turns that into a crash.

  The test was run against a copy of the HAL with the `else` branch deleted —
  the pre-fix source — and fails there with 4 of 30 checks down, all of them
  the sharing cases, while the first-device and NULL/disabled cases still pass.
  That asymmetry is the point: it shows the test is sensitive to this change
  specifically rather than globally. Worth recording that the *refusal*
  assertions pass vacuously against the pre-fix code, which refused everything —
  they only carry weight alongside the sharing assertions, and the test says so
  at the case that contains them.

  Four-target build green with no warning in the file. `esp32c3` is a real check
  rather than a fourth repetition here: it has `SOC_I2C_NUM 1`, so the channel
  table is a single element there, against the host stub's two.

  Not fixed, and recorded under Known gaps instead:
  `EZPI_hal_i2c_master_deinit()` is declared in the header and called from
  nowhere in the tree.

### Particulate matter sensor — [bug]

- **Configuring a PMS5003 panicked the hub on its own success path.**
  [`PMS5003_sensor_prepare_devices_and_items`:100](ezlopi-sensors-devices/sensor-0037-pms5003-sensor/pms_dev_items_prepare.c#L100)
  built its nine devices through nine `__prepare_*_device_and_items()` helpers
  and passed each return value to `ESP_ERROR_CHECK()`, which aborts whenever
  its argument differs from `ESP_OK` — and `ESP_OK` is zero. Every reachable
  path of all nine helpers assigns `ret = 1` or `ret = -1`; the `int ret = 0;`
  each one opens with is dead, because both arms of `if (device)` assign before
  the return. So the macro aborted unconditionally, on success exactly as
  readily as on failure, at the very first call. Nothing here was
  configuration-dependent and nothing was optimised away: all four
  `sdkconfig.defaults.*` set `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y`
  with both `..._SILENT` and `..._DISABLE` explicitly unset, which selects the
  variant of the macro that calls `_esp_error_check_failed()`.

  Underneath that sat a second defect that the first one hid. In eight of the
  nine helpers, the `else` arm reached when `EZPI_core_device_add_device()`
  returns `NULL` set `ret = 1` — the same value as success. Only the
  standard-particles-1µm helper set `-1` there, which is what the other eight
  were meant to say. Replacing the macro with a plain comparison and nothing
  else would therefore have produced a sensor reporting success for eight
  devices that were never created.

  Both outcomes are now named — [`PMS_PREPARE_OK`:73](ezlopi-sensors-devices/sensor-0037-pms5003-sensor/pms_dev_items_prepare.c#L73)
  and [`PMS_PREPARE_FAILED`:74](ezlopi-sensors-devices/sensor-0037-pms5003-sensor/pms_dev_items_prepare.c#L74)
  — deliberately not `esp_err_t`, and deliberately not bare integers: `return 1`
  reads as "fine" to anyone who has not gone looking for the macro that
  disagrees, and that reading is how this got shipped.

  The caller now splits the two cases it actually has. Device 0 is handled on
  its own, because it is special three times over: it is the parent whose id the
  other eight are children of, it is the only helper that writes `*parent_id`,
  and its item is the sole owner of the shared `pms_object`. Had it failed,
  `*parent_id` would never have been written, and the remaining eight calls
  would each have been handed an uninitialised value off the caller's stack. The
  other eight are now each allowed to fail without taking the sensor down — a
  hub with seven of nine dust channels is more use than no hub — which is safe
  precisely because none of them owns `pms_object`. On the one path that does
  matter, `pms_object` is freed; it used to leak there, a leak unreachable only
  because the abort arrived first.

  Ownership was audited and deliberately left as it was. Nine items receive
  `user_arg` and exactly one claims it, at
  [`is_user_arg_unique = true`:186](ezlopi-sensors-devices/sensor-0037-pms5003-sensor/pms_dev_items_prepare.c#L186)
  on the parent device. The other eight leave the flag false implicitly rather
  than setting it, which is sound here only because
  [`memset(new_item, 0, sizeof(l_ezlopi_item_t))`:548](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L548)
  zeroes every item at allocation — worth knowing before anyone writes a
  tenth channel by copying one of the eight.

  Built green for both esp32 and esp32c3, with the object confirmed recompiled
  in each run and no new warnings from the component. The build was watched to
  fail first: an undefined call placed in this file gives `rc=2`, and
  redefining `PMS_PREPARE_OK` raises a redefinition warning at the definition
  itself, which is what proves the macro is genuinely in the translation unit
  rather than merely present in the text. **Not exercised on hardware** — no
  PMS5003 is attached here, so the abort is established by reading the macro
  and the assignments, not by watching a hub reboot.

  Not fixed, and recorded under Known gaps instead: whether removing the parent
  device frees `pms_object` while the items on children 1–8 still point at it.

### Device settings — [bug]

- **A setting the hub refused to write was answered as if it had been written.**
  The three cloud methods for device settings each called the setting's own
  handler and discarded what it said.
  [`EZPI_device_settings_value_set_v3`:176](ezlopi-cloud/ezlopi-cloud-settings/ezlopi_cloud_settings.c#L176)
  called it and ignored the result,
  [`EZPI_device_settings_reset_v3`:234](ezlopi-cloud/ezlopi-cloud-settings/ezlopi_cloud_settings.c#L234)
  did the same for every setting it walked, and
  [`EZPI_device_settings_list_v3`:96](ezlopi-cloud/ezlopi-cloud-settings/ezlopi_cloud_settings.c#L96)
  stamped `"status": "synced"` on each entry before asking whether the read had
  worked. In all three the reply was an empty success object, identical whether
  the value had been stored or rejected. An app could set a setting, get an
  acknowledgement, and read back the old value with nothing anywhere saying why.

  The convention being thrown away is not implicit. The handler is declared as
  [`int (*func)(...)`:139](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.h#L139)
  — plain `int`, zero for success — and all three drivers in the tree that
  register one (`device_0001_digitalOut_generic.c`,
  `hilink_presence_sensor_setting.c`,
  `sensor_0068_ENS160_gas_sensor_settings.c`) honour it. The value survived the
  whole way up to the cloud layer and died at the last step.

  This is the mirror image of the PMS5003 defect above, and the pair is worth
  reading together: one file aborted the hub on every return value including
  success, this one discarded every return value including failure. Both come
  from the same habit of treating a callee's answer as decoration.

  The set method now separates two silences that used to be one. An unknown
  `settingId` is `-32602 "Wrong params"` with
  [`SETTINGS_TOKEN_ID_NOT_FOUND`:74](ezlopi-cloud/ezlopi-cloud-settings/ezlopi_cloud_settings.c#L74);
  a handler that found the setting and refused it is `-32500 "Internal error"`
  with
  [`SETTINGS_TOKEN_SET_FAILED`:75](ezlopi-cloud/ezlopi-cloud-settings/ezlopi_cloud_settings.c#L75).
  Those have different causes and different fixes, and collapsing them into one
  error would have satisfied any check that only asked whether *something* was
  reported.

  The reset method accumulates failures across its walk instead of keeping the
  last one. Resetting a device resets every setting it owns, so the obvious
  `ret =` inside the loop lets a setting that succeeds afterwards erase a
  failure that already happened. The counter is declared before the walk and
  incremented in both inner loops — the device-id branch and the setting-id
  branch — because both can refuse.

  The list method's status moves to *after* the handler and is omitted when the
  handler fails, rather than being downgraded to some other word. An entry with
  no status member is the honest answer, and it is the same reasoning as
  `devcfg`'s omitted `badIndex`: omit a member rather than invent a sentinel
  whose meaning no client has agreed to.

  [`__prepare_error`:343](ezlopi-cloud/ezlopi-cloud-settings/ezlopi_cloud_settings.c#L343)
  adds `error: { code, message, data }` *alongside* the `result` member that was
  already there, not in place of it. That is deliberate and it is the precedent
  set by the identically named helper in `ezlopi_cloud_devcfg.c`, whose own
  comment records that the addition is backward-compatible: anything that
  parsed the old empty-success reply keeps parsing it, and only a client looking
  for `error` sees the new information.

  Covered by [`tests/host/test_settings_api.c`](tests/host/test_settings_api.c)
  — **13 cases, 43 checks**, the 35th binary in the host suite. Every case
  asserts both that `error` appears *and* that `result` is still present,
  because a fix that replaced `result` would break every existing client while
  passing a test that only looked for the error.

  **Each of the three subtle cases was watched to fail.** Three regressions were
  applied to the subject one at a time — the status stamped unconditionally, the
  set return discarded, the reset loop written last-wins — and each reddened
  exactly the cases that describe it and nothing else: 1, 4 and 3 failures
  respectively out of 39 checks. That asymmetry is the evidence; a single
  breakage that reddened everything would not distinguish a precise test from a
  blunt one. The failing setting is deliberately placed *first* in the reset
  fixture, which is the only arrangement under which a last-wins implementation
  is visible at all.

  One dependency of the subject is faked, and `nm -u` on the compiled object
  decided that rather than a guess: `EZPI_core_device_get_head`, which is
  precisely the thing a test wants to control. `cjext.c`,
  `ezlopi_cloud_keywords.c` and `ezlopi_core_cjson_macros.c` are linked real, so
  the response objects asserted on are the ones the firmware sends, the member
  names are the real wire names, and a `settingId` in a request is parsed by the
  hub's own code instead of by a convenience reimplementation that would have
  let "a request for id X" mean whatever the test wanted.

  Built green on esp32 and esp32c3 with the object confirmed recompiled in each
  run. **Not exercised on hardware** — no device with a settable setting is
  attached here, so the reply shapes are established by the host test, not by
  watching an app receive them.

  Observed and not fixed, because it is a separate thing: in the reset method
  the `found_id` flag is declared inside the device loop, so the
  "settingId not found" warning fires once per device that does not have that
  setting. Log noise only, pre-existing, left for its own commit.

- **Every settings list response carried its `status` member twice.** Found
  while testing the entry above, and older than it. Five GET paths across the
  two drivers in the tree that have settings write
  [`status`:569](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L569)
  — and again at `:609` and `:649`, and at
  [`status`:396](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/sensor_0068_ENS160_gas_sensor_settings.c#L396)
  and `:428` — into the properties object themselves.
  [`EZPI_device_settings_list_v3`:96](ezlopi-cloud/ezlopi-cloud-settings/ezlopi_cloud_settings.c#L96)
  then added its own, and `cJSON_AddStringToObject` appends without looking for
  an existing key, so the member went out twice. Stamping it before the handler
  rather than after produced the same duplicate, so this is not a consequence of
  the fix above.

  Harmless only while both copies agree, which is a property of today's drivers
  rather than of the API. A client taking the first member reads a different one
  than a client taking the last — a difference no response should have — and a
  driver that wanted to report anything other than `synced`, a setting mid-write
  say, would have been shadowed by whichever copy the far end's parser happened
  to pick. The layer now supplies the member only when the handler left it out,
  so a handler with something specific to say keeps the field and one that says
  nothing still gets the default.

  **This is the kind of defect a lookup cannot find**, and that is the
  transferable part. `cJSON_GetObjectItem` stops at the first match, so every
  check anyone could have written against this response would have passed; the
  test asserts it by walking the sibling chain and counting. The counter has a
  case of its own asserting that a key added twice counts as two and an absent
  key as zero, because a counter that always returned 1 would have made the
  duplicate case pass by not working. The duplicate case fails against the
  unfixed layer, naming exactly that assertion and nothing else.

- **The ENS160 compensated its air-quality readings against a heap address.**
  [`ENS160_get_ambient_temp_setting`](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/sensor_0068_ENS160_gas_sensor_settings.c#L165)
  and
  [`ENS160_get_relative_humidity_setting`](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/sensor_0068_ENS160_gas_sensor_settings.c#L182)
  cast the *settings node* to the driver's payload type and read the first
  member:

  ```c
  s_sensor_..._ambient_temperature_t *t =
      (s_sensor_..._ambient_temperature_t *)ens160_..._sensor_setting;
  ret = t->ambient_temperature;          /* not the stored temperature */
  ```

  The payload is not the node. Settings initialization allocates it separately
  and hangs it off the node — `->user_arg = ambient_temperature;` — which is
  how the cloud get- and set-callbacks in the same file have always read it.
  The fix is `->user_arg` in both getters.

  It was silent rather than a crash because of two layout coincidences, and
  both are now asserted rather than assumed:
  [`user_arg`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.h#L137)
  is the *first* member of `l_ezlopi_device_settings_v3_t`, and the float is
  the first member of each payload. So the wrong read was in bounds, correctly
  aligned, and returned the payload pointer's low four bytes reinterpreted as a
  float. Nothing trapped and no warning fired — the cast is explicit, so the
  compiler had nothing to object to.

  The number it produced is the reason this survived. Every address an ESP32
  heap can hand out decodes to an IEEE-754 float in `[1.0, 2.0)`: internal DRAM
  `0x3FF80000-0x3FFFFFFF` lands in `[1.9375, 2.0)` and the mapped PSRAM window
  `0x3F800000-0x3FBFFFFF` in `[1.0, 1.5)`. So the getters returned roughly 1.96
  where the file's own defaults are 25 °C and 50 %RH — a plausible small
  number, not a NaN or an infinity that something downstream might have
  rejected.

  Where it went:
  [`__init`](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/sensor_0068_ENS160_gas_sensor.c#L219)
  calls both getters and feeds them to
  [`DFROBOT_ens160_set_temp_and_hum`](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/ens160.c#L114),
  which writes `TEMP_IN` and `RH_IN` — `(t + 273.15) * 64` and `rh * 512`. At
  1.9375 that is 17605 and 992, so the sensor was told the room was about 2 °C
  and about 2 %RH. That happened on every boot, and again in
  [`__notify`](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/sensor_0068_ENS160_gas_sensor.c#L175)
  whenever a setting changed. The ENS160 compensates its AQI, TVOC and eCO₂
  outputs with those registers, so every reading this driver has ever published
  was compensated against a pointer. The line above the call in `__init` is the
  author's commented-out `set_temp_and_hum(..., 25.0, 50.0)` with the note
  *"These should be able to be changed froms settings"* — the literals were
  replaced by getters that returned about 2.

  `0.0f` is deliberately kept as the no-payload return rather than raised to
  25/50. The only way to reach it is an allocation failure during settings
  init, and returning the default there would make a broken setting
  indistinguishable from a working one — to a reader and to the test. A
  `TRACE_E` was added so it is not silent.

  Evidenced by [`tests/host/test_ens160set.c`](tests/host/test_ens160set.c) —
  the first device-settings file in the host suite, 18 checks for this defect
  and 29 in total once the allocation-failure defects below were added to it.
  It calls the
  real `ENS160_gas_sensor_settings_init()` and the real getters rather than
  re-typing the expression and checking its own arithmetic, which would have
  passed on the broken file. Six functions are stubbed, the list learned from
  `nm -u` rather than guessed; the NVS read stub reports *no stored value* so
  initialization takes its own default branch and the 25 and 50 asserted are
  the file's own defaults, not something the stub planted. One stub is a
  deliberate copy rather than a refusal —
  [`EZPI_core_device_add_settings_to_device_v3`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L119)
  appends the node it allocates to `device->settings`, and that linking matters
  to a second defect in the same file (below), so the 20 lines are reproduced
  and flagged as a copy to keep the list state the test sees equal to the
  firmware's.

  Seen red first: 18 checks, 2 failures against the unfixed file, both of them
  the value read-backs. The other 16 passed there, which is correct — they are
  about the layout and the address arithmetic that made the defect silent, and
  those were true before the fix and after it. Re-confirmed after the address
  sweep was widened to cover both heap regions by re-breaking the two getters
  and restoring: 18/2 broken, 18/0 fixed, the same two lines red both times.

  The defect does not reproduce on the host and the test does not pretend it
  does: an x86-64 heap pointer's low four bytes are arbitrary, and under
  AddressSanitizer the misread came out as a denormal printing as `0.000000`.
  That is why the ~1.96 claim is swept over the ESP32 address ranges — 1025
  PSRAM and 129 DRAM addresses, endpoints visited explicitly — rather than
  observed from a live allocation.

  | Target | `set-target` | build | diagnostics in the file | both getters in ELF | symbols read | image |
  | --- | --- | --- | --- | --- | --- | --- |
  | esp32 | 0 | 0 | 0 | yes | 13910 | 0x19d420 |
  | esp32s2 | 0 | 0 | 0 | yes | 10509 | 0x151800 |
  | esp32s3 | 0 | 0 | 0 | yes | 15200 | 0x1a0f90 |
  | esp32c3 | 0 | 0 | 0 | yes | 14047 | 0x1af320 |

  The symbol column is there because a clean build says nothing about two
  functions the linker might have discarded, and the symbol *total* is there
  because of a mistake worth recording: the first run of this check reported
  `n/a` on all four targets, its `nm` glob one directory level too shallow for
  the real `tools/<name>/<ver>/<name>/bin/` layout. That is the same lookup
  failure already recorded under *Addressable LED configuration* in this
  release — reintroduced by copying the older probe script rather than the
  corrected one. `n/a` is a skip dressed as a measurement, so the check now
  resolves the toolchain per target by name and prints how many symbols it
  actually read before any count of its own is believed.

  Two things were edited after that table was produced, and both were
  re-checked rather than assumed harmless. A comment line was added to the
  subject, so `esp32` was rebuilt in the same tree: `0x19d420`, identical —
  which is what a comment ought to do, and now what it is known to have done.
  And the two citations to the getters above had to move from `#L137`/`#L154`
  to `#L138`/`#L155` for exactly that reason. The citation gate described below
  caught them, inside the same commit that wrote them, which is the smallest
  possible demonstration of why it exists. They moved a second time, to
  `#L165`/`#L182`, when the allocation-failure fix below added comments ahead of
  them — same gate, same commit, so the pair has now been re-derived twice
  rather than carried.

- **A settings allocation that did not fit rebooted the hub, and would have
  corrupted the settings list if it had not.** Two defects on the same path in
  the same file, fixed together because fixing either one alone makes things
  worse.

  The reboot first, because it is the one that shipped.
  [`ENS160_gas_sensor_settings_init`](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/sensor_0068_ENS160_gas_sensor_settings.c#L128)
  wrapped both of its initializers in `ESP_ERROR_CHECK`, and so did all four
  cloud-facing dispatchers —
  [`__settings_get`](ezlopi-sensors-devices/sensor-0068-I2C-ENS160-gas-sensor/sensor_0068_ENS160_gas_sensor_settings.c#L469),
  `__settings_set`, `__settings_update`, `__settings_reset`. Ten sites. Every
  callee is a plain `int` returning 1 on failure, not an `esp_err_t`, and
  `ESP_ERROR_CHECK` aborts on *any* non-zero value. Which abort depends on the
  build: this project sets `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y`
  and defines no `NDEBUG`, so the macro expands to the
  `_esp_error_check_failed()` variant — a panic and a reboot, in the shipped
  configuration, not a stripped-out assert. A function whose contract is
  *"return 1 on failure"* was therefore a reboot switch at each of those ten
  sites.

  The nineteen-site version of this had already been measured across the tree
  before the scope was chosen: **102** `ESP_ERROR_CHECK` call sites with a
  direct function call, **39** of them on a callee that returns only plain
  `int`. Ten in this file, 19 in `hilink_presence_sensor_setting.c`, 9 in
  `pms_dev_items_prepare.c`, 1 in `sensor_0067_hilink_presence_sensor.c`. Only
  this file's ten are fixed here, because only this file has a host test; the
  other 29 are recorded under Known gaps. They are not one change — the hilink
  file carries its own copy of this pair plus three further defects on the same
  path, and merging them into this commit would mean changing 29 sites with
  evidence for 10.

  Four of those ten are reached from the cloud, through
  `l_ezlopi_device_settings_v3_t::func`, whenever the hub serves
  `hub.device.settings`, a setting value set, a setting reset, or a settings
  broadcast. Each per-setting helper returns 1 when the setting has no payload
  — which is precisely the state a failed allocation leaves behind — so the
  first cloud read of a setting that failed to initialize rebooted the hub, and
  would have gone on rebooting it on every retry.

  All four dispatchers also opened with `int ret = ret = 0;` — a
  self-assignment, and `ret` was never written again, so each returned 0
  whatever happened. `__settings_callback` then discarded those four returns in
  turn, which is what made the dead `ret` invisible. Both are fixed: the
  dispatchers OR in what ran and log it, and the callback assigns. It is worth
  being plain that this does not reach the cloud: every settings `func()` call
  site in `ezlopi-cloud-settings` discards the return, so the cloud receives a
  settings object with no value in it rather than an error. That is a defect in
  the shared cloud layer, is not fixed in this file, and is recorded under Known
  gaps. The change that matters here is that the hub stays up.

  The second defect is why the first could not simply be deleted. On a failed
  payload allocation both initializers called
  `ezlopi_free(__FUNCTION__, ens160_..._sensor_setting)` — a node that is not
  theirs to free.
  [`EZPI_core_device_add_settings_to_device_v3`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L119)
  has already appended it to `device->settings` by that point, so freeing it
  leaves that list holding a dangling pointer.

  How soon that is read is sharper than expected, and was measured rather than
  reasoned about: re-breaking the first setting's free and running under
  AddressSanitizer reports the use-after-free inside
  `add_settings_to_device_v3`'s own tail walk, on the **second** call — the next
  line of the same init function, looking for the tail to append to. The node is
  dangling before initialization has finished; it does not wait for the cloud.
  So removing the `ESP_ERROR_CHECK` on its own would have converted a reboot
  into a live use-after-free, which is worse, and that is the reason these two
  are one commit.

  Both nodes are now left linked with `user_arg` still `NULL`, and the failure
  is logged and returned. That is safe because every callback in the file
  already tests `user_arg` before using it, and both getters report `0.0f` for
  it — which is why `0.0f` was kept as the no-payload return in the fix above
  rather than raised to the 25/50 defaults. A setting that failed to initialize
  reads as unavailable instead of as a working default.

  Evidenced by 11 more checks in
  [`tests/host/test_ens160set.c`](tests/host/test_ens160set.c), which needed one
  thing it did not have: an allocation that can be made to fail.
  [`tests/host/stubs-allocfail/EZLOPI_USER_CONFIG.h`](tests/host/stubs-allocfail/EZLOPI_USER_CONFIG.h)
  routes `ezlopi_malloc` and `ezlopi_free` through two functions the test
  defines, and is on the include path of **the subject only** — `cjext.c`
  allocates through the same macro, and sharing the header would have put
  cJSON's allocations into the same failure budget and made *which* allocation
  failed depend on how cJSON is implemented. An `nm -u` guard in the Makefile
  asserts the subject object really references the wrapper, since if the
  include order ever slipped the checks would be exercising a `malloc` that
  cannot fail. Both wrappers call libc underneath, so ASan sees the same blocks
  and the same frees it would have seen through the plain macros.

  Seen red first, three separate times, because the three things fixed fail in
  three different ways and only one of them is a failed check:

  | Re-break | What it reports | rc |
  | --- | --- | --- |
  | `ESP_ERROR_CHECK` back on the first initializer | `ESP_ERROR_CHECK(1) failed`, no tally printed at all | 134 |
  | the first node's `ezlopi_free` back | ASan heap-use-after-free in `add_settings_to_device_v3` | 1 |
  | the second node's `ezlopi_free` back | 27 checks, 1 failure — *"the second node was not freed either"* | 1 |

  The first row is the firmware's reboot reproduced exactly, since
  `tests/host/stubs/esp_err.h` keeps the abort deliberately rather than
  stubbing it out. The second is why the test's free log is not the whole story
  and the comment on that test says so. The third is why the free log exists at
  all: nothing walks the list after the second node is appended, so ASan sees
  nothing there and the log is the only witness. Each was produced against the
  fixed file and the file restored byte-identically between them, verified with
  `cmp`.

  One flaw in the test was found by running it rather than by reading it. The
  first run against the unfixed subject printed the abort and nothing else:
  `stdout` was fully buffered into a pipe, so every line already written was
  lost with the buffer — including, on the third re-break above, a `FAIL` line
  that had been printed correctly. `setvbuf(stdout, NULL, _IOLBF, 0)` now runs
  first in `main`, and the two getter calls in the last case are guarded on
  their own node so a freed node is reported as a failed check instead of
  aborting past it.

  | Target | `set-target` | build | diagnostics in the file | both getters + init in ELF | symbols read | image | vs. pre-fix |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | esp32 | 0 | 0 | 0 | yes | 13905 | `0x19d280` | −5 syms, −416 bytes |
  | esp32s2 | 0 | 0 | 0 | yes | 10504 | `0x151640` | −5 syms, −448 bytes |
  | esp32s3 | 0 | 0 | 0 | yes | 15195 | `0x1a0de0` | −5 syms, −432 bytes |
  | esp32c3 | 0 | 0 | 0 | yes | 14042 | `0x1af160` | −5 syms, −448 bytes |

  The image got *smaller* on all four, which is the opposite of what adding ten
  log calls suggests, so the symbol column was followed rather than left as a
  curiosity. The count fell by exactly five on each of the four targets; the
  *names* were established on esp32 only, by diffing the full `nm` output
  before and after, and the other three are recorded here as a matching count
  and nothing more. That diff shows ten names present only in the pre-fix
  build — five `__func__$N` and five `__FUNCTION__$N` — against five
  `__FUNCTION__$N` present only in the fixed one.

  The five `__func__$N` are the fix. `ESP_ERROR_CHECK` passes `__ASSERT_FUNC` to
  `_esp_error_check_failed` (ESP-IDF `esp_err.h:118`), newlib's `assert.h`
  resolves that to `__func__` under C99, and the macro was the only thing in the
  file reaching it. The ten call sites sat in exactly **five** functions, two
  apiece — `ENS160_gas_sensor_settings_init` and the four cloud dispatchers —
  and each function emits one shared `__func__` string, so removing the macro
  removed five strings, not ten. The `__FUNCTION__$N` count is five before and
  after; only the counter suffix moves, which is a compiler-local numbering that
  shifts when earlier strings disappear and carries no information.

  Those five strings do not themselves account for 416 bytes, and the symbol
  table cannot say what does: each site also passed `__FILE__` and a `#x`
  stringification of its whole argument expression — here function names 60-odd
  characters long — and those are anonymous string literals, which `nm` does not
  list. The ten suppressed calls into `_esp_error_check_failed` are code rather
  than data. So the five named symbols are the part of the shrink that was
  identified by measurement, and the rest is attributed, not proven.

  That diff was produced by rebuilding `esp32` in the same warm tree against the
  subject restored to `HEAD`, then restoring the fix and rebuilding again: the
  second rebuild returned to 13905 symbols and `0x19d280`, and the symbol list
  matched the first run exactly. So the restore is proven through the compiler
  and not only through `cmp`.

- **A CI check for this cast was first declined on a count that was wrong.**
  The reasoning recorded while fixing the above was that the tree holds
  "exactly one `*settings*.c` file", so a pattern check would guard a
  population of one. Re-deriving it before writing this entry: there are three
  such files, and more to the point there are **three** device-settings
  implementations — the ENS160 one, `hilink_presence_sensor_setting.c` and the
  block in `device_0001_digitalOut_generic.c` — carrying **30** payload-read
  sites between them. Twenty-eight read through `->user_arg`; the two ENS160
  getters were the only ones that did not. The other two files were then read
  rather than assumed: every site in both is correct.

  So the check is justified by the population after all, and is not in this
  commit only because it is a separate concern. The rule it would need is
  exactly the shape of the defect: a cast to an `s_*_t *` payload type whose
  operand is a settings-node pointer and which does not end in `->user_arg`.
  Recorded here with the corrected numbers rather than left standing on the
  wrong one, because the wrong count was the whole argument for not doing it.

- **Choosing a presence mode on the hilink sensor stored the mode and never
  applied it.** Six of the seven NVS *string*-write tests in
  [`hilink_presence_sensor_setting.c`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c)
  were written `EZPI_SUCCESS ==` where they meant `EZPI_SUCCESS !=`. On the
  initialization paths that cost a wrong return value. On the set and reset
  paths it cost the feature, because the branch guarded by the test is the
  branch that programs the radar:

  ```c
  if (EZPI_SUCCESS == EZPI_core_nvs_write_str(value_str, strlen(value_str), "userdef"))
  {
      TRACE_E("Failed to write to NVS");    /* reached when the write worked */
      ret = 1;
  }
  else
  {
      ret = __setting_set_change_setting_template(...);   /* the radar */
  }
  ```

  So a *successful* write logged "Failed to write to NVS", returned an error,
  and skipped the radar; only a *failed* write went on to configure the sensor.
  The mode was stored, so the app read it back correctly on that call and on
  every boot afterwards — while the radar kept whatever template it already
  had. A user would report it as "the modes do nothing", and reading the
  setting back could never have shown it.

  Re-deriving the count before writing this entry: **10** write-result tests,
  **6** of them inverted. All three `EZPI_core_nvs_write_int32` tests were
  correct, and six of the seven `EZPI_core_nvs_write_str` tests were not — the
  single exception being
  [`__setting_set_pre_defined_setting`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L882).
  An earlier *Known gaps* entry described this as "its NVS-write test is
  **inverted**", singular, from a reading rather than a count; that entry has
  been corrected in place.

- **The one write test written the right way round had its answer thrown
  away.** Immediately after that correct test,
  [line 908](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L908)
  assigned over `ret` rather than accumulating into it, on every call where the
  template lookup succeeded — which is every call carrying a mode name the app
  can actually send. So the correct test bought nothing: a presence mode that
  could not be stored still reported success, and came back as the old mode on
  the next boot. Now `ret |=`.

- **An entire error path reported success unconditionally, which is why five
  other fixes could not be seen.**
  [`__setting_set_change_setting_template`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L771)
  discarded the return value of all three of its calls to
  [`__setting_set_change_user_defined_template`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L704),
  and then returned a `ret` that nothing in that branch had ever written. Five
  distinct faults inside the callee — a failed NVS write, a radar refusing the
  custom template, a missing settings node, a missing payload, and a setting
  that could not be serialized — each set `ret = 1` and had it dropped one
  frame up.

  This one was found by the test suite rather than by reading, and that is
  worth recording. The five faults in the callee had already been fixed, and
  the suite was green. Re-breaking each of the five one at a time left it
  green *every time*. Five independent fixes all failing to be observable is
  not five coincidences, and the shared cause was the caller. Without the
  re-break step those five would have been committed as proven when nothing
  outside the function could observe any of them.

- **The remaining defects fixed in the same file.** All verified by re-break
  unless noted:

  - Nineteen `ESP_ERROR_CHECK` wrappers on functions returning plain `int`,
    including three in
    [`HILINK_presence_sensor_initialize_settings`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L210)
    and the four cloud dispatchers. The file now has none. The initialization
    one is the reason this was reached at all: with the predefined write test
    inverted, the first-ever configuration of a presence sensor returned 1 from
    a write that had worked, and `ESP_ERROR_CHECK(1)` panicked and rebooted the
    hub. It fires exactly once, because `EZPI_core_nvs_write_str` returns only
    after `nvs_commit` succeeds — so the value is durable before the panic, and
    the next boot takes the "already exists" path. One unexplained restart
    while adding a sensor, then never again.
  - [`__settings_callback`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L496)
    discarded the return of all four dispatchers. Fixing the dispatchers
    without this would have been a change with no observable effect at all.
  - A settings node freed while the device list still pointed at it, in two
    initializers. ASan reports it as a heap-use-after-free on the *next*
    initializer's `while (curr->next)` walk — dangling before initialization
    has even finished, not at some later shutdown.
  - [`HILINK_presence_sensor_target_in_detectable_range`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L247)
    dereferenced the settings node while checking only the payload behind it.
    Its caller is the UART frame handler, so once that one allocation had
    failed the hub could not process presence at all.
  - [`__setting_reset_user_defined_setting`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L1069)
    guarded on `setting` — the pointer the line above had already tested — so
    the guard was always true and the `memset` below it wrote **20** bytes
    through a NULL payload pointer. (An earlier draft of this entry, and the
    comment in the source, both said 56. `sizeof` was measured rather than
    recalled before publishing it: four `float` plus a `uint8_t` and a `bool`
    is 20 bytes, confirmed on the host and on `xtensa-esp32-elf-gcc` by
    `_Static_assert`. The 56 was not derived from anything.)
  - Four leaks: the string `EZPI_core_nvs_read_str` hands the caller, leaked on
    every boot with a stored predefined mode; the serialized setting string at
    three call sites; and the `cJSON` object in
    [`__prepare_user_defined_setting_str`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c#L1368),
    which was never deleted on any path. Two of these are provable **only** by
    LeakSanitizer and have no named check: `cjext.c` is compiled without
    `stubs-allocfail` on its include path, so its allocations go to libc
    `malloc` and are invisible to the test's own free log.
  - `strlen(ret)` on the result of `cJSON_PrintBuffered` before anything
    checked it for NULL — in a trace line whose whole purpose is the
    allocation-failure case.
  - A radar payload that could not be allocated reported `ret = 0`.
  - Four missing `else` branches that let a failure return success, and four
    dead frees of pointers that were provably NULL. The dead frees are removed
    as dead code, not as bugs; no test can show a difference.

- **The evidence, and what it does not cover.**
  [`tests/host/test_hilinkset.c`](tests/host/test_hilinkset.c) is new: **75**
  checks, which brings the host suite to 33 binaries and 6572 checks with no
  failures. Each fix was then re-broken one at a time against the fixed file,
  rebuilt, and run — **26 of 31** reversals went red in the expected place,
  with every restore verified byte-identical by SHA-256.

  The five that stay green are listed in full at the foot of the test file
  rather than papered over. Four are unobservable for structural reasons: two
  added NULL guards on file-scope statics that are NULL only before
  initialization has run, when nothing can reach them; one arm of the apply
  path that differs only when a global and the caller's own node disagree; and
  one fix observable only with two hilink sensors on one hub, which these
  file-scope statics break far more generally — see *Known gaps*. The fifth
  needs a `cJSON` allocation to fail, which this harness cannot yet arrange.

  Staging was necessary and is recorded in the test's comments: the unfixed
  file does not fail, it *aborts* at the first `ESP_ERROR_CHECK`, so "see every
  check red before anything changes" was impossible. The panic was removed
  first, which made the later checks reachable, and each was then proven red in
  turn.

  Two checks in this file passed against the unfixed subject before they were
  made to fail, for two different wrong reasons, and both are written into the
  test rather than quietly fixed. The first: a single shared `nvs_write_result`
  let the one correctly-written test satisfy a check meant for the other two.
  The second, after splitting it per key, is the trap the inversion itself
  sets — an inverted test reports failure when the write *succeeds*, so a
  healthy write contributed a spurious error that made the check pass for a
  reason that had nothing to do with its subject. Planting all three keys is
  what makes the return value attributable.

  The rewritten file was compiled for **four** targets — `esp32`, `esp32s2`,
  `esp32s3` and `esp32c3` — all four reaching `build=0` with the subject
  confirmed actually compiled, so a zero diagnostic count means something.
  Images are `0x19d070`, `0x151420`, `0x1a0bb0` and `0x1aef50`. Diagnostics
  were filtered to this commit's two files, so a pre-existing warning elsewhere
  could not be read as either a pass or a regression here. Three of the four
  report exactly the two `#warning "NABIN ..."` lines that were already in the
  file.

  `esp32c3` reports a third, and it is worth naming rather than rounding off,
  because catching exactly this is why more than one target is built:

  ```
  hilink_presence_sensor_setting.c:430:65: warning: passing argument 1 of
    'EZPI_core_nvs_read_int32' from incompatible pointer type
  ```

  It is **pre-existing, not introduced by this change** — `HEAD` carries the
  identical two lines (`int read_value = 0;` then `&read_value`) and this
  diff does not touch either. It is invisible on the xtensa targets because
  [`EZPI_core_nvs_read_int32`](ezlopi-core/ezlopi-core-nvs/ezlopi_core_nvs.h#L242)
  takes `int32_t *`, and `int32_t` is `int` on xtensa but `long int` on
  riscv32. Both are 32 bits with the same representation, so nothing
  misbehaves today; it is a latent portability fault, left alone here as a
  separate concern and recorded under *Known gaps*.

  Nothing here has been exercised on hardware. No hilink presence sensor is
  attached to this board, so the radar behaviour above is reasoned from the
  driver's interface and proven only against a stub that counts calls. Only
  `esp32` has ever been run on a device; the other three are compile evidence
  and nothing more.

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

### BLE read-back — [sec]

Both BLE characteristics that hand data *out* — the device config and the
provisioning record — base64-encoded it into a fixed buffer and then checked
the wrong thing to decide whether that had worked.

- **`mbedtls_base64_encode()`'s failure was undetectable by the check used.**
  Both call sites tested `0 == out_put_len`. On a short output buffer mbedtls
  returns `MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL` and sets `*olen` to the size it
  *wanted* — `n + 1`, necessarily non-zero — without writing a single byte to
  the destination (`library/base64.c`, the `dlen < n + 1` branch). So the check
  passed on exactly the failure it was meant to catch, and the function returned
  a heap buffer it had never written. That buffer was then `strlen()`-ed, with
  no terminator anywhere in it, and sent out over BLE as the device's
  configuration or its provisioning record. The provisioning one is the record
  carrying the SSL private key and certificates. The encode return value is now
  the check; `0 == out_put_len` is kept alongside it only for `slen == 0`, the
  one input mbedtls accepts without writing a NUL.

- **The buffers were too small for payloads the device accepts.** Both were
  fixed sizes — 4096 for the config, 6 KB for the provisioning record — where
  base64 needs `ceil(n/3)*4 + 1`. They are now derived from the length actually
  being encoded.

  For the config this was a reachable range and not a corner: the field is
  `EZLOPI_FINFO_LEN_EZLOPI_CONFIG_JSON` = `0x1000`, whose base64 needs 5465
  bytes, so a 4096-byte buffer encoded configs up to 3069 bytes and refused
  every larger one. The *write* path in the same file bounds incoming base64 by
  `EZPI_BLE_DYNCFG_MAX_BASE64_LEN`, which is `ceil(0x1000/3)*4` and was
  deliberately given a ceiling rather than a floor so that a config exactly
  filling the field is accepted. The two directions therefore disagreed, and a
  config between 3070 and 4096 bytes — 1027 of the sizes the field holds —
  stored correctly and could never be read back.

  The threshold is not asserted from the formula: `ceil(n/3)*4` was checked
  against coreutils `base64 -w0` at the boundary, which agrees at 3069 fitting
  and 3070 not.

  For the provisioning record the sizing was worse in practice.
  `ezpi_provisioning_info_jsonify()` embeds three PEM fields, each capped at
  `0x1000` by `EZLOPI_FINFO_LEN_CA_CERTIFICATE` and its siblings, so an
  ordinarily provisioned device clears the 4605-byte ceiling that 6 KB implied.
  Nothing upstream held it below that either — the `6 * 1024` passed to
  `cJSON_PrintBuffered()` is a growth hint, not a cap.

- **`ezpi_provisioning_info_read_func()` dereferenced the buffer before
  checking it for NULL.** `strlen(g_provisioning_info_base64)` ran twice on the
  line after the assignment; the `if (NULL != g_provisioning_info_base64)` test
  was one line too late. `ezpi_provisioning_info_base64()` returns NULL on a
  failed `cJSON_CreateObject`, a failed `cJSON_PrintBuffered`, a failed malloc,
  or the short-buffer case above — so this was a remote NULL dereference on a
  characteristic readable by anyone in radio range, before anything in that file
  has authenticated them. `ezpi_dynamic_config_read_func()` already guards the
  identical block; only this copy was missing it. Same shape as the
  check-after-use fix in
  [`ezlopi_core_factory_info.c:564`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L564),
  where `isprint(ret[0])` read the buffer before the `if (ret)` below it. Two
  instances of the idiom now, in unrelated files, which is enough to look for it
  deliberately rather than treat each as a one-off — recorded as a known gap.

The matching `mbedtls_base64_decode()` calls were left alone deliberately.
They discard their return value too, but the destination is `bzero`-ed first,
so a failed decode yields an empty string that then fails `cJSON_Parse` — the
`bzero` is doing the work the return check would, and their `o_len` was already
a `size_t`.

`out_put_len` was also a `uint32_t` where mbedtls declares `size_t *`, and the
reason that had gone unnoticed is worth recording, because it is a general
property of this four-target build and not a detail of this call. `size_t` is
`unsigned int` on all four targets; `uint32_t` is `unsigned int` on xtensa but
`long unsigned int` on riscv32. Distinct types, identical 32-bit width — so the
store always landed correctly, and **only esp32c3 diagnosed it**. The esp32,
esp32s2 and esp32s3 builds said nothing. Any `uint32_t`/`size_t` pointer
confusion in this tree is therefore a c3-only diagnostic, which is an argument
for keeping c3 in the build matrix beyond its `SOC_I2C_NUM` differences.

Verified by four-target build rather than by host test, which is weaker
evidence than this repo's norm and worth naming: both functions reach cJSON,
the factory-info partition and the GATT layer, so a host harness would have to
stub more than the change touches. What the build does establish is the
`size_t *` prototype match — invisible to the host suite, which has no mbedtls
header — the `TRACE_*` format strings against their newly cast arguments, and
that removing the `#ifndef CONFIG_EZPI_UTIL_TRACE_EN` pair around the encode
call left no set-but-unused `ret` in the trace-disabled configuration. The
arithmetic is covered independently by the `base64 -w0` comparison above.

The warning count moved the way the change predicts: esp32 146 → 146, esp32s2
143 → 143, esp32s3 146 → 146, esp32c3 152 → 151, the single removal being the
`-Wincompatible-pointer-types` above. esp32s2's image is byte-identical before
and after, which is the expected result rather than a null one — s2 has no
Bluetooth radio, so both of these files preprocess away entirely there. On the
three BLE-capable targets the image grew 16, 32 and 32 bytes.

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

- **Telemetry with logs on and traces off did not compile.**
  `CONFIG_EZPI_OPENTELEMETRY_ENABLE_LOGS` and
  `CONFIG_EZPI_OPENTELEMETRY_ENABLE_TRACES` are two independent `bool`s, both
  `default n`, so Kconfig permits all four combinations — but one of the four
  was never built. `ezlopi_service_otel_fetch_string_value_from_cjson()` was
  declared *and* defined under `ENABLE_TRACES` alone, and
  `ezlopi_service_webprov.c` declared, filled and freed its `method_str` under
  the same guard while the logs path also read it. Selecting logs without traces
  failed the build.

  The helper is not trace-specific despite living in the otel service: it copies
  one string member out of a cJSON object. Its declaration and definition moved
  out to the enclosing `CONFIG_EZPI_ENABLE_OPENTELEMETRY` guard, which is the
  honest scope — both features depend on that symbol and neither is more
  entitled to the helper than the other. In `webprov.c` the three `method_str`
  sites are now behind one `EZPI_OTEL_WANTS_METHOD_STR` macro defined as
  `LOGS || TRACES`, so the variable exists exactly when something reads it
  rather than when one of its two readers happens to be enabled.

  All four combinations now build, and the three that already worked produce
  **byte-identical images before and after the fix** — 1 683 184, 1 687 024 and
  1 689 824 bytes on esp32 for logs-off/traces-off, logs-off/traces-on and
  logs-on/traces-on respectively. That is what establishes the broken
  combination was unblocked without paying for it anywhere else: a fix in this
  shape could easily have pulled the helper into builds that do not want it, and
  three unchanged byte counts are the evidence that it did not.

  Those three figures were measured against the tree as it stood when this fix
  landed. They no longer reproduce as absolute numbers, because the dead-code
  removals recorded below later took 284 bytes of content out of every
  configuration — the *equality* between the three before-and-after pairs is the
  claim here, not the specific totals.

- **`__otel_publish()` sent the shared transmit buffer without checking that
  anything had been written into it.** Two missing lines, and it is the pair of
  them that turned a cosmetic bug into a read off the end of a heap buffer.

  `cJSON_PrintPreallocated()` returns false for an object that does not fit —
  and it returns false *after* writing a truncated prefix, leaving no
  terminator at the truncation point
  ([`cjext.c:1537`](ezlopi-components/cjext/cjext.c#L1537) → `print_value` →
  `ensure`, which refuses to grow a `noalloc` buffer).
  `EZPI_core_buffer_acquire()` hands back the shared `__buffer` exactly as the
  previous holder left it
  ([`ezlopi_core_buffer.c:158`](ezlopi-core/ezlopi-core-buffer/ezlopi_core_buffer.c#L158)
  — it takes the semaphore and returns the pointer, with no clearing). The
  three other live holders each do the same two things before trusting the
  buffer: `memset(buffer, 0, buffer_len)`, then `if (true ==
  cJSON_PrintPreallocated(...))`.
  [`__otel_publish()`](ezlopi-services/ezlopi-service-otel/ezlopi_service_otel.c#L218)
  did neither. It printed, ignored the result, then called `strlen(buffer)` and
  sent that many bytes over the websocket.

  So on an object that did not fit, `strlen()` did not stop at the end of the
  truncated JSON — there was no terminator there — and it did not stop at the
  end of the previous holder's bytes either, because nothing had put one there.
  It ran off the end of the buffer, and whatever length it came back with was
  what got transmitted.

  The premise was measured on the host rather than inferred, with the
  destination pre-filled with a non-zero pattern standing in for a previous
  holder's leftovers. Printing a 61-byte object into 40 bytes:

  ```
  returned false
  bytes the printer changed: 35 of 40
  is there a NUL in the buffer at all? no
  buffer as bytes: {"method":"hub.items.list","token":RRRRR
  ```

  Three things in that are worse than the reasoning predicted. There is no
  terminator anywhere in the destination, so the read is unbounded rather than
  bounded by the residue. The truncation lands mid-key, so the bytes that *are*
  valid stop exactly where a credential would have started, which is the shape
  that makes this look like harmless truncation in a log. And the fixture is not
  contrived: `hub.items.list` with a token is an ordinary API response, and
  responses are what the previous holder of this buffer had been printing.

  Fixed at
  [`ezlopi_service_otel.c:248`](ezlopi-services/ezlopi-service-otel/ezlopi_service_otel.c#L248)
  and
  [`:250`](ezlopi-services/ezlopi-service-otel/ezlopi_service_otel.c#L250),
  with the dropped object traced rather than discarded silently. Of the two it
  is the return check that makes this safe; the `memset` only bounds the damage
  should the check ever be dropped again, and it is not free — it clears 10 KB
  per published object. It is there because it is what the three other holders
  do and because an unbounded read is the failure mode worth spending 10 KB of
  `memset` to rule out.

  **Two things this fix corrects about how it was first written down.** It was
  recorded under *Known gaps* first, and re-reading the code to fix it
  contradicted the note twice.

  The note said the object had to exceed "the 2 KB it asks for". There is no
  2 KB and nothing is asked for: the `2000` in the call is
  `wait_to_acquired_ms`, the length arrives through `buffer_len`, and it is
  `CONFIG_EZPI_CORE_STATIC_BUFFER_SIZE` — `default 10240` at
  [`Kconfig.projbuild:565`](ezlopi-main/Kconfig.projbuild#L565), `=10240` in all
  four committed `sdkconfig.defaults.*`, and passed to `EZPI_core_buffer_init()`
  at
  [`ezlopi_core_ezlopi.c:123`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L123).
  The bar is 10 KB, not 2 KB. That is a materially higher bar and it is reached
  by content rather than by accident: the trace attributes include `method`,
  `id` and `error` copied verbatim out of the incoming message, with no length
  cap, at
  [`ezlopi_service_webprov.c:313`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L313)
  and
  [`ezlopi_service_broadcast.c:142`](ezlopi-services/ezlopi-service-broadcast/ezlopi_service_broadcast.c#L142).

  The note also said the previous holders were "the API response path and the
  cloud broadcast path", which undercounts. There are three other live holders —
  the cloud response path (`ezlopi_service_webprov.c:489`), the local websocket
  server's response path (`ezlopi_service_ws_server.c:844`) and the broadcast
  path (`ezlopi_core_broadcast.c:199`) — plus a fourth behind `#if 0`. Both
  errors are the same kind: a number and a count written from reasoning about
  the call rather than from reading what the call does.

  **Why this is [bug] and not [sec].** The marker in this file means reachable
  from an unauthenticated BLE, UART or local-network client, and this is not,
  on two counts. The entire file is inside `#ifdef
  CONFIG_EZPI_ENABLE_OPENTELEMETRY`, which is `is not set` in all four committed
  `sdkconfig.defaults.*` and which a CI check already forbids committing as a
  default — so no shipped image contains this code at all. And the bytes go to
  the operator's own collector, not back to whoever provoked the oversized
  object. Whether a local-network client can drive a trace past 10 KB on an
  opted-in build was not traced, which is the other reason not to claim the
  stronger marker.

  **Pinned by a new hygiene check**,
  `tests/ci/check_print_preallocated_result.py`, which enforces two rules: no
  `cJSON_PrintPreallocated()` result may be discarded anywhere in the
  first-party trees — there are nine call sites and all nine now consume it —
  and every `EZPI_core_buffer_acquire()` holder must clear the buffer before
  printing into it. The two are reported separately on purpose, because they
  fail differently: with the `memset` but no return check the damage is a
  malformed fragment in zeroed memory, with the return check and no `memset`
  nothing escapes at all, and only the pair reads out of bounds. Thirteen
  fixtures run before the tree is scanned, one of which is the comment this fix
  left behind in `otel.c` — a check in that directory has matched its own
  documentation before.

  The check was run against three mutations, not one. Against `8235e1a`'s
  `otel.c` it reports both rules at both lines; with the `memset` restored and
  the return check removed it reports only the discarded result; with the return
  check restored and the `memset` removed, only the uncleared buffer. A
  single-stage mutation would have proved one rule and credited it with both.

  **Measured.** All four targets build at the committed defaults with rc=0 and
  **zero size delta** — but that zero means the file was not compiled, not that
  the change is free, and the instrument that was supposed to prove that was
  wrong: the object file is produced either way, empty, so counting objects
  showed `1` in every configuration. What does show it is the 6 640-byte
  difference between esp32 with the symbol off (1 683 856) and on (1 690 496).
  Against that otel-on build the fix costs **96 bytes** (1 690 496 → 1 690 592)
  for a `memset` call, a branch and an error trace, with no warnings in
  `otel.c` and no `-Wshadow` diagnostics. Nothing was flashed.

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

### Credentials — [sec]

Titled for the credential rather than for the surface, because the sweep in the
middle of this section took it past the local API: the leaks below are on the
serial console, the BLE provisioning characteristic, and an OpenTelemetry
export, and the last two bullets are about comparing a credential rather than
printing one.

- **The local API key was printed to the serial console on every login
  attempt.** `ezlopi_core_offline_login.c` contained
  `TRACE_D("password: %s", password_saved)`, where `password_saved` is the
  device's only local-API credential. Now removed.

  The reason this is listed as a defect rather than as debug noise is that a
  `TRACE_D` here is *not* compiled out or filtered in a shipped build, contrary
  to how it reads. These are EzloPi's own trace macros, not `ESP_LOG`, so
  `CONFIG_LOG_DEFAULT_LEVEL` does not apply to them. The severity was chased to
  the actual `printf`: `TRACE_D` is `trace_log_sev_debug`
  ([`ezlopi_util_trace.h:213`](ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h#L213)),
  which passes severity 4
  ([`:175`](ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h#L175));
  `serial_log_severity` defaults to `ENUM_EZLOPI_TRACE_SEVERITY_TRACE` = 5
  ([`ezlopi_core_log.c:131`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L131));
  the upcall is registered at `:375` and ends in `printf` at `:478`. 4 ≤ 5, so
  the default build printed it. That default used to be
  `ENUM_EZLOPI_TRACE_SEVERITY_MAX` = 6, which is the table's length and not a
  severity at all; the entry below on the severity-as-an-array-index fix has
  the detail. It does not change this conclusion — 4 was ≤ 6 and is ≤ 5 — and
  the two line numbers above moved with that fix.

  There *is* a switch that compiles these macros away, and it is worth naming
  precisely because finding it is the obvious way to talk yourself out of this
  whole section. `ezlopi_util_trace.h:224-233` defines `TRACE_D(X, ...)` as
  nothing — but that is the `#else` of
  `#if defined(CONFIG_EZPI_UTIL_TRACE_EN) || defined(CONFIG_EZPI_LOG_CLOUD_EN)`
  ([`:74`](ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h#L74)), and
  `EZPI_UTIL_TRACE_EN` is
  [**`default y`**](ezlopi-main/Kconfig.projbuild#L16). Confirmed as
  `CONFIG_EZPI_UTIL_TRACE_EN=y` in the `sdkconfig` a default `idf.py
  set-target esp32` actually generates, not just read off the Kconfig. So the
  empty definitions exist and no default build uses them.

  That argument is now also *measured* rather than reasoned, by looking for the
  format strings in the linked image — see the testing note at the end of this
  section.

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

The three above were found one at a time, which is the wrong way to find them —
the first was read off the screen, the second turned up while verifying the
first, and after the third it was clear this was a *class* rather than three
incidents. The four below came from searching for the shape instead of waiting
for the next one: every `TRACE_*`/`printf` whose format string pairs a `%s`
with an argument named like a secret, across the whole tree. The fourth of them
is the reason the sweep was worth doing — it exports off-device, and no amount
of reading the serial console would have revealed it.

- **The Wi-Fi password was printed on every network change.**
  `ezlopi_core_wifi.c` had
  `TRACE_D("Trying to connect to %s with password %s", ssid, pass)` in
  `ezlopi_wifi_try_connect_task()`. Now
  [a length](ezlopi-core/ezlopi-core-wifi/ezlopi_core_wifi.c#L389).

  This is a sibling of the `ezlopi_core_nvs.c` bullet above and not the same
  line: that one printed the PSK when the stored config was *loaded*, this one
  printed it when a *new* network was joined. Fixing the load path left the join
  path printing the password of every network the user typed in.

  The SSID is kept. The access point broadcasts it over the air regardless, and
  it is the half of the line that makes it useful.

- **The provisioning blob — including the device's SSL private key — was
  printed on every read of the BLE provisioning characteristic, in a build that
  compiles that path. No shipped build does.** This is the one bullet in this
  section that is *latent* rather than active, and the distinction was not
  visible from the source.
  `ezlopi_service_ble_provisioning.c` had
  `TRACE_D("str_provisioning_data[len: %u]: %s", prov_len, str_provisioning_data)`.
  Now [the length only](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L914).

  What is in that string is why it is listed at all.
  [`ezpi_provisioning_info_jsonify()`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L826)
  embeds three PEM fields at
  [`:856-858`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L856)
  — `ssl_private_key`, `ssl_shared_key` and the CA certificate — so in such a
  build the device's own private key goes to the console in full, in PEM, every
  time anything reads the characteristic.

  **It is not compiled in the default build**, and finding that out is the
  useful part of this bullet. The function sits inside
  `#ifdef EZPI_SERV_BLE_ENABLE_READ_PROV`, opened 76 lines above the fix at
  [`:825`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L825).
  That macro is `#define`d at
  [`EZLOPI_USER_CONFIG.h:153`](ezlopi-user-config/EZLOPI_USER_CONFIG.h#L153)
  and then **`#undef`d 57 lines later** at
  [`:210`](ezlopi-user-config/EZLOPI_USER_CONFIG.h#L210), under a
  `/////// Disable user configs here ///////` banner. So reading either site
  alone gives the wrong answer. The `#endif` comments in the provisioning file
  were mislabelled against the actual nesting too, which did not help; they are
  corrected under **Build and configuration** below.

  I did not find this by reading. The CHANGELOG at this point claimed the key
  "went to the console every time anything read the characteristic", and the
  measurement below contradicted it: the new format string was **absent** from
  the linked image. Chasing that down — other strings from the same file were
  present, the string was absent from
  `ezlopi_service_ble_provisioning.c.obj` itself, and
  `ezpi_provisioning_info_base64` was absent from that object's symbol table —
  put it beyond doubt that the compiler never saw the function, rather than the
  linker having dropped it. The claim is corrected here rather than shipped.

  Which left the same hazard as the OpenTelemetry bullet below: an edit no
  build compiles is an edit nobody has syntax-checked. So a sixth build was run
  with the two `#undef`s commented out, confirming the fix compiles, that
  `body withheld: contains SSL keys` is then present in the object file and the
  image, and that the old format string is not. The header was restored
  afterwards — **this change does not enable the read path**, it only makes the
  code correct for whoever does.

  One claim was dropped on inspection: the sweep also reported a provisioning
  *token* in this blob. Reading the function, the only identifier-ish field is
  `uuid_provisioning`, which is an identifier and not a credential. The comment
  in the code names the three fields that are actually there and does not
  mention a token.

- **Every inbound local-API request body was printed, and the first one of
  every session contains the device's local API key.** The per-message
  `TRACE_D("payload[len: %d]:\r\n%.*s", ...)` in `ezpi_msg_handler()` is now
  behind
  [`CONFIG_EZPI_WS_LOG_PAYLOADS`](ezlopi-main/Kconfig.projbuild#L465),
  **`default n`**, with the length still logged
  [either way](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L662)
  so the "traffic arrived" signal survives.

  Three methods on this socket carry a credential in their params:
  `hub.offline.login.ui` → `params.token`, the local API key;
  `hub.network.wifi.try_connect` → `params.network.key`, the Wi-Fi PSK;
  `hub.mqtt.config.set` → `params.password`. The login is the one that makes
  this unconditional rather than occasional: an unauthenticated client may call
  that method and no other — the gate in `__message_upcall()`
  ([`:336`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L336))
  admits nothing else — so it is the *first* payload of every session. The
  device printed its own key on every connection attempt by anything, including
  peers that then failed to log in.

  A switch rather than per-field redaction, deliberately. Redaction means
  parsing untrusted input here purely to decide what to log, and it fails
  silently the first time a new method adds a secret field that nobody adds to
  the list — the failure mode being that it keeps printing and nothing
  complains. A switch cannot drift. It is also the answer this repository
  already gives for the same question:
  [`CONFIG_EZPI_LOCAL_KEY_ANNOUNCE`](ezlopi-main/Kconfig.projbuild#L443) gates
  printing the local key the same way, so the new option was modelled on it
  rather than invented.

- **The HTTP bodies this firmware sends and receives were printed to the serial
  console, and one of them is the device's own SSL private key.** Four
  whole-body dumps in `ezlopi_core_http.c` are now behind
  [`CONFIG_EZPI_HTTP_LOG_PAYLOADS`](ezlopi-main/Kconfig.projbuild#L492),
  **`default n`** — the sibling of
  [`EZPI_WS_LOG_PAYLOADS`](ezlopi-main/Kconfig.projbuild#L465) above, and
  modelled on it. Each `#else` keeps the length, so "a request was assembled"
  and "a reply came back" still reach the console.

  The worst of the four is not the request. It is
  [`HTTP_EVENT_ON_DATA`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L845),
  which printed every chunk of every `esp_http_client` response body. Both live
  callers of that handler are
  [`EZPI_core_http_get_request`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L258)
  calls in `ezlopi_service_webprov.c`, and the second is
  [provision-sync](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L583)
  — whose reply `__provision_update()` reads
  [`ssl_private_key`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L742),
  [`ssl_shared_key`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L754),
  [`signing_ca_certificate`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L760)
  and a fresh
  [`provision_token`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L698)
  out of. Those arrive as the body, in PEM, and were printed chunk by chunk.
  Nobody has to ask for it: provisioning runs by itself on a device that has
  network and a token, so the device's own private key went to the console on a
  path no operator triggers. It is the same material the BLE provisioning read
  was fixed for two entries above — a second route to it.

  The other two live sites are the request/response pair in
  `EZPI_core_http_mbedtls_req`
  ([`:235`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L235),
  [`:245`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L245)), reached by a
  `sendHttpRequest` scene action. `request` is a credential envelope rather than
  a string: for a GET,
  [`__ezlopi_http_generate_request`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L701)
  writes `?username=%s&password=%s` straight into that buffer, and any
  `Authorization` header the action set is
  [appended](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L781) to the same
  buffer a few lines later — the credentials being
  [parsed out of an `EZLOPI_VALUE_TYPE_CREDENTIAL` field](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L405).
  So this was not "an opaque header might be in there"; a user and password were
  logged in the clear, in a query string. The response is withheld on the same
  switch because it is the chosen server's reply, `Set-Cookie` included.

  The fourth,
  [the per-chunk dump](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L425) in
  [`EZPI_core_http_post_request`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L350),
  is gated for its shape and not for a leak that shipped. That function has no
  caller in the tree — header declaration and component readme only — and the
  linker agrees: its `.text`, `.literal` and `.rodata…str1.1` sections are all
  in the *Discarded input sections* block of the map, in the baseline image as
  well as this one. It has never been in a shipped binary. It is gated anyway,
  because it prints a server's response body unconditionally and the first
  caller to arrive would inherit that.

  **Live in every shipped image, not latent.** This is worth stating because the
  obvious assumption is wrong twice over. EzloPi's trace macros are not
  `ESP_LOG`, so `CONFIG_LOG_DEFAULT_LEVEL=0` in the `sdkconfig.defaults.*` files
  does not gate them;
  [`ENABLE_TRACE`](ezlopi-util/ezlopi-util-trace/ezlopi_util_trace.h#L75) comes
  from `CONFIG_EZPI_UTIL_TRACE_EN`, which is `=y` in **4 of 4** of those files.
  And `trace_color_print()` does no severity filtering at all — the only filter
  is in the registered serial upcall
  ([`:476`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L476)), which
  [`EZPI_init()` defeats on every boot](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L119)
  by raising `serial_log_severity` to `TRACE` after reading NVS. So a
  severity-3 `TRACE_I` printed regardless of what anyone configured.

  The exposure is the UART and only the UART, which is worth being exact about
  rather than overstating. Cloud log severity is capped at `WARNING` in both
  setters ([`:245`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L245),
  [`:434`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L434)) and
  `EZPI_core_send_cloud_log` gates on it
  ([`:331`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L331)), so these
  traces never left the device by that route.

  **Testing, all four targets.** The fix removes three format strings from
  rodata and adds one, so the binary can be asked directly rather than being
  trusted because it compiled. Baseline is `git archive HEAD`; both trees built
  for esp32, esp32s2, esp32s3 and esp32c3.

  | in the linked `.bin` | baseline | fixed |
  |---|---|---|
  | `request[capacity: %d]:` | 1 | 0 |
  | `*result[%p] =>` | 1 | 0 |
  | `HTTP_EVENT_ON_DATA, len=%d, data:` | 1 | 0 |
  | `…is off` (the withheld messages) | 0 | 3 |

  Identical on all four. Three and not four withheld messages because the
  fourth is in the discarded function, which is how that claim above was
  checked rather than assumed. The image grows 128–144 bytes. No new warning in
  `ezlopi_core_http.c` on any target. The per-chunk site's format string is
  `"%.*s"` — four bytes shared with every other user of that conversion — so it
  is not separable in rodata and is not claimed to be; it is covered by the
  compile and by the source-level check.

  A Kconfig-gated fix has a failure mode that looks exactly like success: if the
  macro name in the `.c` did not match the one in `Kconfig.projbuild`, the
  `#ifdef` would simply be false, the `#else` would compile, the dumps would
  vanish from rodata, and every row above would read correctly — leaving a
  switch that can never be turned on. Two things rule that out. The generated
  `sdkconfig` mentions the option in the fixed tree and not in the baseline
  (`# CONFIG_EZPI_HTTP_LOG_PAYLOADS is not set`), and a fifth build turned it
  **on**: the three dumps come back, the withheld message goes, and the image
  returns to exactly the baseline's 1683856 bytes.

  **What the CI check does and does not do here.**
  [`GATED_SWITCHES`](tests/ci/check_no_secret_logging.py#L189) now carries an
  [entry for the new option](tests/ci/check_no_secret_logging.py#L210) pinning
  it at `default n`, so flipping it in `Kconfig.projbuild` fails the check
  until someone edits the table — which is the moment to have the argument. The
  pin was seen to fail: with the expected value mutated to `'y'`, the check
  exits 1 with the drift message.

  Its pattern matching was deliberately *not* extended, and the reason is
  recorded in the file's own
  [LIMITS](tests/ci/check_no_secret_logging.py#L84) section. All four buffers
  are named after the transport rather than the contents — `request`,
  `config->response`, `curr_chunk->ptr`, `evt->data` — and the check passed on
  every one of them before this change. No rule that works on identifiers
  reaches `evt->data`. `payload` in `SECRET_SUBSTRINGS` covers exactly one such
  buffer, the one the websocket server happens to call `payload`; it is a name,
  not a rule. These four were found by reading the provisioning path and asking
  what was in the body.

  Scope note: the backlog item that prompted this named one site, and described
  it as a request dump that "would include any `Authorization` header". Grepping
  the file found four of the same class, and following the fourth to
  `__provision_update` found the private key. Three of the four were not in the
  item.

- **An unparseable cloud message was exported to the OpenTelemetry collector
  with its body attached.** `ezlopi_service_webprov.c` passed
  `rx_message->payload` to `TRACE_OTEL` on the `method == NULL` path. Now
  [a byte count](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L305).

  This is the one that is categorically different from the other three. The
  rest print to a serial console, which needs physical access to read;
  `TRACE_OTEL` ships what it is given *off the device*, to a collector, over
  the network. And the messages that reach this branch are by definition the
  ones whose shape the device did not understand, so there is no basis for
  assuming the body is safe to forward. If it needs to be diagnostic again, the
  leak-free form is the list of top-level member names — it identifies the
  shape without exporting a single value, and the code comment says so.

  The length needs `strlen`, and `#include <string.h>` is now stated. The file
  was already using `strlen` and `memset` while containing no `string.h` —
  checked against the committed version, which calls
  `strlen(str_data)` once, at the websocket send, and `memset` three times — so
  those declarations were arriving transitively down somebody else's include
  edge, and had been all along. Named now for the same reason the
  `esp_system.h` include in `ezlopi_core_api.c` is: a declaration that reaches
  you by accident disappears without a warning when the header that happened
  to carry it is reshuffled, and what you get then is an implicit declaration
  rather than a compile error.

  **Testing.** This line is inside `#ifdef CONFIG_EZPI_OPENTELEMETRY_ENABLE_LOGS`,
  which is `default n`, so **the four-target CI sweep does not compile it** and
  a green sweep says nothing whatever about it. Shipping an edit to code the
  compiler never saw is how a syntax error reaches the one user who enables the
  option, so a fifth build was run for this line alone: esp32 with
  `CONFIG_EZPI_ENABLE_OPENTELEMETRY=y`, `..._ENABLE_LOGS=y` and
  `..._ENABLE_TRACES=y`, verified by `grep` for `ezlopi_service_webprov.c.obj`
  in the build log rather than inferred from the config. It built.

  That fifth build also surfaced a **pre-existing** break, left alone as out of
  scope: in this same file `method_str` is declared under
  `#ifdef CONFIG_EZPI_OPENTELEMETRY_ENABLE_TRACES` but tested under
  `#ifdef CONFIG_EZPI_OPENTELEMETRY_ENABLE_LOGS`, so LOGS-without-TRACES does
  not compile at all, with or without this change. The fifth build enables both
  to get past it.

**Testing, all four.** Six builds. Four targets at default config — all passed,
images 1 646 704 / 1 337 056 / 1 661 472 / 1 720 000 bytes for
esp32 / s2 / s3 / c3 — plus two builds that exist only to compile lines the
default config does not: esp32 with the OpenTelemetry log options on, and esp32
with the BLE provisioning read path on. Each changed file was confirmed to have
actually been compiled, by `grep -c '<file>.c.obj'` over the build log, so that
"build OK" is evidence about these lines rather than about the project.

**The measurement that mattered more than the builds.** A green build says the
code parses; it says nothing about whether the credential is still in the
firmware. So the linked `esp32` image was searched for the format strings
themselves, in both directions — the new length-only string must be *present*,
the old secret-printing string must be *absent*:

| In `Ezlo_Pi_v4x.bin` (esp32, default config) | want | got |
| --- | --- | --- |
| `with a %d-char password` (new) | 1 | 1 |
| `with password %s` (old, the PSK) | 0 | 0 |
| `payload[len: %d] (body withheld;` (new) | 1 | 1 |
| `payload[len: %d]:` (old, the dump) | 0 | 0 |
| `Trying to connect to %s` (control, kept half of a fixed line) | 1 | 1 |

Both directions are needed. Present-only would not distinguish "compiled in"
from "linker kept an unreachable copy"; absent-only would not distinguish
"fixed" from "the whole macro is stripped and the check cannot fail". Together
they also settle the claim this section opens with — that `TRACE_D` reaches a
shipped image — by observation rather than by argument from Kconfig: the new
string is *in the binary*.

That pairing is also what caught the wrong claim about the BLE leak. The new
BLE string came back **absent**, which under the reasoning above means the
compiler never saw it; chasing that produced the `#undef` in
`EZLOPI_USER_CONFIG.h` and the correction recorded in that bullet. A build-only
test suite would have passed and the overstated claim would have shipped.

**And a CI guard, because this is a class.** Eight lines across seven files,
four of which were only found by going looking — that is a class, and fixing
eight lines does nothing to stop a ninth.
`tests/ci/check_no_secret_logging.py` fails the build if a credential-named
value is passed to a `TRACE_*`/`printf`/`TRACE_OTEL` call that has an
`%s`-class conversion, unless it is wrapped in `strlen`/`sizeof` — reporting a
length being the accepted fix throughout this tree.

It also understands the *other* accepted fix, a dump behind a switch, and does
something slightly more than tolerate it: `GATED_SWITCHES` records the default
each such option is expected to carry and the check verifies it against
`Kconfig.projbuild`. The exemption applies only to the branch where the guard
holds, so a secret dumped in the `#else` — the path a default build takes — is
still reported.

That table **pins** the value rather than demanding `n`, and the reason is that
the first version demanded `n` and was immediately wrong:
`EZPI_LOCAL_KEY_ANNOUNCE` is `default y` deliberately, because a self-flashed
device generates its own local key and with the banner off that key is
unknowable. The repository had already reasoned this through in that option's
help text. Pinning keeps the value honest without overruling it — changing when
a credential is printed fails the check until someone edits the table, which is
the moment to argue about it.

Verified by mutation rather than by passing: each of the four fixes was
reverted in place, the gate was removed from the websocket dump, and the pinned
Kconfig default was flipped — **five mutations, five caught**, each naming the
right file and line, and the tree clean afterwards. The self-test also refuses
to run the check at all unless a set of known-bad samples still fails, which is
how the first version's bug was found: it used `\bpassword\b`, which does not
match `password_saved`, the variable in the actual defect.

Two of the shell script's checks are worth recording too, because on the first
run both read as **failed fixes and neither was**. `with password %s : 1
(want 0)` matched `ezlopi_core_wifi.c:373` — inside the new comment, which
quotes the removed line on purpose so the file records what it used to do. And
`ws payload switch = 2 (want 3)` was a miscounted expectation: two occurrences
is correct, the `#ifdef` and the else-branch string. A grep for the removed
*text* is the wrong instrument when the fix deliberately preserves that text as
documentation; the checks now match a live call — a `TRACE_*`/`printf` at the
start of the line content with no `**` comment marker in front of it — and the
script carries a note saying a naive grep had read as a failure. The corrected
check was then confirmed **able to fail**, by appending a function containing
the original line and watching the count go 0 → 1 → 0.

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

### Log severity — [bug]

- **The default serial log severity was the length of the table it indexes.**
  [`ezlopi_log_severity_enum[]`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L136)
  has six entries, `"NONE"` through `"TRACE"`, and
  `ENUM_EZLOPI_TRACE_SEVERITY_MAX` is simultaneously that length and the enum's
  past-the-end sentinel. `serial_log_severity` was initialised to it, and
  `EZPI_core_serial_log_get_current_severity_enum_str()` returned
  `ezlopi_log_severity_enum[serial_log_severity]` directly — index 6 of a
  6-element array. The value read is whatever pointer-sized bytes follow the
  array, and all three callers of the two getters hand it straight to
  `cJSON_AddStringToObject()` or `snprintf("%s")`, so it is read as a string.

  **What this is not, stated first, because the first conclusion drawn here was
  wrong and the wrong one was nearly written down.** It is *not* reachable on a
  booted hub through that default. Two call sites raise `serial_log_severity`
  to `TRACE` unconditionally, immediately after the NVS read —
  [`ezlopi_core_ezlopi.c:137`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L137)
  and
  [`ezlopi_core_setting_commands.c:258`](ezlopi-core/ezlopi-core-setting-commands/ezlopi_core_setting_commands.c#L258)
  — each sitting under a commented-out `// #warning "remove this in release"`.
  So the out-of-bounds index is real in the source and masked in the image, by
  a debug line that is explicitly marked for deletion. Deleting that line, which
  is the stated intent, is what would expose it.

  Those two overrides are also the evidence for the fix's choice of default.
  `TRACE` is what every shipped boot has actually run with, so initialising to
  `TRACE` rather than to `NONE` changes the behaviour of nothing while making
  the initialiser a severity instead of a count.

  **One route in is not masked.** `EZPI_core_read_set_log_severities()` also
  reads both severities out of NVS, and `cloud_log_severity` is not touched by
  either override. That read was unchecked three ways over: the value was
  assigned with no range test, the `EZPI_SUCCESS` return was discarded, and
  `&cloud_log_severity` — an `e_trace_severity_t *` — was passed to a parameter
  declared `uint32_t *`. `EZPI_core_nvs_read_uint32()` does not write through
  its output pointer when it fails, so "the key is absent" silently meant "keep
  whatever was there", which only ever worked out because of the initialiser.
  A stored value outside the enum, from a downgrade, a renumbering or a
  partially written entry, became the index. Measured on a host with
  `0xDEADBEEF` in the entry: the unfixed code segfaults inside
  `EZPI_core_cloud_log_get_current_severity_enum_str()`. That is not a claim
  that an unauthenticated client can set it — both setters cap cloud severity
  at `WARNING` — it is a claim that nothing in the read path cared.

  **Fixed** by three independent guards and one corrected initialiser:
  [`__is_a_severity()`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L389) on
  every path that writes the index,
  [`__severity_str()`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L418) on
  every path that reads it, and an explicit unsigned bound on the NVS value
  ([`:208`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L208),
  [`:216`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L216)) read through a
  `uint32_t` temporary rather than a pointer pun. An out-of-range severity
  answers `"NONE"` rather than `NULL`, deliberately: every caller would read
  through a `NULL`, so returning one would trade an out-of-bounds read for a
  null dereference.

  Two smaller things came out of the same file and are fixed with it. The
  cloud-log gate compared a signed `int severity` against the enum, whose
  underlying type the compiler is free to make unsigned — and on this toolchain
  does, so `-1` was promoted to a huge unsigned value and *accidentally* failed
  the gate. The correct outcome rested on a choice no line of source made; the
  bound is now explicit at both ends
  ([`:331`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L331),
  [`:476`](ezlopi-core/ezlopi-core-log/ezlopi_core_log.c#L476)). Those two
  comparisons were also the file's two `-Wsign-compare`, which is why it can now
  be compiled `-Wall -Wextra -Werror` with no exemption.

  **Evidence.** [`tests/host/test_logseverity.c`](tests/host/test_logseverity.c)
  is the 36th host binary: 7 cases, **108** checks, 0 failures. It compiles the
  real `ezlopi_core_log.c` — with an `nm` guard for the case that nearly wasted
  the whole harness, because with `CONFIG_EZPI_UTIL_TRACE_EN` undefined the file
  and its header preprocess to an empty translation unit and the compiler
  reports success. `EZPI_core_serial_log_upcall` is `static`, so the test
  captures the pointer the subject hands to `ezlopi_util_set_log_upcalls()` and
  reaches it through that, redirecting `stdout` to a `tmpfile()` around each call
  to assert on what was printed as well as what was returned.

  Watched to fail against the pristine pre-fix subject, in three regimes,
  because each says something different:

  | regime | what it reported |
  |---|---|
  | `-fsanitize=undefined` | `ezlopi_core_log.c:194:36: runtime error: index 6 out of bounds for type 'char *[6]'` |
  | `-fsanitize=address` | `global-buffer-overflow`, `0 bytes after global variable 'ezlopi_log_severity_enum' ... of size 48`, `READ of size 8` |
  | no sanitizers | 6 assertion failures unaided, then a segfault once a case stores `0xDEADBEEF` |

  The third row is the one worth keeping: the sanitizers supply the line number,
  not the finding. (The probe compiled the unfixed file with `-Wno-sign-compare`,
  since it carries two of those and this suite is `-Werror`. Suppressing a
  diagnostic is not changing behaviour.)

  Selectivity, each breakage applied alone to the fixed subject and reverting
  exactly one part of it, out of 108 checks: default back to `MAX` — **2**
  failures; NVS value assigned unchecked — **2**; `__severity_str()` indexing
  with no bound — **0**; cloud gate dropping its lower bound — **4**. The zero
  is not a gap being passed over. With the other three guards standing, nothing
  can hand `__severity_str()` an out-of-range value, so its bound is unreachable
  by construction; applying the first and third breakages *together* brings back
  the ASan `global-buffer-overflow`, which is the measurement that shows what the
  innermost guard is for. It turns a regressed default from an out-of-bounds read
  into a merely wrong default.

  On hardware toolchains, both targets build `rc=0` with the object confirmed
  recompiled in the same run — a green build of a stale object looks identical to
  a green build of the fix — and zero diagnostics of any kind in the file. The
  fix also removes the two `-Wincompatible-pointer-types` that the esp32c3 build
  reported at the pre-fix `:109` and `:110`, those being the pointer pun
  described above. The current full-tree count is in the entry below, which this
  fix corrects.

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
  target. Now the handler asks `httpd_ws_get_fd_info()` instead of assuming.
  That check first answered a non-websocket GET with a one-line plain-text
  explanation of the port; it is now the discriminator that serves the local
  configuration page instead — see the Local operation section.

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

### Local API replies — [sec]

- **Every error reply carried two `error` members, and a browser read the
  wrong one.** [`EZPI_core_api_consume_cjson()`](ezlopi-core/ezlopi-core-api/ezlopi_core_api.c#L146)
  is the single function every one of this firmware's API methods answers
  through. After the method handler had run it added `error: null`
  unconditionally:

  ```c
  cj_response = __execute_method(cj_request, method);
  if (cj_response)
  {
      cJSON_AddNullToObject(__FUNCTION__, cj_response, ezlopi_error_str);
  ```

  cjext's `add_item_to_object()`
  ([`cjext.c:2511`](ezlopi-components/cjext/cjext.c#L2511)) ends in
  `add_item_to_array()`. It appends — no replace, no duplicate check. So a
  handler that had already reported a failure produced a reply with that member
  twice:

  ```json
  {"result":{...},"error":{"code":-32602,"data":"devcfg.chipset.mismatch"},"error":null}
  ```

  Which of the two a reader sees is decided by its JSON parser, and the two
  common answers disagree. cjext's own `get_object_item()` returns the **first**
  match, so every C caller inside this firmware saw the failure — which is why
  this survived: from the device's own side the API behaved correctly.
  JavaScript's `JSON.parse` keeps the **last** duplicate, as do Python's `json`
  and Go's `encoding/json`. A browser therefore read **every error reply from
  this API as a success with `error === null`**.

  Not deduced. A throwaway probe compiled against the real cjext built the
  response the way `ezlopi_cloud_devcfg.c` does, applied the line above, and
  printed the bytes: two children named `error`,
  `cJSON_GetObjectItem()` → the object, last-wins → `null`.

  The fix is a guard, three lines, and the shape of it matters as much as the
  presence:

  ```c
  if (NULL == cJSON_GetObjectItem(__FUNCTION__, cj_response, ezlopi_error_str))
  {
      cJSON_AddNullToObject(__FUNCTION__, cj_response, ezlopi_error_str);
  }
  ```

  Simply deleting the unconditional add would also have removed the duplicate,
  and would have broken every client that tests `error === null` on a success:
  a successful reply would carry no verdict at all. Both directions are
  regression-tested, and both mutations were run (see Testing).

  Scope, measured rather than estimated. Three files add an `error` *object* to
  a reply that then passes through this envelope, each through a local
  `prepare_error` helper, and between them they have **21 refusal paths**:
  [`ezlopi_cloud_devcfg.c`](ezlopi-cloud/ezlopi-cloud-devcfg/ezlopi_cloud_devcfg.c#L128)
  (8),
  [`ezlopi_cloud_mqtt_config.c`](ezlopi-cloud/ezlopi-cloud-mqtt-config/ezlopi_cloud_mqtt_config.c#L127)
  (8) and
  [`ezlopi_cloud_offline_login.c`](ezlopi-cloud/ezlopi-cloud-offline-login/ezlopi_cloud_offline_login.c#L75)
  (5). An earlier draft of this entry said "27 sites across 9 files"; that
  counted the six prototype and definition lines as call sites and the file
  count was simply wrong. The corrected number is smaller and the consequence
  is worse, because of which three files they are.

  The sharpest one is the login. `hub.offline.login.ui` refuses a wrong local
  key with `{"code":-32500,"message":"Bad password","data":"user.login.badpassword"}`,
  and that reply was shipped with `error: null` appended after it. A browser
  reading `error === null` concludes the login succeeded. The device itself was
  never fooled — it kept refusing every subsequent call, because the session
  gate is server-side — so the failure mode is a UI that shows a connected hub
  and then cannot do anything with it, with no error anywhere to explain why.

  [`ezlopi_cloud_ota.c`](ezlopi-cloud/ezlopi-cloud-ota/ezlopi_cloud_ota.c#L92)
  is a fourth case and a milder one: it adds its own `error: null` at two sites,
  so those two replies carried the member twice with the *same* value. Harmless
  to read either way round, still malformed, and now gone for the same reason.

  This matters now in a way it did not before, because the thing on the other
  end is becoming a browser rather than the Ezlo app.

- **`ezlopi_core_api.c` now includes `esp_system.h` for the `esp_restart()` it
  calls.** It did not, and the declaration arrived down a chain traced through
  the ESP-IDF checkout rather than guessed at: `cjext.h` →
  `EZLOPI_USER_CONFIG.h` → `driver/uart.h` → `freertos/task.h` →
  `freertos/portmacro.h` → `esp_system.h`. Unconditional in both the xtensa and
  the riscv port, so it held for all four targets — but both ports carry IDF's
  own `required by esp_get_...() functions in portable.h. [refactor-todo] Update
  portable.h` against that very include, at xtensa `portmacro.h:50` and riscv
  `portmacro.h:45`. When `portable.h` is updated and the include goes,
  `esp_restart()` here becomes an implicit declaration returning `int` rather
  than a compile error — which is the quiet kind of breakage, since the real
  `esp_restart()` returns `void` and is `noreturn`.

  Noted because it is also what made the host test below possible: once the file
  names the header it needs, the only ESP-IDF surface it has is one function.

**Testing.** [`tests/host/test_api_envelope.c`](tests/host/test_api_envelope.c)
compiles and links the real `ezlopi_core_api.c` against the real `cjext.c` — not
a transcription of either — and exercises the envelope through twelve tests and
58 checks. The Makefile rule asserts with `nm` that the subject symbol
`EZPI_core_api_consume_cjson` is actually present in the object it just built,
because a file that compiles to an empty object also links and also passes.
Eight symbols are faked: the four method-list lookups, `rpc_method_notfound`
(a copied-and-cited reproduction of
[`ezlopi_core_api_methods.c:145-156`](ezlopi-core/ezlopi-core-api/ezlopi_core_api_methods.c#L145)),
`broadcast_add_to_queue` (which serialises and frees, because the real queue
takes ownership), `event_group_set_event`, and `esp_restart`. Everything else is
the shipping code.

Two of them carry the fix directly — exactly one `error` member on a failure, and
an explicit `error: null` still present on a success, which is the pair that
pins the fix between its two wrong neighbours. A third is deliberately broader
than this defect: `assert_no_duplicate_members()` walks the entire reply tree,
objects and arrays, and fails on any object with two children of the same name.
It is written that way because the cause is a property of `cjext` rather than of
this call site, so the next instance of it will be somewhere else.

The harness was proved able to fail before its pass was trusted, by mutating the
worktree file in both directions and restoring it under a sha256 comparison:

```
=== pre-fix (unconditional add) ===
    exit 1  58 checks, 3 failures
    FAIL: exactly one "error" member on a failure (two is the defect)
    FAIL: first-wins and last-wins readers see the same "error"
    FAIL: failing handler: member "error" appears twice in one object

=== careless fix (never add) ===
    exit 1  58 checks, 4 failures
    FAIL: exactly one "error" member on a success
    FAIL: a success still carries an explicit error: null
    FAIL: the error member is still there exactly once
    FAIL: the reply's own error member is not doubled

restored byte-identical: True

=== fixed tree (the committed state) ===
    exit 0  58 checks, 0 failures
```

The three failures under the first mutation include the general invariant firing
on its own, independently of the two checks written for this bug. The second
mutation is the plausible wrong fix, and it fails differently — which is the
point of running it.

The changes to `tests/host/stubs/` (FreeRTOS `BIT0`–`BIT7`, `trace.h` including
`FreeRTOS.h`, and the removal of a `CJSON_TRACE` definition the real header does
not have) sit on the include path of twenty other host binaries, so the full
suite was rebuilt from clean rather than just the new target: **21 binaries,
4688 checks, 0 failures**, consistent with the previous 4630 plus these 58.

The `esp_system.h` include is the part a host pass cannot speak to — on a host it
resolves to a one-line stub, and the question is whether the real header is
reachable from this component's include path in every target configuration. All
four CI targets were built, and then built again from an export of `HEAD`, so the
comparison is a set difference rather than a count. This project has been burned
by counting warnings before.

| Target | Image before | Image after | Δ | Warnings added | Removed |
|---|---|---|---|---|---|
| esp32 | 1 646 640 | 1 646 656 | +16 | 0 | 0 |
| esp32s2 | 1 336 992 | 1 337 008 | +16 | 0 | 0 |
| esp32s3 | 1 661 408 | 1 661 440 | +32 | 0 | 0 |
| esp32c3 | 1 719 920 | 1 719 936 | +16 | 0 | 0 |

`implicit declaration` appears zero times in all four logs, which is what
confirms the explicit include resolves everywhere. The one warning the sweep
reported in `ezlopi_core_api.c` on each target was present identically in the
baseline: an unused `cj_method` inside a `CONFIG_EZPI_UTIL_TRACE_EN`
else-branch whose only use of the variable was commented out. The set
difference listed it as both added and removed — same file, same variable, same
`-Wunused-variable`, at a line number moved down by the comment this change
adds above it. That was the entire difference between the two warning sets on
all four targets.

It was left alone at the time, on the grounds that choosing between restoring
the trace and deleting the dead branch is a logging question rather than an
envelope one. That question has since been answered under **Dead code —
[bug]** below: the branch and the variable are both gone, and with them the
warning, so `ezlopi_core_api.c` now compiles without diagnostics on all four
targets.

### JSON library — [bug]

Two defects in the vendored `cjext`, with one root cause: the length recorded
beside a string does not mean the same thing in all the places that write it,
and the code that reads it assumed it did. They are here rather than under
*Memory safety* because neither is reachable on a shipped build — the first is
a wrong answer rather than a bad access, and the second is latent. A third
defect of the same family **is** reachable on a shipped build; it is fixed
separately, under **JSON library, printing — [sec]** below.

The three conventions, since every claim below depends on them:

| written by | value | text is |
|---|---|---|
| copying parser ([`cjext.c:783`](ezlopi-components/cjext/cjext.c#L783)) | span from the opening quote to the closing one — `strlen + 1`, more with `\uXXXX` escapes | owned, NUL-terminated |
| reference parser ([`cjext.c:1279`](ezlopi-components/cjext/cjext.c#L1279)) | the decoded length, which is now always exactly `strlen` — a string with escapes in it is no longer referenced at all, see the printing entry below | points into the caller's payload, followed by `"`, **not** NUL-terminated |
| built in code (`add_item_to_object`, `cJSON_CreateString`, `cJSON_CreateRaw`, `cJSON_SetValuestring`, …) | `strlen` | owned, NUL-terminated |

The first of those was established by reading
`parse_string()` rather than by trusting the previous entry on this, which had
it as "the parser stores the inclusive length" without distinguishing the two
parsers — and the two parsers are on opposite sides of this.

- **`cJSON_GetObjectItem()` returned a member whose key is merely a prefix of
  the name asked for.** The lookup passes the *stored* key's `str_key_len` to
  `case_insensitive_strcmp()` as a byte budget
  ([`cjext.c:2396`](ezlopi-components/cjext/cjext.c#L2396)), and the old
  comparison returned equal as soon as that many characters had matched,
  without ever checking that the requested name ended there. So under the two
  conventions that record `strlen`, a key matched any name it was a prefix of:
  on an object holding `config`, `cJSON_GetObjectItem(o, "configSize")`
  returned the `config` node — and did so even when no `configSize` member
  existed at all, which is the shape that makes it dangerous, because the
  caller gets a non-`NULL` answer where the correct one is `NULL`.

  **The previously recorded diagnosis was wrong about which trees are
  affected.** It said parsed trees were safe and built trees were not. Only the
  *copying* parser is safe, and only by accident: its extra one lets the
  compare reach the key's terminator. `cJSON_ParseWithRef()` records `strlen`,
  so reference-parsed trees were affected exactly as built ones were — which
  matters, because that is the parser the stored device configuration and the
  BLE provisioning payload go through. The old entry's claim that
  "parsed trees cannot misfire regardless of the name" covered both parsers and
  was true of one.

  **The previously recorded *fix* would have introduced a heap overread.** It
  was "make `add_item_to_object()` store the inclusive length". But
  `cJSON_strdup()` at the time copied `string_len + 1` bytes, so a built key
  recorded as `strlen + 1` would have been copied as `strlen + 2` — one byte
  past its own allocation — every time `cJSON_Duplicate()` ran. It would also
  have left the reference parser broken, since nothing about it would have
  changed. This is why the fix went in on the reading side instead.

  Fixed by making the budget bound the walk without deciding equality:
  equality now also requires the name to end where the key does
  ([`cjext.c:117`](ezlopi-components/cjext/cjext.c#L117)). That is correct under
  all three conventions without knowing which one produced the key, which was
  the property worth having — the conventions are not going to be unified
  cheaply. The budget could not simply be widened by one instead: a
  reference-parsed key is followed by a quote rather than a NUL, so
  `key[str_key_len]` must never be read, and the new comparison never reads it.

  **The recorded blast radius turned out to be empty.** The reason this was
  left unfixed before was that `str_key_len` also feeds the key printer, "and
  every response from all 123 API methods goes through that printer". The
  printer is in fact insensitive to the recorded length being *too large*: its
  scan loop tests the NUL before the budget
  ([`cjext.c:906`](ezlopi-components/cjext/cjext.c#L906), `(*input_pointer &&
  input_len--)`, short-circuiting), and it derives its output length from how
  far the pointer got rather than from the budget. So no amount of
  over-counting changes a printed key. That is asserted rather than argued:
  `test_key_printing_does_not_depend_on_the_recorded_length()` builds one tree
  whose keys carry two different conventions at once and pins the exact
  serialisation. The claim about a board was wrong too — no board was needed to
  settle any of this.

- **`cJSON_Duplicate()` produced keys and values that were not C strings.**
  Latent, and found by following the first fix rather than by looking for it.
  `cJSON_strdup()` allocated `string_len + 1` bytes and then `memcpy`'d
  `string_len + 1` bytes out of the source — which is only correct if
  `string_len` is a `strlen`, so that the extra byte read is the source's own
  terminator. Two of its seven callers do not pass one, and both are in
  `cJSON_Duplicate()`, which propagates whatever length the source item
  recorded. That count is worth stating carefully, because the obvious way to
  take it is wrong: *six* of the seven pass a `str_value_len` or a
  `str_key_len` rather than a literal `strlen()` call, which looks alarming
  until each is read — four of those six have the field assigned from
  `strlen()` on the line immediately above the call, so they are `strlen`s in
  value.

  Measured, not inferred. Duplicating a reference-parsed `{"config":null}`
  under AddressSanitizer gave a duplicated key of seven bytes, `config"`, in a
  seven-byte allocation with no terminator anywhere, and `strlen()` on it
  reported `heap-buffer-overflow … 0 bytes after 7-byte region`. The value side
  ([`cjext.c:3311`](ezlopi-components/cjext/cjext.c#L3311)) is the more
  dangerous half of the two, because `valuestring` is read as a C string in
  hundreds of places across the firmware whereas a key is mostly only printed.

  Fixed in `cJSON_strdup()` itself — copy `string_len` bytes and write the
  terminator here — rather than at the two call sites, after checking all seven
  callers individually: the five that pass a `strlen` are unaffected, and the
  two in `cJSON_Duplicate()` propagate a length that is at or above the
  source's `strlen` and never below, so copying one byte fewer cannot truncate
  anything.

  **It is latent rather than live**, and the reason is worth stating so that it
  is not re-derived: the only two callers of `cJSON_ParseWithRef()` in the
  firmware — [`ezlopi_core_devices.c:630`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L630)
  for the stored device configuration and
  [`ezlopi_service_ble_provisioning.c:416`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L416)
  for the provisioning payload — neither duplicates its tree. Copying-parsed
  and built trees were over-reading by one byte within an allocation they
  owned, which is harmless in practice and still wrong.

**Evidence.** Three new tests in
[`tests/host/test_cjext.c`](tests/host/test_cjext.c), which those took to 100
checks, 0 failures; the printing entry below has since taken the file to 124.
The lookup test was written before the fix and failed in
exactly the three places predicted — the built tree, the reference-parsed tree,
and the reference-parsed tree with the longer key first — and passes in all
fifteen of its assertions after it, with `cJSON_GetObjectItemCaseSensitive()`
as a control that was correct throughout. The duplication test was shown to fail against the old
`cJSON_strdup()` spliced back in, reading the key back as `config"` and the
value as `abcd"`; its first two assertions read the last byte of the
duplicate's own allocation, so they are in bounds either way and catch the
defect without a sanitizer, while ASan is what turns a future regression that
runs *further* off the end into a hard error.

Beyond the new tests, the fix retired a workaround:
[`tests/host/test_devcfg_api.c`](tests/host/test_devcfg_api.c) had its own
exact-match reader, written because the firmware's own lookup could not be
trusted to read a response, plus a test pinning the defect in place. Both are
gone and its 25 response reads now go through `cJSON_GetObjectItem()`, so those
assertions exercise the fixed lookup against real built API responses rather
than working around it. That file is at 319 checks, 0 failures.

**Two long comments elsewhere in the tree describe these same conventions and
were deliberately left alone**, which is worth saying because they are the
obvious thing for a reader to suspect of being stale after this commit: the
header comment on `__take_field()` in
[`ezlopi_cloud_mqtt_config.c`](ezlopi-cloud/ezlopi-cloud-mqtt-config/ezlopi_cloud_mqtt_config.c#L417)
and the do-not-swap-this-line note in
[`ezlopi_core_api.c`](ezlopi-core/ezlopi-core-api/ezlopi_core_api.c#L108).
Both were re-read against the fixed source. Both are about *values* and about
the parsers, neither of which this commit touches — the parsers still disagree,
a referenced value is still terminated by a quote rather than a NUL, and
`EZPI_STRNCMP_IF_EQUAL()` still takes the longer of the two lengths — so both
conclusions still hold and neither comment cites a `cjext.c` line number that
the insertion could have moved. The one thing that changed is a reason the
`ezlopi_core_api.c` note did not list: reference-parsed *keys* were also
mis-compared on lookup. That reason is now gone, which strengthens its
conclusion rather than weakening it, so the note stands as written.

All four targets build with no new warnings: esp32 `1683856` (unchanged),
esp32s2 `1374192` (+16), esp32s3 `1698720` (+16), esp32c3 `1757104`
(unchanged). Sixteen bytes is the image padding quantum, so the real cost of
both fixes together is somewhere between nothing and 16 bytes per target — a
rewritten comparison loop and one fewer byte in a `memcpy` length. Unlike most
entries here the generated code genuinely did change, so a zero delta on two
targets is the padding absorbing it rather than a build that did not pick the
change up; the +16 on the other two is what confirms the change is present.
**Nothing was flashed.**

**The tree-wide sweep that preceded the fix**, kept because it is the reason
nobody has to go looking for broken behaviour in the field: no call site in the
firmware was getting a wrong answer that mattered. It ran while the defect was
still open, to decide how urgent fixing it was, and two of its conclusions were
wrong in ways the fix exposed.

The sweep started by measuring rather than by reading more `cjext.c` — a first
pass at reading `parse_string()` had got the convention backwards. A 17-check C
probe under ASan/UBSan reported `str_key_len` beside `strlen()` for the same
key reached both ways, identifying a returned child by its *index* via pointer
identity so that a correct-by-accident value could not pass. Measured: a
copying-parsed `"config"` stores 7, a built one stores 6. Seven of the
seventeen checks disagreed with a correct lookup. The probe also showed the
exposure was wider than sibling pairs: an object holding `{"config":1}`
*alone* returned that member for a `configSize` lookup, because the comparison
stopped before the mismatch.

With that, the sweep was mechanical:

- **40 colliding names.** Of the key names this firmware looks up, the pairs
  where one is a strict case-insensitive prefix of another.
- **127 call sites** look up one of those 40 names.
- **25 of the 127 search an object this firmware built**, found by an awk pass
  that, per function body, intersects the locals assigned from
  `cJSON_CreateObject`/`CreateArray`/`CreateRaw`/`Duplicate`/
  `AddObjectToObject`/`AddArrayToObject` with the locals used as the search
  object. The first version of that scan counted only `Create*` and so missed
  two sites; `AddObjectToObject` returns a freshly built child and matters just
  as much.
- **23 of those 25 are immune by construction.** They search an item-value
  object, and every such object in the firmware is built by the single
  chokepoint
  [`ezlopi_core_valueformatter.c`](ezlopi-core/ezlopi-core-valueformatter/ezlopi_core_valueformatter.c),
  which adds only `value` and `valueFormatted`. `value` is the shortest key
  present, so nothing can be a strict prefix of it — confirmed mechanically
  rather than by eye, by grepping for the only four names that could be
  (`ezlopi_v_str`, `ezlopi_va_str`, `ezlopi_val_str`, `ezlopi_valu_str`): zero
  matches.
- **2 sites were genuinely exposed**, both in
  [`ezlopi_core_scenes_operators.c`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_operators.c)
  at lines 1102 and 1148, and both look up `valueType` on a freshly-built
  item-value object. Because that object never contains a `valueType` key, the
  lookup resolved to the `value` item instead. Both were masked: the result
  goes to `cJSON_GetStringValue()`, which returns `NULL` for anything that is
  not a string, into a `NULL`-safe `STR_OP_COMP` comparison against
  `"dictionary"`. They now get the `NULL` a correct lookup returns, so their
  behaviour is unchanged by the fix as well as by the defect.

**The first wrong conclusion was the scoping.** "Everything else searches a
parsed tree" treated the two parsers as one and wrote off every site that
searches a parsed tree, including explicitly `cJSON_ParseWithRef` — which
records `strlen` and was exposed exactly as a built tree was. So the sweep's
25-of-127 classification under-counted. The count was not redone, and the
reason is that the fix removes the question: all 127 sites now resolve
correctly whichever of the three ways their tree was made, which is a stronger
statement than any recount of which ones were at risk.

**The second was the residue.** The sweep left one item open:
`ezpi_value_scales_list()` at
[`ezlopi_cloud_scenes_block_data_list.c:268`](ezlopi-cloud/ezlopi-cloud-scenes/ezlopi_cloud_scenes_block_data_list.c#L268)
keys a built object by the runtime `value_type` of an item and then looks that
key back up, so a prefix pair among the value-type names would silently merge
two types' scale lists into one — zero such pairs among the 26 value types
shipped drivers use, but one driver away from not being zero, with no test that
would catch it. That is closed, not by counting the names again but because the
lookup no longer matches a name against a key it merely extends: a
`volume_flow` lookup against a `volume` key now returns `NULL`. The
absent-name assertions in
`test_object_lookup_is_not_prefix_ambiguous()` are what hold that, and they
cover both directions — a name longer than any key, and a name shorter than
the key it is a prefix of.

One claim of the sweep's that stands unchanged: the item-id keys of
`cj_device_items_data` cannot collide, because every id is rendered `%08x` and
so every key is exactly 8 characters, and no fixed-width set can contain a
strict prefix pair.

### JSON library, printing — [sec]

The third defect of the family above, and the one the two entries there were
careful to say they were not: this one **is** reachable on a shipped build, on
every boot. It is one defect in two halves — the printer wrote outside the
buffer it had just sized, and the string it was writing was not the value it
claimed to be — and they are fixed together because the second is what makes
the first reachable, but they are *proved* separately, because each fix holds a
different property.

- **`print_string_ptr()` wrote past the end of the allocation it had just
  sized.** The function
  ([`cjext.c:875`](ezlopi-components/cjext/cjext.c#L875)) walks the string
  twice. The first walk counts the characters needing escapes and is bounded by
  the NUL *or* the recorded length, whichever comes first
  ([`cjext.c:906`](ezlopi-components/cjext/cjext.c#L906), `(*input_pointer &&
  input_len--)`); the allocation is then that span plus the escapes found in
  it. The second walk copies, and was bounded by the NUL alone.

  The two bounds agree whenever there is a NUL at or before `input_len` — which
  is every string cjson built or copy-parsed, and no string
  `parse_string_with_ref()` produced. Those point into the caller's payload and
  are terminated by their own closing quote, with no zero byte until the end of
  the whole payload. So a reference-parsed string with anything in it that needs
  escaping was copied until that far-away zero byte, into a buffer sized for the
  value alone. A heap **write** overflow, not an overread.

  It stayed hidden because the escape-free case is a different path: a
  reference-parsed string with nothing to escape is `memcpy`'d with the measured
  span as its length ([`cjext.c:942`](ezlopi-components/cjext/cjext.c#L942)) and
  was always correct. One quote, backslash, tab, newline or control character
  inside a stored string is what it takes.

  **Reachability, read step by step rather than assumed.**
  [`EZPI_device_prepare()`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L608)
  parses the stored device configuration with `cJSON_ParseWithRef()`
  ([line 630](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L630)), and
  on the path where that parse *succeeds* it writes the same tree straight back
  ([line 720](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.c#L720) →
  [`EZPI_core_factory_info_v3_set_ezlopi_config()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L938)),
  which serialises it with `cJSON_PrintPreallocated()` into a 4 KB heap buffer
  ([`ezlopi_core_factory_info.c:946`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L946)).
  Every boot with a stored configuration therefore prints a reference-parsed
  tree.

  The three writers of that configuration all use the *copying* parser —
  [`ezlopi_service_ble_dynamic_config.c:347`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_dynamic_config.c#L347),
  [`ezlopi_service_uart.c:1421`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L1421)
  and the local API's
  [`EZPI_CLOUD_devcfg_port_write()`](ezlopi-cloud/ezlopi-cloud-devcfg/ezlopi_cloud_devcfg_port.c#L88)
  — so the *write* was safe and the fault fired on the next boot instead, and on
  every boot after that until the configuration changed. None of those three
  requires authentication on a shipped build, and
  `EZPI_util_devcfgval_check()` refuses a configuration on its chipset and its
  ids without ever looking at the characters inside a value, so a device name
  containing a quote passes it.

  Measured before the fix, under AddressSanitizer, on the fixture now committed
  in `test_cjext.c`: `WRITE of size 1`, `0 bytes after 115-byte region`, inside
  `print_string_ptr`.

  Fixed by bounding the copy with the span the first walk measured: it is
  recorded as `input_end`
  ([`cjext.c:929`](ezlopi-components/cjext/cjext.c#L929)) and the copy loop
  compares against it
  ([`cjext.c:982`](ezlopi-components/cjext/cjext.c#L982)) instead of testing for
  a NUL. The sizing and the copying then come from the same number, so the
  writes cannot leave the allocation whatever the string's provenance — which is
  the reason to prefer it to re-deriving `input_len`, and the reason it is kept
  even after the second half below removes the only case that needed it.

- **A referenced string containing an escape described a value that was not in
  the payload.** Referencing works because the bytes in the payload already
  *are* the bytes of the value. An escape breaks that: the payload holds six
  bytes for `\u00e9` where the value is two, so there is nowhere to point that
  spells the value. `parse_string_with_ref()` recorded the *decoded* length
  ([`cjext.c:1279`](ezlopi-components/cjext/cjext.c#L1279)) — the only honest
  number — against raw bytes, so a reader stopped short of the closing quote
  mid-escape and the printer re-escaped the raw backslash. Measured:
  `{"dev_name":"a\tb"}` parsed and printed back came out as
  `{"dev_name":"a\\t"}`.

  That is in-bounds and silent, and it is on the way to NVS: the same
  `EZPI_device_prepare()` round trip above is what stores it. So the two halves
  compound — the first turns an escaped stored name into a heap overflow, and
  the second turns it into a corrupted name even once the overflow is gone.

  Fixed by not referencing such a string at all: when the scan found any escape,
  `parse_string_with_ref()` now delegates to `parse_string()`
  ([`cjext.c:1274`](ezlopi-components/cjext/cjext.c#L1274)), which copies and
  decodes it exactly as `cJSON_Parse()` would.

  **The alternative was recording the raw span instead** — keeping the reference
  and adding `str_value_len + skipped_bytes` as a second length so the printer
  could be handed the raw bytes with a raw length. Rejected: it makes the
  recorded length mean a *fourth* thing on one path, which is the root cause of
  all three defects in this family, and it fixes only the printer. Every other
  reader of `valuestring` — and there are hundreds — would still see raw
  escapes under a decoded length. Delegating costs an allocation on the path
  chosen to avoid allocating, which is worth being precise about: only for a
  string that actually contains a backslash, and no key or value in a device
  configuration has one unless a user put it there.

  **The teardown check the earlier gap entry demanded was performed, and it
  found the hook already in place.** Mixing owned and referenced strings inside
  one reference-parsed tree is only safe if `cJSON_Delete()`
  ([`cjext.c:212`](ezlopi-components/cjext/cjext.c#L212)) decides per item, and
  it does — through two complementary pairs of frees, one keyed on
  `cJSON_IsReference`/`cJSON_StringIsConst` in `->type` and one on
  `is_key_ref`/`is_value_ref`, each NULLing the pointer between them.
  `parse_object_with_ref()` sets `cJSON_StringIsConst`
  **conditionally**, only when the key really is a reference
  ([`cjext.c:2171`](ezlopi-components/cjext/cjext.c#L2171)), which is exactly
  what makes a copied key in a reference-parsed tree get freed rather than
  leaked. That was established by probe, not by reading: a first pass at
  reasoning about it concluded the opposite — that a reference key would be
  freed as though it were owned, a double-free — and a 13-fixture probe under
  ASan/LSan showed every reference key carrying `cJSON_StringIsConst` and
  disproved it. The probe also round-tripped `cJSON_Duplicate()` over a
  deliberately mixed tree. No leaks, no double frees.

**Evidence: each half was shown to fail on its own.** Two new tests in
[`tests/host/test_cjext.c`](tests/host/test_cjext.c), and a two-stage mutation
run that is the point of the exercise rather than a formality:

| subject | result |
|---|---|
| `HEAD`'s `cjext.c` — both defects present | 3 failures, then an ASan `heap-buffer-overflow` abort in `print_string_ptr` |
| the fix with **only** the parser half reverted — span bound kept, delegation removed | 118 checks, 6 failures, **0 sanitizer reports**, run completes |
| the tree as committed | 124 checks, 0 failures |

The second row is what separates them. With the loop bound alone the memory
safety fault is gone and the output is merely wrong; with the delegation alone
the output is right. Neither row is reachable by reverting the whole change, so
a single-stage mutation would have proved one fix and credited it with both.
The 118 against 124 is not a missing test either: the six assertions about a
copied key sit behind the `if (NULL != escaped)` guard that its lookup fails,
so they are skipped rather than passed.

The fixture exercises every escape branch in one payload — a tab, an escaped
quote, a doubled backslash, a `\uXXXX` above 127 and one below 32, and a plain
neighbour with nothing to escape — and compares the reference parser's printed
output against the copying parser's, with the expected text *measured* rather
than written by hand. The plain neighbour is asserted to still be
`is_value_ref == cJSON_True`, or the whole test would pass for the wrong reason:
a parser that copied every string would satisfy everything else in it. The
second test covers all four (key, value) × (owned, referenced) combinations and
checks that a copied key does not alias the payload; its teardown half has no
assertion at all, because the thing being checked is that ASan and LSan stay
quiet, so the test *failing to crash* is the result.

**A pre-existing property found while writing those tests, now pinned.**
`cJSON_PrintPreallocated()` needs `strlen(output) + 2`, not `+ 1`. Measured
across ten value shapes — bare number, bare string, flat object, nested object,
array, with and without escapes — and identical at `HEAD`, so it is a property
of the preallocated path's own accounting and not of this change or of
escaping. The test asserts it in both directions: `strlen + 2` must succeed and
produce the right text, and `strlen + 1` must be *refused* rather than written
past. Worth recording because a failed print leaves a partial write with no
terminator anywhere in the destination — measured, under **`__otel_publish()`**
in *Open-telemetry* above, which was the one caller that neither cleared the
buffer first nor checked the return and is fixed
there. `EZPI_core_factory_info_v3_set_ezlopi_config()`, the caller on the
path fixed here, does check it, so a configuration too large for its 4 KB
buffer is refused rather than half-written to the partition.

All four targets build clean: esp32 `1683856`, esp32s2 `1374192`, esp32s3
`1698720`, esp32c3 `1757104` — **zero delta on every target** against the
previous commit, with no new warnings and no `-Wshadow` diagnostics. That needs
saying rather than passing over, because unlike most entries here the generated
code genuinely did change: a loop bound went from a NUL test to a pointer
comparison, and `parse_string_with_ref()` gained a parameter, a branch and a
call. The call is to `parse_string()`, which was already linked in, so the new
code is a test and a call — below the 16-byte image padding quantum, which is
why four zeros is the expected answer here rather than a build that missed the
change. The host suite is the evidence that it is present. **Nothing was
flashed.**

### Device configuration — [bug]

- **A pin number a board does not have no longer turns into one it does.** The
  valid-pin test every driver uses is not a range check. `GPIO_IS_VALID_GPIO(n)`
  expands to `((1ULL << (n)) & SOC_GPIO_VALID_GPIO_MASK) != 0`
  ([`driver/gpio.h:50`](https://github.com/espressif/esp-idf/blob/v4.4.6/components/driver/include/driver/gpio.h#L50)),
  so the number out of the configuration is a *shift count*. Shifting by a count
  outside 0–63 is undefined, and the way it actually behaved was not a rejection:
  every shift instruction involved truncates the count to the register width, so
  the number wrapped and named a different, real pin.

  `EZPI_core_cjson_get_value_gpio()` is the one place that number entered. It
  checked nothing — it returned `(int)o_item->valuedouble` for whatever the
  document carried, and `-1` only when the key was missing — and 144 call sites
  across 10 of the pin keys fed it straight to the macro.

  Measured on the host, with the macro's own expression and the esp32 mask, at
  runtime values so nothing could be constant-folded:

  | `"gpio"` in the document | `GPIO_IS_VALID_GPIO` | `.pin_bit_mask` built next | pin actually configured |
  |---|---|---|---|
  | 70 | accepted | `0x40` | **GPIO 6 — SPI flash clock** |
  | 64 | accepted | `0x01` | GPIO 0 |
  | 72 | accepted | `0x100` | GPIO 8 |
  | 100 | accepted | `0x10_0000_0000` | GPIO 36 |

  GPIOs 6–11 are the flash bus on a standard esp32 module. A configuration
  naming pin 70 passed the valid-pin test and had the driver reconfigure the
  flash clock as a push-pull output, which takes down the running image. The
  aliasing *target* is architecture-specific and was read off the assembly both
  target compilers emit for `1ULL << n`, neither of which contains a range
  check; "accepted when it should not be" is a host measurement, not an
  inference.

  **The fix is one bound at the read, not 144 changes at the call sites.** A
  number is returned only if it is in `[0, GPIO_NUM_MAX)` for the target being
  built, or is one of the two sentinels. The test is on the `double`, before the
  narrowing cast, because converting a `double` that does not fit an `int` is
  itself undefined and `cJSON` parses numbers with `strtod` — `"gpio":1e999`
  arrives as `+inf`.

  Three things were deliberately left alone:

  - **The mask is not consulted.** In-range-but-reserved pins (24 and 28–31 on
    esp32) are still the callers' business, and `GPIO_IS_VALID_GPIO` answers
    that correctly once the shift count is in range. This closes the undefined
    shift; it does not duplicate the valid-pin test. It also keeps the bound
    expressible on a host, where `GPIO_NUM_MAX` exists and the mask does not.

    Still true of the mask, and no longer true of the flash pins: the same
    accessor now also refuses a pin the chip is using for flash or PSRAM, which
    is a question `GPIO_IS_VALID_GPIO` is not able to answer at all — see *Pins
    the chip is using for flash or PSRAM* below for why that distinction is the
    one that decides where the test belongs.
  - **`-1` and `255` pass through untouched.** Both are "no pin" spellings the
    drivers test for by hand, and a configuration using either works on a board
    today. They remain out-of-range shift counts; both were checked against the
    xtensa and riscv32 compilers at `-O2` and both fold to `0`, which is the
    answer the callers need. That is a measured accident and is named in the
    source rather than left to be rediscovered.
  - **The document is not refused.** An unusable pin number disables that one
    pin, exactly as an absent key does. Refusing configurations that work today
    is the failure mode this area already has a correction recorded for under
    *Known gaps*.

  **Three other readings of this code were wrong, and finding that out is most
  of what the work was.** Each had been written down as the headline defect
  before being checked:

  | Claim | Why it is not a defect |
  |---|---|
  | Output pins are validated with the input-capable macro, so an input-only pin can be driven | ESP-IDF's `gpio_config()` already returns `ESP_ERR_INVALID_ARG` with `"GPIO can only be used as input mode"` for exactly that, and the drivers check the return. Adding `GPIO_IS_VALID_OUTPUT_GPIO` to ~25 sites would move *where* the rejection happens, not whether |
  | `device_0001_digitalOut_generic.c:303` dereferences `item` outside the `if (item)` guard | The dispatcher at [`ezlopi_core_ezlopi.c:217`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L217) walks `while (curr_item)`, so `item` is never NULL. The guard is dead defensiveness, not a missing one |
  | The `gpio_config()` return is discarded at `:325` | For an input-mode config its only failure paths are an empty mask or a bit outside the valid mask, both already excluded by the `GPIO_IS_VALID_GPIO` three lines earlier. Unreachable failure — sloppy, not broken |
  | The sentinel shifts are undefined and therefore dangerous | Undefined, yes, and both toolchains fold `-1` and `255` to `0` at `-O2` with no warning. Latent and measured benign, which is a different entry than a live bug |

  The per-target input-only pins were computed from the two SOC masks rather
  than recalled: esp32 → 34–39, esp32s2 → 46, esp32s3 and esp32c3 → none.

  **Tested by [`tests/host/test_cjsongpio.c`](tests/host/test_cjsongpio.c)**, 77
  checks, which compiles the real `ezlopi_core_cjson_macros.c` rather than a
  copy. It does what the call sites do — `1ULL << pin` on every value the table
  can produce — and it was run against a subject broken on purpose in both
  directions before being believed:

  | Mutation | Result |
  |---|---|
  | The range check removed | Under sanitizers the process aborts at check 30 — the undefined shift is fatal because this rule sets `-fno-sanitize-recover`. With `SANITIZERS=` it runs to the end and **28 of the 77 fail**, so the table and not the sanitizer is what catches it |
  | The `255` clause removed | 2 checks fail — the table row asserting the sentinel survives, and the one in the flash-pin test that asserts the reservation is not applied to it |

  Both numbers are re-measured against the suite as it now stands rather than
  carried over. The earlier version of this table read "30 checks fail, and with
  `SANITIZERS=` too", which conflated the point the sanitizer aborts at with a
  count of failing assertions; they are different numbers, and only the second
  is evidence the table has teeth of its own.

  Checking the second direction is the point. The earlier attempt in this area
  was withdrawn because it would have refused a normal two-sensor I²C
  configuration; a test that only checks that bad input is rejected cannot
  notice that kind of mistake.

  Two things the mutations turned up about the harness itself, both fixed in the
  Makefile rule for this test rather than assumed away:
  `-fsanitize=float-cast-overflow` is **not** part of `-fsanitize=undefined` in
  gcc, and gcc's sanitizer default is to print `runtime error: ...` and continue
  with exit 0 — so without `-fno-sanitize-recover` a sanitizer finding would have
  sat in the log of a suite reporting success. Nothing in this tree sets
  `UBSAN_OPTIONS`.

  Also in the same file, and only because compiling it on a host surfaced it:
  the two `"%.*s"` calls in `EZPI_core_cjson_get_value_string_by_copy()` and
  `..._by_alloc()` passed a `size_t` where the precision form reads an `int`
  from the variable argument list. On every ESP target those are the same 32-bit
  type, which is why it has shipped; they stop being the same type the moment
  the file is compiled anywhere `size_t` is wider, and the precision is then read
  from the wrong four bytes. Now cast. A no-op on target.

  Four-target build sweep: all of esp32, esp32s2, esp32s3 and esp32c3 build, the
  new warning string is present in all four images, and the edited file produces
  **zero** warnings on any of them. The `-Wincompatible-pointer-types` in the
  build are pre-existing: 87 on esp32/s2/s3, all of them from the header
  documented above `CJSON_GET_VALUE_INT`, and 92 on the C3 — 85 from that header
  plus 7 that only the riscv32 compiler is able to report, because `uint32_t` is
  `long unsigned int` there and `unsigned int` on xtensa. See *Device
  configuration pins* below.

- **Pins the chip is using for flash or PSRAM are no longer accepted as sensor
  pins.** A device configuration naming GPIO 6 on an esp32 passed every test in
  the tree and had a driver configure the SPI flash clock as a push-pull output.

  The reason nothing caught it is that the only pin test the drivers apply is
  `GPIO_IS_VALID_GPIO`, which is

  ```c
  ((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0
  ```

  — an *existence* test against a compile-time mask. The masks were read out of
  `soc_caps.h` by the compiler rather than by eye, and what they exclude is not
  what the name suggests:

  | Target | `SOC_GPIO_VALID_GPIO_MASK` excludes | flash pins | pass? |
  |---|---|---|---|
  | esp32 | 24, 28–31 | 6–11 | **yes, all six** |
  | esp32s2 | 22–25 | 27–32 | **yes, all six** |
  | esp32s3 | 22–25 | 27–32 | **yes, all six** |
  | esp32c3 | **nothing at all** | 12–17 | **yes, all six** |

  So the macro never excluded a flash pin on any target, and on the C3 it
  excludes no pin whatever — it cannot return false. There was no second line of
  defence either: the place such a test would belong,
  [`ezlopi_hal_gpio.c:72`](ezlopi-hal/ezlopi-hal-gpio/ezlopi_hal_gpio.c#L72)
  onward, is commented out in its entirety, and the drivers reach
  `gpio_config()`, `ledc_channel_config()` and `uart_set_pin()` directly rather
  than through one chokepoint.

  **The test is at the read, for a reason that is about who can answer the
  question.** `GPIO_IS_VALID_GPIO` can answer "is 24 a pin on this part" — the
  mask is a constant that says so. It cannot answer "is 6 the flash clock". A
  question no caller is in a position to answer has to be answered where the
  number enters, and because the sixteen stragglers were already routed through
  `EZPI_core_cjson_get_value_gpio()` (see above), that is again a single place
  covering all 144 reads.

  **The set is asked of the chip, not tabulated, and that is the whole design.**
  The obvious implementation is a table — "6–11 on esp32" — and it is wrong.
  `soc/esp32/include/soc/spi_pins.h` carries a *second* set of pin numbers,
  `SPI_D2WD_PIN_NUM_*` = 17, 8, 6, 16, 7, 11, because parts with embedded flash
  remap the bus; the bootloader picks between the two by reading efuse
  ([`bootloader_flash_config_esp32.c:88`](https://github.com/espressif/esp-idf)).
  On such a part a table would refuse 9 and 10, which are free, and allow 16 and
  17, which are flash — wrong in both directions at once, and the expensive one
  is refusing pins that work.

  So `EZPI_core_pin_is_mspi()` asks `esp_mspi_get_io()`, the function ESP-IDF
  uses for this itself. It handles the efuse decode, the esp32 WP pad (which
  lives in the image header rather than efuse) and the PSRAM chip select. The
  answer is cached, since efuse does not change while the firmware runs.

  **Two pad groups are deliberately not reported**, and both would have been
  bugs:

  - **A decoded pin number of `0`.** ROM uses `0` to mean *unset* for every pad
    except WP — `flash_ops.c:985` states it outright: "rom code take 0x3f as
    invalid wp pad num, but take 0 as other invalid mspi pads num". Treating a
    returned `0` as GPIO 0 would have reserved the boot-strap and user-button
    pin on most of these boards. No part uses GPIO 0 for flash, so dropping it
    cannot hide a real reservation.
  - **The octal pads** `ESP_MSPI_IO_DQS` and `D4`–`D7`. On an esp32s3 with quad
    flash — which is what `sdkconfig.defaults.esp32s3` configures — their
    default numbers 33–37 are free for a user to wire a sensor to. Reporting
    them would refuse working configurations. A board genuinely running octal
    flash has them in efuse, and this is named as a limit rather than left to be
    discovered.

  **It is a reservation, not a validity test, so the document is not refused.**
  The pin comes back `-1` and is unused, exactly as an out-of-range number or an
  absent key does. The two "no pin" sentinels are tested *before* the flash
  question, because neither denotes a pin.

  `esp_mspi_get_io()` is declared in `esp_private/spi_flash_os.h`, so IDF
  promises nothing about it across versions. That cost was weighed against the
  alternative: decoding `esp_rom_efuse_get_flash_gpio_info()` by hand needs the
  6-bits-per-pad layout, the `0`-means-unset convention, the `0x3f` WP
  convention and the esp32 WP special case reimplemented — a copy of the
  function that cannot be corrected by an IDF update, rather than a dependency
  that can. If a future IDF removes it the build breaks at the call, which is
  the better failure than a table silently describing the wrong chip.
  `spi_flash` is declared `PRIV_REQUIRES`; the build already succeeded without
  it, because the include path reaches `esp_private/` transitively through other
  components, and relying on that accident would break on whichever unrelated
  component dropped `spi_flash` first.

  **Tested on a host, with the reserved set as an input.** The policy lives in
  `ezlopi_core_cjson_macros.c`, which the host suite compiles for real;
  `ezlopi_core_pin_reserved.c` is not in that rule's sources and
  `test_cjsongpio.c` supplies its own `EZPI_core_pin_is_mspi()` instead — the
  link seam `stubs/driver/gpio.h` describes as the intended arrangement. That
  buys what a header stub would not: the set is chosen per test, so the suite
  checks the standard esp32 arrangement **and the remapped one**, asserting that
  16 and 17 are refused while 9 and 10 are allowed. A hard-coded table passes
  the first and fails the second.

  The set defaults to empty, which is what lets the pre-existing table keep
  testing the bound rather than the reservation — including its `{"6", 6}` row,
  which on a real esp32 now returns `-1`.

  | Mutation | Result |
  |---|---|
  | The reservation made unreachable | 8 checks fail — the six flash pins plus the two remapped ones |
  | The reservation applied to the sentinels too | 2 checks fail, one of them the assertion that `255` survives — the "refuses input that works today" direction |
  | Neither (control) | 77 checks, 0 failures |

  The control inside the test matters as much as the mutations: after the
  refusals it clears the set and asserts GPIO 6 and 16 come back. Without that,
  a subject that had merely acquired a second hard bound at 6 — or a helper that
  had stopped reaching the subject at all — would pass every refusal assertion
  above it.

  Four-target verification: all four build with **zero** warnings in the two
  edited files and zero format warnings. Unlike a no-op cast, this adds real
  code, so the images must get *bigger*, and a size equal to the old baseline
  would mean the new translation unit was not linked:

  | Target | before | after | delta |
  |---|---|---|---|
  | esp32 | 1683072 | 1683616 | +544 |
  | esp32s2 | 1373488 | 1373952 | +464 |
  | esp32s3 | 1697872 | 1698464 | +592 |
  | esp32c3 | 1756320 | 1756848 | +528 |

  `esp_mspi_get_io` and `EZPI_core_pin_is_mspi` are both present in all four
  ELFs, which is the end-to-end proof that a private-header dependency resolves
  and links on the xtensa and riscv32 toolchains alike. It is worth recording
  how that was established, because two earlier attempts at it **reported
  success and proved nothing**: the first probe function was called from nowhere
  and `--gc-sections` dropped it; the second added
  `__attribute__((constructor))` expecting `.init_array` to be a GC root, which
  it is — but the component is a static library and `ld` only extracts an
  archive member that resolves some undefined symbol, so the member was never
  pulled in and its `.init_array` never considered. Both builds came out at
  *exactly* the baseline size with the symbol absent, and both looked like a
  clean pass. The size assertion is phrased as "must have changed" for that
  reason.

  What this does **not** cover is in *Known gaps*: the console UART pin and the
  pins of a configured I²C or SPI bus are still unprotected, and those depend on
  `sdkconfig` rather than on efuse.

- **A device that fails to initialise no longer disappears without saying so.**
  `__EZPI_initialize_devices_v3()` walks the prepared device list, calls each
  item's `EZLOPI_ACTION_INITIALIZE`, and on a negative return unlinks and frees
  the whole device
  ([`ezlopi_core_ezlopi.c`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c)).
  Nothing on that path said a device had been dropped, or why.

  **It is worth being exact about this, because the first draft of this entry
  said "the free was silent" and that is not true.** The free path does print —
  `EZPI_core_device_free_device()` emits one of `TRACE_W("PARENT_TREE_ID: …")`,
  `TRACE_D("Head Device-ID: …")` or `TRACE_E("To free Device-ID: …")` depending
  on where in the list the device sits, and `ezlopi_device_free_single()` adds
  `TRACE_E("free single Device-ID: …")`. What those lines carry is a bare
  device id at three different severities. They do not carry the device's
  name, they do not carry a reason, and they are the same lines that print when
  a device is removed for any other reason, so seeing one does not tell you an
  initialisation failed. Meanwhile the driver logs whatever upset it in its own
  words — "I2C init failed", or nothing at all — with no device id on it.

  For anyone holding the board those two halves never meet. A device configured
  through the app, stored correctly, parsed correctly and then dropped at
  initialisation looks the same as one that was never stored: the thing you are
  looking for is absent, and nothing names it. That is close to the worst
  failure mode a plug-and-play product can have, because the only move it leaves
  is to configure it again.

  There are now two messages, and between them they name the device, the item
  and the cause:

  | situation | message |
  |---|---|
  | an item returned `< 0` | `device '<name>' [0x<id>] dropped: item '<name>' [0x<id>] failed to initialise (ret=<n>)` |
  | an item has no `func` | `device '<name>' [0x<id>] item '<name>' [0x<id>]: no action function; item left uninitialised` |

  The second replaces `TRACE_E("Function is not defined!")`, which was true and
  unactionable. Neither changes behaviour: the device is dropped in exactly the
  same cases as before.

  **Correction, measured later: the first of those two messages is unreachable
  from every driver in the tree, and this entry originally failed to say so.**
  The drop is gated on `< 0`, but `ezlopi_error_t` is a `typedef int32_t` whose
  only non-positive values are `EZPI_SUCCESS` (0), `EZPI_FAILED` (-1) and
  `EZPI_NOT_AVAILABLE` (-2) — every named `EZPI_ERR_*` code is a **positive**
  bit flag, including `EZPI_ERR_INIT_DEVICE_FAILED` (`0x200`), whose own comment
  in [`ezlopi_core_errors.h`](ezlopi-core/ezlopi-core-errors/ezlopi_core_errors.h#L73)
  says it is returned "to abort futher initialization of the device in case it
  has to be deleted from the tree". Brace-matching every
  `static ezlopi_error_t __init*` body under `ezlopi-sensors-devices/` finds
  **64** of them: **32** name `EZPI_ERR_INIT_DEVICE_FAILED`, and **none** names
  `EZPI_FAILED` or `EZPI_NOT_AVAILABLE` or returns a negative literal.

  So no driver can currently report an init failure in the form the core tests
  for, the device is never dropped, and the message describing the drop cannot
  print. What the entry above says about the *reason* a drop was invisible is
  still accurate, and the message is still the right message — it is simply
  waiting on a defect one layer down. The claim that this addressed "close to
  the worst failure mode a plug-and-play product can have" was too strong as
  written: the real failure mode is the opposite one, a device that failed to
  initialise being **kept** and presented as working. Recorded under *Known
  gaps* rather than fixed here, because making the gate fire would start
  dropping devices across 32 drivers at once and that is a behavioural change
  to put in front of a human, not a cleanup.

  `curr_item` is the failing item rather than `NULL` at the point the message is
  built, because the inner loop `break`s on failure and only runs off the end on
  success. It is still NULL-guarded, since reading a pointer because of a
  control-flow argument is how the NULL-dereference entries elsewhere in this
  file came about.

  **The `(unsigned)` casts on the two id arguments are not redundant**, though
  they look it next to the `TRACE_S` directly above them that prints the same
  `uint32_t` with a bare `%x`. `uint32_t` is `unsigned int` on
  `xtensa-esp32-elf-gcc` and `unsigned long` on `riscv32-esp-elf-gcc`, so the
  bare form is a mismatch on esp32c3 — and esp32c3 is the one target that cannot
  report it, because IDF appends `-Wno-format` to the whole build there. Both
  are 32 bits wide so the bare form happens to work; the cast means it does not
  have to. The measurement behind that, and why it makes a clean esp32c3 log
  weaker evidence than it appears, is under *Format strings*.

  Verified on all four targets: `rc=0`, no warnings in the file, no format
  warnings, and the image grows on every one of them.

  | target | before | after | delta |
  |---|---|---|---|
  | esp32 | 1683616 | 1683824 | +208 |
  | esp32s2 | 1373952 | 1374160 | +208 |
  | esp32s3 | 1698464 | 1698688 | +224 |
  | esp32c3 | 1756848 | 1757088 | +240 |

  The size has to move, because a log line that is not linked is a log line that
  does not exist, and an unchanged size is how two probes earlier in this work
  reported success while proving nothing. What is **not** verified is the text
  appearing on a real console: that needs a device that fails to initialise on
  hardware, and nothing here has been flashed.

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
  ([`cjext.c:1456`](ezlopi-components/cjext/cjext.c#L1456)) and resizes it with
  `ezlopi_realloc` ([line 1474](ezlopi-components/cjext/cjext.c#L1474)) before
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
  [`ezpi_service_uart_set_config()`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L1418):

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

- **A `cjext` lookup defect surfaced here, and is now fixed under *JSON
  library* above.** `cJSON_GetObjectItem()` passed the *stored* key's
  `str_key_len` to `case_insensitive_strcmp()` as the number of bytes it may
  compare ([`cjext.c:2396`](ezlopi-components/cjext/cjext.c#L2396)), and the
  comparison returned equal as soon as that many characters matched, so a key
  was matched by any name it was a prefix of. It surfaced here because a
  *response* is a built tree, so two checks reading `configSize` out of one
  were getting the `config` node:

  ```
  built  tree: cJSON_GetObjectItem(o, "configSize") -> the "config" node
  parsed tree: cJSON_GetObjectItem(o, "configSize") -> the "configSize" node
  ```

  `cJSON_GetObjectItemCaseSensitive()` was unaffected; it compares
  `max(strlen(name), str_key_len)` bytes
  ([`cjext.c:2388`](ezlopi-components/cjext/cjext.c#L2388)).

  **It never reached this feature's behaviour**, which is why the feature
  shipped alongside it. Requests arrive parsed, and the serialised response is
  correct JSON either way, since the ambiguity existed only inside `cjext`'s
  lookup. What this entry originally said beyond that was wrong on three
  counts, each corrected under *JSON library* above: the defect was not
  confined to built trees (`cJSON_ParseWithRef` records `strlen` too, so
  reference-parsed trees were equally affected — only the *copying* parser
  escaped, and by accident); the fix was not "make `add_item_to_object()` store
  the inclusive length", which would have made `cJSON_Duplicate()` read a byte
  past every built key; and the key printer, named here as the reason to defer,
  is provably insensitive to the recorded length.

  The workaround this entry describes is gone. `test_devcfg_api.c` no longer
  has its own exact-match walk and no longer pins the defect; its 27 response
  reads call `cJSON_GetObjectItem()` directly, which is what retiring the
  workaround was supposed to look like.

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
  [`ezlopi_service_ws_server.c:331`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L331)
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

- **The device now serves a configuration page to a browser on the port it
  already had.** Before this, everything the local API can do needed a
  websocket client and a hand-written JSON envelope, which means the 117
  methods above were reachable in principle and not in practice. Pointing a
  browser at `http://<device-ip>:17001/` got back one line of `text/plain`
  saying the port speaks WebSocket. It now gets
  [`config_ui.html`](ezlopi-services/ezlopi-service-ws-server/config_ui.html):
  five tabs, each named after the methods behind it — log in with the local
  key, read the hub's own identity and reboot it (**Hub**), list devices and
  items and write a value to a writable one (**Devices**), see the current
  network and scan for and join another (**Wi-Fi**), read, set and clear the
  broker (**MQTT**), and send an arbitrary method with arbitrary params for
  everything the four panels do not cover (**Console**, with a `datalist` of 39
  method names — hand-written, because there is no method that lists methods,
  and including the handful a default build compiles out, since the list is a
  typing aid and not a claim about what this device answers). Every frame in
  and out is shown in a traffic log beneath. **No compile step, no toolchain,
  no app.**

  It is one file — inline CSS, inline JS, a `data:` URI favicon — because the
  server registers exactly one URI, `"/"`. A second request for `/style.css`
  would reach the same handler and be answered with the page, so a page split
  across files could not work without adding routes, and routes on this handler
  would have to be reasoned about against the websocket branch. One file is the
  shape the existing server already supports.

  It reaches the firmware through
  [`EMBED_TXTFILES`](ezlopi-services/ezlopi-service-ws-server/CMakeLists.txt#L55),
  which gives `_binary_config_ui_html_start/_end` and appends a NUL, so the
  body length is `end - start - 1` and not a 36 KB `strlen` per request over
  flash. `TXTFILES` rather than `BINARIES` for exactly that NUL. **Gzip was
  measured and declined**: the page compresses from 36 554 to 12 099 bytes, but
  claiming that means either committing a `.html.gz` beside the `.html` — two
  files that drift apart with nothing in CI to catch it — or making `gzip` a
  host-build dependency, which ESP-IDF itself does not require. 24 KB is not
  worth either today. The CMake comment records that if headroom ever tightens,
  the host-gzip option is the better of the two, with `Content-Encoding` set in
  the handler.

- **Serving the page was moved out of `__send_lock`, and that mattered more
  than the page did.** The GET branch of `ezpi_msg_handler()` ran inside the
  send mutex, so the obvious implementation — answer where the old `text/plain`
  hint was answered — would have held that lock for the whole 36 KB write.
  `esp_http_server`'s `send_wait_timeout` is 5 s *per write* and
  `httpd_resp_send` loops until the body is gone, while the broadcast paths
  take the same lock with a **2000 ms** timeout and give up when they miss it
  ([`:268`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L268),
  [`:444`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L444)).
  One browser loading this page on a slow link could therefore have made the
  device drop item-value broadcasts to the Ezlo app — a page added to make
  configuration easier, degrading the thing it sits beside.

  So the non-websocket GET now returns
  [before the lock is taken](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L548),
  and only the websocket half — registering the client as a broadcast target,
  which does touch shared state — stays inside it. This is safe rather than
  merely convenient: `__send_lock` serialises sends to *registered websocket
  clients*, a connection that never negotiated a websocket is never registered,
  and the handler runs on the single `esp_http_server` task, so there is no
  second writer to that descriptor by any route. The discriminator itself is
  not new — `httpd_ws_get_fd_info()` was already there from the broadcast-target
  fix in the Security section, which is what made this page cheap to add: the
  branch that knows a browser from a websocket client existed, it just had
  nothing useful to say.

  **No cache headers, deliberately.** The page ships inside the image, so a
  cached copy surviving an OTA would be a stale UI against a newer API, and
  that failure presents as the device misbehaving rather than as a cache. 36 KB
  on a LAN does not buy that.

- **This is not first-time onboarding.** The page is served by the local
  websocket server, which runs only once the device is on a network as a
  station; there is no SoftAP fallback, so a device that has never joined a
  network cannot be reached at all. It is the second step — after BLE or Improv
  gets the device onto Wi-Fi, this is how you drive it without an app. Saying
  so because "web config page" implies a captive portal to most readers, and
  this is not one.

- **Four-target sizes, with the real partition figure.** esp32 1 683 216,
  esp32s2 1 375 552, esp32s3 1 697 984, esp32c3 1 756 496 bytes, against an
  `ota_0` of `0x1EFC00` = 2 030 592 bytes
  ([`ezlopi_partitions_v3_1_4_181_4mb.csv`](ezlopi_partitions_v3_1_4_181_4mb.csv)),
  leaving 339 / 639 / 324 / 267 KiB — the tightest target, esp32c3, is at 86.5%
  of its app partition. The page costs 36 512 bytes of flash, measured as the
  esp32 image delta against the same tree without it. Each image was checked
  with `strings` for the page's own markers, for the redaction helper, and for
  the *absence* of the old `text/plain` hint, and each ELF for
  `_binary_config_ui_html_start`, because a green build proves the handler
  compiles and not that the page landed in the binary it points at.

- **The page was driven against a stand-in device, not hardware.** Roughly 500
  lines of browser JavaScript that had never executed is not something to ship
  on the strength of reading it, so the device side was reimplemented as a
  stdlib-only Python stand-in — HTTP and WebSocket on **one** port, as the real
  server does, which is what makes a `location.host` mistake visible that a
  two-port harness would hide. Eleven methods were exercised and each contract
  checked on the wire: the unauthenticated path admitting `hub.offline.login.ui`
  and nothing else, a bad key refused and a good one accepted, reconnect with
  automatic re-login from `sessionStorage`, a method that always fails
  surfacing its `error` rather than reading as success (the bug fixed above),
  `hub.reboot` never replying and the 12 s timeout being swallowed instead of
  shown as a fault, an unsolicited scan broadcast routed by
  `id === "ui_broadcast" || msg_subclass` rather than mistaken for an RPC
  reply, `hub.item.value.set` converting by the item's own `valueType` so a
  numeric item gets `23` and a bool gets `true` rather than the text out of the
  box, and a blank MQTT password *omitted* from the params rather than sent as
  `""` — the device never returns a stored password, so an empty box has to
  mean "leave it alone" and not "clear it". Zero console output across all of
  it.

  The page builds every element with `textContent` and `setAttribute`
  ([`el()`](ezlopi-services/ezlopi-service-ws-server/config_ui.html#L313)) and
  never assigns `innerHTML`, so hostile SSIDs and device names are inert. That
  was tested with an `<img src=x onerror=...>` SSID and a `<script>` device
  name — **and the harness was then proved able to fail**, by injecting the
  identical payload through `innerHTML` and watching it fire. A clean XSS
  result means nothing until the payload has been shown not to be a dud.

- **The traffic log put the device's Wi-Fi PSK on screen, and it was found by
  reading, not by the suite.** The log is the feature's own best idea — it shows
  every frame in and out, which is what makes the page a usable way to learn the
  local API — and it is also the thing a person screenshots into a bug report.
  Its first version printed frames verbatim:

  ```js
  logLine("rx", JSON.stringify(m));
  ```

  `hub.network.get` answers with the device's **stored Wi-Fi password**, added
  as `key` beside `ssid`
  ([`ezlopi_cloud_network.c:174`](ezlopi-cloud/ezlopi-cloud-network/ezlopi_cloud_network.c#L174)).
  That wire format is deliberately left alone — the Security section above made
  the same call for the serial-console copy of this same field, because the app
  and cloud depend on it — which means every fix for this PSK has to land in
  whatever *displays* a frame. This page added a new display and did not get
  that treatment. So merely clicking the **Wi-Fi** tab wrote the PSK into the
  page, in plain text, in the pane most likely to be pasted into a public issue.

  The outbound half leaked too, and less excusably: the log special-cased
  `hub.offline.login.ui`'s `token` and missed the other two methods that carry a
  credential in their params — `hub.network.wifi.try_connect` (`network.key`)
  and `hub.mqtt.config.set` (`password`). A hand-maintained list of one. Two
  further sinks did the same thing by a different route: the **Hub** tab's
  deliberate print-every-unknown-field loop, and the **Console** pane's
  pretty-printed result.

  Four leaks, one class. The fix is a single
  [`redact()`](ezlopi-services/ezlopi-service-ws-server/config_ui.html#L505)
  that walks a frame and blanks credential-shaped fields to `"***"`, with
  [`logFrame()`](ezlopi-services/ezlopi-service-ws-server/config_ui.html#L521)
  as the only way the log is written, applied in **both** directions and at both
  display dumps. Three decisions in it are worth recording:

  - **`this.ws.send(JSON.stringify(req))` is left verbatim.** The wire is not a
    sink. Redacting there would send `"***"` to the device *as the password*, so
    the page would stop working in exactly the flows that matter. Redaction
    belongs at the screen, and the distinction had to be taught to the CI check
    below as well.
  - **`key` is blanked only beside `ssid`.** `key` is an ordinary word that
    appears in non-secret frames; it is a credential specifically in the
    Wi-Fi-shaped object, which is also the only shape the firmware emits it in
    (both fields are added together, under one guard, at the link above).
  - **Empty and null values are preserved, not blanked.** "No password set" and
    "password hidden" have to stay distinguishable, or the log stops answering
    the question people open it to ask.

- **The page's redaction is a weaker guarantee than the firmware's, and says
  so.** `check_no_secret_logging.py` argues against exactly this design — see
  the Security section above: per-field redaction "fails silently the first time
  a new method adds a secret field that nobody adds to the list", which is why
  the firmware got a switch that logs nothing instead. That objection applies here
  and is accepted rather than argued away. It is taken anyway for two reasons:
  the page is a *client* and cannot enumerate what a future firmware will
  return, so no exhaustive rule is available to it; and the switch-off answer
  would delete the traffic log, which is the page's main reason to exist. The
  page carries that trade-off in its own comment so the next reader does not
  have to rediscover that it was a choice.

- **The harness that had passed could not have failed.** The page tested clean
  before any of this — because the stand-in's `hub.network.get` never put a
  `key` in its reply. The leak was invisible to the only thing exercising the
  page. The stand-in now sends a canary PSK, and the retest was made conclusive
  rather than merely green: a **raw WebSocket client bypassing the page
  entirely** confirmed the canary really was on the wire, while the page showed
  `"key":"***"` and the canary was absent from the live DOM *and* from the
  served HTML. The MQTT password was matched the same way, page against server,
  on a single request id. `redact()`'s own behaviour was pinned with 10 cases
  covering nesting, arrays, the `ssid`-qualified rule, and the empty/null
  carve-out.

- **The regression is now guarded, and the guard was proved able to fail.**
  [`tests/ci/check_ui_frame_redaction.py`](tests/ci/check_ui_frame_redaction.py)
  fails on three things, each the shape of a leak that actually happened: a
  frame-shaped identifier stringified without `redact()` on a line that also
  feeds a sink; `redact()` or `logFrame()` missing, or `logFrame()` no longer
  routing through `redact()`; and a credential field name dropped from
  `SECRET_FIELDS` or the `ssid`-qualified `key` rule removed. Five mutations
  were applied to the page in a scratch mirror — one per failure mode, each
  asserted to produce its *specific* message rather than just a non-zero exit —
  and **all five were caught**, with the mirror confirmed clean before and
  after. It is a text check over one file and not a JavaScript parser: it cannot
  see a leak built by string concatenation or one that renames the frame
  variable, and it says nothing about whether `redact()` is correct. What it
  catches is the quiet re-introduction — pasting `logLine("rx",
  JSON.stringify(m))` back in looks like ordinary debugging and breaks nothing
  visible.

- **The serve path has never run on a board.** What is established is that four
  targets build, that the page is present in all four images with the old
  response gone, that the embedded symbols resolve in every ELF, and that the
  page's own logic holds against a faithful stand-in. What is *not* established
  is anything that needs the real device: that `httpd_ws_get_fd_info()` returns
  what is expected on a live non-websocket fd, that a 36 KB `httpd_resp_send`
  completes to a real browser over real Wi-Fi, that the page and a real
  `ezpi_msg_handler` agree on every envelope, or that a browser and the Ezlo app
  can be served at once. Also untested: the two methods the stand-in does not
  implement, `hub.network.wifi.scan.stop` and `hub.mqtt.config.reset`.

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

### Addressable LEDs — [feat]

- **`ezlopi-util-aled` is the colour arithmetic for addressable strips, and
  nothing else.** `EZPI_aled_scale8` (scale a channel by a brightness),
  `EZPI_aled_hsv_to_rgb` (a hue/saturation/value byte triple to RGBW),
  `EZPI_aled_wire_len` (how many bytes a strip of *n* pixels needs), and
  `EZPI_aled_pack` (pixels into the byte order a particular chip expects).
  `EZPI_aled_fill` and `EZPI_aled_unpack_first` were added later, when the
  driver needed them — see *Addressable LED configuration* below. Six
  functions now; this entry said four when it was written and four is what the
  component shipped with.

  Its `idf_component_register()` has an empty `REQUIRES`, and that is a
  requirement of the component rather than a happy accident: needing an IDF
  header is the signal that whatever is being written belongs in the driver
  instead. That is what keeps the whole of it host-testable.

  **It drove no hardware and nothing called it.** There was no RMT driver, no
  device, and no control surface in this commit. It was the half that can be
  got wrong silently, separated from the half that needs a board. That is no
  longer the state of the tree: `device-0009` now calls `EZPI_aled_wire_len`,
  `EZPI_aled_fill` and `EZPI_aled_unpack_first` directly, and reaches
  `EZPI_aled_pack` through the fill. The past tense is the correction — the
  component was deliberately dead when it was written, and the thing that made
  it worth writing that way is that the half needing a board arrived against
  arithmetic that had already been swept.

- **Clean-room, deliberately and specifically.** Nothing was taken from WLED
  (Christian Schwinne / Aircoookie, **EUPL-1.2**, an Arduino/PlatformIO
  firmware), from WS2812FX (Harm Aldick, **MIT**, an Arduino library), or from
  `hoene/esp32-led-controller` (Christian Hoene / Symonics, **AGPL-3.0**, and
  the one of the three that is ESP-IDF). Those are three different projects by
  two different Christians under three different licences, and the AGPL one is
  the one an ESP-IDF developer would reach for first. This was written from the
  published WS2812 wire format and the textbook definition of HSV, so the
  component stays under EASL with no further licence question to answer.

- **Three decisions that look like inefficiencies and are not.** `scale8`
  divides by 255 rather than shifting by 8, because `(v * 255) >> 8` is 254 —
  full brightness is not full. The hue is split as `h * 6 >> 8` rather than
  `h / 43`, because six sectors of 43 do not cover 256: the last one is 41 wide
  and the offset within a sector spans 0..252 instead of 0..255. And `pack`
  refuses an undersized destination by writing **nothing** and returning 0,
  rather than reporting a short count — a half-packed frame is still a frame,
  and the strip will happily display it.

- **The test was 68 checks, and ten of them exist because the other 58 were not
  enough.** It is **201** now; the 133 added with `EZPI_aled_fill` and
  `EZPI_aled_unpack_first` are described below.
  `tests/host/test_aled.c` sweeps all 65,536 `scale8` pairs and all
  16,777,216 `(h, s, v)` triples, twice. Exhaustive rather than a table of
  colours, because the way this conversion gets written wrong is at a sector
  boundary, and arithmetic that is off by one or two inside an arc still sends
  pure red, green and blue to exactly the right places.

  That turned out to be the finding. Swapping sector 0's green and blue — so
  the wheel runs red→magenta instead of red→yellow — **passed all 58 original
  checks**, including `max(r, g, b) == v`, `s == 0` implies grey, and every
  named primary. At a sector's endpoints the two swapped channels are both
  zero, so the invariants cannot see it. This is precisely the defect the
  component exists to catch: a strip with its colours permuted lights up,
  answers every command, and looks like a working product.

  Closed by measuring the correct implementation first rather than guessing a
  tolerance — the largest Manhattan step between adjacent hues is **6**, and
  the six sector midpoints rank their channels in six distinct orders — and
  then asserting both. The sector-0 swap now fails with 61,096 discontinuities
  and a worst step of **509**.

  Continuity alone would not have been enough either, and that was also tested
  rather than assumed: reversing the entire wheel is perfectly continuous and
  is caught only by the ranking check.

- **20 mutants, 18 killed, 2 survived, measured in one sweep.** The two
  survivors are `out.w = 0` deleted and the `0 == n_pixels` early return
  removed. Both were *predicted* equivalent before being run — the return
  struct is zero-initialised, and `0 * 3` is 0 either way — and both were run
  anyway so the prediction is tested instead of asserted.

  Two were killed by the sanitizers alone, with no check failing: removing the
  colour-order range validation gives UBSan `index 6 out of bounds for type
  'uint8_t [6][3]'`, and removing the `NULL` pixel check gives a null member
  access and an ASan SEGV at address 0. Neither would have been caught by the
  assertions.

  The sweep was re-run from scratch for this entry rather than stitched from
  two earlier rounds, because those rounds contained **two failures of my own
  tooling** that reported identically to survivors: a `sed` expression whose
  `|` delimiter collided with a `||` in its own anchor, and a transient rename
  denial from this filesystem. Those were my errors, not findings about the
  subject, and a mutant that never applied is indistinguishable from one that
  survived. The single clean sweep reports 0 failed edits and 0 mis-anchored
  no-ops, which is what makes 18/2 a measurement rather than a residue.

- **One check in there is redundant, and saying so is the point.** The
  "a hue never sets the white channel" check is satisfied twice over: the
  return struct is zero-initialised *and* `out.w = 0` is assigned. Removing
  either one alone leaves the other, and nothing fails — measured. Removing
  both fails it with all 16,777,216 triples violating it.

  The zero-initialiser is kept despite being redundant, because an
  uninitialised read is the one defect class this suite cannot catch:
  AddressSanitizer does not model it, and MemorySanitizer, which does, cannot
  be combined with ASan. A seventh sector arm added later that forgot a channel
  would return uninitialised stack. A structural guarantee is worth more than a
  test where no test is available — and it is recorded in both the source and
  the test, rather than left for someone to find by mutating the line and
  seeing nothing fail.

- **On onboarding: the cloud side already supports this, and a strip already
  onboards today — one pixel of it.** `category_dimmable_light`,
  `subcategory_dimmable_colored` and `dev_type_dimmer_outlet` all exist
  already, and `device-0009-other-RMT-SK6812` is already registered through all
  four files of the device contract and appears in the onboarding list as
  `device-rgp_strip-RMT_SK6812`.

  It sets `sk6812_strip.length = 1`, hardcoded, and the only configuration keys
  it reads are `ezlopi_gpio1_str`, `ezlopi_duty_cycle_str`, `ezlopi_freq_hz_str`
  and `ezlopi_dev_type_str`. There is no pixel-count key and no colour-order
  key anywhere in the config schema. Since `led_strip_fill()` is called with
  that same `length`, a 60-pixel strip wired to a provisioned EzloPi lights its
  first LED and nothing else. That is the gap this component is the foundation
  for closing, and it is a configuration gap rather than a driver one.

  `category_led_strip` and `subcategory_addressable_*` do **not** exist in this
  tree — they are not being added, because the existing dimmable-colour
  properties already describe a strip to the cloud and adding new ones would
  need a cloud-side change this work does not have.

- The full host suite is **6257** checks across **29** binaries, 0 failures,
  with `test_aled` contributing 68 of them and all 29 binaries reporting in the
  shape the aggregate greps for.

### Addressable LED configuration — [feat]

- **A strip onboards today and lights exactly one LED.** This is the measured
  state of the tree, not a design gap:
  `device-0009-other-RMT-SK6812` is registered through all four files of the
  device contract, appears in the onboarding list as
  `device-rgp_strip-RMT_SK6812`, reports
  `category_dimmable_light` / `subcategory_dimmable_colored` /
  `dev_type_dimmer_outlet`, and accepts and answers colour and dimmer commands.
  It also contains, verbatim:

      dimmer_args->sk6812_strip.length = 1;

  and the only configuration keys it reads are `gpio1`, `duty_cycle`, `freq_hz`
  and `dev_type`. There was no pixel-count key anywhere in the config schema.
  `led_strip_fill()` is then called with that same `length`, so a 60-pixel
  strip wired to a provisioned EzloPi lights its first LED and nothing else —
  while the app shows a working, responding device.

  Separately, `led_strip`'s `COLOR_SIZE(strip)` is `3 + (strip->is_rgbw != 0)`,
  and **`is_rgbw` is never assigned anywhere in the firmware** — the args
  struct is `memset` to 0 — so the white channel of an SK6812-RGBW is
  unreachable as well.

- **`ezlopi-util-aledcfg` is the parser for that, and only the parser.**
  `EZPI_aledcfg_from_json()` resolves a pixel count, a white-channel flag and a
  wire byte order from a device-config object;
  `EZPI_aledcfg_order_from_str()` parses `"grb"`; `EZPI_aledcfg_wire_len()`
  wraps `EZPI_aled_wire_len()` so the byte count a resolved shape implies is
  written once, carries the overflow check, and is host-tested. The three key
  names — `pixel_count`, `rgbw`, `color_order` — are in
  `ezlopi-cloud-constants` with every other config key.

  A correction to what an earlier draft of this entry claimed: that wrapper is
  **not** what sizes the strip's buffer, and describing it as "provably the
  same arithmetic" as the driver's allocation was wrong. `led_strip_init()`
  does its own `calloc(length, COLOR_SIZE(strip))` inside vendored third-party
  code, and nothing forces the two expressions to agree — they are two
  statements of one rule that happen to match, which is worth saying plainly
  rather than overstating into a guarantee.

- **device-0009 now reads that config, and the one-LED defect is closed in
  source.** `__prepare()` resolves the shape once — it is the only place the
  stored document is in scope, since `__init()` sees the item and not the
  config — and stores it in `s_dimmer_args_t`. `__init()` then sets both
  `sk6812_strip.length` and `sk6812_strip.is_rgbw` from it, before
  `led_strip_init()`, which is required rather than tidy: that function sizes
  its frame buffer as `calloc(length, COLOR_SIZE(strip))` and reads both fields
  to do it. `is_rgbw` had never been assigned anywhere in the firmware, so this
  is the first time the white channel of an SK6812-RGBW is reachable at all.

  **Not verified on hardware.** No board has driven a multi-pixel strip, and
  this is the half of the fix no host test can reach — it needs an RMT
  peripheral and the IDF. What *is* verified is a build on **all four CI
  targets** with zero diagnostics in either touched file, and `nm` on each
  linked ELF showing `EZPI_aledcfg_from_json` and
  `EZPI_aledcfg_order_from_str` present, so the driver's call survives
  `--gc-sections` rather than being compiled and dropped:

  | Target | `set-target` | build | diagnostics in touched files | resolver in ELF | image |
  |---|---|---|---|---|---|
  | esp32 | 0 | 0 | 0 | present | `0x19d1c0` |
  | esp32s2 | 0 | 0 | 0 | present | `0x151590` |
  | esp32s3 | 0 | 0 | 0 | present | `0x1a0d30` |
  | esp32c3 | 0 | 0 | 0 | present | `0x1af080` |

  The esp32 image leaves 17% of the app partition free. esp32c3 is in that
  table for a reason rather than for completeness: device-0009 drives an RMT
  peripheral, the RMT driver differs across these parts, and the DHT sensors
  are already target-guarded off c3 for timing reasons — so "an RMT-based
  device builds on c3" was not something to assume. It does.

  `EZPI_aledcfg_wire_len` and the `EZPI_aled_wire_len()` it wraps were **absent
  from all four** ELFs at this commit, and that was the expected result rather
  than a missing link: nothing in the firmware called the wrapper, so both were
  collected, and both were reached only from the host test. Recorded because an
  absent symbol that is *fine* and an absent symbol that means the feature was
  dropped look identical in an `nm` listing, and only one of them was this.

  Re-measured since: `EZPI_aled_wire_len` is now **present in all four**,
  because `__fill_strip()` calls it to size the frame buffer. The wrapper
  `EZPI_aledcfg_wire_len` is still absent in all four — still nothing calls it.
  That split is why the two are named separately rather than as a pair.

  One note on how that was measured, because it nearly went unrecorded. The
  first run of the symbol check reported `resolver=n/a` for all three new
  targets — its `nm` glob was one directory level too shallow for the real
  `tools/<name>/<ver>/<name>/bin/` layout, so the toolchain path came back
  empty and the check silently did nothing. An empty toolchain path is a
  *skip*, not a failure, and `n/a` sits in a summary table looking much like
  `ABSENT` when only one of the two would have been a finding. The corrected
  check picks the toolchain per target by name, proves `nm` can actually read
  each ELF and prints its symbol total, and only then believes a negative
  result.

- **Two functions were added to `ezlopi-util-aled` first, and the permutation
  still has exactly one implementation.** `EZPI_aled_fill` packs one colour
  into every pixel of a frame buffer; `EZPI_aled_unpack_first` reads a colour
  back out of one. The fill does not re-derive the byte order: it hands the
  first pixel to `EZPI_aled_pack` — which already knew all six orders and was
  already swept — and `memcpy`s that stride across the rest. So the order table
  is read in one place, and filling the 2048-pixel cap needs no 8 KB temporary.
  An undersized destination is refused by writing **nothing**, matching `pack`,
  because a half-filled frame is still a frame and the strip will display it.

  The unpack is the inverse permutation written as **the same table read
  backwards** rather than as a second table, since a second table is a thing
  that can disagree with the first.

  **133 checks added, 68 → 201**, sweeping all six orders against both pixel
  widths with the expected bytes derived from the three-letter order *names*
  rather than from the subject's own table. 12 mutants, 12 killed, 0 survived;
  two of them killed by AddressSanitizer alone with no check failing — dropping
  the `wire_len` guard on the white byte, and dropping the order bound, both
  giving a stack-buffer-overflow read.

  One of those twelve is the most useful measurement in this section. Reading
  the order table **forwards** instead of backwards in the unpack is killed by
  `brg` and `gbr` and **by nothing else** — 8 failing checks, all of them in
  those two orders, isolated rather than inferred. `grb`, `rgb`, `rbg` and
  `bgr` are involutions: their forward and inverse permutations are the same
  mapping, so the bug is invisible in four of six. Only `brg` and `gbr` are
  3-cycles. A test that swept the default `grb` — or any subset missing those
  two — would have passed that mutant while five of the six orders reported the
  wrong colour to the app.

- **All six colour orders are honoured, and no vendored code was changed to do
  it.** The obstacle was real: `led_strip_t` has **no colour-order field**, the
  order lives in a static table indexed by strip *type*
  (`led_params[LED_STRIP_SK6812].order = ORDER_GRB`), and `color_order_t` there
  offers only `ORDER_GRB` and `ORDER_RGB` — two of the six. So for a while this
  entry said the key was parsed, warned about and then ignored.

  Adding an order field to the vendored MIT component and threading it through
  `led_strip_set_pixel()` was the obvious route and is not the one taken.
  `__fill_strip()` packs the frame buffer directly with `EZPI_aled_fill()`
  instead and then flushes, which honours all six orders without touching
  third-party code — and the packing it uses was already written and already
  swept by `tests/host`. The vendored component keeps RMT ownership, which is
  the part worth vendoring.

  Two details that are load-bearing rather than tidy. Brightness is passed as
  **255** to the fill: the vendored RMT adapter applies `strip->brightness`
  while transmitting, so scaling the buffer as well would apply the dimmer
  twice and bend its curve. And the white byte is still `rgb_luma(colour)`,
  which is what `led_strip_set_pixel()` wrote, so an RGBW strip is driven
  exactly as before — an `rgbcolor` from the cloud carries no white value, so
  there is nothing else to derive it from. A refused fill does not flush, so
  the strip holds its previous frame instead of showing a half-written one.

- **The colour read-back was wrong for five orders and could dereference
  NULL.** Reporting a strip's colour means reading the wire buffer, because
  that buffer is the only place the colour is stored — and the getter indexed
  it as `buf[0]` green, `buf[1]` red, `buf[2]` blue with three integer
  literals. Correct for the one order the driver could write; a wrong colour
  reported to the app for the other five. It goes through
  `EZPI_aled_unpack_first()` with the configured order now.

  Separately, it indexed `buf[0]` unconditionally. `led_strip_init()` returns
  early with `buf` still NULL when the frame-buffer allocation fails, so a
  cloud getter arriving after a failed init dereferenced NULL. It reports
  zeroes instead.

  And its `user_arg` cast is now `s_dimmer_args_t *`, which is what
  `__prepare()` actually stores for all six items. The old cast to
  `led_strip_t *` worked only because the strip is the **first member** of that
  struct — reordering the struct would have silently turned the getter into a
  read of the wrong field. That is the same first-member type confusion found
  (and left alone, being a different device) in the ENS160 settings getters.

  What *is* verified about the driver half is that it builds and that the calls
  survive the link, on all four CI targets — `set-target` 0, build 0, zero
  diagnostics in either touched file:

  | Target | image, table above | image now | `EZPI_aled_fill` / `_unpack_first` |
  |---|---|---|---|
  | esp32 | `0x19d1c0` | `0x19d370` | absent → **present** |
  | esp32s2 | `0x151590` | `0x151750` | absent → **present** |
  | esp32s3 | `0x1a0d30` | `0x1a0ee0` | absent → **present** |
  | esp32c3 | `0x1af080` | `0x1af230` | absent → **present** |

  432 bytes of growth, 448 on s2 — measured against the build in the table
  above, which is the last one recorded rather than the immediately preceding
  commit. The commit between the two added `EZPI_aled_fill` and
  `EZPI_aled_unpack_first` to a component nothing called yet and was not
  re-measured for image size.

  The **absent → present** flip is the evidence rather than the presence: both
  symbols were absent from all four ELFs before the driver called them, because
  `--gc-sections` collected them. A symbol that is present because it is
  reached and one that is present because it was never collected look the same
  in an `nm` listing; a symbol that *became* present can only have done so
  because a call to it survived the link.

  **The order itself is still unobserved.** No strip has been lit in any order,
  `grb` included. Four green builds and a symbol table say nothing about
  whether the strip lights.

- **The defect this component exists to prevent, stated exactly.**
  `EZPI_core_cjson_get_value_uint32()` assigns `*item_val = 0` *before* it looks
  the key up, and leaves the 0 there when the key is absent or is not a JSON
  number. So the obvious way to write the caller —

      uint32_t n = 1;                           // looks like a default
      CJSON_GET_VALUE_UINT32(cj_device, key, n); // n is now 0

  — discards the default silently. And 0 is not a harmless value:
  `led_strip_init()` begins `CHECK_ARG(strip && strip->length > 0 && ...)`, so a
  resolved count of 0 does not dim the strip, it fails initialisation and the
  device never comes up. That asymmetry is the whole reason the resolver owns
  the defaults instead of its callers, and why every path through it — including
  a `NULL` device object — leaves a usable config behind.

- **The defaults are the shipped behaviour, deliberately, and that is the
  load-bearing property.** A config with none of the three keys resolves to one
  GRB pixel with no white channel: exactly the hardcoded `length = 1`, the
  `is_rgbw` left zero by the `memset`, and the order `led_strip` uses for
  SK6812 anyway.

  Every strip already provisioned in the field has a stored config containing
  none of these keys, so a "better" default would be a silent behaviour change
  pushed to working hardware by a firmware update. An absent key is also
  reported as agreement rather than as a substitution, so the eventual caller
  does not log about every strip provisioned before the keys existed.

- **The 2048-pixel bound is derived from the wire protocol, not picked.** A
  WS2812/SK6812 bit is 1.25 µs. An RGBW pixel is 32 bits, so 40 µs per pixel,
  so 2048 pixels is 81.9 ms of unbroken transmission — about 12 frames per
  second, before the ≥50 µs reset gap. RGB is 24 bits, 30 µs, 61.4 ms, 16 fps.
  So the cap sits where the strip stops looking animated on a single RMT
  channel; raising it would not buy a usable strip, it would buy a 32 KB
  allocation that fails on a loaded heap. At the cap the buffer is 8 KB.

  An out-of-range count falls back to **1, not to the cap**: a config saying
  100000 is far likelier a typo or a unit confusion than a request for the
  longest strip this can drive, and quietly lighting 2048 pixels would hide
  that. 1 is conspicuous and recoverable.

- **Three refusals that could each have been a silent coercion.** A quoted
  `"60"` is refused rather than parsed, because that is what a form field sends
  when nobody converted it. `2.5` is refused rather than truncated to 2 — a
  count is the one field where rounding yields a strip that works at almost the
  right length, which is the hardest kind of wrong to notice. And the value is
  read from `valuedouble` rather than `valueint`, compared as a double *before*
  any cast to `size_t`, because casting a negative or huge double to an
  unsigned type is undefined and a bound applied after the cast would be
  testing `SIZE_MAX` rather than `-1`.

  A correction to the obvious reasoning there, which was measured rather than
  assumed: the `1e9` case does **not** discriminate `valueint` from
  `valuedouble`, because cJSON's `INT_MAX` clamp is itself out of range and both
  readings refuse it. Swapping the two fields was caught by the fractional case
  and by nothing else, and the test now says so instead of claiming credit for
  a check that does not do the work.

- **One malformed key does not discard the others.** Each field falls back
  individually, so a config with a good `pixel_count` beside a nonsense `rgbw`
  keeps the count. Checked in both key orders, so the outcome does not depend on
  which is read first.

- **`color_order` is swept over all 17,576 three-letter lowercase strings**, of
  which exactly the six genuine permutations of r/g/b are accepted — with the
  expectation computed in the test rather than copied from the subject's table.
  Case-insensitive, via an explicit ASCII fold rather than `tolower()`, whose
  behaviour is locale-dependent and undefined for a negative `char`.

  The length is checked *before* the compare, and both near-misses are pinned:
  `"rg"` would match a prefix compare, `"rgbx"` would match a bounded compare of
  3. A refused string also leaves the caller's variable untouched rather than
  zeroing it to GRB, so a caller that ignores the return value keeps its own
  order instead of silently getting a different one. The name-to-enum pairing is
  checked against names written in the test, because a table merely shuffled
  against the enum round-trips perfectly and lights the strip in the wrong
  colours.

- **No schema change was needed, and that is by prior design rather than luck.**
  `ezlopi-util-devcfgval` is deliberately permissive — it rejects only shapes
  that cannot work on this build and accepts unknown members, precisely so a
  key added by a newer app is not refused by an older hub. The config is stored
  as raw JSON in the factory-info partition and re-parsed on each boot, so the
  three new keys persist with no storage-layer change. There is no key
  whitelist to register with.

- **61 checks; 17 mutants, 17 killed, 0 survived.** One is killed by ASan alone
  with no check failing: dropping the length check before the string compare
  reads past a two-byte string and reports a global-buffer-overflow.

  Two of the seventeen were **mis-anchored on the first pass** — the default
  pixel count is a `#define` in the header rather than in the `.c` the sweep was
  editing, and one anchor said `return` where the source says `ret =`. Both were
  reported as no-ops by the sweep's own detector rather than as survivors, which
  is the reason that detector exists, and both were re-run with correct anchors
  and killed. A mutant that never applied is indistinguishable from one that
  survived, so the distinction is recorded rather than smoothed over.

- **How to onboard a strip is now written down, in
  [`docs/addressable-led-onboarding.md`](docs/addressable-led-onboarding.md).**
  The keys reach the driver without a schema change, as above — but the shape of
  the document carrying them was documented nowhere.
  [`provisioning-schema.md`](docs/provisioning-schema.md) covers the hub payload
  (`user_id`, certificates, `cloud_server`), which is a different document sent
  by a different route, so a user with a 60-pixel strip had to read the resolver
  to find out what to send. The new file states the five required keys and the
  three strip keys with their JSON types, the `chipset` that must match, the
  under-4096-byte bound on the whole document, and the three routes in. It is
  explicit about the three places a reasonable guess is wrong — a quoted count,
  a fractional count and an out-of-range count all resolve to **1** rather than
  to an error — and about what the firmware will not do: a `pixel_count` change
  takes effect on the next boot, not immediately. It repeats that no board has
  driven a multi-pixel strip, in the file a user reads first.

  That list was one item longer when the file was written: five of the six
  colour orders were accepted and then ignored, and the doc said so. The
  sentence is gone because the limitation is, and what replaced it is the thing
  a user with a miscoloured strip actually needs — if green comes out where red
  was asked for, the strip is `rgb` rather than `grb`, and the part number does
  not settle it.

- **That doc named the wrong serial command and the wrong BLE characteristic,
  and it is corrected.** The three routes in were a three-row table, and two of
  its three rows would not have worked:

  **It said serial `cmd:7`. Set-config is `cmd:3`.** `cmd:7` is
  `EZPI_UART_CMD_SET_PROV` — the hub *provisioning* payload, serial number and
  certificates and `cloud_server`, on a different handler. A user following the
  doc would have sent `dev_detail` to the certificate handler and configured no
  strip. The two numbers are adjacent in the same enum and the two documents
  are both "the config", which is the whole reason this was easy to write and
  hard to notice.

  **It linked the BLE row to the provisioning characteristic.** The row pointed
  at `provisioning-schema.md#ble-provisioning`, whose text says to write to
  `0xE301`. The device configuration goes to `0xE501` on service `0x00E5`.
  Nothing was wrong with the chunk envelope — that part really is shared — but
  the handle a reader would have come away with was the other one.

  **And it did not say the serial payload goes at the top level.** The
  provisioning route wraps its payload as `{"cmd":7,"data":{…}}`; this one does
  not — `ezpi_service_uart_set_config()` hands the whole parsed line to the
  storer and reads no `data` member. A document nested the way the neighbouring
  route nests its own parses, validates, is stored, is acknowledged, and
  produces a hub with no devices. That is the worst available outcome, because
  every signal the user can see says it worked. The firmware's own read-back
  path is the corroboration: `ezpi_service_uart_get_config()` deletes `cmd`
  from what it pulled out of flash before replying, which it would not need to
  do unless the stored document carried it.

  The table is gone and the three routes are now three subsections, because
  what matters about them is where they differ — one of them reboots the hub,
  one of them tells you to, one of them does neither — and a table row invites
  a reader to skim the one column they think they need.

- **That doc's example is machine-checked, so it cannot drift.**
  `tests/host/test_aledonboard.c` reads the markdown, extracts three marked
  blocks, and runs the first through the real `EZPI_util_devcfgval_check()` and
  `EZPI_aledcfg_from_json()`, comparing the result against the shape the second
  block claims. The test holds **no copy of the JSON and no expected numbers**:
  a copy is the thing that drifts, and two copies agreeing with each other and
  not with the firmware would both pass. The expected chipset string is linked
  from `ezlopi_cloud_keywords.c` rather than typed, so a renamed constant cannot
  leave the test green while every real config is refused. The doc is listed as
  a prerequisite of the binary, so editing it rebuilds the test. **17 checks**,
  clean under ASan and UBSan.

  It extracted two blocks when it was written, and the third is there because
  the corrections above were the kind a reworded paragraph does not prevent
  from happening again. The serial route's example is now its own marked block,
  and four of the seventeen checks are about it: that its `cmd` is
  **`EZPI_UART_CMD_SET_CONFIG`, taken from the real
  `ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.h`**, that it has no
  `data` member, and that the block with its `cmd` removed is
  `cJSON_Compare`-equal to the worked example. There is deliberately **no `3`
  in the test file**; the one that was wrong in the doc was a literal, and a
  literal in the test would have agreed with it.

  That is the first host test to include a header out of `ezlopi-services/`.
  Only the header, and only for one enumerator — the service itself is FreeRTOS
  and ESP-IDF end to end and nothing from it is compiled. It needed an empty
  `tests/host/stubs-aledonboard/sdkconfig.h`, which is the first stub
  `sdkconfig.h` in this suite that switches nothing on: the enum sits outside
  every `#ifdef`, so all that was missing was the file. `CONTRIBUTING.md` is
  updated, because the note there said a stub `sdkconfig.h` exists to switch a
  subject *on* and now one of them does not, and the measurement that note is
  built on — an empty shared one lets 52 more files compile and all 52 define
  nothing — is exactly the trap this use looks like from a distance. What keeps
  it out of the trap is that the test asserts against the symbol it linked
  rather than compiling a translation unit and trusting it to be non-empty.

  Compiling that header on a host turned up two pre-existing defects, both
  recorded and both left alone as out of scope for a documentation fix.
  `ezlopi_service_uart.h` uses `uint8_t` at line 141 while including nothing
  but `"sdkconfig.h"`, so it cannot be the first header in a translation unit;
  every real build gets `<stdint.h>` through something else, which is why this
  has never been noticed. The test includes `<stdint.h>` ahead of it on
  purpose, rather than relying on `cjext.h` continuing to pull it in.

  `ezlopi_service_ble.h` has the same `uint8_t` problem and one of its own:
  **its C++ guard is spelled `__cpluplus`**, at lines 61 and 348. Symmetric, so
  nothing is unbalanced and no C build is affected — but the `extern "C"` it
  guards is never emitted, so a C++ translation unit including it gets mangled
  names for every BLE function and a link error that names none of this. That
  header was read because the GATT UUIDs in the doc were a candidate for the
  same enum treatment as the command number; they are stated in prose instead,
  with the doc saying in as many words that they are *not* machine-checked.
  Scraping hex out of a sentence is a fragile check, and a fragile check on a
  true statement is worse than a labelled unchecked one.

  It was 14. The removed one asserted that the example asks for `grb`
  *specifically*, on the stated grounds that no other order could be honoured.
  That reason stopped being true, so the check went with it rather than being
  kept for the shape of it: a check whose stated justification is false is
  worse than no check, because the next reader believes the justification. The
  order is still compared against the doc's own claimed-shape block, which is
  what the test is for.

  Confirmed able to fail before being believed, which for a test whose subject
  is a *file* is the whole question — the easy mistake is one that reports
  "0 checks, 0 failures" and exits 0 when it cannot find the document, which
  reads as a pass. **Fourteen mutants; thirteen red, one green, none
  surviving.** Twelve are mutated copies of the doc under `/root` with
  `DOC_PATH` repointed at each: count changed in one block, `rgbw` flipped in
  another, chipset lowercased, `id_item` 9 → 11, the config marker deleted, the
  marker and its fenced block deleted, the file absent, the claimed shape
  saying `rbg` where the config says `grb`, the serial block's `cmd` 3 → 7, the
  serial block nested under `data`, the serial copy drifted to 120 pixels while
  the worked example stayed at 60, and the serial marker deleted. The real doc
  passed; all twelve went red at the check that should catch them.

  Three of those are worth naming individually. **`cmd` 3 → 7 is the defect
  this doc actually shipped with**, so that mutant is the regression test for
  it. **The `data`-nesting mutant is the one that would otherwise be caught by
  nothing**, because a nested document is valid JSON that the doc's reader
  would send and the firmware would store. And **deleting the serial marker
  reports 14 checks where the real doc reports 17** — four checks stop running
  and the extraction counts one failure in their place, which is the difference
  between a red test and a *smaller green*, and a smaller green is
  indistinguishable from a pass inside a suite aggregate.

  **The thirteenth red one does not touch the doc at all.** It compiles the
  test against a copy of `ezlopi_service_uart.h` with one command inserted
  above `SET_CONFIG`, so the enum says 4 while the real, unmutated doc still
  says 3. That is the mutant that demonstrates the number is *linked*: a `3`
  typed into the test would have stayed green with the document naming a
  command that means something else. It cannot be done with `-D` — defining an
  enumerator as a macro makes the header itself a syntax error — which is why
  it is a header copy on the front of the include path rather than one more
  compiler flag.

  **The fourteenth must stay green, and it is the one that makes the other
  thirteen mean anything.** It changes `pixel_count` from 60 to 120 in *all
  three* blocks: a doc describing a different strip, which is not a drift. It
  passes — which is what demonstrates the test compares the blocks against each
  other rather than pinning the literal 60. Without it, every red mutant could
  have been red for the wrong reason, and the test could have been a copy of
  the numbers wearing a different hat. One earlier mutant was rewritten for the
  same reason: it had been changing `color_order` in both blocks at once, which
  only the deleted "must be `grb`" check was catching, so after that deletion
  it was testing nothing. It now changes the claimed block alone.

  One of the mutants was green on the first run and **the fault was in the
  mutation, not the test**: the edit anchored on `"rgbw": true` at end of line,
  every occurrence in the doc carries a trailing comma, so it wrote out a
  byte-identical copy. A no-op mutant is indistinguishable from a test that
  failed to notice — the same confusion as the mis-anchored aledcfg mutants
  above and the empty-toolchain `nm` skip — so the falsification script now
  compares each mutant against the original before it will report a green, and
  the four added for the serial block refuse to run at all if their edit
  matched nothing.

- The full host suite is **6497** checks across **32** binaries, 0 failures,
  with every binary reporting in the shape the aggregate greps for. The
  aggregate is re-run rather than adjusted whenever it changes; this figure is
  what the run printed, not an arithmetic result. The steps behind it:
  **6390 + 61 + 17 = 6468** across 31 binaries, where the 6390 includes
  `test_aled`'s 201, then **+18** for `test_ens160set` as the 32nd binary, and
  then **+11** when the settings allocation-failure checks were added to that
  same binary. The figure before this work was 6464.

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

### Null dereferences — [sec]

- **34 pointers were dereferenced above the `NULL` test written to guard
  them**, across thirteen files in `ezlopi-core` and `ezlopi-services`. Thirty
  were the same copy-pasted line:

  ```c
  char *updated_rooms = cJSON_PrintBuffered(__FUNCTION__, cj_rooms, 4096, false);
  TRACE_D("length of 'updated_rooms': %d", strlen(updated_rooms));
  if (updated_rooms)                       /* the guard, one line too late */
  {
      EZPI_core_nvs_write_rooms(updated_rooms);
  ```

  Both halves of that matter, and both were checked rather than assumed.
  `cJSON_PrintBuffered()`
  ([`cjext.c:1508`](ezlopi-components/cjext/cjext.c#L1508)) returns `NULL` on
  three paths — a negative prebuffer
  ([1514](ezlopi-components/cjext/cjext.c#L1514)), an `ezlopi_malloc()` failure
  ([1520](ezlopi-components/cjext/cjext.c#L1520)), and a `print_value()`
  failure ([1531](ezlopi-components/cjext/cjext.c#L1531)) — which is exactly
  why every one of these sites already had an `if (ptr)` written for it. And
  the trace is not compiled out: `CONFIG_EZPI_UTIL_TRACE_EN` is `default y` in
  [`Kconfig.projbuild`](ezlopi-main/Kconfig.projbuild#L18) and `=y` in all four
  committed `sdkconfig.defaults.*`, so `strlen(NULL)` ran in every shipped
  image. The paths reaching them are the ordinary ones: renaming a room, adding
  a device group, saving a scene, editing an expression, the BLE device-info
  characteristic, and the UART config query.

  The fix is a relocation, not a rewrite — each trace moved inside the guard
  that was already there. It was applied by a script that refuses any site not
  matching the template exactly and reports it instead, so the two sites where
  the trace was already correctly placed were skipped and named rather than
  silently touched.

  The other four were the same error in different shapes:

  - [`ezlopi_core_scenes_scripts.c:204`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_scripts.c#L204)
    — `curr_script->next = __scripts_create_node(...); if (curr_script)`. The
    test names the pointer the loop above has already proved non-`NULL`, so it
    was always taken and `curr_script->next->id` was read regardless.
    `__scripts_create_node()` returns `NULL` for any script object missing a
    string `name` or `code`, which arrives over the cloud API. The `else`
    branch three lines below tests `script_head`, the pointer it actually
    assigned — so the correct form was already in the same function.
  - [`ezlopi_core_scenes_operators.c:272`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_operators.c#L272)
    and [`:343`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_operators.c#L343)
    — `if (devgrp && itemgrp && value && comparator)` with the
    `else` dereferencing `comparator` unconditionally. The first branch tests
    it, so `NULL` is a value that reaches the second. The callee
    ([`:1235`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_operators.c#L1235))
    already guards all three of its own arguments at
    [`:1239`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_operators.c#L1239),
    so only the caller's deref needed a guard.
  - [`ezlopi_core_scenes_value.c:120`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_value.c#L120)
    — `if (cj_root && (NULL == cj_root->valuestring))` with the `else` reading
    `cj_root->type`. That condition is false when `cj_root` is `NULL`, so the
    `NULL` case fell into the branch that dereferences it. The test is now
    hoisted so it dominates both branches.

  Found by wiring up a static analyser, which the previous release recorded as
  the honest way to close this gap and left undone. `cppcheck` reported 34
  findings covering 33 of these 34 defects — two of the findings are the one
  `scripts.c` defect, and the thirtieth trace site — `ezlopi_core_scenes_v2.c`
  line 2091 as the tree stood before this change, now
  [`:2094`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_v2.c#L2094) —
  it did not report at all. That one was found by grepping for the trace text
  once the shape was known, and it is fixed with the rest.

  Before being trusted the analyser was run against known-bad input the way a
  test harness should be. Of the two check-after-use defects fixed earlier in
  this branch it finds one (the `isprint(ret[0])` in `factory_info`) and misses
  the other (the `strlen()`s in `ezpi_provisioning_info_read_func()`), even at
  `--enable=all --check-level=exhaustive`. Recall is therefore 1 of 2 on the
  known cases and 29 of 30 on the idiom. That is recorded in the check itself,
  because a green run means these 34 cannot come back silently — not that the
  class is gone.

  It is now [`tests/ci/check_null_deref.py`](tests/ci/check_null_deref.py) in
  the hygiene job, scoped to the null class over the seven first-party
  component directories, where the baseline is zero. It fails with exactly 34
  findings against the preceding commit and passes after, and it self-tests on
  three fixtures first — broken ordering must be flagged, correct ordering must
  not be, and the misdirected-guard shape must be — so a missing or renamed
  checker fails loudly instead of passing vacuously. The scan takes 12 seconds,
  plus the `cppcheck` install.

  `--enable=warning` alone is enough: `style` and `performance` add about 460
  further findings in this tree, none in this class, and a check whose baseline
  moves with style churn is a check that gets switched off. `ezlopi-components`
  is out of scope on measured grounds — it reports 70 findings and every one is
  in vendored code (31 in `cjext/`, 39 in the upstream Lua interpreter), with
  nothing flagged in this project's own drivers there.

  Verified by building all four targets and comparing against the preceding
  commit by set difference rather than by count: **zero warnings added and zero
  removed** on esp32, esp32s2, esp32s3 and esp32c3. The images grow by 112–160
  bytes, which is the right direction — the four new guards are real tests, and
  a trace that had been *deleted* rather than moved would have made the image
  shrink. Confirmed directly: the esp32c3 image contains 25 `length of ` format
  strings both before and after.

  Counting was deliberately not used as the evidence here. The first attempt
  counted warnings in files the change touched and reported 10–14 per target;
  every one was a pre-existing `#warning` pragma or a `-Wdiscarded-qualifiers`
  at a line the fix never went near — `ezlopi_service_uart.c:443` against an
  edit at `:1508`. A count over a file is the wrong unit.

### A 35th null dereference, and the fallback written for it — [bug]

- **`EZPI_core_modes_init()` dereferenced the parse result one line after the
  parse, and set the flag meaning "it worked" one line before it.** Stored
  house modes are read from NVS at boot, parsed, and then — as the code stood:

  ```c
  if (cj_custom_modes)
  {
      _is_custom_mode_ok = 1;
      sg_custom_modes = EZPI_core_modes_cjson_parse_modes(cj_custom_modes);
      EZPI_core_modes_set_current_house_mode(EZPI_core_modes_get_house_mode_by_id(sg_custom_modes->current_mode_id));
      cJSON_Delete(__func__, cj_custom_modes);
  }
  ```

  That the callee can return `NULL` was checked, not assumed:
  `EZPI_core_modes_cjson_parse_modes()` allocates at
  [`ezlopi_core_modes_cjson.c:158`](ezlopi-core/ezlopi-core-modes/ezlopi_core_modes_cjson.c#L158),
  does every bit of its work inside `if (parsed_mode)` at
  [`:159`](ezlopi-core/ezlopi-core-modes/ezlopi_core_modes_cjson.c#L159), and
  has one exit, `return parsed_mode;` at
  [`:452`](ezlopi-core/ezlopi-core-modes/ezlopi_core_modes_cjson.c#L452) — so
  an allocation failure returns `NULL` and the next line reads through it.

  The flag is the worse half. Because `_is_custom_mode_ok = 1` was set *before*
  the call, the same failure also made
  [`:1259`](ezlopi-core/ezlopi-core-modes/ezlopi_core_modes.c#L1259) —
  `if (0 == _is_custom_mode_ok) { EZPI_core_default_init(); … }`, the
  default-mode fallback — unreachable in exactly the case it exists for. Had
  the dereference somehow been survived, the device would have come up with no
  modes at all. The flag is now set only once there is something to report, so
  the failure routes into the fallback; that the fallback is itself safe was
  also checked rather than assumed —
  [`EZPI_core_default_mode_get()`](ezlopi-core/ezlopi-core-modes/ezlopi_core_default_modes.c#L138)
  returns `&sg_default_mode`, the address of a static, and cannot be `NULL`.

  Marked **[bug]** rather than **[sec]**, and the trigger is genuinely
  unlikely: it needs the `sizeof(s_ezlopi_modes_t)` allocation to fail during
  boot, which is when the heap is at its emptiest. What makes it worth an entry
  is less the defect than where it was hiding. The 34 above were closed with a
  check whose recall was known to be 1 of 2, recorded as such, and left there —
  this is what the missing half looks like.

  Found by [`tests/ci/check_check_after_use.py`](tests/ci/check_check_after_use.py),
  the nineteenth check and the sixteenth the `hygiene` job invokes. It covers
  the same class as `check_null_deref.py` by a different route, because the
  shape cppcheck misses is structural rather than a matter of tuning: a guard
  sitting in a *sibling* block of the one holding the dereference. Neither the
  real build's 244 include paths, nor `--enable=all --check-level=exhaustive`,
  nor the platform model changes that.

  The scanner grew from a prototype written earlier in this branch and recorded
  then as unverified, which turned out to be right in both directions —
  measured against the two known check-after-use defects it **missed the one it
  had been written for**, and it produced three findings against an
  already-fixed file. Both were worth chasing rather than tuning around:

  - The miss had nothing to do with sibling blocks. Instrumenting the scanner
    rather than re-reading it showed the assignment being registered under the
    wrong name: the optional leading type group, there to match the `char *` in
    `char *p = f();`, required no separator before the name, and being greedy
    it took as much of a bare assignment's own name as it could. Measured on
    the provisioning defect's own line, `g_provisioning_info_base64 = f();`
    registered as an assignment to **`e64`** — the group took everything up to
    the last character a name could start with. Every later dereference of the
    real identifier was then invisible. The other known case was found only
    because it is written `char *ret = …`, a declaration, where the group has a
    real type to match and leaves the name alone.
  - The largest false class is correct short-circuits —
    `if (p && strstr(p, …))`, `(NULL != s) && (s[0] …)`,
    `(NULL == s) || strcmp(s, k)`, and the same thing written as a ternary or
    a call argument rather than an `if`. Which polarity guards depends on the
    operator and the order decides it, since `if (strlen(p) && p)` *is* a
    defect. Handling this is not a detail: switching the rule off on the
    current tree brings back **225** findings, every one of them correct code.
    The first run showed only a handful, because the name-mangling bug above
    was hiding most assignments at the time — which is why a first-run triage
    total is no measure of a class's size.
  - Two narrower ones. A dereferencing call's argument list was read past its
    closing paren, so `strlen(buf), now` looked like `now` being handed to
    `strlen`. And assertion macros that `goto` on failure are early-exit
    guards: the one in this tree,
    [`PING_CHECK`](ezlopi-core/ezlopi-core-ping/ezlopi-ping/ping_sock.c#L80),
    accounts for 26 findings on its own. Those are detected by shape rather
    than by name — the file's own `#define`s are scanned for a body that tests
    `!first_arg` and then jumps — so the technique generalises instead of
    hard-coding one macro.

  Generalising the short-circuit rule once silenced a *genuine* defect in the
  fixtures, which is the failure mode a check like this is supposed to be
  protected against. One function was answering two different questions: which
  pointer a condition proves non-`NULL` for the block it opens (`&&` conjuncts
  only — `if (A || B)` proves nothing inside the block) and whether the author
  tested the pointer at all anywhere in the condition, which is what makes the
  later-guard anchor precise. They are now separate functions.

  Recall after six fixes is **2 of 2** on the known cases, with both of the
  fixed files silent, and the tree is at **0 findings over 159 files**. All 41
  findings from the first run were read individually rather than filtered:
  one real defect and 40 false, every one of them in a class named above or
  the one below. That last class is vouched for inline rather than solved, in
  [`ezlopi_core_wifi.c`](ezlopi-core/ezlopi-core-wifi/ezlopi_core_wifi.c#L783),
  where `found = 0` in the failure branch is what makes the loop below
  unenterable — data flow, which no line-oriented scanner can see. The
  suppression comment must name the pointer *and* give a reason; the stated
  cost is that those six dereferences are no longer watched by this check.

  Evidence that a green run means something, all of it re-measured against the
  file that CI actually runs rather than the prototype it was copied from:
  it exits 1 on the pre-fix `ezlopi_core_modes.c` extracted from git and 0 on
  the fixed one; it reports 6 findings on the pre-suppression `wifi.c` and 0
  after; stripping only the *reason* out of that suppression brings all 6 back;
  and a copy with the scanner's `return findings` replaced by `return []`
  exits 1 with `SELF-TEST FAILED` instead of passing silently. Sixteen fixtures
  run before the tree is scanned — eight correct shapes that must stay silent,
  each paired with the same shape written the wrong way round, which must
  fire. One of those fixtures immediately caught a hole in the escape hatch:
  in `/* check-after-use: p */` the comment's own `*/` satisfied the
  "reason is mandatory" pattern, so a bare marker did suppress. The reason now
  has to contain at least three letters once `*/` is removed.

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

### Dead code — [bug]

- **28 `cJSON_DeleteItemFromObject()` calls that could never find anything to
  delete.** Fourteen of the mode updaters in
  [`ezlopi_cloud_modes_updaters.c`](ezlopi-cloud/ezlopi-cloud-modes/ezlopi_cloud_modes_updaters.c)
  opened by deleting `id` and `method` from `cj_response` before writing them.
  `cj_response` is always empty on entry: updaters are reached only through
  `__execute_method()` in `ezlopi_core_api.c`, which hands over a freshly
  created object, and the one updater that calls another —
  `EZPI_cloud_modes_changed_alarmed()` — allocates a new object for the inner
  call too.

  What settles it is not that the calls are no-ops but that the file disagrees
  with itself. Twenty-two functions in that file write `id` into `cj_response`,
  and only 14 of them had the pair — **eight never did**. If `cj_response`
  really could arrive populated, those eight would be the bug: cjext's
  `add_item_to_object()` appends without replacing, so they would emit two `id`
  members and clients would be left to guess which one is real — the same defect
  already fixed in `ezlopi_core_api.c` and recorded under **Local API replies**
  above. A guard covering 14 of 22 writers is not insurance; it is a reader being
  told the input might be dirty when it cannot be. Either all of them need it or
  none do, and the call graph says none. The call graph is written into the file
  so the next person does not have to re-derive it.

- **Three `#if 1` blocks that had been pretending to be configuration guards
  since the file was added.** In
  [`ezlopi_service_ws_server.c`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c)
  the bodies of `ezpi_ws_async_send()`, `ezpi_msg_handler()` and
  `ezpi_start_server()` each opened with `#if 1 // def
  CONFIG_EZPI_LOCAL_WEBSOCKET_SERVER` and closed with an `#endif` whose comment
  named that option. The guard was commented out, not designed out, and it has
  been that way since `f2a74e5`. An `#if 1` with no `#else` cannot change which
  code is compiled, so the pairs are **removed rather than converted**: no
  configuration gains or loses a single statement.

  Being precise about "unchanged", since it is the whole basis for doing this at
  all: the token stream the compiler sees is identical, but the image is *not*
  byte-identical, because removing six lines and adding a comment block shifts
  `__LINE__` for every `TRACE_*` below them in the file. Those macros pass
  `__LINE__` as an `int` argument rather than stringifying it, so what changes is
  the line number printed on a console diagnostic — not behaviour, and not the
  size of anything.

  Converting them to real `#ifdef`s was the obvious reading of the comment and
  it is wrong. Measured: with esp32 defaults and only
  `CONFIG_EZPI_LOCAL_WEBSOCKET_SERVER` turned off — the websocket *client* left
  on — converting the three guards takes the link from clean to **127 undefined
  references**, among them `EZPI_items_list_v3`, `EZPI_hub_cloud_log_set` and
  most of `EZPI_cloud_modes_*`, all reached from `method_list_v2` in
  `ezlopi_core_api_methods.c`. That configuration links today and carries a
  complete method table, so the conversion breaks something that works.
  Removing code cannot create undefined symbols by itself; what changes is
  which archive members the linker extracts, and that rule is ESP-IDF's. It was
  not chased further because the useful answer was already in hand, and the
  measurement is recorded in the file so nobody repeats the experiment.

  What the option actually controls is real but narrower than the old labels
  suggested. `ezpi_respond_cjson()` and `ezpi_ws_server_send()` are genuinely
  `#ifdef`'d, and `ezlopi_main.c` calls `EZPI_service_ws_server_dummy()`
  instead of `EZPI_service_ws_server_start()` when the option is off, so nothing
  in these three regions ever runs in that build. Dead at runtime, not absent
  from the binary: the measured cost is **672 bytes** of image in the MQTT-only
  configuration CI builds. The 36 KB embedded config page is not part of that —
  `_binary_config_ui_html_start` is present in server-on images and absent in
  server-off ones, so `--gc-sections` already drops it.

  One probe used along the way was worthless and is called out because the
  result looked convincing: an `nm` check for `ezpi_serve_config_page` returned
  zero in server-off builds, which proves nothing, because it returns zero in
  server-**on** builds too. It was caught by deliberately running every probe
  against a known-positive build first. The two that do discriminate are
  `_binary_config_ui_html_start` (1 → 0) and `httpd_ws_send_frame` (2 → 0).

- **A trace branch that could not be revived.** The `else` of
  `if (NULL != cj_response)` in `ezlopi_core_api.c` was compiled only under
  `CONFIG_EZPI_UTIL_TRACE_EN` and contained a `cj_method` lookup whose only use
  was commented out, plus `TRACE_E("Error - cj_response: %d",
  (uint32_t)cj_response)`. In that branch `cj_response` is NULL by
  construction, so the message could only ever print `0` — and it was `%d`
  against a `uint32_t` cast of a pointer, the same class as the format-string
  entries below, invisible to `cppcheck` because `TRACE_E` is an unexpanded
  macro to it. The branch now logs which allocation failed and takes no
  arguments at all: `cJSON_CreateObject failed; dropping request without a
  reply`.

- **What this change set costs, measured on all four targets rather than
  assumed.** Each target was built twice in the same directory, with the same
  `managed_components` and a freshly regenerated `sdkconfig`, differing only in
  whether the twelve touched files sat at `HEAD` or at this change set — so
  nothing from how an earlier baseline happened to be captured can leak in. The
  builds reproduce byte-exactly on repeat.

  | Target | `.flash.text` | `.flash.rodata` | `.iram0.text` / `.dram0.data` / `.dram0.bss` | Image |
  |---|---|---|---|---|
  | esp32 | −356 | +32 | unchanged | 1 683 216 → 1 682 880 (−336) |
  | esp32s2 | −316 | +32 | unchanged | 1 375 552 → 1 373 264 (**−2 288**) |
  | esp32s3 | −336 | +32 | unchanged | 1 697 984 → 1 697 680 (−304) |
  | esp32c3 | −422 | +32 | unchanged | 1 756 496 → 1 756 096 (−400) |

  Three things in that table are worth saying out loud:

  - **No RAM moves anywhere.** `.dram0.data`, `.dram0.bss` and `.iram0.text` are
    identical on every target. This change set is flash-only.
  - **`.flash.rodata` grows by exactly 32 bytes on all four**, which is the one
    thing here that *adds* anything: the replacement `TRACE_E` string in
    `ezlopi_core_api.c` is 35 characters longer than the one it replaced, and 32
    is what that costs after alignment. The rest is removal.
  - **esp32s2's −2 288 is not code**, and it was taken apart rather than
    reported, because −2 288 for 28 call sites would be a sign that something
    else had gone missing. Content there accounts for −284 bytes. The other
    −2 004 is a `[PADDING]` segment: `esptool image_info` shows `HEAD` carrying
    a 2 004-byte `[PADDING]` entry that the new image does not have at all, and
    `IROM` moving from file offset `0x60018` to `0x50018`. Shrinking
    `.flash.text` let ESP-IDF reorder the segments so the pad that aligns `IROM`
    to its 64 KB MMU page is no longer needed. Segment count is 5 either way,
    and both checksums and validation hashes verify. The other three targets
    land within 12 bytes of their content delta, which is ordinary end-of-image
    alignment.

  One plausible explanation turned out to be false, and is recorded because it
  is the one a reader will reach for: `--gc-sections` dropping cjext's
  delete/detach chain once the 28 callers went away. It does not —
  `cJSON_DeleteItemFromObject` has nine other callers across the cloud and core
  components, is still present in the post-change image, and the entire
  delete/detach family is 187 bytes, so it could not have been the answer even
  if it had been dropped.

- **A `NULL` assignment that cleared a copy, leaving the caller's websocket
  handle pointing at freed memory.**
  [`EZPI_core_websocket_client_kill()`](ezlopi-core/ezlopi-core-websocket_client/ezlopi_core_websocket_client.c#L142)
  took its handle by value and ended with `client = NULL`, which assigns the
  parameter and nothing else. `esp_websocket_client_destroy()` had already freed
  what it pointed at, so the caller was left holding a dangling handle.

  The one caller in the tree is `EZPI_service_web_provisioning_deinit()` in
  [`ezlopi_service_webprov.c`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L187),
  and its handle is not a local that goes out of scope: `wss_client` is a
  file-static, read from other tasks by `__send_str_data_to_nma_websocket()` at
  `:531` and by `EZPI_service_webprov_is_connected()` at `:156`. The first is
  registered as a broadcast method at `:265`, so it runs on whichever task is
  sending.

  **The statement that makes that window look closed is itself dead.** `deinit()`
  opens with `if (_task_handle) { vTaskDelete(_task_handle); }`, which reads as
  though the task that sends on the handle is stopped before the handle is
  destroyed. `_task_handle` is declared `NULL` at `:149` and is never assigned
  anywhere — the only other mentions in the file are those two reads — so the
  test is always false. GCC reaches the same conclusion and has already acted on
  it: in the esp32c3 image, before and after, the entire function is three
  instructions with no reference to `vTaskDelete` in it. That is the compiler
  constant-folding a static it can prove never changes, not the symbol being
  absent — `vTaskDelete` is in the image, reached from elsewhere.

  The function now takes the handle **by address**, and clears it **before** the
  teardown rather than after, so a reader that loads `wss_client` at any point
  during `stop()` and `destroy()` sees `NULL` instead of a pointer about to be
  freed. No new guards were added, because every IDF entry point these readers
  reach already rejects `NULL`: `esp_websocket_client_is_connected` at
  `esp_websocket_client.c:898`, the shared send path at `:848`, `destroy` at
  `:448`, `stop` at `:740`.

  The whole change is two instructions of the caller, which is the clearest
  statement of what it does. Baseline `lui a5` / `lw a0,-216(a5)` loads the
  *value* of `wss_client`; fixed `lui a0` / `addi a0,a0,-216` computes its
  *address*. Both then tail-jump into the same function.

  **This narrows the race; it does not close it**, and the source says so. A
  reader that loaded the old value before the store still holds it. Closing that
  needs a lock around the handle rather than a wider parameter, and is not
  attempted here.

  Why this is **[bug]** and not **[sec]**: the only caller of `deinit()` is
  `EZPI_core_reset_reboot()`, which calls `esp_restart()` on the next statement.
  Fifteen sites call that function, including BLE and UART paths, so it is
  reachable — but the device is restarting either way, so the worst outcome is a
  crash microseconds before an intended reboot rather than anything an attacker
  gains from.

  Found as `uselessAssignmentPtrArg` by the header-aware `cppcheck` sweep, and
  **it is only findable that way**, which is the part worth recording.
  `esp_websocket_client_handle_t` is an opaque typedef; without headers cppcheck
  does not know `client` is a pointer and reports nothing on this file — before
  or after the fix. Reproduced with the 250 `-I`/`-D` flags for this translation
  unit taken from a compilation database: **1 finding at `:128` before, 0 after.**

  No check was added for the class, and the reason is a measurement rather than a
  judgement. Reinstating the old signature is *not* caught by the build: a tree
  with the fixed definition and the old by-value call site **compiles and links
  successfully**, with one `-Wincompatible-pointer-types` warning at
  `webprov.c:194` — and this tree already carries 87 of those on the Xtensa
  targets and 90 on esp32c3, all pre-existing, so a new one would not stand out.
  A static check cannot cover the class either: the only instance cppcheck sees
  without headers is
  [`EZPI_core_factory_info_v3_free()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L135),
  which is deliberately left alone. It takes `void *`, and its **39 live call
  sites** all pass a function local or one heap struct member freed on the next
  line — checked, none is a file-scope object — so the dead store there is
  unobservable, and a `void **` signature would need a cast at all 39.

  Built on four targets, baseline and fixed. `CONFIG_EZPI_WEBSOCKET_CLIENT=y` in
  all four defaults was confirmed rather than assumed, because `webprov.c` is
  wrapped end to end in that guard from line 90 and a target with it off would
  compile the call site to nothing and still link.

  | Target | Baseline | Fixed | Delta |
  |---|---|---|---|
  | esp32 | 1 683 856 | 1 683 856 | 0 |
  | esp32s2 | 1 374 192 | 1 374 192 | 0 |
  | esp32s3 | 1 698 720 | 1 698 720 | 0 |
  | esp32c3 | 1 757 104 | 1 757 120 | **+16** |

  The one nonzero delta is accounted for rather than waved through:
  `EZPI_core_websocket_client_kill` grows from **58 to 68 bytes** on esp32c3 —
  the `NULL` test and the store — `.flash.text` grows by exactly those 10 bytes,
  `.dram0.data`, `.dram0.bss`, `.iram0.text` and `.flash.rodata` are identical,
  and the remaining 6 bytes are end-of-image alignment. The three Xtensa targets
  absorb the same 10 bytes into alignment and report 0. Both touched files
  produce object files and both functions are in the ELF on all four targets,
  with no warning at a line of either file.

### Format strings — [bug]

- **Five format strings that disagreed with their arguments — nine `cppcheck`
  findings between them**, since one line can carry several bad conversions and
  the uptime line carries four. All are `%d` against a `uint32_t`, or a
  signed/unsigned mismatch on a 64-bit serial, reported as
  `invalidPrintfArgType`. Fixed where the output leaves the device:

  | Site | Was | Now | Why it matters |
  |---|---|---|---|
  | `ezlopi_core_mdns.c` | `"%lld"` | `"%llu"` | the Serial TXT record every discovery client reads |
  | `ezlopi_core_mdns.c` | `"%s_%d"` | `"%s_%u"` | the mDNS hostname, from a `uint32_t serial_last4` |
  | `ezlopi_main.c` | `free_heap":%d` | `free_heap":%u` | JSON a provisioning client parses |
  | `ezlopi_core_info.c` | `"%dd %dh %dm %ds"` | `"%ud %uh %um %us"` | uptime, all four operands `uint32_t` |
  | `ezlopi_cloud_scenes.c` | `"%s_cloned(%d)"` | `"%s_cloned(%u)"` | the generated name of a cloned scene |

  No real device ever misprinted any of these — the widths were already right
  and the values are far below the signed limits — so this is a correctness and
  future-proofing change, not a bug report.

- **One `cppcheck` finding that is a false positive and must stay as it is.**
  `ezlopi_service_otel.c` formats a serial with `%llu`, and `cppcheck` reports
  it needs `unsigned long long` where the argument is `unsigned long`. The tool
  is modelling a 64-bit host. Xtensa is ILP32: `uint64_t` *is* `unsigned long
  long`, and `EZPI_core_factory_info_v3_get_id()` is declared exactly that.
  Re-running with `--platform=unix32` drops this finding and keeps the other
  15, which is how it was identified as a host-model artefact rather than
  asserted to be one. Changing it to `%lu` would be the bug, so the site now
  carries a comment saying so — the single most useful line in this entry,
  because it is the one that prevents a future "fix".

- **The denominator, which is the part worth knowing.** The whole
  `--platform=unix32` run found 15 of these, and all 15 sit inside a much larger
  class that `cppcheck` cannot see: **558 `TRACE_*` call
  sites pass a `%d` a `uint32_t`**, and none are reported, because the `TRACE_*`
  macros are never expanded for the analyser. `ezlopi_main.c` is the clean
  illustration — two `TRACE_W` lines were missed while a `snprintf` three lines
  later was caught. The 558 are **deliberately not changed**: they are console
  diagnostics, editing them touches almost every file in the tree for no
  behavioural gain, and that churn is exactly what risks disturbing working
  Ezlo behaviour. Six further findings in `ezlopi_core_heap.c` are left for the
  same reason — a debug heap dump, on the console only.

  After the fixes a clean `--platform=unix32` run reports **6 findings, all in
  `ezlopi_core_heap.c`** — 15 minus the 9 fixed, with nothing new appearing.

- **Eighteen places where a `*` field width or precision was handed a
  `size_t`.** A `*` reads one argument of type `int` — C11 7.21.6.1p5 says so
  for both the width and the precision — so

  ```c
  TRACE_D("name: %.*s", sizeof(node->name), node->name);
  ```

  passes a `size_t` where an `int` is read. Where `size_t` is wider than `int`,
  `vfprintf` takes four bytes off a `va_list` holding eight and every later
  argument is misaligned, which here is the pointer that was meant to be
  printed.

  **Why no build in this project reports it.** Measured with a two-compiler
  probe rather than assumed, because the answer is the reason the sites
  survived:

  | compiler | `sizeof(size_t)` | `-Wall -Wformat` precision complaints |
  |---|---|---|
  | `xtensa-esp32-elf-gcc` 8.4.0 | `== sizeof(int)` | 0 |
  | `riscv32-esp-elf-gcc` 8.4.0 | `== sizeof(int)` | 0 |
  | host `gcc` (x86-64) | 8, `int` is 4 | 2 |

  `size_t` is `int`-sized on every ESP target, so `-Wformat` has nothing to
  object to and the whole class is invisible to all four target builds — this
  being not a `TRACE_*` blind spot of the sort the entry above describes, since
  `TRACE_*` does carry a printf format attribute and the project builds with
  `-Werror=format`. Confirmed against the logs: **0 format warnings on esp32,
  esp32s2, esp32s3 and esp32c3.** The class is visible only where `size_t` is
  wider, which is a 64-bit host build — and that is exactly how the two sites
  in `ezlopi_core_cjson_macros.c` were found earlier. The other sixteen were
  found by nothing.

  **"0 format warnings on all four targets" is sound evidence on three of them
  and no evidence at all on the fourth.** That sentence above stays because it
  is what the logs say, but on esp32c3 a clean log is not the same as a clean
  file, and this was measured rather than reasoned:

  | target | deliberate `TRACE_E("%s", 12345)` | why |
  |---|---|---|
  | esp32 | **caught**, `error: format '%s' expects argument of type 'char *' … [-Werror=format=]` | `-Wall` implies `-Wformat`, `-Werror=all` promotes it |
  | esp32c3 | **not caught**, rc=0, 0 diagnostics | `-Wno-format` is on the command line |

  `-Wno-format` is not ours and is not in any `CMakeLists.txt` in this
  repository. It comes from
  `$IDF_PATH/components/esp32c3/project_include.cmake`, whose entire contents
  are three flags — `-Wno-error=format=`, `-nostartfiles`, `-Wno-format` —
  appended with `idf_build_set_property(COMPILE_OPTIONS … APPEND)`. Because that
  is a *build* property rather than a component one, it lands on every component
  in the project and not just on `esp32c3`: **1444 of the 1445 translation
  units** in an esp32c3 build carry it. There is no equivalent file under
  `components/esp32`, `components/esp32s2` or `components/esp32s3`, so the three
  xtensa targets are checked normally.

  Which matters more than it looks, because esp32c3 is the **only** target whose
  `uint32_t` differs. Probed with `_Generic` on both compilers:
  `__UINT32_TYPE__` and `uint32_t` are `unsigned int` on
  `xtensa-esp32-elf-gcc` and `unsigned long` on `riscv32-esp-elf-gcc`. So the
  558 `%d`-against-`uint32_t` `TRACE_*` sites above are invisible for two
  *different* reasons: on xtensa `%d` against `unsigned int` is a pure
  signedness mismatch, which GCC reports only under `-Wformat-signedness`, and
  on riscv32 — where it is a genuine type mismatch and would be reported — the
  diagnostic is switched off. IDF presumably switched it off for exactly that
  reason. Either way, the compiler is not the thing protecting format strings in
  this tree on any target; `check_trace_format_attr.py` keeping the attribute
  alive is, and on esp32c3 even the attribute has nothing to act on.

  So **no device has ever misprinted any of these**, and they will not until
  someone builds for a 64-bit target. What they do break today is host
  testability, which is the thing this tree has been adding.

  **Not all of them are diagnostics**, which is what separates this from the
  558 `%d`-against-`uint32_t` `TRACE_*` sites deliberately left alone above.
  Five of the eight live sites use the conversion to *construct stored data*,
  not to print: `ezlopi_core_scenes_expressions.c` and
  `ezlopi_core_scenes_then_methods_helper_func.c` build the `code` string a
  scene expression is evaluated from, and `ezlopi_service_otel.c` builds the
  id, error and method strings that leave the device for an OpenTelemetry
  collector. The remaining three are console output.

  How the eighteen were disposed of:

  | sites | where | what was done |
  |---|---|---|
  | 2 | `ezlopi_core_cjson_macros.c` | cast earlier, found by the host tests |
  | 8 | scenes ×5, otel, ws-server, meshbot | cast — the live sites |
  | 4 | `ezlopi_core_devices.c` | cast, though inside `#if 0` |
  | 4 | `ezlopi_service_broadcast.c` | the enclosing `#if 0` block removed |

  The last two rows were a judgement call in two directions, and the `#if 0`
  is why. Code behind `#if 0` is text, not a defect, and "fixing" it makes a
  diff look like repairs it is not. But a check that has to carve out a
  preprocessor state is a check with a hole in it, so the dead sites could not
  simply be left:

  - `ezlopi_core_devices.c` holds a controller-information dump kept behind
    `#if 0 // Defined but not used`. It is retained deliberately for
    debugging, and the whole point of retaining it is that it works when
    switched on, so it was corrected in place.
  - `ezlopi_service_broadcast.c` was the opposite: its `#if 0` block was a
    superseded hand-rolled copy of the four reads **three lines above it**,
    which now go through `ezlopi_service_otel_fetch_string_value_from_cjson()`.
    It was removed rather than corrected, because it could not have been
    re-enabled as it stood. It assigned over `id_str`..`msg_subclass_str`,
    leaking the four buffers the live calls had just allocated, and its second
    guard tested `cj_id->str_value_len` where `cj_error` was meant — so with an
    `error` field present and `id` absent it dereferenced a null `cj_id`. Two
    defects that had sat in plain sight; dead text does not get reviewed, which
    is how both survived.

  **What is deliberately *not* changed.** Only `size_t` is wrong here.
  `uint16_t` and `unsigned short` promote to `int` before the call, and
  `uint32_t` is the same width as `int` with C11 6.2.5p9 making the two
  interchange without reinterpretation. The tree relies on exactly that:
  `ezlopi_core_http.c` passes an `int`, the BLE services pass a `uint32_t`
  `copy_size`, and `ezlopi_service_ble_provisioning.c` passes a `uint16_t`
  `param->write.len`. None are defects and none were touched. The probe agrees
  — the 64-bit host warns on the `size_t` and `strlen` cases and **not** on the
  `unsigned int` one.

  **Verified as behaviour-neutral, and the one way it is not.** An A/B built by
  reverse-applying this diff inside the build mirror gives **the same image size
  on all four targets** — esp32 1 683 072, esp32s2 1 373 488, esp32s3
  1 697 872, esp32c3 1 756 320 — with `.flash.rodata`, `.dram0.data` and
  `.iram0.text` **byte-identical**.

  `.flash.text` is not byte-identical: **67 bytes differ**, same total size. The
  expected explanation — that an `(int)` cast of an already-32-bit value is a
  no-op — does not account for that, so it was tracked down rather than waved
  through. `TRACE_*` expands `__FILE__` and `__LINE__` into every call
  (`ezlopi_util_trace.h:172`), so inserting the explanatory comments renumbers
  every trace site below them: same instruction, same width, a different
  line-number immediate. `addr2line` over all 67 offsets puts **every one of
  them inside five of the edited files** and **none anywhere else** —
  `ezlopi_core_devices.c` 21, `ezlopi_core_scenes_expressions.c` 20,
  `ezlopi_service_ws_server.c` 18, `ezlopi_service_meshbot.c` 4,
  `ezlopi_core_scenes_then_methods_helper_func.c` 4.

  That attribution is only safe because of the control: two builds of
  *identical* source give **0** `.flash.text` differences, so the section is
  deterministic and the 67 bytes are genuinely this change. The same control
  also retires `md5` of the `.bin` as an A/B instrument here — two builds of
  identical source differ in 67 `.bin` bytes too, in the app descriptor and the
  appended image hash, because `CONFIG_APP_REPRODUCIBLE_BUILD` is off. Comparing
  whole-image checksums across any change in this tree reports a difference that
  means nothing.

- **New check: `tests/ci/check_printf_star_int.py`.** The only check in the
  hygiene job whose subject no compiler in the pipeline can report, which is
  why it is a check and not a flag.

  It parses each format string, counts how many variadic arguments every
  specifier consumes — a `*` takes one, `%%` takes none — and inspects only the
  argument that actually lands on the star. That machinery is not decoration.
  The cheap version of this check, "a line with `%.*s` that also mentions
  `sizeof`", reports `ezlopi_service_ble_dynamic_config.c:475`:

  ```c
  snprintf(data_buffer, sizeof(data_buffer), "%.*s", copy_size, ...);
  ```

  where the `sizeof` is `snprintf`'s own size parameter — correctly a `size_t`
  — and the precision beside it is a `uint32_t` and fine. A check that reports
  working code gets switched off, so that line is now
  `SELF_TEST['decoy_dest_size']`. Comment stripping is literal-aware for the
  same reason: the tree contains URLs with `//` in them, and a stripper that
  treats those as comments deletes the rest of the line.

  **Measured teeth, in both directions.** Stripping the `(int)` casts back out
  of all fourteen corrected sites makes it report **13**. Separately, making
  `%%` consume an argument — a one-line mutation that silently shifts every
  index after it — trips the self-test on `bad_percent_percent`. On the tree as
  it stands it scans **496 files** and reports nothing.

  **The one it misses is the thirteenth-of-fourteen**, and it is recorded in
  the check's own docstring rather than left to be discovered. Recognition is
  by spelling — `sizeof`, `strlen`, `->str_value_len` — so
  `ezlopi_service_ws_server.c`, which prints the `size_t` field `ws_pkt.len`,
  is corrected but unguarded. Widening the spelling list to catch `.len` is the
  wrong trade: `.len` is an `int` in `ezlopi_core_http.c` and a `uint32_t` in
  the BLE services, both correct. A 64-bit host build of the file remains the
  complete check; this is the cheap guard that runs on every commit.

- **A `sscanf` that let the caller choose the length of a stack write.** The
  last arm of the URL-parsing ladder in
  [`EZPI_parse_http_url()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L136)
  was

  ```c
  char host[100] = {0};
  char page[200] = {0};
  ...
  else if (sscanf(field_value_string, "%*[^:]%*[:/]%[^:]:%d%s",
                  host, &port, page) == 3)
  ```

  `%[^:]` writes into `host[100]` and `%s` into `page[200]`, and neither
  carries a field width, so both write as far as the input reaches.
  `field_value_string` is the `url` field of a `sendHttpRequest` scene action;
  [`EZPI_scenes_populate_fields_get_value`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_populate.c#L494)
  allocates it at `strlen(valuestring) + 1` with no cap, so its length is
  whatever the request said.

  **Why eight correct arms above it did not make it safe**, which is the
  interesting part and the reason it survived review. Every other arm is
  width-bounded — the author plainly knew to write `%99[^:]` and `%199[^\n]` —
  and the last one reads as a corner nobody can reach. It is reachable. Arms
  one to eight each open with a literal `http://` or `https://`, and arms seven
  and eight are catch-alls for those two prefixes, so a 599-character host
  behind an `http://` prefix lands on arm seven and is bounded at 99 there.
  Give the URL any third scheme and all eight literals fail, leaving the one
  arm with no widths.

  **Measured, not reasoned about.** The ladder was reproduced verbatim in a
  host program and run under AddressSanitizer. `http://` + 599 characters +
  `:80/x` lands on arm seven with `host_len=99`, confirming the catch-all does
  its job. `ftp://` + 599 characters + `:80/x` reaches arm nine and ASan
  reports a `stack-buffer-overflow` write beginning at the first byte past
  `host`; `ftp://h:80/` + 599 characters gives a **601-byte** write past the
  end of `page`.

  **Why this is [bug] and not [sec].** Both transports that can deliver a
  scene action are authenticated: the local websocket server admits only
  `hub.offline.login.ui` until the connection is logged in, and the cloud link
  is the device's own TLS session. It is still a stack smash whose length the
  caller picks, and it needs no scene to be stored —
  `hub.scenes.action.block.test` runs this parse on a block taken straight out
  of the request.

  Fixed by giving both conversions the width the rest of the ladder already
  uses, `%99[^:]` and `%199s`. A host longer than 99 now fails the `:` that
  follows, so the arm returns 1 instead of 3 and the URL is reported
  unparseable — which is the right answer rather than a regression, because
  silently truncating a host would send the request to a different host than
  the action named.

  **Found by the header-aware `cppcheck` sweep**, as `invalidscanf`, in the
  course of testing whether a compilation database raises the null-dereference
  checker's recall. It does not — see *Known gaps* — but the sweep that
  answered the question surfaced this on the way past, in the category the
  committed check filters out.

  **Guarded by
  [`check_scanf_field_width.py`](tests/ci/check_scanf_field_width.py)**, the
  eighteenth check and the fifteenth the `hygiene` job invokes. A host test
  was considered first and rejected: this translation unit cannot be compiled
  in `tests/host/` without stubbing seven ESP-IDF headers and four enum
  families, the only in-process oracle for a stack overflow is an ASan abort —
  which the harness has no pattern for, since nothing in it forks — and a test
  would cover the inputs somebody thought to write down. Whether a conversion
  carries a width is purely syntactic, so a check covers the whole class
  instead.

  **Measured teeth.** Against this tree it scans **603 files**, finds **11**
  scanf-family calls and reports nothing; against `git archive HEAD` with the
  same script copied in, it fails and names both conversions at `:195`
  separately. **24 fixtures** run before either, 12 of which must be reported
  and 12 of which must not. The decoys are the ones that matter: a check
  looking for an unbounded `%s` in any string literal would report most of the
  tree, so `SELF_TEST['decoy_trace_printf']` is the `TRACE_S("host = '%s'",
  host)` eleven lines below the defect, and `decoy_wrapper_name` is a call to a
  function whose name merely ends in `scanf`. Assignment-suppressed
  conversions — `%*[^:]`, the first two in the defect itself — write nothing
  and must stay quiet, and a leading `]` inside a scanset is a literal `]`
  rather than the end of the set, so `bad_scanset_literal_bracket` guards a
  parser that would otherwise read the conversion as finished and miss the
  missing width.

  Unlike its sibling above, this one also scans `ezlopi-components`, and the
  cost was measured before the directory was added rather than after: all 107
  `.c` and `.h` files under it hold exactly one scanf-family call,
  `cjext.c:546`, which converts with `%lg` into a `double`. A format held in a
  variable rather than written as a literal is reported rather than skipped —
  no call in the tree does that today, so the branch costs nothing until it is
  needed.

### Scene HTTP requests — [bug]

- **Every response body on the mbedTLS request path was truncated, by one byte
  for every 255 received.** The read loop inside
  [`__ezlopi_http_req_via_mbedTLS`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L463)
  grew its buffer correctly and then filled it with two `snprintf` calls:

  ```c
  snprintf(resp_buf_dummy, len, "%s", tmp_buf);                        /* first chunk */
  snprintf(resp_buf_dummy + strlen(resp_buf_dummy), len, "%s", tmp_buf); /* later chunks */
  ```

  `snprintf`'s size argument counts the terminator it writes, so a limit of
  `len` stores `len - 1` bytes of payload. `len` here is the byte count
  `mbedtls_ssl_read` just returned, so each chunk lost its last byte. The read
  is issued with
  [`tmp_buf_size - 1`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L617),
  which is **255**, so the loss is one byte per 255 received — and a reply that
  arrives in a single read loses its final character, which for a JSON body is
  the closing brace.

  **Which paths this reaches, stated narrowly, because the first draft of this
  entry said "every HTTPS response this firmware received" and that is false.**
  There are two response paths in this file and only one of them is affected.
  The other —
  [`HTTP_EVENT_ON_DATA`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L845),
  used by `EZPI_core_http_get_request` and `EZPI_core_http_post_request` —
  accumulates with `memcpy(tmp_data, evt->data, evt->data_len)` into a chunk
  list and loses nothing. That is the path provisioning uses, and
  `__provision_update()` does parse its reply successfully. So "nothing in this
  firmware has ever parsed a response body" — which the first draft also said —
  is wrong twice over.

  The affected path has exactly **three** callers, and the reason the defect
  survived this long is that none of them could see it:

  | caller | what it does with the body |
  |---|---|
  | `sendHttpRequest` then-method | never reads it; frees it |
  | `hub.scenes.action.block.test` | reads only the status line |
  | `__issunsate_update_sunstate_tm` | never reads it, and leaks it |

  `block.test` is the only one that reads anything, and what it reads is
  `sscanf(response, "HTTP/1.1 %d %99s[^\n]", …)` — the status line, which sits
  at the front of the first chunk, where a loss at the *end* of each chunk never
  reaches it. So no existing caller was visibly broken. The defect was latent,
  not dormant: any caller that wanted the body could not have one, which is
  precisely the obstacle in front of the sunstate feature, whose whole purpose
  is to read sunrise and sunset times out of a JSON reply.

  Measured against the loop as it was, with only `mbedtls_ssl_read` replaced by
  a generator handing out a known body:

  | body | came back as | lost |
  |---|---|---|
  | 15 bytes (`{"status":"OK"}`) | 14 | 1 |
  | 255 bytes — one full read | 254 | 1 |
  | 256 bytes — two reads | 254 | 2 |
  | 1299 bytes — six reads | 1293 | 6 |

  **Nothing could have reported this.** It wrote strictly *less* than it had
  room for, so there was no overflow for AddressSanitizer to trap and no
  truncation for a compiler to warn about. It was not a crash, so no target
  build showed it. The trace that printed the body printed the damaged copy with
  nothing to mark where the byte had gone. The only instrument that can see a
  defect of this shape is a check that compares what came out against what went
  in, byte for byte.

  **Two further defects in the same twenty lines**, both found while writing
  that check rather than before:

  The append offset was `strlen(resp_buf_dummy)`, which is the accumulated
  length only while the body contains no NUL. Nothing requires an HTTP body to
  be text — any image, gzip stream or protobuf has NULs — and for such a body
  every chunk after the first was written *on top of* the earlier ones. The
  replacement carries the length as a parameter instead. The test measures the
  difference: for a 600-byte body with a NUL at offset 10, `strlen` reports
  **10**.

  When the buffer could not grow, the old loop dropped that chunk and kept
  reading. A reply the heap could not hold therefore came back as a shorter
  reply that looked complete — the same class of defect as the off-by-one,
  reached from the other direction. The accumulator now reports the failure, the
  loop stops, and the handover discards the partial body rather than passing it
  off as whole: `if (strlen(resp_buf_dummy) > 0)` became
  [`if ((resp_used > 0) && (false == resp_truncated))`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L662).

  **That last change costs `block.test` something, and the trade is deliberate
  rather than overlooked.** On an allocation failure part-way through a reply,
  the status line is already in the buffer, so the old behaviour — drop the
  chunk, carry on, hand back what there is — let `block.test` still report the
  server's real status code. It now gets `NULL` and reports **404** instead.
  Taken on its own that is worse. It is still the right default: the reason the
  old behaviour looked harmless is that the only caller reading the body reads
  the four bytes least likely to be affected, and preserving that convenience
  means every future caller receives bodies it cannot distinguish from complete
  ones. An allocation failure mid-reply is also not a condition where the status
  code is the useful signal. Handing back the partial body *and* a truncation
  flag would serve both, and is not done here because it means a new field on
  `s_ezlopi_core_http_mbedtls_t` for a case no caller currently distinguishes;
  recorded here so that the next caller to want it knows the trade was made
  knowingly.

  **The fix is a new function in a new translation unit**, and the second half
  of that matters. `ezlopi_core_http.c` cannot be compiled on a host — it needs
  **10** headers this repository does not ship, **9** of them under `mbedtls/`
  and `esp_tls/` — so a defect that only a host test can see could not be
  regressed while it lived there. The accumulator is pure buffer arithmetic with
  no ESP-IDF surface at all, so it went into
  [`ezlopi_core_http_buf.c`](ezlopi-core/ezlopi-core-http/ezlopi_core_http_buf.c),
  which includes `ezlopi_core_http.h` and nothing else of consequence.
  [`EZPI_core_http_dyna_relloc`](ezlopi-core/ezlopi-core-http/ezlopi_core_http_buf.c#L76)
  moved with it, unchanged: the accumulator grows through it, and a test that
  stubbed the growth would be testing the half that was never in doubt. The
  component's `CMakeLists.txt` uses `file(GLOB_RECURSE config_src "*.c")`, so
  the new file needed no build change on target, and its other caller in the
  scenes then-methods helper is unaffected — same component, same declaration in
  the same header.

  **[`tests/host/test_httpresp.c`](tests/host/test_httpresp.c) — 60 checks.** It
  compiles the real `ezlopi_core_http_buf.c` under
  `-fsanitize=address,undefined` and transcribes the firmware's read loop around
  it, keeping the buffer sizes, the 255-byte reads and the growth policy. It
  keeps the *old* loop too, as a second arm, so each case reports the size of
  the loss rather than only asserting its absence — the table above is the test's
  own output. Five assertions per body, including one in the direction of the
  bug (`the old loop did lose bytes on this body`), so a future edit that
  reintroduces the `snprintf` calls fails rather than passes.

  **Six mutations, all detected**, and the fourth earned its keep. Reverting the
  copy to `chunk_len - 1` fails 8 checks; restoring the `strlen` offset fails 2;
  growing to a tight fit instead of geometrically fails the realloc-count check;
  reporting a growth failure as success fails 3; removing the terminator fails 2.

  The fourth mutation restored the overflow guard I had written first, and it
  did not merely fail — it crashed with an AddressSanitizer SEGV on a 4GB write,
  inside `memcpy`, from the one input that distinguishes the two guards. My
  first version tested the 32-bit sum with `need < chunk_len`, which is the
  standard idiom for catching a wrap and is wrong at exactly one point: with
  `*used == UINT32_MAX` the sum wraps to `chunk_len` itself, the comparison is
  false, and the function proceeds to `memcpy` at an offset of four gigabytes.
  No reply this firmware can receive comes near that — the heap is a few hundred
  kilobytes — so the hole was unreachable in practice. It is rewritten rather
  than documented, in 64-bit arithmetic bounded against `INT_MAX`, because
  "nearly right, and the shortfall is unreachable" is precisely the shape of the
  defect this whole function exists to remove. `INT_MAX` and not `UINT32_MAX`
  because `dyna_relloc` takes its size as an `int`: a larger request would be
  narrowed to a negative and refused as though the heap had failed.

  **Growth is geometric, and the test measures it rather than trusting it.** The
  caller is in a read loop, so a tight-fitting `realloc` per chunk is a `realloc`
  per 255 bytes. For a 1300-byte body in six reads the policy costs **3**
  reallocations where a tight fit costs **5**; the assertion is set at ≤ 4, which
  discriminates between the two without breaking if the growth factor is
  retuned.

  **One stub directory was added for one macro.**
  [`tests/host/stubs-httpresp/EZLOPI_USER_CONFIG.h`](tests/host/stubs-httpresp/EZLOPI_USER_CONFIG.h)
  shadows the shared stub on the include path and changes only `ezlopi_realloc`,
  routing it through a function the test can make return NULL. The contract on a
  failed growth is that nothing is appended and none of `*buf`, `*cap` or
  `*used` moves, and the only way to check that is to make growth fail.
  Requesting a size the real `realloc` would refuse makes the test depend on the
  host's overcommit settings and aborts under ASan rather than returning NULL;
  `-Wl,--wrap=realloc` hides the mechanism in the link line and collides with
  ASan's own interceptor for that symbol; defining the macro on the command line
  trips `-Werror` against the shared header's `#define`. Three other macros are
  byte-for-byte the shared stub's, so ASan still watches every block the subject
  touches.

  No `nm` guard on this test's subject object, which is a deliberate difference
  from `test_httpurl` and `test_ble_buffer` rather than an omission:
  `ezlopi_core_http_buf.c` is not wrapped in any `#ifdef`, so there is no macro
  whose absence could silently empty the object.

  **What is checked by reading and not by running**, since the loop itself lives
  in the file that cannot be compiled: that it passes `ret` as the chunk length
  (it does — `len = ret` immediately precedes the call), that it stops on a
  failed append instead of continuing, and that the handover reads `resp_used`
  rather than measuring with `strlen`. The transcription in the test is only as
  good as that reading.

- **A request with no host is no longer assembled, logged and handed to the
  resolver.**
  [`EZPI_core_http_mbedtls_req()`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L164)
  is the one entry point for every outbound request a scene makes, and it used
  to check its argument and nothing in it:

  ```c
  if (config)                                   /* was */
  if (config && config->web_server)             /* is */
  ```

  `web_server` is the host, and the
  [`__ezlopi_http_req_via_mbedTLS` call](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L241)
  passes it on unexamined, from where it reaches `mbedtls_net_connect()` and
  `getaddrinfo()`.

  **Two of the three callers could arrive with it NULL, by two different
  routes.** The
  [`sendHttpRequest` then-method](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods.c#L494)
  and
  [`hub.scenes.action.block.test`](ezlopi-cloud/ezlopi-cloud-scenes/ezlopi_cloud_scenes.c#L592)
  both `memset` the struct to zero and then let
  [`EZPI_parse_http_url()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L136)
  fill it. That function allocates `web_server` only inside its
  `if (succ_parsing)` branch, so a `url` that matches none of the nine arms —
  or an action carrying no `url` field at all — leaves the field as the zero it
  was memset to. The second route is subtler and survives a *successful* parse:
  [`EZPI_core_http_mem_malloc()`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L117)
  returns the size it wanted even when `ezlopi_malloc` returned nothing, so an
  allocation failure leaves `web_server_maxlen` positive beside a NULL
  `web_server`, and the
  [`web_port = port`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L273)
  on the next line still runs with the real port. The third caller,
  [`__issunsate_update_sunstate_tm()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L1893),
  was never at risk: its `web_server` is a `char` array initialised from a
  literal.

  **What this actually cost, stated precisely, because it is less than the
  first reading suggests and more than nothing.** lwIP resolves a NULL node
  name with no `AI_PASSIVE` to the *loopback* address rather than returning an
  error — `components/lwip/lwip/src/api/netdb.c` in the pinned IDF, under the
  comment *"service location specified, use loopback address"* — and
  `mbedtls_net_connect()` zeroes its hints, so it never sets that flag. The
  connect does not then succeed: the only TCP listener this firmware starts is
  the websocket server, [`httpd_start()`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L810)
  on [`EZPI_SERV_WS_SERVER_PORT`](ezlopi-user-config/EZLOPI_USER_CONFIG.h#L148)
  = 17001, and the port reached here is 0 on the parse-failure route or the
  URL's own port on the allocation-failure one. The damage is upstream of the
  connect. The whole request is built first, and the
  [`TRACE_I`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L235) below it
  printed the assembled buffer in full — including any `Authorization` header
  the action put in `header`, and the `?username=…&password=…` the GET branch
  writes into the same buffer. So a request that could never be delivered was
  assembled and printed anyway, and the resolver was quietly aimed at the
  loopback interface instead of reporting a missing host. That dump is now
  behind `CONFIG_EZPI_HTTP_LOG_PAYLOADS` — see *Credentials* above — so only
  the length is printed by default; this entry's fix is the one that stops the
  request being built at all.

- **The same parse failure also emitted a literal `Host: \r\n`.** In
  `EZPI_parse_http_url()` the header creation and the
  [`Host` append](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L286)
  sat *after* the `if (succ_parsing) ... else` rather than inside the success
  arm, so they ran on both paths — and on the failing path `host` is still the
  `char host[100] = {0}` it was declared as. Both are now inside the success
  branch. Nothing downstream lost its header as a result: the other four
  functions that append to this buffer —
  [`EZPI_parse_http_content_type()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L300),
  [`EZPI_parse_http_content()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L326),
  [`EZPI_parse_http_headers()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L356)
  and
  [`EZPI_parse_http_skipsecurity()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L383)
  — open by calling
  [`__ezlopi_core_scenes_then_create_fresh_header()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L761)
  themselves, and that function allocates only when `header` is still NULL and
  otherwise returns the existing length, so it is idempotent and the move
  cannot starve a later append.

- **Measured rather than argued.** The nine-arm ladder was copied verbatim into
  a host program alongside both versions of the header append. On
  `http://example.com/x` and on `ftp://example.com:21/x` — which reaches the
  fallback arm — both versions emit `Host: example.com`. On `not a url at all`
  and on an empty `url`, the old shape emits an **8-byte** header line whose
  value is empty, `Host: \r\n`, and the new shape appends nothing. The same
  program then calls `getaddrinfo(NULL, "0", &hints, …)` with the hints
  `mbedtls_net_connect()` sets, and it returns `rc=0` with two results, `::1`
  and `127.0.0.1`. That last part corroborates the reading of lwIP's `netdb.c`
  rather than executing it — the resolver in the harness is glibc's — and the
  lwIP behaviour itself was established by reading the pinned IDF's source.

  **Why [bug] and not [sec],** on the same reasoning as the `sscanf` entry
  above: both transports that can deliver a scene action are authenticated, the
  local websocket server admitting only `hub.offline.login.ui` until the
  connection is logged in. What keeps it from being merely cosmetic is that
  `hub.scenes.action.block.test` needs no scene to be stored, so the parse runs
  on a block taken straight out of the request — and that it was the request
  *logging*, not the connect, that did the work.

  **The guard is at the chokepoint rather than at the call sites,** which was a
  deliberate change of plan: the first version of this fix put an
  `if (tmp_http_data->web_server)` at each of the two exposed callers, and that
  was replaced once the third caller was read and the allocation-failure route
  found. One test inside `EZPI_core_http_mbedtls_req()` covers all three
  callers and any added later, which is the property that matters here — the
  defect's whole shape was a call site that did not know what the callee
  required. Nothing was needed at the cloud caller to keep its reply correct:
  `response` stays NULL and the
  [`httpAnswerCode` 404](ezlopi-cloud/ezlopi-cloud-scenes/ezlopi_cloud_scenes.c#L609)
  branch already answers for exactly that case.

  **Verified on four targets, baseline against fixed.** `esp32`, `esp32s2`,
  `esp32s3` and `esp32c3` all build, and the disassembly of
  `EZPI_core_http_mbedtls_req` grows by exactly **2 instructions** on every one
  of them — a load and a branch. That is the evidence that matters here: this
  fix adds no string literal for the usual rodata grep to find, and a test
  folded away by the optimiser would leave the function exactly as long as the
  baseline's. `esp32c3` is RISC-V, so four targets agreeing is two instruction
  sets agreeing. Image size moves by −16, 0, 0 and −16 bytes — the guard's two
  instructions are offset by the header-append move, which deletes a
  duplicated path — and warnings in the two touched files are 0 in both
  builds. `EZPI_parse_http_url` is in the symbol
  table of all four, which is not decoration — the scenes helper is wrapped end
  to end in `#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS` and would otherwise
  compile to nothing and still link. `ezlopi_core_http.c` carries no such
  wrapper, so the chokepoint half ships whatever that option is set to.

  **Two things deliberately not done.** There is no host test for *this* guard,
  and the reason was measured rather than assumed: compiling
  `ezlopi_core_http.c` under `tests/host/` needs **10** headers that do not
  exist in this repo, **9** of them `mbedtls/` and `esp_tls/`, so the test would
  stand in for the TLS stack and stop testing the thing it names. (That count
  was recorded as **12** when this entry was written and is corrected here; it
  was re-measured, not re-remembered, while building the test described at the
  end of this section.) The scenes helper needs **7** — which independently
  confirms the count already recorded in
  [`check_scanf_field_width.py`](tests/ci/check_scanf_field_width.py)'s
  docstring, and which turned out to be *cheap* enough to pay: the last entry in
  this section is a host test that compiles that file for real. Whether a
  translation unit is hostable is two measurements, not one — how many absent
  headers the preprocessor wants, and how many undefined symbols the link
  leaves — and only the first had been taken here. There is also no new CI
  check: the invariant is one condition in one function, and unlike a missing
  `scanf` width there is no syntactic class for a check to cover, so a check
  here would guard a single line and claim to be more.

  A third thing was deliberately not done at the time and has since been done
  as its own change: `target_page`, the field this guard does *not* cover,
  which reaches a `"%s"` in
  [`__ezlopi_http_generate_request()`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L701).
  It was recorded under *Known gaps* rather than folded in here. The next entry
  is that fix, and the reason it needed its own change is that reading it
  properly found a defect of a different shape than the gap entry described.

- **Every sunrise/sunset scene condition was sending `GET /(null) HTTP/1.1`,
  and it took no allocation failure to do it.** The *Known gaps* entry that
  held this open described an allocation-failure edge case reaching one `"%s"`.
  Both halves of that were wrong, and the correction is the interesting part.

  There are **four** `"%s"` reads of `target_page`, not one — the
  [GET](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L748),
  [POST](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L754),
  [PUT](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L759) and
  [DELETE](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L764) arms of
  `__ezlopi_http_generate_request()`. And the field is NULL on a path with no
  malloc on it at all.
  [`__issunsate_update_sunstate_tm()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L1893)
  — the third caller, the one the previous entry establishes was never at risk
  for `web_server` — builds its config with a
  [designated initializer](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L1938)
  that named **10** of the struct's **20** members. `target_page` was one of the
  10 it omitted, so C zero-initialized it. (Read that line today and it names
  **11**, because `.target_page` is what this entry adds — the count was
  re-derived against the current file and the tense corrected rather than left
  reading as a description of code that no longer exists. The struct's 20 was
  re-counted by hand: a grep says 19 because `TaskHandle_t
  mbedtls_task_handle;` does not match the pattern.) Its `method` is
  `HTTP_METHOD_GET` and
  it sets no `username` or `password`, so it takes the GET arm that reads
  `target_page`. That is not an edge case: it is every evaluation of every
  `isSunState` condition in the device, deterministically, on first boot with
  nothing misconfigured.

  **Why it never surfaced as a crash, measured rather than inherited.** The old
  gap entry asserted "newlib prints `(null)` rather than faulting", which is
  true of glibc and was worth not taking on faith for the libc these images
  actually link. The toolchain ships libc as a binary, so the binary was asked:
  the `(null)` literal appears **6** times in each target's own `libc.a`
  (checked for `esp32s2` and `esp32s3`, including the no-RTTI variants), and
  `CONFIG_NEWLIB_NANO_FORMAT` is unset in all four `sdkconfig.defaults.*` — so
  the full formatter is linked, and it is the one that carries the NULL case.
  The claim holds for this firmware, now on its own evidence.

  **The fix is two changes, and the upstream one is the one that matters.**
  [`EZPI_core_http_mem_malloc()`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L117)
  now [returns 0](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L149) when
  `ezlopi_malloc` returns nothing, instead of returning the capacity it wanted.
  Every one of its call sites assigns the result straight into a `*_maxlen`
  field, so the old contract advertised a capacity beside a pointer that the
  `FREE_IF_NOT_NULL` two lines up had just set to NULL. 0 was already this
  function's answer for a NULL `src_ptr` — the other way to end up with no
  buffer — so this makes the two agree rather than inventing a convention.

  The blast radius is **9** call sites, **7** of them compiled, all nine in
  `ezlopi_core_scenes_then_methods_helper_func.c`; the two that do not compile
  are inside the disabled alternate host parser at its `#if 0`, and all nine
  sit inside the file's end-to-end `#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS`.
  The gap entry this closes said **10** and **8**. The likely origin of the
  discrepancy is that it counted the definition in `ezlopi_core_http.c` as a
  call site, which is the kind of off-by-one that a re-derived count catches
  and a carried-forward one does not. Recorded rather than quietly amended
  because the gap entry was written by this same audit: a number stated once
  and then cited is not evidence, and this one survived until something was
  built on it.

  That immediately activates a guard that was already written and could not
  previously fail:
  [`if (tmp_http_data->url_maxlen > 0)`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L143)
  in `EZPI_parse_http_url()`. A failed `url` allocation now skips the nine-arm
  parse instead of running it and leaving the struct half-filled. Worth being
  precise about what the old return value cost here, because it is sharper than
  "wrong number": for an empty `src_ptr` the function returns **5** on success
  — `strlen("") + 5` — and it used to return **5** on failure too, so the two
  outcomes were not merely confusable, they were *identical*, and no caller
  could have told them apart however carefully it was written.

  Second, the four request-line reads now go through
  [one guarded local](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L734)
  rather than being tested four times. The empty string is the right substitute
  and not merely a safe one: `GET / HTTP/1.1` is the request for the root path,
  and a legitimately empty page *already* arrives here as `""` — a URL with no
  path takes the seventh arm of the ladder and leaves `page` as the `{0}` it
  was declared as. So NULL and `""` now produce the same well-formed request
  line, which is what the caller meant in both cases. This is why guarding
  `target_page` at the call site would have been wrong, as the gap entry
  argued: the test would have had to distinguish "not allocated" from
  "allocated empty", and after the upstream change it does not have to.

  **Behaviour-preserving for everything that was already working,** shown on
  the host rather than reasoned about. The `snprintf` form was lifted verbatim
  and run under `-fsanitize=address,undefined` on four inputs. A NULL page goes
  from `GET /(null) HTTP/1.1` to `GET / HTTP/1.1`. An empty page gives
  `GET / HTTP/1.1` before and after. A real page — `json?lat=…&lng=…` — is
  byte-identical before and after. Only the NULL case moves.

  **Verified on four targets, baseline against fixed.** `esp32`, `esp32s2`,
  `esp32s3` and `esp32c3` all build, with `ezlopi_core_http.c` carrying **0**
  warnings in every fixed build and whole-build warning counts unchanged from
  baseline — 181, 176, 181 and 184 respectively, identical on both sides. The
  non-vacuity probe had to be chosen differently from the two fixes above it:
  those removed format strings, so a rodata grep answered directly, whereas
  this one leaves all four format strings untouched and a rodata grep at those
  sites would see nothing either way. What it *does* add is one new literal,
  the `TRACE_E` on the new failure branch, and that is the probe —
  `allocation of %d bytes failed` is absent from all four baseline images and
  present exactly once in all four fixed ones. Image size moves by +80, +96,
  +96 and +96 bytes. The ternary is checked a third way short of running it:
  the new local is named `target_page`, the same as the struct member it reads,
  which is legal but worth having the compiler confirm — and `-Wshadow` is a
  build *error* in this project's components, so the builds succeeding is that
  confirmation. The host suite was run on the fixed tree as the regression net
  for the scenes sources it can compile: **77** checks, **0** failures.

  **Two things deliberately not done, and one harness bug worth recording.**
  There is no committed host test, for the reason the previous entry measured
  and this one inherits: `__ezlopi_http_generate_request` is `static` in a file
  that needs **10** absent headers, 9 of them `mbedtls/` and `esp_tls/`, so a
  test would exercise a *copy* of the ladder — the drift
  `tests/host/Makefile`'s own header argues against. That remains true of
  `ezlopi_core_http.c`; it did *not* remain true of the scenes helper, which the
  last entry in this section now tests for real. And the sunstate request is still functionally wrong: its path
  should be the `json?lat=…` it built into `tmp_url`, not the root. That is a
  different defect in a function with several, and it is recorded under *Known
  gaps* rather than guessed at here. The harness bug: two of the sweep's
  source-level sync lines reported **0** where the truth was **4**, because
  their patterns were written `esp32..r..n` — six characters where the source
  has the four `\r\n` — and this was caught by the reported number
  contradicting an independent `grep -F`, not by the sweep. They are now `-F`
  patterns. Both affected lines were *descriptive*, not gating, so no result
  above rested on them; the sweep is kept rather than discarded for the same
  reason.

- **A URL with credentials in it no longer puts them in `web_server`, in the
  `Host:` header or on the console, and the port it asks for is no longer
  swallowed.** This closes a *Known gaps* entry, and the entry's own
  recommendation was followed rather than its first draft: the fix is not only a
  logging switch, because the root cause was that `host` contained credentials
  at all.

  `http://user:pass@example.com:8080/x` is a legal URL. It reached the **fifth**
  arm of the then-nine-arm ladder, `http://%99[^/]/%199[^\n]`, whose conversion
  stops only at a `/` — so `host` came back as the whole
  `user:pass@example.com:8080`, and `port` stayed at the **80** it was
  initialised to. That string was then the `web_server`, the `Host:` header
  value, and the subject of a `TRACE_S`. Arms **6**, **7** and **8** had the
  same shape and the same outcome.

  **The split is one new leaf function,**
  [`__ezlopi_core_scenes_then_split_authority()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L1017),
  [called once](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L202).
  It works in place and returns a pointer into the caller's `authority[256]`:
  nothing is moved, which is what keeps the username and password pointers —
  which point at the front of that same buffer — valid while they are copied
  out. Three decisions in it are not arbitrary:

  - the authority splits at the **last** `@`
    ([`strrchr`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L1028)),
    because RFC 3986 §3.2.1 allows `@` inside a userinfo, so `a:b@c@example.com`
    has the userinfo `a:b@c`;
  - the userinfo splits at the **first** `:`
    ([`strchr`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L1037)),
    for the mirror-image reason — a password may contain one, a username may
    not;
  - the port is taken from the last `:` only when the tail is **entirely digits
    denoting 1..65535**
    ([the acceptance test](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L1095)).
    The digits rule is what leaves `http://[::1]/x` alone, whose last colon is
    inside the address and whose tail is `1]`.

  This function is the part of the entry that outlived the ladder. The ladder
  itself is gone — see *The URL ladder is gone* below — and removing it
  **widened** this function's job rather than narrowing it: it is now the only
  place in the file that reads a port, reached by every URL instead of by the
  four arms that scanned the authority whole. Two clauses described here have
  changed as a direct result, and both are recorded where they happened rather
  than quietly edited out of this entry:

  - the acceptance test above also required that the host contain no `?` or
    `#`, which existed because arms 7 and 8 were `%99[^\n]` catch-alls that
    handed a query string over whole, so `http://example.com?q=a:5` would
    otherwise have grown a port 5. The replacement ends the authority at `?`
    and `#` as well as `/`, so neither character can reach this function at
    all. The clause was **removed as provably dead** after a mutation showed
    deleting it changed no observable behaviour.
  - the paragraph that used to follow — arms 1-4 and 9 convert with `%99[^:]`,
    so for them the call is a no-op — described an arrangement that no longer
    exists. Every URL now routes through this split, which is most of what the
    replacement is.

  **An explicit `credential` field still wins, in either field order.** The
  action's fields are walked in whatever order the request listed them and
  dispatched by name, so `credential` may be parsed before `url` or after it.
  The URL's userinfo fills `username` and `password`
  [only while they are still NULL](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L231),
  and
  [`EZPI_parse_http_creds()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L402)
  overwrites unconditionally as it always has, which makes the two orders agree.

  **Two of the function's four traces are now behind
  `CONFIG_EZPI_HTTP_LOG_PAYLOADS`,** the switch
  [`ezlopi_core_http.c:234`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L234)
  already uses for the assembled request and the server's reply. `*source` is
  the URL verbatim and is the only trace here that can still print a password.
  `page` is the second one, and that is a correction to the gap entry's
  reasoning rather than a copy of it: arms 1, 2, 5 and 6 put everything after
  the host into `page`, query string and all, and `…/v1/thing?api_key=…` is an
  ordinary way to authenticate an HTTP request — so withholding the *generated*
  `?username=…&password=…` at `ezlopi_core_http.c:235` while printing the
  action's own query would have left the same door open. `host` and `port` stay
  unconditional: they are what distinguishes a parse that worked from one that
  aimed somewhere unexpected, and after the split neither can carry userinfo.
  The withheld page's
  [length is still printed](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L266).

  So the gap entry's "three live `%s` traces, gate all three" became **two
  gated and one made safe at the source**, which is the better arrangement: a
  switch nobody turns on protects nothing, and `host` is the field a reader
  actually needs.

  **This is the first fix in this file with a host test, and the measurement
  that said one was impossible was wrong.** Two earlier entries in this section
  record that there is no host test because the file needs absent headers; that
  claim was re-measured rather than inherited, and the subject translation unit
  does compile on a host. It needs **7** stub headers — independently confirming
  the number recorded in
  [`check_scanf_field_width.py`](tests/ci/check_scanf_field_width.py)'s
  docstring — and produces a **204128**-byte object with **0** errors and
  **10** exported functions. Of its **63** undefined symbols, 21 are the
  sanitizer runtime and 8 are libc, leaving **22**: three are glibc's own
  renames of `sscanf`, `strtoul` and `memset`, six are string constants, and
  **13** are firmware functions.

  **That docstring is edited, not just contradicted.** Its sentence read "the
  host test suite cannot compile this translation unit without stubbing seven
  IDF headers and four enum families" — the premise is what made this file look
  untestable in two earlier entries in this section, so leaving it standing
  would keep inviting the same wrong conclusion. The seven was right. The rest
  was not, in two ways. The suite now *does* compile the file, so "cannot
  compile" is simply dead. And what the seven headers supply beyond merely
  existing is **six type names**, of which **five** are enums and one is not:
  `esp_http_client_method_t` and `HttpStatus_Code` in
  [`stubs-httpurl/esp_http_client.h`](tests/host/stubs-httpurl/esp_http_client.h),
  `spi_device_handle_t` — a `void *` — in
  [`stubs-httpurl/driver/spi_master.h`](tests/host/stubs-httpurl/driver/spi_master.h),
  and `gpio_mode_t`, `gpio_pull_mode_t` and `gpio_int_type_t`, which are **not
  in that directory at all**: they went into the suite's pre-existing
  [`stubs/driver/gpio.h`](tests/host/stubs/driver/gpio.h) beside the `gpio_num_t`
  already there, because a second GPIO stub is a copy that can drift. The other
  **five** of the seven headers are header guards and nothing else — they exist
  so an `#include` resolves.

  What the rewritten paragraph keeps is the argument the docstring was actually
  making, which the correction does not touch: a host test covers the inputs
  somebody wrote down. `test_httpurl` drives nine arms of one ladder in one
  file; a missing width in a different `sscanf`, in a file with no host test,
  is still invisible to it. That is why the static check earns its place — not
  because the file could not be compiled.

  [`tests/host/test_httpurl.c`](tests/host/test_httpurl.c) therefore compiles
  the **real** translation unit under `-fsanitize=address,undefined`, not a
  transcription of the ladder — the drift that `tests/host/Makefile`'s own
  header argues against at length.
  [`tests/host/stub_httpurl.c`](tests/host/stub_httpurl.c) supplies the 13, in
  two kinds: **10** are unreachable from the subject and `abort()` with a
  message naming themselves rather than returning a plausible value, so a future
  test that wanders into the expression engine stops instead of passing on a
  stub's invention. The other **2** are on the hot path and are
  reimplementations carrying the line ranges they came from —
  `EZPI_core_http_mem_malloc` (`ezlopi_core_http.c:117-158`) and
  `EZPI_core_http_calc_empty_bufsize` (`:106-115`).

  There were **3** of those until the response-accumulator work below.
  `EZPI_core_http_dyna_relloc` was the third, and it was a copy only because the
  single definition of it sat in a file no host can compile. That stopped being
  true when the accumulator was moved into
  [`ezlopi_core_http_buf.c`](ezlopi-core/ezlopi-core-http/ezlopi_core_http_buf.c)
  and took `dyna_relloc` with it, so the copy is deleted and `tests/host/Makefile`
  links the real function into this test. The `Host:` header these 153
  assertions check is now grown by the same code the device runs.

  That swap also answered a question the copy left open. The assertions were
  written against the copy, so if the copy had drifted from the original they
  would have been asserting the copy's behaviour — and replacing the copy with
  the original is the one experiment that can tell. All **153** still pass
  unchanged, and `nm` confirms the symbol now comes from the firmware object and
  not from `stub_httpurl.o`. That is the only evidence available that the copy
  had been faithful, and it is worth more than the eye-diff the line ranges were
  there to support. Linking the real `ezlopi_core_http.c` instead was
  measured and rejected: it needs **10** absent headers, nine of them `mbedtls/`
  and `esp_tls/`, and leaves **44** further diagnostics, most of them designated
  initializers against `esp_http_client_config_t` — a stub reproducing the IDF's
  HTTP-client config layout field for field is a far worse copy than two leaf
  functions. The six string constants are **not** stubbed: the real
  `ezlopi_cloud_keywords.c` and `ezlopi_cloud_value_type_str.c` are linked,
  because both are self-contained and a stubbed string constant is the kind of
  copy that goes wrong quietly.

  Because the two remaining copies sit on the path the test exercises, the test asserts
  on **two** independent surfaces: the struct the subject fills, and a recorded
  log of every `(destination, string)` pair the subject handed the allocator.
  The second does not pass through the copies at all, so a copy that drifts
  cannot make a parsing assertion pass wrongly.

  **Non-vacuity, shown before the fix was written.** Against the unfixed
  subject, all **nine** ordinary URL shapes pass and the first credentialed case
  fails with `web_server 'example.com' (got 'user:pass@example.com')` — the
  defect demonstrated by running the firmware's own code, which the gap entry's
  copied-ladder measurement could not do. After the fix the suite reports
  **153** checks and the whole of `tests/host` reports **5027** checks across
  **24** binaries, **0** failures. (Both figures were **4967** across **23**
  when this entry was written; `test_httpresp` and its 60 checks are the
  difference, and the 153 are unchanged by having the real `dyna_relloc`
  linked in place of the copy.)

  **Then the assertions written *with* the fix were checked the same way,
  because passing tests had not yet been shown capable of failing.** The suite
  exits at its first failure, so everything after the credentialed table had
  never run against a wrong subject. Six mutations of the new helper were built
  and run. Two were caught at once. **Three survived** — splitting the userinfo
  at the last colon instead of the first, splitting the authority at the first
  `@` instead of the last, and dropping the `NULL ==` test that gives an
  explicit credential precedence — and each survivor bought a new assertion: two
  table rows (`user:pa:ss@…` and `user:p@ss@…`) and
  `test_explicit_credential_beats_the_url_userinfo`, which drives the
  `credential` and `url` fields together in both orders. All six are now caught.

  **A mutation also found dead code in the fix itself.** The port split
  originally carried a third guard rejecting a tail containing `]`. Deleting it
  changed no observable behaviour, for a reason that is obvious once stated:
  `]` is not a digit, so no string the all-digits test accepts can contain one.
  It is removed rather than kept as an unreachable condition with a comment.

  **Verified on four targets, baseline against fixed,** and the probe is rodata
  rather than instruction counts because this change removes two format strings
  and adds one. `esp32`, `esp32s2`, `esp32s3` and `esp32c3` all build with **0**
  warnings in the subject on both sides, and on every one of them `*source =
  '%s'` goes **1 → 0**, `page = '%s'` goes **1 → 0**, and the withheld-page
  message goes **0 → 1**. The fourth count is the one that makes the first three
  mean anything: `host = '%s'` stays **1 → 1**. Without it, a translation unit
  that had vanished entirely — `CONFIG_EZPI_SERV_ENABLE_MESHBOTS` off, the trap
  the host suite's `nm` guard exists for — would have produced the same two
  zeros and looked like a success. Image size moves by **+256, +256, +240 and
  +272** bytes.

  **A fifth build turns the switch on,** because the four above cannot tell a
  working gate from a misspelled macro. A typo in the `#ifdef` makes it
  permanently false, compiles the `#else`, and produces *exactly* the four
  counts reported above — the `host` control does not help, since it rules out
  the translation unit vanishing and nothing else. With
  `CONFIG_EZPI_HTTP_LOG_PAYLOADS=y` on esp32 both dumps come back (**1** and
  **1**), the withheld message goes (**0**), and `host` is unmoved (**1**). The
  three esp32 images are **1684080** baseline, **1684336** fixed with the
  switch at its default, and **1684176** fixed with it on: switch-on sits
  **+96** above baseline, which is the new helper's code, and **−160** below
  switch-off, which is the withheld message's string and its `strlen` minus the
  two short format strings they replaced. The switch moves the image in both
  directions, so the name in the `.c` is the name in `Kconfig.projbuild`.

- **An isSunState scene fired at 05:48 or 17:48 local time, anywhere on
  earth.** The function that was supposed to fetch today's sunrise or sunset,
  [`__issunsate_update_sunstate_tm`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L1893),
  was 51 lines long and had five independent defects in them. What makes it
  worth writing up as one entry rather than five is that each defect made the
  next one undetectable.

  It read the stored coordinates, traced them, and then ignored them: both
  branches of the `if (lat_long_vals)` test wrote the *same* hardcoded
  Kathmandu URL, so the only effect of having coordinates was one extra trace
  line. It wrote that URL into the config's `url` field — which an
  uncredentialed GET never reads, because
  [`__ezlopi_http_generate_request`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L701)
  builds the request line from
  [`target_page`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L748) — so
  every request actually went out as `GET /`. It then discarded whatever came
  back without freeing it.
  Finally it assigned `05:48:42`, or twelve hours later for a sunset, and
  tested *those literals* for being all-zero to decide whether the lookup had
  failed. They cannot be all-zero, so the failure branch below was unreachable
  and `curr_tm_day` was recorded as fetched every time.

  **Read as a chain, the behaviour is consistent and silent.** The wrong URL
  did not matter because the URL was not sent. The unsent request did not
  matter because the reply was discarded. The discarded reply did not matter
  because the time came from literals. And the literals did not matter to the
  error path because the error path could not run. A scene using this trigger
  therefore worked — it fired, once a day, on time to the second, at a time
  that was correct for one street in Kathmandu and for nowhere else, and leaked
  one HTTPS reply per day while doing it. There is no log line, no assertion
  and no build warning anywhere in that chain. The one instrument that would
  have caught it is a test that asks what time came back for a known pair of
  coordinates, and that could not be written, because every part of the
  calculation was inside a function that needs a radio.

  **So the three parts that do not need a radio were moved out.** The query
  built from stored coordinates, the shape of the reply, and the clock
  arithmetic are now
  [`EZPI_core_suntime_build_query`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L157),
  [`EZPI_core_suntime_from_reply`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L221)
  and
  [`EZPI_core_suntime_parse_clock`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L48)
  in what is now a **490**-line translation unit. The boundary was drawn where
  it is for
  one reason: everything on this side of it can be compiled and run on a
  development machine. Its only dependencies beyond `stdio.h` and `string.h`
  are `cjext.h`, the trace macros and the cloud keyword table, and the host
  suite builds the real file against the real `cjext.c` and the real
  `ezlopi_cloud_keywords.c` — nothing in the calculation is standing in for
  anything.
  [`tests/host/test_suntime.c`](tests/host/test_suntime.c) compiles the real
  file and makes **206** assertions against it, **109** of them for the three
  functions in this entry and the rest for the offset arithmetic described under
  *Scene triggers*. They include every one of the 1440
  minutes in a day put through both the 12-hour and the 24-hour rendering and
  compared against each other — **0** mismatches either way. The rule carries
  the same `nm` guard as the rest of the suite, so if
  `CONFIG_EZPI_SERV_ENABLE_MESHBOTS` ever stops reaching the file and it
  compiles to an empty object, the build fails rather than reporting 206 checks
  against nothing.

  Ten mutations of the subject were run and all **10** were detected: `+12` on
  every PM reading with no 12 special-case, midnight and noon swapped, the
  `status` check dropped, coordinates rounded to two decimals, a truncated
  query reported as success, the coordinate range check removed, three-digit
  clock fields accepted, the sunrise/sunset mode ignored, the 12-hour range
  check removed, and the outputs written before validation. The sweep also
  carries a control row — an appended comment, which applies but changes no
  behaviour — and it sits at **0** failures. Without it, "all ten detected" and
  "the harness fails on everything" produce identical output.

  One of the 109 checks is there to catch a regression rather than a defect: it
  asserts that what comes back for the fixture is *not* `05:48:42`. Reinstating
  the old hardcoded time would therefore fail the suite rather than pass it.

  **The repair needed a third piece that nobody had written: HTTP body
  framing.** `EZPI_core_http_mbedtls_req` hands back everything the socket
  produced — status line, headers, blank line, then the body in whatever
  framing the server chose. `cJSON_Parse` on that fails at the first character,
  so the parser above could never have been pointed at `config->response`
  directly. That was not visible while the reply was being thrown away; it
  became the blocking problem the moment anything wanted to read it.
  [`EZPI_core_http_body_in_place`](ezlopi-core/ezlopi-core-http/ezlopi_core_http_buf.c#L268)
  finds the body, honours `Content-Length`, and reassembles a
  `Transfer-Encoding: chunked` body in place via
  [`__dechunk_in_place`](ezlopi-core/ezlopi-core-http/ezlopi_core_http_buf.c#L583).
  Chunked is not an exotic case to skip: it is what an HTTP/1.1 server sends
  whenever it does not know the length in advance, and a chunked body begins
  with a hex length on its own line. In place is safe because the reassembled
  body is strictly shorter than the chunked form — every chunk costs a header
  and a trailing CRLF — so the write cursor can never overtake the read cursor.

  A reply that cannot be accounted for *in full* is a failure rather than a
  best-effort body: no blank line, a status other than 200, a `Content-Length`
  longer than what arrived, a chunk sequence that ends without its terminating
  zero. The reason is the same one that governs the clock parser above it — a
  body missing its middle parses exactly as well as a complete one, and is
  worse than no body at all.

  [`tests/host/test_httpresp.c`](tests/host/test_httpresp.c) grew from **60**
  checks to **176** for this, and the new ones were measured the same way:
  twelve mutations of the body locator plus a control row, with the control at
  **0** failures and **11** of the twelve detected. The twelfth is an
  *equivalent* mutant and is recorded as one rather than chased — removing the
  step over the status line changes no observable behaviour, because the header
  name match is anchored at a line start and a status line this function
  accepts always begins `HTTP/1.`, so the status line cannot match a header
  name whether it is skipped or not. It is kept in the mutation list so the
  claim is re-measured rather than remembered, and the subject carries the same
  note at the skip itself. Two of the eleven are worth naming because they are
  the ones a reader would assume were covered and were not: dropping the `:`
  anchor after a header name makes `Transfer-Encoding-Hint: chunked` put the
  function into de-chunking a body that was never chunked, and believing a
  `Content-Length` longer than what arrived is caught by an AddressSanitizer
  heap-overflow rather than by an assertion, because the terminator then lands
  past the allocation.

  **This caller asks for `Connection: close`,** which is not tidiness. There is
  no read timeout configured anywhere on the mbedTLS socket in this tree —
  `mbedtls_ssl_conf_read_timeout` is never called — so on a keep-alive
  connection the read loop blocks forever once the body has arrived, with the
  scene-evaluation task inside it. That was harmless while the reply was being
  discarded and would have become a hang the moment this change started waiting
  for the body.

  The rewritten function is **117** lines. It clears `curr_tm_day` first and
  sets it only once a time has actually been parsed, so every exit that does
  not produce one leaves the caller's "no data for today" signal set — which
  makes the error path reachable for the first time. It frees the reply. It has
  no hardcoded coordinates, no hardcoded time, and traces a usable message
  naming `hub.coordinates.set` when no coordinates are stored. **What it did
  not have, at the time this paragraph was written, was a backoff:**
  `ISSUNSTATE_get_suntime` re-enters it whenever `curr_tm_day` differs from
  today, so a hub that could not reach the service asked again on every
  condition evaluation — right for a transient failure, wrong for a persistent
  one, and recorded under *Known gaps* rather than fixed at the time. It is
  fixed now; see *A failed suntime lookup is no longer retried once a second*
  below. The sentence is put in the past tense rather than deleted because the
  rest of this paragraph is an account of what that rewrite did and declined to
  do, and the decline is part of the account.

  **Verified on four targets, against a baseline built from `git archive
  HEAD`.** The rewritten caller cannot be host-compiled — it pulls in FreeRTOS,
  NVS and the scenes graph — so a target build is the first instrument that
  compiles it at all, and a baseline is what turns "it builds" into a
  measurement:

  | target | baseline | fixed | delta | `.flash.text` | `.flash.rodata` |
  |---|---|---|---|---|---|
  | esp32   | 1684672 | 1688208 | +3536 | +2404 | +1136 |
  | esp32s2 | 1375008 | 1378528 | +3520 | +2380 | +1136 |
  | esp32s3 | 1699552 | 1703312 | +3760 | +2632 | +1136 |
  | esp32c3 | 1757984 | 1761856 | +3872 | +2666 | +1200 |

  The two section deltas reconcile to the image delta to within 4 to 8 bytes on
  each target (3540 against 3536, 3516 against 3520, 3768 against 3760, 3866
  against 3872) — the remainder is segment alignment padding in the image
  container, and it is reported rather than rounded away because a reconciliation
  that is forced to balance exactly stops being a check. `.flash.rodata` moves
  by the same **+1136** on all three Xtensa targets and **+1200** on the RISC-V
  one, which is the expected shape: the added constant data is the same strings
  and tables everywhere, and only its padding is target-specific.

  **Every one of the four new functions is absent from the baseline ELF and
  present in the fixed one, on all four targets.** That direction is the half
  that matters. Presence alone would only prove something compiled; absence
  from the baseline is what proves the probe is looking at this change. And
  both halves are needed here specifically, because the entire scenes
  translation unit sits inside `#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS` — a
  target with meshbots off would compile the rewritten function to nothing,
  link successfully, and report "build: OK". No new warning appears in any of
  the three touched files on any target.

  The full host suite is **5510** checks across **26** binaries, 0 failures.

  **Untested on hardware.** No board has fetched a sunrise time. Nothing here
  has made a TLS connection to `api.sunrisesunset.io`, so the reply this parses
  is a fixture written from the service's documented shape and not a capture,
  and the `Connection: close` reasoning above is read off the absence of
  `mbedtls_ssl_conf_read_timeout` rather than measured against a socket that
  hung. What is covered is every part of the calculation; what is unverified is
  that the request goes out, that something comes back, and that what comes
  back looks like the fixture. The failure direction is at least the safe one:
  anything this cannot account for leaves `curr_tm_day` cleared, so an
  isSunState scene that cannot reach the service does not fire at all, where
  before the fix it fired at the wrong time with complete confidence.

- **The URL ladder is gone, and with it twenty-three more measured defects.** A
  `sendHttpRequest` scene action's `url` field was turned into a host, a port
  and a page by nine `sscanf` arms: eight spelling out `http`/`https` against
  present/absent port and present/absent path, and a ninth catch-all for
  anything else. The two defects the audit opened on were a URL carrying
  credentials — which reached an arm that scanned the whole authority, so
  `web_server` became the string `user:pass@example.com` and the password left
  the device as a `Host:` header — and the same URL with an explicit port,
  which that arm had no conversion for.

  Those two were the reason to look. They were not the reason to rewrite.
  Compiling the real translation unit for the host and driving it found
  twenty-three more, every one of them silent. The list is tabulated below so
  that the count can be recounted rather than taken on trust; each row is
  pinned by a check that is named in the failing output of the control run
  described further down.

  The count is rows 4–26 of that table, and getting it right took three
  attempts, which is worth recording because it is the argument for tabulating
  it. A first draft said "twelve" — the figure when the entry was sketched,
  never revised as the measurement went on. A second said "twenty-two", a hand
  count taken before the table existed. Building the table brought it to
  twenty-one, and then checking every row against the control run's own list of
  failing checks found two defects that had no row at all: a leading zero in
  the port (row 12) and an unclosed IPv6 bracket (row 20). Both are measured,
  both were already pinned by passing checks, and neither was in any prose
  until the enumeration was made to account for the measurement line by line.
  The table is the enumeration; the number is whatever the table says, and it
  is counted from the table rather than carried forward.

  | # | what the ladder did | input |
  |---|---|---|
  | 1 | credentials became the host | `http://user:pass@example.com/x` |
  | 2 | …and went out as the `Host:` header | same |
  | 3 | …and an explicit port behind them was never parsed | `http://user:pass@example.com:8080/x` |
  | 4 | a query with no path landed in the host *and* the `Host:` header | `http://example.com?q=a` |
  | 5 | a fragment did the same | `http://example.com#frag` |
  | 6 | the scheme was matched case-**sensitively**, so this missed all eight arms and fell to the catch-all | `HTTPS://www.testhttp.com` |
  | 7 | every other scheme parsed and got a TLS handshake on the port it named | `ftp:`, `ws:`, `gopher:`, `file:` |
  | 8 | a long host was silently truncated to 99 bytes and accepted | 599-character host |
  | 9 | a long page was silently truncated to 199 bytes and accepted | 599-character page |
  | 10 | `%i` read a hex port | `http://h:0x50/x` → 80 |
  | 11 | `%i` read an octal port | `http://h:010/x` → 8 |
  | 12 | a leading zero made `%i` stop at the first `8`, so the arm failed and dropped through to arm three, which kept the 0 | `http://h:08080/x` → port **0** |
  | 13 | `%i` accepts a sign, so a negative port was handed to mbedTLS as `"-1"` | `http://h:-1/x` |
  | 14 | an out-of-range `%i` is undefined behaviour **inside libc** | `http://h:99999999999999/x` → 276447231 |
  | 15 | port 0 accepted | `http://h:0/x` |
  | 16 | ports above 65535 accepted | `http://h:65536/x`, `http://h:99999/x` |
  | 17 | trailing junk after the port discarded | `http://h:8090junk/x` → 8090 |
  | 18 | an empty port left the colon on the host | `http://example.com:/x` → host `example.com:` |
  | 19 | two ports accepted | `http://example.com:8080:9090/x` |
  | 20 | an unclosed IPv6 bracket became the host, and the colon inside it became the port | `http://[::1/x` → host `[:`, port **1** |
  | 21 | an empty host accepted | `http:///x`, `http://@/x`, `http://user@/x` → host `user@` |
  | 22 | a space in the host produced a two-piece `Host:` header | `http://exam ple.com/x` |
  | 23 | a space in the page produced `GET /a b HTTP/1.1` | `http://example.com/a b` |
  | 24 | a tab, a `0x1F` or a `0x7F` in the page went through untouched | `http://example.com/a\tb` |
  | 25 | **a bare `CR` in the page went through**, so the request line could be split | `http://example.com/x\r\nX-Injected: 1` → page `x\r` |
  | 26 | a newline silently truncated the page instead of refusing it | `http://example.com/a\nb` → page `a` |

  Rows 1–3 are the two defects the audit opened on — one of them in two of its
  consequences, plus the missing port. Rows 4–26 are the **twenty-three** the
  measurement added. Row 25 is the one worth naming separately: `%199[^\n]`
  stopped at the LF and kept the CR in front of it, so the page could carry a
  carriage return into a request line. Nothing downstream rejected it.

  Row 6 is the one most likely to have been noticed in the field, because it
  breaks URLs that are otherwise perfectly ordinary. `HTTP://host:8090/foo_page`
  landed on the ninth arm, which got the host and port right and handed back
  the page with a stray leading slash — `GET //foo_page`. The three forms
  *without* an explicit port had nothing for that arm's `%d` to take either, so
  they matched nothing at all and were reported unparseable: `web_server
  (null)`, `web_port 0`, `target_page (null)`. An uppercased scheme turned a
  working scene into a dead one, and RFC 3986 §3.1 says schemes are
  case-insensitive in as many words.

  **The replacement is one pass, and the port is parsed in exactly one place.**
  [`EZPI_parse_http_url()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L136)
  now calls
  [`__ezlopi_core_scenes_then_split_url()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L854),
  which refuses any byte `<= 0x20` or equal to `0x7F`
  [before it looks at anything else](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L908),
  matches the scheme against a
  [two-entry table](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L861)
  through
  [`__ezlopi_core_scenes_then_lower_ascii()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L814),
  [refuses every other scheme](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L941),
  and finds the authority and the page by position. The port and the
  credentials are then taken by
  [`__ezlopi_core_scenes_then_split_authority()`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_then_methods_helper_func.c#L1017)
  — which was **already in the file and already correct**, and which four of
  the nine arms were going around. That is the shape of the fix: not nine
  parsers made careful, but one parser that the existing correct one is reached
  from. Nine `sscanf` calls became zero.

  The `lower_ascii` helper compares through `(unsigned char)` on purpose. Plain
  `char` is signed on the x86 host and **unsigned on xtensa**, so without the
  cast byte `0xC3` compares as `-61`, lands below `0x20`, and is refused on the
  host while sailing through on the target. A divergence in that direction only
  surfaces as a test failure if something asserts the *accepting* answer, so the
  suite asserts it: a UTF-8 host parses, and is listed under Known gaps as a
  gap rather than presented as a fix.

  **The suite distinguishes the two implementations in both directions.** 478
  checks, 0 failures on the rewrite. The same file, unchanged, run against the
  shipped ladder by swapping the subject for `git show HEAD:` of itself:
  **150 of those 478 checks fail**, `rc=1`. Without that second number "0
  failures" and "cannot fail" print the same thing. `probe/hurestore.sh` is
  that run, and it deletes the object files first, because `/mnt/c` is
  OneDrive-backed and a clock skew that skipped the rebuild would print the
  rewrite's numbers under the heading "run against the shipped ladder".

  **A 34-mutant sweep over the three new functions: 28 detected, 0 rejected by
  the compiler, 6 survived.** Control row clean before and after (478/0,
  `rc=0`). Every survivor was predicted in advance and is argued equivalent
  rather than excused:

  | mutant | why it survives |
  |---|---|
  | `lower_ascii` drops its `'Z'` bound | no scheme in the table holds a `Z` |
  | the control-char scan starts at `url[1]` | `url[0]` must be `h`/`H` for any scheme to match, so byte 0 is already forced by the prefix test |
  | the scheme loop stops after one match | at most one of `http://`/`https://` can prefix-match a given URL |
  | the authority-fit check is dropped | the caller refuses an empty host too — though its message is only *accurate* because this check guarantees `auth_len > 0` |
  | the port's `value > 0` test is dropped | the `idx > 0` test already refuses an empty tail |
  | the port loop's `value <= 65535` bound is dropped | **equivalent on this host only** |

  That last row is the one the host suite structurally cannot catch, and it is
  recorded in the source as a survivor rather than left to look like dead code.
  `long` is 64 bits on the host, so `99999999999999` fits and the range test
  refuses it anyway; `long` is 32 bits on xtensa, where the same removal is
  signed overflow. No host input can distinguish them. The bound stays.

  Writing a comment that the sweep then disproved is the other thing worth
  recording here. The leading-space check carried a claim that it covered an
  off-by-one at the start of the guard's scan. The mutant above shows it does
  not and that no input could — `url[0]` is pinned by the scheme match two
  decisions later. The comment was corrected to say so, and the loop still
  starts at `url[0]`, because a guard that depends on a coincidence in a caller
  is not a guard.

  **Four targets, baseline against fixed, built from a `git archive` of `HEAD`
  rather than from a guess at what the branch ships:**

  | target | baseline | fixed | Δimage | Δ`.flash.text` | Δ`.flash.rodata` |
  |---|---|---|---|---|---|
  | esp32   | 1690816 | 1691056 | +240 | +32 | +208 |
  | esp32s2 | 1380512 | 1380752 | +240 | +32 | +208 |
  | esp32s3 | 1706048 | 1706288 | +240 | +32 | +208 |
  | esp32c3 | 1764112 | 1764448 | +336 | +126 | +200 |

  The two section deltas reconcile to the image delta **exactly** on all three
  Xtensa targets (32 + 208 = 240) and to within 10 bytes on the RISC-V one (126
  + 200 = 326 against 336) — segment alignment padding in the image container,
  reported rather than rounded away. Twenty-three defects for +240 bytes.

  **The probe is rodata strings, not symbol names, and that choice is the
  point.** Both new functions are `static` and small, so at `-Os` they inline
  away completely: `nm` finds **neither of them in the fixed ELF on any of the
  four targets**, which an `nm`-based probe would have reported as a failed
  build. Format strings cannot be inlined away.

  | probe | baseline | fixed |
  |---|---|---|
  | actual `sscanf(` calls in the source | 9 | 0 |
  | arm-nine format `%*[^:]%*[:/]%99[^:]:%d%199s` | 1 | 0 |
  | arm-five format `http://%99[^/]/%199[^` | 1 | 0 |
  | `only http and https are implemented` | 0 | 1 |
  | `refusing rather than writing it into a request line` | 0 | 1 |
  | `or a host holding a stray` | 0 | 1 |
  | **CONTROL:** `failed to parse URL` | 1 | 1 |

  Identical on all four targets. The control row carries the weight: the whole
  translation unit sits inside `#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS`, so a
  build with meshbots off would make the ladder's strings vanish, the new
  strings never appear, the image link, and the build report OK — which is
  exactly the pattern a successful removal produces. `failed to parse URL`
  exists in both trees, so **1 → 1** on it is what separates "the ladder was
  removed" from "the file was not compiled". No new warning appears in the
  subject on any target, on either side.

  The full host suite is **6257** checks across **29** binaries, 0 failures.

  That figure replaces a recorded **6160 across 27**, which was wrong in both
  numbers, and the arithmetic of the correction is worth keeping because the
  cause was a reporting bug rather than any change in what is tested. The
  suite totals itself by grepping each binary's output for the bare line
  `N check(s), M failure(s)`. Four binaries did not print that shape, so their
  checks summed as zero: `test_issun` prefixed the line with its own name
  (29 checks), while `test_devcfg_api` (319) and `test_api_envelope` (58)
  printed `checks`/`failures` without the parentheses and `test_scenefields`
  printed `test_scenefields: N checks passed` (24). All four now print the
  canonical shape.

  The recorded 6160 had a second problem: it was produced by an ad-hoc count
  using a looser pattern than the suite's own, which happened to pick up
  `test_devcfg_api`, `test_api_envelope` and `test_scenefields` (401 between
  them) while still missing the prefixed `test_issun`. So those 401 were
  already inside 6160, and the correction from it is just
  **6160 + 29 + 68 = 6257** — the recovered `test_issun` and the new
  `test_aled`. Cross-checked from the other direction: the bare-shape total
  was 5788 before this work, + 68 for `test_aled` + 401 for the three
  canonicalised binaries = 6257. The count of binaries is 29 because that is
  the length of `TESTS`, every one of which now reports.

  The lesson is in the direction of the error: a tally line that matches
  nothing understates the suite silently and forever, and the figure is quoted
  in prose here, so nothing contradicts it. It was found only by counting
  binaries-with-a-tally alongside the total and noticing the two disagreed.

  **Untested on hardware.** No board has sent a scene HTTP request. Every
  number above is from a host build of the real translation unit or from a
  cross-build of the image; none of it is from a socket. The trace output is
  also outside what this can see — the suite's `ezlopi_util_trace.h` stub makes
  `TRACE_*` a no-op, so whether a refusal message reaches the UART is settled
  by the build probe above and not by the tests. The failure direction is the
  safe one: everything newly refused was previously turned into a request line
  the caller did not intend, so a URL this rejects produces a scene action that
  does not fire, where before it fired at something unintended with complete
  confidence.

- **A failed suntime lookup is no longer retried once a second.** An
  `isSunState` when-condition that could not get a sunrise/sunset time opened a
  fresh TLS connection to `api.sunrisesunset.io` on every evaluation, and
  when-conditions are evaluated about once a second —
  [ezlopi_service_meshbot.c:455](ezlopi-services/ezlopi-service-meshbot/ezlopi_service_meshbot.c#L455)
  calls the protothread with a 1,000 ms routine delay and
  [`PT_WAIT_UNTIL`](ezlopi-services/ezlopi-service-meshbot/ezlopi_service_meshbot.c#L440)
  releases once that has elapsed. So a hub with no route to the service, or no
  coordinates stored, made on the order of **86,400 TLS connections a day, per
  isSunState scene, indefinitely**. That figure is not an estimate; it is one of
  the two numbers the test below measures.

  **Nothing throttled it because the state that would have throttled it was
  being deleted.**
  [`__issunsate_update_sunstate_tm`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L1893)
  clears `curr_tm_day` on entry and sets it only once a time has actually been
  parsed, so a failure leaves it clear — which is correct, and is the signal the
  caller reads to tell a failure from a success. But the caller's response to
  that signal, in
  [`ISSUNSTATE_get_suntime`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L470),
  was to delete the whole `user_arg` block. The next evaluation therefore
  allocated a fresh zeroed one and started over from nothing. Any attempt to
  remember "I just failed, wait a bit" had nowhere to live: the block holding it
  was freed a microsecond later. **Removing that delete is the fix**; the
  backoff is what becomes possible once the state survives.

  **The policy is three pure functions, and that is deliberate.** Whether to ask
  again is a function of a day number and a time of day, so it can be tested;
  the thing it governs is a TLS connection, which cannot. So
  [`EZPI_core_suntime_backoff_delay`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L423),
  [`EZPI_core_suntime_should_attempt`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L442),
  [`EZPI_core_suntime_note_failure`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L468)
  and
  [`EZPI_core_suntime_note_success`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L494)
  sit next to the existing decidable suntime helpers, with the reasoning in the
  header beside them
  ([HOW OFTEN A FAILED LOOKUP IS RETRIED](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.h#L178)).
  The hub-side state is one new field,
  [`backoff`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.h#L155),
  of type
  [`s_suntime_backoff_t`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.h#L276).

  **The ladder is 60 s doubling to 32 minutes and then held** —
  [`EZPI_SUNTIME_BACKOFF_BASE_SEC`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.h#L239)
  and
  [`EZPI_SUNTIME_BACKOFF_MAX_STEP`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.h#L240)
  — which costs **exactly 50 attempts across a full day of outage** instead of
  86,400, while still retrying inside the first minute. The step is saturated
  *before* the shift rather than the result after it
  ([suntime.c:439](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L439)):
  an unbounded step would shift an `int` by 31 or more, which is undefined
  rather than merely large. That distinction is not academic here — see the
  sanitizer note below, where it is exactly what let a mutant survive a green
  suite. The comparison is `>=` and not `>`
  ([suntime.c:465](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_suntime.c#L465)),
  because at one-second resolution a deadline that can only be passed and never
  met would lose the attempt whenever the evaluation lands exactly on it.

  **A per-day attempt counter was considered and rejected, and the rejection is
  itself a test.** My own earlier note on this gap — in Known gaps, below — said
  the fix was "a per-day attempt counter or a timestamped backoff", as though
  the two were interchangeable. They are not, and the counter is *worse than no
  backoff at all* for the case that actually happens: a hub reboots, the scene
  starts evaluating before DHCP and DNS are up, N attempts are spent in N
  seconds, and the scene is then finished for the day even though the network
  came up a minute later. `test_backoff_survives_a_late_network` models both
  designs against a network that arrives at 00:03:00 and asserts that the
  timestamped version obtains the suntime at 180 s while a five-attempt daily
  budget **never obtains it at all**. The budget has to be spent across the day,
  which means remembering *when* rather than *how many*.

  **Keeping the block is behaviour-identical, and the reason is what the readers
  test.** `ISSUNSTATE_get_offs_tmval`, `ISSUNSTATE_eval_range` and
  `ISSUNSTATE_check_flag_result` all gate on `0 != user_data->sunstate_mode`, so
  clearing that single field
  ([`sunstate_mode`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L572))
  makes every one of them skip the scene exactly as a NULL `user_arg` did. It is
  safe to clear because it is not configuration — it is re-read from
  `curr_sunstate_mode` on every re-entry. `moment_valid` is cleared with it;
  no reader can reach it while `sunstate_mode` is 0, so that is not
  load-bearing, but leaving a `true` beside a `defined_moment` computed from a
  suntime that has since been discarded is an invariant waiting to be broken by
  whoever adds the next reader. The per-second `TRACE_S` is also now behind the
  gate rather than in front of it
  ([when_methods_helper_functions.c:503](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L503)):
  it was flooding the log at the same rate the connections were flooding the
  wire, and the withheld path is deliberately silent, because the attempt that
  started the backoff already said what was wrong and when the next one would
  be.

  **One case changes more than the rate, and it is reported because it is the
  worst one**
  ([when_methods_helper_functions.c:553](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L553)).
  Of the three failure conditions, two leave `curr_tm_day` at 0 and so stay on
  the ladder. The third — `tmoffs_type` UNDEFINED, meaning the offset field
  holds something that is not `intime`, `before` or `after` — is reached with
  `curr_tm_day` already set to today, because the suntime was fetched before the
  offset string was examined. Retaining the block therefore makes that scene go
  quiet until midnight rather than retry at all. That is correct: retrying
  cannot fix a malformed offset field, and the old code retried it *hardest* — a
  TLS connection every second, forever, because of a typo in a scene field. A
  correction is not delayed until midnight either, because
  `EZPI_core_scenes_delete_when_blocks` tears the field list down on a scene
  edit, `user_arg` with it. The cost that *is* real: a configuration failure
  backs off on the same ladder as an outage, because nothing at this layer can
  tell them apart, so after `hub.coordinates.set` on a hub that has been failing
  for a while the scene can take up to 32 minutes to notice. A bounded one-off
  delay after a setup action, against a permanent connection storm.

  **206 checks before this change, 277 after** — +71 across nine new sections in
  [`tests/host/test_suntime.c`](tests/host/test_suntime.c), 0 failures, and **0
  sanitizer findings**, under `-Werror` with ASan and UBSan. Both numbers are
  re-derived by building the subject at `git show HEAD:` of itself rather than
  quoted from a previous run.

  **The control row is the awkward one here, and it is reported honestly.**
  Every other change in this branch has a control of the form "run the new test
  against the old subject and watch it fail". That form does not exist for this
  one: before this change the three functions did not exist, so the new test
  does not compile against the old subject and the measurement would be "link
  error", which says nothing about whether the checks can fail. So the control
  is in two parts, and neither is that form:

  - **A 28-mutant sweep over the three new functions: 25 detected, 1 survived,
    2 refused by the compiler, 0 misapplied.** Control row clean before and
    after. The two refusals are both "remove the only use of a parameter", which
    `-Wunused-parameter -Werror` rejects outright — scored in a separate column
    so a compiler refusal is not read as a failing check. The one survivor is
    removing the `0 == backoff_step` early-out, and it is argued equivalent
    rather than excused: whenever the step is 0 the deadline is 0 too, and
    `now_sod >= 0` always holds. To show that is a redundancy and not two pieces
    of dead code, a further mutant applies that removal *together* with the
    deadline-clearing one; it fails two checks where the deadline mutant alone
    fails one, confirming the pair are jointly load-bearing.
  - **An exact before-and-after measurement, inside the passing suite.** The old
    behaviour has a precise representative: a NULL `bo`, which the header
    documents as "always attempt" and which is what deleting the block on every
    failure amounted to. `test_backoff_a_full_day_of_outage` runs the same
    86,400-second loop against both and asserts **86,400 attempts against 50**.
    That is the defect and its fix, measured, rather than described.

  **Two of those mutants found defects in this work rather than in the subject,
  which is the point of running them.** A mutant that loosened `step <= 0` to
  `step < 0` made `delay(0)` evaluate `60 << -1`; gcc's default UBSan handler
  prints `runtime error: shift exponent -1 is negative` and *carries on*, and on
  x86 the count masks to 5 bits so `60 << 31` yields 0 — precisely the value the
  test asserts for step 0. Every check passed, the suite exited 0, and the
  finding sat unread in the suite's own output. The fix is
  `-fno-sanitize-recover=undefined`, scoped to this subject's object file only
  ([tests/host/Makefile:691](tests/host/Makefile#L691)); turning it on for all
  27 binaries is probably right and is not something to do as a side effect of
  one change. Separately, a mutant that cleared the day in `note_success`
  survived because my assertion for it was placed *after* a `should_attempt`
  call, which reads the cleared day as a day change and writes the value
  straight back — the check existed and could not fail. It is now asserted
  before that call, with the ordering documented as the check
  ([tests/host/test_suntime.c:1133](tests/host/test_suntime.c#L1133)).

  **Four targets cross-built against a baseline from `git archive HEAD`**, all
  clean with 0 warnings in either subject on either side: `esp32` **+256**
  bytes, `esp32s2` **+272**, `esp32s3` **+240**, `esp32c3` **+224**. Rodata and
  symbol probes all moved in the expected direction — the old per-second trace
  string 1 → 0, the new backoff trace 0 → 1, the four backoff symbols 0 → 4 —
  against two control strings that held at 1 → 1 and 2 → 2 on every target, so
  the probe distinguishes "string removed" from "the file did not get compiled".
  Reconciling the deltas against the per-section sizes agrees to within 6 bytes
  on three targets and exactly on `esp32s3`; the remainder is segment alignment
  padding in the image container.

  The full host suite is **6257** checks across **29** binaries, 0 failures.
  (Recorded as 6160 across 27 when this entry was written; four binaries were
  printing a tally line the suite's aggregate did not match, and so counted as
  zero. Reconciled in full in the scene-URL entry above.)

  **Untested on hardware.** No board has fetched a sunrise time, and nothing has
  been flashed. The 86,400-against-50 figure is a host loop over a pure
  function, not an observed connection count; what happens on the wire when
  `api.sunrisesunset.io` is unreachable is settled by the build probe showing
  the new code is linked in, not by watching a hub. The wiring that reads the
  policy — the gate, the two call sites, the cleared `sunstate_mode` — is in the
  half of this path that has no test that can fail, and that split is listed
  under Known gaps rather than papered over here: a mutation of the policy fails
  a check, a mutation of the wiring fails nothing.

### Hub location — [bug]

- **`hub.info.get` reported the hub at latitude 0, longitude 0 after every
  reboot, while `hub.coordinates.get` reported the real stored location.** Two
  methods, one datum, two different answers, both served to clients.

  The stored pair has a cache in RAM —
  [`sg_latitude` and `sg_longitude`](ezlopi-cloud/ezlopi-cloud-coordinates/ezlopi_cloud_coordinates.c#L97)
  — and
  [`EZPI_cloud_get_latitude`](ezlopi-cloud/ezlopi-cloud-coordinates/ezlopi_cloud_coordinates.c#L268)
  returns it to `hub.info.get`. Nothing loaded it at boot. It was zero from
  reset until a `hub.coordinates.set` arrived, whereas
  [`EZPI_hub_coordinates_get`](ezlopi-cloud/ezlopi-cloud-coordinates/ezlopi_cloud_coordinates.c#L258)
  reads NVS directly and so was right all along. A hub configured once and then
  power-cycled therefore told you where it was and that it was at the origin,
  depending which method you asked.

  **Zero is the failure that looks like data.** 0,0 is a real point in the Gulf
  of Guinea, so nothing downstream can tell it from a location the user chose.
  Which is why the fix does not substitute a default when nothing is stored:
  [`EZPI_cloud_coordinates_init`](ezlopi-cloud/ezlopi-cloud-coordinates/ezlopi_cloud_coordinates.c#L165)
  leaves the cache at zero and traces a line naming `hub.coordinates.set`. A
  location the user never chose produces answers that look right — a sunrise
  time, correct for somewhere else.

  It is called from
  [`EZPI_init`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L112),
  at [the point just after the stored settings are read](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L123) —
  after NVS is up and before anything that reads the location back. An explicit
  init rather than a lazy load inside the getters, and that choice was checked
  rather than taken on taste: the worry was a layering inversion, a core
  component calling into a cloud one. Both `CMakeLists.txt` files say the cycle
  is already there — `ezlopi-core-ezlopi` requires `ezlopi-cloud-coordinates`
  and `ezlopi-cloud-coordinates` requires `ezlopi-core-ezlopi` — so the call
  adds no edge that did not exist, and an explicit `*_init()` that reads NVS is
  what the rest of this tree does.

- **`EZPI_core_nvs_write_latitude_longitude` logged "Error writing" on every
  successful write, and said nothing at all when the write failed.** The test
  in [`ezlopi_core_nvs.c`](ezlopi-core/ezlopi-core-nvs/ezlopi_core_nvs.c#L877)
  was `if (EZPI_SUCCESS == error)`. Both halves were wrong and the silent half
  was the worse one: a failed write is precisely the case where the cache and
  flash are about to disagree, and it was the case that produced no output.

  **The rest of the tree was swept for the same copy-paste and has none.**
  Every other `if (EZPI_SUCCESS == error)` in the repository guards a success
  path correctly. Recorded as a measured non-finding rather than left as an
  open suspicion.

- **`hub.coordinates.set` stored whatever it was sent, including coordinates
  that are not coordinates, and stored the caller's whole request object to do
  it.** Three coupled problems in six lines, all now closed in
  [`EZPI_hub_coordinates_set`](ezlopi-cloud/ezlopi-cloud-coordinates/ezlopi_cloud_coordinates.c#L196).

  There was no validation. `CJSON_GET_VALUE_DOUBLE` assigned the request's
  numbers straight into the statics, so a latitude of 500, of −1e9, or of
  infinity — `1e999` parses to infinity through `strtod`, and a client can send
  that — was accepted and persisted.
  [`__coords_in_range`](ezlopi-cloud/ezlopi-cloud-coordinates/ezlopi_cloud_coordinates.c#L282)
  now bounds latitude to [−90, 90] and longitude to [−180, 180]. It rejects NaN
  and the infinities by arithmetic rather than by testing for them: every
  comparison against NaN is false, and an infinity fails a bound. There is no
  `isnan` or `isinf` in the component.

  The statics were assigned *before* the write, and the write's return value
  was discarded. So a failed or impossible write left the cache reporting a
  value that was not in flash — the same disagreement as the first defect
  above, arriving from the other direction and lasting until the next reboot
  flipped the answer back. The cache is now assigned only once
  `EZPI_core_nvs_write_latitude_longitude` has returned success, and a refusal
  gets an error object in the reply instead of silence.

  And the whole thing stored was
  `cJSON_PrintPreallocated(cj_params, …, 256, …)` — the request's `params`
  printed wholesale into a 256-byte buffer. Two consequences. Whatever else the
  caller put in that object went into flash under a key named for coordinates.
  And because the statics had already been assigned by the time the print was
  attempted, a `params` too large for 256 bytes updated the cache and wrote
  nothing, which is the desynchronisation again — reachable from a single
  oversized request. What is stored now is a canonical two-member object built
  from the two numbers that were accepted, rendered with
  [`COORDINATES_JSON_MAX`](ezlopi-cloud/ezlopi-cloud-coordinates/ezlopi_cloud_coordinates.h#L84)
  bytes of room and the `snprintf` return checked. The widest pair it can
  produce is `{"latitude":-90.000000,"longitude":-180.000000}` — measured at
  **47** bytes, **48** with the terminator, against a bound of **64**. Canonical
  storage is what makes the two readers agree by construction rather than by
  both being careful.

  **One guess of mine about this code was wrong, and the record says so.** I
  expected a `hub.coordinates.set` carrying only a latitude to leave the
  previous longitude in place. It does not: `CJSON_GET_VALUE_DOUBLE` zeroes its
  output first and overwrites only if the key is present and numeric, so the
  missing member became 0 rather than stale. Milder than predicted, and it is
  still refused now — a pair with one member missing is not a location. That
  macro's zeroing is also why the new reader does not use it: it cannot tell
  "longitude was 0" from "longitude was not sent", and those need different
  answers. `ezlopi_core_cjson_macros.h` became a dead include as a result and
  is gone, which was checked rather than assumed — no `CJSON_` macro remains in
  the file, and both of that header's own includes were already direct.

- **The two properties, and a test that was wrong about one of them.**
  [`tests/host/test_coords.c`](tests/host/test_coords.c) compiles the real
  component against the real `cjext.c` and the real `ezlopi_cloud_keywords.c`
  and makes **258** assertions under AddressSanitizer and
  UndefinedBehaviorSanitizer, 0 failures. The NVS surface is two functions,
  supplied as recording fakes; the read fake returns a fresh allocation because
  the real one does, so a missing or double free in the subject is visible to
  the sanitizer rather than hidden by the fake.

  A host process is the right instrument here rather than a substitute for a
  board: a freshly started process has its file statics at zero for the same
  reason a freshly reset chip does. **But the first draft of the test faked the
  reboot and did not notice.** It called init a second time after a successful
  `set` — at which point the cache already held the right answer, so the
  re-init had nothing to prove and the check passed no matter what init did.
  The mutation sweep is what exposed it: an init with an empty body scored 2
  failures where it should have scored 16, and the gap between those numbers
  was a bug in the test, not in the subject. The test now poisons the cache
  through the real setter before each simulated reboot, so an init that loads
  nothing leaves the sentinel visible and the assertion fails. The same mutant
  now scores **16**.

  The round-trip property is swept rather than sampled: **2701** coordinates —
  a 37×73 grid at 5° steps, offset so the values are not binary-representable —
  are set, read back out of the fake flash the way `hub.coordinates.get` reads
  it, and compared against what `hub.info.get` would report. **0** mismatches.
  Comparison is to 1e-6, which is the resolution six decimal places actually
  has, about 11 cm; asserting tighter would be asserting about `snprintf`.

  **Twenty mutations plus a control row. The control sits at 0 failures and
  19 of the 20 were detected.** Including the original defect reproduced as an
  init with an empty body, the cache assigned before the write instead of
  after, init assigning on parse failure instead of success, each of the four
  range bounds moved or loosened one at a time, the `cJSON_Number` type check
  dropped, the two members' stored precision cut to two decimals, latitude and
  longitude swapped in the stored form, and the outputs written before the
  range test. Two are caught by the sanitizers rather than by an assertion —
  not requiring `longitude` to be present dereferences a null `cJSON` at
  `ezlopi_cloud_coordinates.c:136`, and dropping the null-output check stores
  through a null `double *` at line 138 — and two by LeakSanitizer, which is
  the only instrument in this repository that would see a dropped
  `cJSON_Delete`.

  **The twentieth is an equivalent mutant and is recorded as one rather than
  chased.** Deleting the `snprintf` return check changes no observable
  behaviour, because the bound that makes it unreachable is the range check
  upstream of it: 47 bytes of worst case against 64 of buffer is 16 bytes of
  headroom, so the format cannot truncate. The check stays, and the header says
  why — a buffer whose safety depends on a bound enforced somewhere else is one
  edit away from not being safe. A twenty-first mutation, a double free in
  `hub.coordinates.get`, was caught by the compiler under
  `-Werror=use-after-free` and never reached the test at all; it is counted as
  detected by the build rather than credited to the suite.

  **What the sweep proves and what it does not.** Compiling this test against
  the tree as it was stops at four compile errors — the three new functions
  undeclared and `COORDINATES_JSON_MAX` undefined. That is loud, and it is
  shallow: it proves the functions are new, not that the test can see the
  behaviour go wrong. The mutant that proves the second thing is the empty init,
  which compiles, links, and reproduces the shipped defect exactly.

- **Built on four targets against a baseline from `git archive HEAD`.** The
  component is host-compilable, so unlike the previous section a target build is
  not the first thing that compiles it — but `EZPI_init` is not host-compilable,
  and the call placed there is.

  | target | baseline | fixed | delta | `.flash.text` | `.flash.rodata` | sum | vs image |
  |---|---|---|---|---|---|---|---|
  | esp32   | 1688224 | 1689616 | +1392 | +800 | +592 | 1392 | exact |
  | esp32s2 | 1378528 | 1379872 | +1344 | +748 | +600 | 1348 | +4 |
  | esp32s3 | 1703312 | 1704672 | +1360 | +752 | +608 | 1360 | exact |
  | esp32c3 | 1761856 | 1763376 | +1520 | +882 | +632 | 1514 | −6 |

  The two flash sections account for the whole image delta exactly on esp32 and
  esp32s3, and to within 4 and 6 bytes on esp32s2 and esp32c3 — segment
  alignment padding in the image container. Reported rather than rounded away,
  for the same reason as the table above it: a reconciliation forced to balance
  exactly stops being a check.

  **`.flash.rodata` does not move by the same amount on every target here**
  — +592, +600, +608, +632 — which is a difference worth naming because the
  previous section's entry *did* see a uniform figure across the three Xtensa
  targets and explained it by the added constant data being identical
  everywhere. That reasoning does not transfer, and it has not been borrowed:
  these four numbers are what was measured.

  All three new functions are absent from the baseline ELF and present in the
  fixed one on all four targets — **12** of 12 checks in the expected
  direction, none in the other. Absence is the half that matters; presence
  alone would prove only that something compiled.

  **A cross-check that mostly passed, and the part that did not.** This entry's
  baseline is HEAD, which is the commit the previous section describes, so the
  baseline column here should equal that entry's *fixed* column. It does
  exactly on esp32s2, esp32s3 and esp32c3. On esp32 it reads 1688224 against
  1688208 recorded there — **16 bytes apart, and I do not know why.** Both
  figures are from the same toolchain and the same build command on trees that
  should be identical. It is recorded unexplained rather than quietly dropped
  or attributed to a cause that was not measured; the delta within each sweep
  is a paired comparison and is unaffected either way, which is why each entry
  carries its own baseline rather than reusing a previous one.

- **The isSunState fix in the previous section was not affected by any of
  this**, which was established by reading the call site rather than assumed
  from the shared subject matter:
  [`ezlopi_core_scenes_when_methods_helper_functions.c#L1902`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L1902)
  calls `EZPI_core_nvs_read_latidtude_longitude()` directly and never touches
  the cache. The blast radius of the stale-cache defect was `hub.info.get`
  alone.

- **Untested on hardware.** Nothing has been flashed. No board has rebooted
  with a stored location and been asked for it over either websocket. What is
  covered is the decision-making and the persistence round trip; what is
  unverified is that `EZPI_cloud_coordinates_init` runs where it was placed in
  the real boot order, and that NVS on a real partition returns what the fake
  returns.

### Hub timezone — [bug]

- **The hub never applied its stored timezone. Scheduled scenes ran on UTC
  until some client happened to call `hub.info.get`.** `TZ` is process-global
  and there was exactly one place in the firmware that set it:
  [`EZPI_core_sntp_get_local_time`](ezlopi-core/ezlopi-core-sntp/ezlopi_core_sntp.c#L211),
  which read NVS, resolved the zone and called `setenv`+`tzset` as a side
  effect of formatting a string for a reply. Nothing called it at boot.
  `EZPI_core_sntp_set_location` resolved the name, traced it, and never
  installed it.

  So the zone was installed by whoever asked for the time first. The four
  `localtime()` calls that decide whether a time-of-day scene should fire —
  [561](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods.c#L561),
  [616](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods.c#L616),
  [672](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods.c#L672),
  [727](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods.c#L727)
  — read whatever was in effect at the time. On a freshly booted hub that is
  UTC, so a scene set for 7am fired at 7am UTC; after a client called
  `hub.info.get` ([`ezlopi_cloud_info.c#L146`](ezlopi-cloud/ezlopi-cloud-info/ezlopi_cloud_info.c#L146))
  the same scene started firing at 7am local. **The bug is not that the hub
  used the wrong zone. It is that a read-only query mutated global state that
  scheduling depends on**, so the behaviour changed underneath a user who did
  nothing, and changed back on the next reboot.

  There is now one installer,
  [`__install_tz`](ezlopi-core/ezlopi-core-sntp/ezlopi_core_sntp.c#L320), and
  two callers of it: a new
  [`EZPI_core_sntp_apply_stored_timezone`](ezlopi-core/ezlopi-core-sntp/ezlopi_core_sntp.c#L133)
  at boot, and
  [`EZPI_core_sntp_set_location`](ezlopi-core/ezlopi-core-sntp/ezlopi_core_sntp.c#L166)
  on a successful set. `get_local_time` now only formats. `setenv` without
  `tzset` would have been the plausible half-fix, and it does not work: libc
  caches the parsed zone and re-reads the variable only in `tzset`. Both calls
  live in the one function so neither can be done without the other.

  The boot call sits in
  [`EZPI_init`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L112) at
  [line 132](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L132), after
  the coordinate load added in the section above and before
  `EZPI_scenes_init_v2`. Ordering matters in one direction only: the zone must
  be installed before anything evaluates a scene, and it does not depend on the
  clock being synchronised, since `tzset` parses a string.

  **When nothing resolves, GMT0 is applied and announced rather than applied
  silently.** A hub quietly on UTC reports plausible times for the wrong place,
  which is the same failure as the 0,0 coordinates above: wrong data that
  cannot be told from a choice. The trace names `hub.location.set` and
  distinguishes "none stored" from "stored location is not in the zone table",
  and the function returns `EZPI_ERR_SNTP_LOCATION_SET_FAILED` even though it
  installed something.

- **`hub.location.set` stored unknown timezones, then reported success.** It
  wrote NVS first and resolved the name afterwards, so a name not in the table
  was persisted anyway. After that `hub.location.get` reported the bogus name
  while every `localtime()` caller ran on the fallback — one datum, two
  readers, two answers, which is the same shape as the coordinate defect in the
  section above. The name is now resolved first and stored only if it resolves.

  A failed NVS write is deliberately *not* followed by installing the zone, so
  the active `TZ` stays with what is still in flash and the two keep agreeing.

  And the cloud handler discarded the result.
  [`EZPI_CLOUD_location_set`](ezlopi-cloud/ezlopi-cloud-location/ezlopi_cloud_location.c#L172)
  called `EZPI_core_sntp_set_location(location_str)` as a statement and then
  echoed the requested name back under `result.location` whatever happened. The
  name a client got back was the one it had just sent, which reads as
  confirmation. It now returns an error object distinguishing a rejected name
  from a failed write, and the rejection names `hub.location.list`, because the
  set of acceptable names is exactly what that method returns and is not
  guessable from the refusal.

- **`hub.location.list` offered 51 of the 425 zones, and the correct loop was
  sitting commented out directly above the wrong one.**
  [`EZPI_CLOUD_location_list`](ezlopi-cloud/ezlopi-cloud-location/ezlopi_cloud_location.c#L84)
  ran `while (i <= 50)` with `// while (tz_list[i].name != NULL)` on the line
  before it. The 51 published were Africa/Abidjan through Africa/Tunis,
  stopping one short of Africa/Windhoek: every zone in the Americas, Asia,
  Australia, Europe and the Pacific was absent. For most of the world the hub
  offered nothing selectable, and with `set` unable to accept a name the list
  never returned, the timezone was effectively unconfigurable from the app.
  The loop now [ends at the terminator the table actually has](ezlopi-cloud/ezlopi-cloud-location/ezlopi_cloud_location.c#L130).

  **Whether the full table fits was measured, because the failure mode if it
  did not is silence.** Both websocket paths serialise a reply with
  `cJSON_PrintPreallocated` into the single 10240-byte buffer from
  `EZPI_core_buffer_acquire`, and that returns false rather than truncating,
  after which neither path sends anything at all. All 425 names plus the
  envelope serialise to **8006** bytes — **2234** under the buffer.

  That headroom is partly the client's to spend: `sender` is
  `cJSON_Duplicate`d straight from the request, so part of the reply size is
  chosen by the caller. At 51 entries a **9230**-character sender still fit; at
  425 the limit is **2248**. Both are far past any real sender, and the margin
  did narrow — stated rather than left out, because it is the kind of number
  that matters later.

  The per-entry `TRACE_E` and `vTaskDelay(1)` went with it. The trace logged
  every zone name at error severity to announce that a loop was looping, and
  the delay cost one tick each — 1 ms on all four targets, which pin
  `CONFIG_FREERTOS_HZ=1000` in their `sdkconfig.defaults.*` — so the complete
  list would have spent **425 ms** yielding to build a few hundred small
  strings.

- **A fifty-year upper bound on the clock sanity check that no target could
  ever satisfy.**
  [`EZPI_core_sntp_get_current_time_sec_abc`](ezlopi-core/ezlopi-core-sntp/ezlopi_core_sntp.c#L267)
  tested `now > BUILD_DATE && now < BUILD_DATE + 1576800000lu`. The upper bound
  is 3315120733, and `time_t` is 4-byte signed on every target this project
  builds for — measured with a `_Static_assert` compiled under all four of
  `xtensa-esp32-elf-gcc`, `xtensa-esp32s2-elf-gcc`, `xtensa-esp32s3-elf-gcc`
  and `riscv32-esp-elf-gcc`, not assumed from the architecture. The largest
  representable value is 2147483647, so the bound sat 1.17 billion seconds
  past it.

  Worse than merely unreachable: the literal was unsigned, so `now` was
  converted to `unsigned long` for the comparison, which made the test true for
  *every* input rather than false. A negative clock wraps to at least
  2147483648 and still compares below 3315120733. The left-hand test is what
  rejected those.

  [What is left](ezlopi-core/ezlopi-core-sntp/ezlopi_core_sntp.c#L298) is the
  half that worked — reject a clock predating the firmware, which is what an
  unsynchronised ESP32 reports. **No replacement bound was invented.** A
  representable one would be new policy rather than a repair, and a short-lived
  one: a signed 32-bit `time_t` expires in January 2038, about thirteen years
  past this `BUILD_DATE`.

- **Three dependencies that had been arriving by accident, and a fourth that
  broke four target builds.** `ezlopi_core_sntp.c` included
  `ezlopi_core_wifi.h` without naming a single wifi or netif symbol. Removing
  it on that evidence alone would have broken the build, which is the point
  worth recording: behind that one line came `freertos/task.h` for `vTaskDelay`
  and the port tick macros, and — via `cjext.h` — `ezlopi_util_trace.h` for the
  `TRACE_*` macros and `EZLOPI_USER_CONFIG.h` for `ezlopi_free`. All four are
  now included directly.

  **An ESP-IDF `REQUIRES` entry can be load-bearing for a header it never
  mentions.** `ezlopi-util-version` had to be *added* to
  `ezlopi-core-sntp`'s requirements when `ezlopi-core-wifi` came out, because
  `BUILD_DATE` — defined in exactly one header in the tree — was only on the
  include path by way of
  `ezlopi-core-sntp → ezlopi-core-wifi → ezlopi-service-uart → ezlopi-core-info`.

  The fourth surfaced only on hardware builds and is the reason this section
  has a build sweep that failed once. All four targets stopped at
  `ezlopi_cloud_protect.c: fatal error: ezlopi_core_cjson_macros.h: No such
  file or directory` — a component two edges downstream, which had been
  reaching that header through
  `ezlopi-cloud-protect → ezlopi-core-sntp → ezlopi-core-wifi → ezlopi-service-uart → ezlopi-core-ezlopi`.
  It uses **none** of that header's 13 macros, so the include was dead and is
  gone. Naming `ezlopi-core-ezlopi` in that component's requirements would also
  have worked and was rejected: it closes a cycle
  (`ezlopi-core-ezlopi → ezlopi-cloud-data → ezlopi-core-devices → ezlopi-core-protect → ezlopi-cloud-protect`)
  to supply a header nobody reads.

  The tree was then swept for the same class of breakage rather than fixed one
  error at a time: for every component, the set reachable through `REQUIRES`
  before and after the change, and every `#include "…"` whose every owner fell
  out of that set. Four components lost reachability —
  `ezlopi-cloud-protect`, `ezlopi-core-http`, `ezlopi-core-protect` and
  `ezlopi-core-sntp` — and only the first included anything affected.
  **The check was then confirmed able to fail**, by re-injecting the deleted
  include in memory: it reports exactly the failure the four real builds
  produced, and goes quiet once the include is gone. An analysis that cannot
  fail is not evidence.

- **`ezlopi_core_nvs.h` declared two functions in terms of `bool` without
  including `<stdbool.h>`.**
  [It does now](ezlopi-core/ezlopi-core-nvs/ezlopi_core_nvs.h#L53).
  [`EZPI_core_nvs_write_bool`](ezlopi-core/ezlopi-core-nvs/ezlopi_core_nvs.h#L266)
  and its reader had been compiling only because every translation unit that
  includes this header happened to pick up `stdbool` from somewhere else first
  — a property of its callers, not of the header. The first include that did
  not make it true was a host test, where `nvs_flash.h` is an empty stub and
  the two declarations became `unknown type name 'bool'`.

- **The test, and what it has been seen to catch.**
  [`tests/host/test_tz.c`](tests/host/test_tz.c) compiles the real
  `ezlopi_core_sntp.c` against the real `zones.c` — the 425-entry table is
  compiled in, not stubbed — and makes **151** assertions under
  AddressSanitizer and UndefinedBehaviorSanitizer, 0 failures.

  A host process is the right instrument rather than a substitute for a board,
  because the defect is about `TZ`, and a host has a real `TZ`, a real `tzset`
  and a real `localtime_r`. The test asserts on the wall clock the subject
  produces, not on whether a string was passed to `setenv`.

  The fixture poisons `TZ` to a deliberately absurd zone before each case, and
  the poison has a self-check that calls `exit(2)` if the poison did not take.
  **That self-check earned its place during development**: it caught
  `FIXED_EPOCH` being set to a constant one day off the date its comment
  claimed.

  **Every published zone is swept, not sampled.** All **425** are set through
  the real `hub.location.set` path and the resulting wall clock compared
  against the table's own `posix_str` installed directly by the test —
  425 accepted, 425 matching, asserted as three separate equalities so that a
  table that silently shrank would fail rather than pass vacuously.

  **Nineteen mutations plus a control row at 0 failures. Eighteen were
  caught** — seventeen by the test, the eighteenth by the compiler. Both halves
  of the original defect fail loudly: a boot that resolves but never installs
  fails 19 checks, a set that resolves but never installs fails 18. **Dropping
  `tzset()` while keeping `setenv()` fails 37, the largest of any mutant**,
  which is the right shape, since that is the mistake most likely to be made by
  someone who thinks setting the variable is enough. Restoring the original
  store-before-validate ordering does not merely fail, it faults: the unknown
  name reaches `setenv` as a NULL and ASan reports a SEGV at address zero.
  Dropping the `free` is caught by LeakSanitizer, and doubling it never reaches
  the test at all — GCC rejects it under `-Werror=use-after-free`, counted as
  detected by the build rather than credited to the suite.

  **One mutant is caught by the sanitizers alone and could not be caught any
  other way.** Dropping the NULL/length guard from `get_local_time` is
  invisible to assertions: glibc declares `strftime` `nonnull` and, handed a
  NULL destination, returns the length it would have written without writing
  anything — measured, not inferred from the declaration. UBSan's "null pointer
  passed as argument 1" is the whole of the evidence. Newlib makes no such
  promise, so on hardware this is not a benign mutation.

  **The nineteenth survived, and it is an equivalent mutant rather than a
  gap.** Removing the NULL guard from `EZPI_core_sntp_set_location` changes
  nothing observable, because `tz_name_cmp` guards both its arguments
  explicitly, so a NULL name resolves to NULL without a dereference and the
  function returns the same error the guard would have produced. The guard is
  kept anyway, and the reason it survived here is exactly why: `stubs-sntp`'s
  `sdkconfig.h` leaves `CONFIG_EZPI_UTIL_TRACE_EN` undefined, which compiles
  `TRACE_W` away. All four `sdkconfig.defaults.*` pin it to `y`, so on hardware
  that line hands a NULL to a `%s` conversion.

  **The mutation harness itself had to be rewritten, and the first version was
  dangerous.** It used `perl -i`, which renames rather than truncates; this
  checkout is OneDrive-backed and denied the rename, so edits silently did not
  apply and several mutants reported "0 failures" while being unmutated. Then a
  crash mid-sweep left the subject mutated in the working tree, and the next
  run read that mutant as its own baseline — producing a control row at 19
  failures and a table in which every row was void. The rewrite writes in place
  by truncation, restores the subject on *any* exit including an exception,
  asserts each pattern matched exactly once so an unapplied edit is reported as
  unapplied rather than as a surviving mutant, treats the control row as a hard
  gate that aborts the sweep if it is not clean, and finally checks the subject
  byte-identical to the original. It also counts a sanitizer report as a
  detection independently of the printed tally, which it did not at first:
  UBSan recovers by default, so the `strftime` mutant tripped a runtime error
  and still printed "0 failures".

  The whole host suite is **27** binaries, **5661** checks, 0 failures, with
  the per-binary totals summed by the script rather than by hand — three
  different tally wordings exist in this suite and a regex matching only one of
  them had previously undercounted it.

- **Built on four targets against a baseline from `git archive HEAD`.**

  | target | baseline | fixed | delta | `.flash.text` | `.flash.rodata` | sum | vs image |
  |---|---|---|---|---|---|---|---|
  | esp32   | 1689616 | 1690336 | +720 | +496 | +224 | 720 | exact |
  | esp32s2 | 1379872 | 1380048 | +176 | −36 | +216 | 180 | +4 |
  | esp32s3 | 1704672 | 1705600 | +928 | +716 | +208 | 924 | −4 |
  | esp32c3 | 1763376 | 1763792 | +416 | +180 | +240 | 420 | +4 |

  **`.flash.text` goes *down* by 36 bytes on esp32s2** while the image grows,
  and that is not noise worth smoothing over: `get_local_time` lost an NVS
  read, a table lookup, a `setenv` and a `tzset`, and on that target the
  removal very nearly pays for the new function. The other three grew. Four
  targets, four different code deltas from one source change.

  The two flash sections account for the image delta exactly on esp32, and to
  within 4 bytes on the other three — segment alignment padding in the image
  container. Reported rather than rounded away.

  `EZPI_core_sntp_apply_stored_timezone` is absent from the baseline ELF and
  present in the fixed one on all four targets: **8** checks in the expected
  direction, none in the other. Absence is the half that matters.

  **The cross-check the previous entry could not quite make, this one can.**
  This baseline is HEAD, which is the commit that section describes, so these
  baseline figures should equal its *fixed* column — and all four match
  exactly: 1689616, 1379872, 1704672, 1763376. The unexplained 16-byte esp32
  discrepancy recorded there did not recur.

- **Untested on hardware.** Nothing has been flashed. No board has booted with
  a stored zone, evaluated a time-of-day scene, and been checked for firing at
  the right local hour — which is the behaviour this section is about. What is
  established is that the zone is installed from one place at boot and on set,
  that the install is what libc actually reads, and that all 425 published
  zones round-trip. What is unverified is newlib's `tzset` agreeing with
  glibc's on the table's POSIX strings, and the boot ordering holding on a real
  device.

### Build and configuration — [infra]

- **Shadowed locals are now a build error, in this project's components and
  nowhere else.** [`CMakeLists.txt`](CMakeLists.txt) attaches
  `-Wshadow=local -Werror=shadow` to each of our components, and every shadowed
  declaration in our code is gone, so the project reports nothing under any of
  the three spellings of the flag. One of them was a real defect — the
  `cj_device_items_data` leak recorded under *Memory safety* above, which made
  the function containing it a no-op — and it is of exactly the class the
  enabled spelling reports, so the guard that now exists would have caught it.

  **This was recorded as a gap whose blocker does not hold.** The old entry
  said the flag "also fires inside ESP-IDF headers, so it cannot simply be
  added with `-Werror`" — one sentence resting on two things that were never
  checked. GCC has three spellings of the flag rather than one, and the ESP-IDF
  noise is not in headers at all. Measuring both is what unblocked this: esp32
  was built three times, once per spelling, each with the flag applied to
  everything and `-Wno-error=shadow` so the build would not stop at the first
  hit. The counts below are of distinct warning lines, which within a single
  run is one per `(file, line, variable)`; the raw line count is the wrong
  instrument because a warning from a header is re-emitted once per translation
  unit that includes it, so it measures include fan-out rather than defects.

  | spelling | distinct reports | ours | ESP-IDF and other |
  |---|---|---|---|
  | `-Wshadow=compatible-local` | 11 | 7 | 4 |
  | `-Wshadow=local` | 14 | 8 | 6 |
  | `-Wshadow` | 25 | 9 | 16 |

  Every one of ESP-IDF's is in its own `.c` files — `bt/common/osi/config.c`,
  `esp_https_ota.c`, `esp_ipc.c`, `essl_sdio.c`, `btm_ble_gap.c`, `esp_wps.c`
  for `=local`, ten more under plain `-Wshadow`. That is the fact the gap entry
  got backwards, and it is the fact that makes this fixable: our translation
  units include ESP-IDF's headers but never compile its sources, so attaching
  the flag per-target removes the upstream noise *completely* rather than
  merely reducing it. A header-borne warning could not have been escaped this
  way.

  The three sets nest the way they should, checked rather than assumed: all 14
  `=local` sites appear among plain `-Wshadow`'s 25, and the 11 it adds are 10
  in ESP-IDF plus one of ours. Comparing the runs needs the `(file, line,
  column)` key and not the warning text, because GCC tags each report with the
  most specific sub-option it matches — the same site reads
  `[-Wshadow=compatible-local]` in the `=local` run and `[-Wshadow]` in the
  plain one, so a first attempt that diffed whole lines concluded every site
  was unique to its run. Those tags also corroborate the triage below from a
  second direction: of our eight `=local` sites, exactly one is tagged
  `[-Wshadow=local]` rather than `[-Wshadow=compatible-local]`, and it is
  `ezlopi_cloud_items.c:124` — the one whose types genuinely differ, a
  `uint32_t` hiding a `cJSON *`.

  `=local`, not plain `-Wshadow`, and the choice is not about the count.
  `=local` fires only when a local or parameter is hidden by another local or
  parameter in the same function, so whether our build passes is decided
  entirely by code in our own function bodies. Plain `-Wshadow` additionally
  fires when a local shares a name with any global, typedef or function that
  anything we include happens to declare — so an upstream header gaining a
  name would break our build for a non-defect. With the flag an error, that
  distinction is the difference between a guard and a liability.

  The scoping is a loop after `project()` over the component basenames in
  `${ALL_COMPONENTS}`, calling `target_compile_options(__idf_<name> PRIVATE …)`.
  Per-component editing was not an option: there are 178 of them. The loop
  carries two guards, and they are not equivalent. The `INTERFACE_LIBRARY` one
  is there because the first attempt without it failed to configure: ESP-IDF
  registers a component that contributes no source file as an INTERFACE
  library, and `PRIVATE` options on one of those are a hard error —
  *"target_compile_options may only set INTERFACE properties on INTERFACE
  targets"*. Four of ours are header-only that way — `pt`,
  `ezlopi-core-boards`, `ezlopi-core-config`, `ezlopi-user-config` — and
  skipping them excludes no code from the check: between them they hold five
  headers, zero `.c` files, and no function bodies at all, and their headers
  are still compiled, with the flag, inside every translation unit that
  includes them. The `TARGET` guard is the weaker of the two and has never
  fired: `ALL_COMPONENTS` is assembled by globbing directories, so it *can*
  name something that is not a registered component, but today all 176
  subdirectories it collects do have a `CMakeLists.txt`. It is kept because
  adding a directory without one is an ordinary thing to do and the failure
  mode is a fatal configure error rather than a missed component.

  The nine sites fixed here — seven renames, one deletion, and one of the
  plain-`-Wshadow` class that the enabled spelling does *not* report:

  | site | was | now |
  |---|---|---|
  | `ezlopi_cloud_items.c:124` | `device_id` | `device_id_num` |
  | `ezlopi_cloud_scenes.c:875` | `new_scene_id` | `cj_scene_id` |
  | `ezlopi_core_broadcast.c:241` | `curr_node` | `tail_node` |
  | `ezlopi_core_scenes_cjson.c:797` | `value_type_name` | `unmatched_type_name` |
  | `ezlopi_core_scenes_operators.c:1064` | `item_id` | `item_id_right` |
  | `gxhtc3.c:224` | `i` | `bit` |
  | `sensor_0037_pms5003_sensor.c:138` | `pms_object` | declaration removed |
  | `ezlopi_service_ws_server.c:337` | `cj_method` | `cj_requested_method` |
  | `ld2410_driver.c:958` | parameter `byte` | `value` |

  Each name was chosen to say what the variable is rather than to dodge the
  warning. `tail_node` walks to the tail to append; `item_id_right` is the
  right-hand operand, the left having already been resolved into `item_left`
  above; `cj_requested_method` answers whether an unauthenticated caller is
  allowed through, which is a different question from the outer `cj_method`
  that is `NULL` at that point and is populated later for the error response.

  `sensor_0037_pms5003_sensor.c` is a deletion rather than a rename, and that
  is the one case where the shadow was hiding something worth removing. The
  inner declaration re-read the same `item->user_arg` the enclosing
  `pms_object` already held, and nothing in the function writes `user_arg` — so
  it was the identical pointer, obtained a second time *without* the NULL check
  the outer had already passed. Removing it is a small improvement as well as a
  rename avoided. The compiler could not reach that conclusion, which is why
  this is also the only edit in the set that changes the image; see the sizes
  below.

  `ld2410_driver.c` is a different class and worth separating. The parameter
  was named `byte`, which is a `typedef unsigned char` in that component's own
  header and is used as a cast throughout the file, so inside the function the
  type name was hidden by the parameter. Only plain `-Wshadow` reports this, so
  the flag now enabled does not guard it — it is fixed, not enforced. The first
  attempt at this one nearly deleted the typedef as dead: a `grep` filtered
  with `-v "_byte\|byte_\|bytes"` hides every `(byte)0xFD`-style cast, and a
  precise re-check found more than twenty live uses in that file alone.

  It is also the site that shows the earlier record was one short, and the
  discrepancy is worth leaving visible rather than quietly reconciled. That
  sweep is recorded above as having used `-Wshadow` and found nine in our code:
  the one defect plus the eight in the table below. But all eight of those are
  in the `=local` class, and plain `-Wshadow` reports a superset — so a sweep
  that really ran the plain spelling should have returned ten, with
  `ld2410_driver.c` among them. Either it ran a narrower spelling than it was
  recorded as running, or it dropped one. The nine in the table above are
  therefore not the same nine; they are the eight benign ones plus the site the
  old record is missing, the defect having already been fixed out of the set.

  Why each rename is behaviour-preserving, kept from the triage that preceded
  it so the reasoning does not have to be redone. In every case the shadowed
  outer variable is either already spent at that point or holds the identical
  value, which is why renaming is a no-op rather than a fix:

  | site | variable | why the shadow was benign |
  |---|---|---|
  | `ezlopi_cloud_items.c:124` | `device_id` | a `uint32_t` hides the `cJSON *` loop variable, but the pointer is consumed on the line above and `cJSON_ArrayForEach`'s increment runs in the enclosing scope, so iteration is unaffected |
  | `ezlopi_cloud_scenes.c:875` | `new_scene_id` | the inner sits in the `else` of `if (new_scene_id && …->valuestring)`, so the outer is known falsy there and is never read again |
  | `ezlopi_core_broadcast.c:241` | `curr_node` | the outer was walked to `NULL` by the duplicate-check loop above; the inner re-seeds from `__method_head`, which is what a fresh walk needs |
  | `ezlopi_core_scenes_cjson.c:797` | `value_type_name` | outer and inner are both initialised from `EZPI_core_scenes_get_scene_value_type_name(curr_field->value_type)` with `curr_field` unchanged between them, so the values are identical |
  | `ezlopi_core_scenes_operators.c:1064` | `item_id` | the outer id was already resolved into `item_left`; the inner resolves the right-hand item. Correct, just identically named |
  | `gxhtc3.c:224` | `i` | the inner is the 8-bit shift counter of a CRC8; the outer byte index is not referenced inside it |
  | `sensor_0037_pms5003_sensor.c:138` | `pms_object` | the inner re-reads the same `item->user_arg` the outer already holds |
  | `ezlopi_service_ws_server.c:337` | `cj_method` | the outer is `NULL` from its declaration until line 361 reassigns it, and line 368 is its only read, so the inner is redundant rather than load-bearing |

  Worth saying plainly, because the first pass over this list got it wrong:
  `scenes_cjson.c:797` looks like the dangerous shape — an inner declaration
  initialised from a call — and is in fact the safest of the eight, because the
  outer is initialised from the very same call. The shape is not the evidence;
  what the outer holds at the point of the shadow is. The anchor in that table
  for `scenes_operators.c` was also wrong while this was a gap entry: it read
  `:1059`, which is a closing brace. The pair is 1045 and 1064, and the
  compiler's own `1064:30` is what settled it.

  **Evidence, in four parts, because a build that merely succeeds is exactly
  what a flag that never reached the compiler also looks like.** The esp32
  build is clean with zero shadow diagnostics. The generated
  `compile_commands.json` shows the flag on 301 of our translation units and
  missing from none — 302 entries carry it in all, the extra being a generated
  `.S` under `build/` belonging to one of our components, where a C warning
  flag is inert — and on 0 of ESP-IDF's 1,165. Three of ESP-IDF's own six
  `=local` sites are confirmed present in that same compile database, which is
  why a passing build is itself proof the flag did not reach them. The
  `-Werror=shadow` is belt-and-braces: the same database shows ESP-IDF already
  passing `-Werror=all` on 1,444 entries, so the promotion would happen without
  it — but it is what the compiler names in the diagnostic, so it no longer
  depends on that. An injected
  block-scope `outer` hiding a parameter `outer` *fails* the build with
  `error: declaration of 'outer' shadows a parameter [-Werror=shadow]`, and
  fails for that reason specifically: the probe was given external linkage so
  that `-Wunused-function` could not be what stopped the build, and the log
  carries one shadow-class error and zero unused-function errors. Removing the
  probe restores a clean build at a byte-identical image size.

  All four CI targets then build clean, with zero shadow diagnostics and the
  flag on 301 of 301 of our translation units on every one of them — the same
  301 each time, since no component here is target-conditional:

  | target | `Ezlo_Pi_v4x.bin` | was | delta |
  |---|---|---|---|
  | esp32 | 1,683,856 | 1,683,856 | 0 |
  | esp32s2 | 1,374,176 | 1,374,192 | −16 |
  | esp32s3 | 1,698,704 | 1,698,720 | −16 |
  | esp32c3 | 1,757,104 | 1,757,104 | 0 |

  Only one of the ten edits can move code at all: renaming a local or a
  parameter cannot change codegen, and neither can a warning flag. So the
  16 bytes off esp32s2 and off esp32s3 are attributable to the one deletion.
  That the `pms5003` inner declaration was *semantically* a copy of a pointer
  the function already held does not make it free — at least one call sits
  between the outer read and the inner one, and the compiler cannot assume a
  call leaves `item->user_arg` alone, so it had to emit a second load.
  Removing the declaration removes that load. Every size in the table is a multiple of 16,
  which is the likeliest reason esp32 and esp32c3 show `0` rather than that
  nothing changed there; a net change below the padding quantum reads as zero.
  The same code is compiled on all four — the compile database holds the same
  301 of our translation units for every target — so there is no second
  explanation available. **Nothing was flashed** — no board was available — so
  every claim here is a compile-time and link-time one.

- **Four `#endif` comments that named the wrong macro.** Comments on a closing
  `#endif` are the only navigation aid in long conditional blocks, and when they
  are wrong they are worse than absent — they send a reader to the wrong
  opening. Three were in
  [`ezlopi_service_ble_provisioning.c`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c),
  the file whose mis-nesting already cost real time while tracking down the
  `EZPI_SERV_BLE_ENABLE_READ_PROV` question recorded above: one `#endif` labelled
  `EZPI_SERV_BLE_ENABLE_READ_PROV` was in fact closing
  `EZPI_SERV_BLE_ENABLE_STAT_PROV`. The fourth was in
  [`dht11.c`](ezlopi-sensors-devices/sensor-0015-oneWire-DHT11/dht11.c), where
  an `#endif` closing a three-way target test was labelled
  `CONFIG_IDF_TARGET_ESP32C3` — naming the one target the block *excludes*. It
  now names all three it includes.

  Comment-only, and each edit rewrites the text of an existing line without
  adding or removing one, so no `__LINE__` shifts and the preprocessed output
  cannot differ. Four further mismatches found by the same scan are left alone:
  they are include-guard `#endif`s whose comment spells the guard macro slightly
  differently, which misleads nobody.

- **The enum-to-`int *` assumption under `CJSON_GET_VALUE_INT` is now
  compile-time enforced.** An esp32, esp32s2 or esp32s3 build emits 87
  `-Wincompatible-pointer-types` warnings from
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

  A related count, recorded here because it looks like it contradicts the 87
  and does not. On esp32c3 the same macro accounts for **83**, not 87. The four
  missing ones are two sites each in the DHT11 and DHT22 drivers, whose entire
  bodies sit inside a `CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 ||
  CONFIG_IDF_TARGET_ESP32S2` guard — so on esp32c3 those two translation units
  preprocess away to nothing and those drivers are not available on that chip
  at all. The enum reasoning above is not target-dependent: asked directly,
  all four toolchains warn identically on passing `enum *` where `int *` is
  declared.

- **A pointer-type defect that only one of the four targets could see.**
  [`hilink_presence_sensor_setting.c`](ezlopi-sensors-devices/sensor-0067-UART-hilink-presence-sensor/hilink_presence_sensor_setting.c)
  read a stored setting into a local declared `int` and passed its address to
  `EZPI_core_nvs_read_int32`, whose parameter
  [`ezlopi_core_nvs.h`](ezlopi-core/ezlopi-core-nvs/ezlopi_core_nvs.h) declares
  as `int32_t *`. The local is now `int32_t`. Pre-existing, not introduced by
  this branch.

  The mechanism, taken from the compilers rather than argued: `__INT32_TYPE__`
  is `int` on all three xtensa compilers and `long int` on
  `riscv32-esp-elf-gcc`. A probe that passes `int *` where `int32_t *` is
  declared is silent on esp32, esp32s2 and esp32s3 and warns on esp32c3. So
  the callee would have been writing a `long int` through a pointer to an
  `int` object. Both are four bytes wide on that target, so nothing
  observably broke — but the constraint violation was real, and three of the
  four builds were structurally unable to report it.

  Measured before and after, as distinct warning sites rather than as
  occurrences, because the same warning is counted once per compilation pass
  and the totals mislead: esp32c3 goes from **8** distinct
  `-Wincompatible-pointer-types` sites to **7**, the site that disappears is
  exactly this one, and none appear. All four targets build, and every image
  is byte-identical to the pre-fix build — `0x19cf10`, `0x1512b0`, `0x1a0a50`
  and `0x1aedb0` — which is the expected outcome for two spellings of the same
  32-bit type and is the evidence that this changed no code generation. The
  pre-fix build of the same tree supplies the reversed half of the comparison,
  so this is a measurement and not an assertion.

  One accounting note, because two figures here look off by one against the
  list further down this entry. The **8** and the **7** count every distinct
  site the build reports, and one of them is not in a `.c` file at all: it is
  the single line inside
  [`ezlopi_core_cjson_macros.h`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_cjson_macros.h#L123)
  that all 87 `CJSON_GET_VALUE_INT` expansions are reported at, described above.
  The "six remaining" list below counts only sites in `.c` files, because that
  macro's call sites are a separate population deliberately left alone. So
  7 = 6 + 1, and the two figures are counting different things on purpose.

  **The host suite cannot see this class of defect at all**, and the stub in
  [`test_hilinkset.c`](tests/host/test_hilinkset.c) had quietly made that
  worse: it declared the two NVS integer functions with `int` and `int`
  pointer parameters, copied from the caller rather than from the header. On
  x86-64 Linux `int32_t` is `int`, so the two spellings are one type and no
  host build could ever have contradicted the caller. Both now match the
  header. The suite is unchanged at 34 binaries, 6613 checks and 0 failures
  before and after, which is the point: this is a correction to what the stub
  claims, not a fix the host can demonstrate.

  **Six such sites in `.c` files remained on esp32c3 when this was written, and
  they were three different defects rather than one.** Three passed
  `uint32_t *` where the IDF's `uart_get_buffered_data_len` wants `size_t *` —
  one in `ezlopi_hal_uart.c` and two in `ezlopi_service_uart.c`. Two passed
  `e_trace_severity_t *` where `ezlopi_core_nvs.h` declares `uint32_t *`, in
  `ezlopi_core_log.c`. The sixth was a function pointer's return type, in
  `ezlopi_service_system_temperature_sensor.c`.

  **All six are now fixed** — the `ezlopi_core_log.c` pair under *Log severity*
  above, the other four in the entry immediately below, which also gives the
  whole-tree figure both of them produce.

  One claim made here did not survive the fix, and is corrected rather than
  quietly dropped. This entry called the temperature-sensor site "a change of
  policy rather than of type" and attached it to the "no driver can report an
  initialisation failure" problem under Known gaps. It was a plain type error:
  of the 63 functions in this tree whose first parameter is
  `e_ezlopi_actions_t`, 62 already returned `ezlopi_error_t`, so the one that
  returned `int` was the exception and not the policy. The
  initialisation-failure gap is real and still open, and that driver is still
  an example of it — its `ezpi_init()` cannot report a failure because both of
  its `ESP_ERROR_CHECK()` calls abort instead of returning — but that gap was
  never what made the compiler warn, and conflating the two is what kept a
  one-line type fix sitting behind a design decision for the length of this
  audit.

  CI already builds all four targets in a matrix, so this diagnostic was
  present in CI output the whole time; it is not escalated, because only
  `-Werror=format` is. Escalating `-Wincompatible-pointer-types` is still not
  available as an option, and now for exactly one reason: the 87
  `CJSON_GET_VALUE_INT` expansions above are deliberately unchanged, and they
  are all that is left of this diagnostic on either ABI.

- **The last four pointer-type mismatches are fixed, and the two ABIs now
  report the same diagnostics.** These are the four left standing by the entry
  above. They were two unrelated defects, and each needed a different argument
  before it could be changed.

  Three were `uint32_t` locals whose address was handed to
  `uart_get_buffered_data_len()`, which writes a `size_t` through that pointer:
  [`ezlopi_hal_uart.c:291`](ezlopi-hal/ezlopi-hal-uart/ezlopi_hal_uart.c#L291),
  [`ezlopi_service_uart.c:718`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L718)
  and
  [`:719`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L719).
  All three are now `size_t`. Value-neutral, and that is asserted rather than
  assumed: a `_Static_assert` probe compiled by both toolchains establishes
  `sizeof(size_t) == 4`, `sizeof(size_t) == sizeof(uint32_t)` and
  `(size_t)-1 > 0` on each. Had `size_t` been wider than 32 bits on either
  target, the IDF would have been writing eight bytes into a four-byte local on
  every UART read and this would be a memory-safety entry instead of a
  portability one — which is the reason to measure it rather than reason about
  it.

  Only `tmp_len` is ever passed by address, so strictly only it had to change.
  `buffred_data_len` changed with it because the two are compared and assigned
  to each other, and leaving one of a matched pair as the other type is how
  this comes back. Its other consumers — `ezlopi_malloc()`, `memset()`,
  `uart_read_bytes()` — already take `size_t`. The one that does not is
  `EZPI_SERV_improv_serial_filter()`'s `uint32_t out_cap`, which now carries an
  explicit cast beside the one that was already on `rxBytes`.

  The fourth was a function pointer's return type.
  [`EZPI_system_temperature_device()`](ezlopi-services/ezlopi-service-system-temperature-sensor/ezlopi_service_system_temperature_sensor.c#L148)
  returned plain `int`, while
  [`ezlopi_core_devices.h:226`](ezlopi-core/ezlopi-core-devices/ezlopi_core_devices.h#L226)
  declares the `item_func` it is registered as returning `ezlopi_error_t`. It
  now returns `ezlopi_error_t`, along with the four file-static helpers reached
  through its action switch. No value changes anywhere in that file, because
  `EZPI_SUCCESS` is `0` and `EZPI_FAILED` is `-1` — exactly the bare literals
  those five functions were already returning. The header names `ezlopi_error_t`
  now, so it includes `ezlopi_core_errors.h` and the component declares
  `ezlopi-core-errors` in `REQUIRES`; it compiled without that line, because
  `ezlopi-core-devices` lists `ezlopi-core-errors` under public `REQUIRES` and
  IDF propagates it, but 95 other component `CMakeLists.txt` in this tree
  declare what they include and this one now does too.

  **Measured as a whole-tree total on both ABIs**, from `fullclean` builds of
  trees synced to the committed bytes — **1491** objects on esp32c3 and
  **1511** on esp32, `rc=0` and zero errors on both, so every file was asked:

  - `-Wincompatible-pointer-types` in a `.c` file anywhere in the tree: **0**,
    on both targets, down from 4 on esp32c3.
  - The only site of that diagnostic left on either target is
    [`ezlopi_core_cjson_macros.h:123`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_cjson_macros.h#L123),
    the `CJSON_GET_VALUE_INT` population described above: **87** expansions
    from 87 distinct call sites on esp32, **83** from 83 on esp32c3, all
    reported at that one line and all deliberately unchanged. The four-site
    difference is the DHT11 and DHT22 drivers, which do not exist on esp32c3.
  - So the riscv-only asymmetry this diagnostic had on this tree is gone. What
    remains is identical in kind on both ABIs and differs only by which drivers
    the chip has.
  - `-Wdiscarded-qualifiers`: **9** on both targets, the same nine sites —
    eight in `ezlopi_service_uart.c` and
    [`ezlopi_core_wifi.c:189`](ezlopi-core/ezlopi-core-wifi/ezlopi_core_wifi.c#L189).
    Still open; this confirms on xtensa what had only been seen on riscv.
  - Nothing else differs between the two targets at all. The remaining tags are
    site-for-site identical: **36** `-Wcpp` (the `#warning` backlog), **5**
    `-Wunused-variable`, **2** `-Wunused-but-set-variable`, **1**
    `-Wunused-function`, **1** `-Wimplicit-fallthrough=`. Four of those last
    five are in IDF or vendored code (`l2c_ble.c`, `hci_layer.c`, `pt.h`,
    `switch.h`) and are not this project's to fix.

  Images are `1767248` bytes on esp32c3 and `1693456` on esp32. Both changed
  files were confirmed recompiled in the same run as every count above, and for
  the temperature sensor so was its caller `ezlopi_core_ezlopi.c` — a green
  build of a stale object looks identical to a green build of a fix, and this
  document has already recorded three tallies that were wrong for exactly that
  reason.

  The return-type edit failed first, `rc=2`, on `conflicting types for
  'ezpi_prepare'`: the four definitions had been changed and their `static`
  forward declarations left as `int`. That is what the recompile-and-watch step
  is for, and it is the second time in this audit that an incomplete edit
  passed inspection and was caught by the build.

  **Not fixed, and recorded because the next reader will find them.** Two more
  call sites with the identical `uint32_t`-for-`size_t` pattern sit at
  [`ezlopi_service_uart.c:958`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L958)
  and
  [`:966`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L966),
  inside the `#if 0` at `:953` around
  `__uart_loop()`; nothing compiles them, so no build could confirm a change to
  them, and editing code that cannot be verified is worse than saying it is
  there. In the temperature sensor, `ezpi_init()` cannot report a failure,
  `ezpi_notify()` reports success even when `temp_sensor_read_celsius()` fails,
  and three of the five return values are discarded by their callers —
  including the entry point's, at `ezlopi_core_ezlopi.c:159`. One
  `int`-returning declaration against `e_ezlopi_actions_t` also survives, at
  `sensor_0030_oneWire_DS18B20.h:120`; it cannot warn, because
  `ds18b20_sensor()` is never defined or called anywhere in the tree and its
  second parameter is `s_ezlopi_device_properties_t`, a device model this code
  no longer uses. Separately, the three `-Wunused-variable` sites at
  `ezlopi_core_factory_info.c:163-165` are named `ca_certificate`,
  `ssl_private_key` and `ssl_shared_key`, which is worth someone's attention
  for what it suggests about that function rather than for the warning.

  One method correction this forced. The xtensa half of this verification was
  first run in `/opt/ezlopi`, which is the **upstream baseline** tree kept for
  comparison, not a build of the audited tree — and it failed `rc=2` on
  `ezlopi_util_devcfgval.h: No such file or directory`, a header this fork
  adds. The failure was the tree, not the change. There is now a real xtensa
  build of the audited tree, and every esp32 figure above comes from it.

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

- **Five places where a target guard on a definition did not match the guard on
  what referenced it**, fixed across nine files. One put an undefined reference
  into a shipped archive, one makes a build fail with five messages that do not
  name the cause, and three declared functions for targets that define them
  nowhere.

  The first is the DHT11 driver. `dht11.c` wraps its whole body, `readDHT11`
  included, in `#if (ESP32S3 || ESP32 || ESP32S2)`, because the bit-banged
  1-wire timing loop needs the per-target `<target>/rom/ets_sys.h` for
  `ets_delay_us` and no esp32c3 branch was ever written. Its caller,
  `sensor_0015_oneWire_DHT11.c`, carried no guard at all. So on esp32c3 that file
  compiled into a **190 228-byte object whose symbol table contains exactly
  `U readDHT11`** — a reference nothing in the build defines.

  **Nothing reports this, and the reason is worth stating precisely, because it
  is a general hole rather than a quirk of this file.** The object is a member of
  a static archive, and a member is only pulled out if something already needs a
  symbol it defines. The only thing that would is the `EZLOPI_DEVICE_LIST` entry
  for device 15 — which `ezlopi_core_devices_list_modules.h:116` guards on the
  same three-target condition. So on esp32c3 the member is never extracted, its
  dangling reference is never resolved, and the link succeeds.

  The compiler had nothing to say either, and for a separate reason worth
  keeping: `dht11.h` was *also* unguarded, so the call site had a perfectly good
  prototype for a function that the same header's `.c` had just preprocessed
  away. A declaration with no definition is legal C to compile — the obligation
  falls on the linker, and the linker never looked. That is why the fix guards
  `dht11.h` as well rather than only the caller: a prototype is a promise that
  something, somewhere, defines this, and on esp32c3 nothing did. Anything that
  referenced
  `SENSOR_0015_oneWire_dht11` on esp32c3 — a new device-list entry, a scene
  action, anything — would have failed with an error naming `readDHT11`, which
  says nothing about the guard that actually caused it.

  `sensor_0015_oneWire_DHT11.c`, its header, and `dht11.h` now carry the same
  condition `dht11.c` always had, which is exactly the shape sensor-0016's DHT22
  equivalent has always had across all three of its files. Verified
  behaviour-neutral rather than assumed: the esp32c3 object goes from 190 228
  bytes to **1152 bytes with no undefined `dht` symbols**, and the **esp32c3
  image is byte-identical at 1 756 320** — which is the proof that the member was
  never linked. esp32/esp32s2/esp32s3 images are unchanged at 1 683 072 /
  1 373 488 / 1 697 872, and on those three targets the wrapper object still
  carries its normal `U readDHT11` resolved from `dht11.c.obj` at link.

  The same drift elsewhere is **not** silent, which is what makes it worth a
  check rather than a one-line fix. `ezlopi_service_led_indicator.c` is one
  `#if (S3||C3) ... #elif ESP32 ... #endif` chain with no esp32s2 arm, but the
  five calls to those effects — line 301 and the switch at 331 — sit *outside*
  the chain. `CONFIG_EZPI_ENABLE_LED_INDICATOR` is `default y` at
  `ezlopi-main/Kconfig.projbuild:546` and `# ... is not set` in all four
  `sdkconfig.defaults.*`, and that is the only reason this never showed: the file
  preprocesses to nothing in every shipped configuration. Turn the option on for
  esp32s2 and the build failed with `undefined reference to
  ezpi_indicator_led_init` plus four `used but never defined` warnings naming the
  effects — five messages, none of which mentions a missing target arm.

  The guard now requires an implemented target, with an `#error` on the
  `#elif` that says so. Measured on esp32s2 with the option forced on: the build
  stops at `ezlopi_service_led_indicator.c:656` with that one message, and
  **0 undefined references and 0 "used but never defined" warnings**. An esp32s3
  control with the same option forced on still builds (1 698 400 bytes against
  1 697 872 with it off, so the option genuinely does add code there) and links
  `EZPI_service_led_indicator_init` — so the failure is specific to esp32s2, not
  to the option.

  A `depends on IDF_TARGET_...` in Kconfig would also work, and the idiom is in
  the same file three lines below this option. It is deliberately not used here:
  it would encode "esp32s2 never has an indicator LED" into the configuration
  system, which is a product decision, where the `#error` states the current
  implementation limit without foreclosing it.

  The remaining three are prototypes declared for targets that define them
  nowhere, each now matching its definition:

  | header | was | definition's guard |
  | --- | --- | --- |
  | `ezlopi_service_uart.h` | `!ESP32` | `!ESP32 && !ESP32C3` |
  | `sensor_0016_oneWire_DHT22.h` | unguarded | `S3 \|\| ESP32 \|\| S2` |
  | `sensor_0018_other_internal_hall_effect.h` | unguarded | `ESP32` |

  The uart one is the instructive case: the two guards were **exactly one target
  apart**, so `EZPI_SERV_cdc_init` was declared on esp32c3, where no definition
  exists. Nothing calls it there today, which is the only reason the link
  succeeds. For sensor-0018 the `.c` body and the device-list entry were both
  already guarded on `ESP32`; the prototype was the only piece that was not,
  leaving esp32s2, esp32s3 and esp32c3 with a declaration and no definition. For
  DHT22 the header was the one piece of that component still promising esp32c3
  an implementation.

  Also removed a dead `#elif CONFIG_IDF_TARGET_ESP32C3` arm in `dht22.c` that
  included `esp32c3/rom/ets_sys.h`. It sits inside a block whose `#if` excludes
  esp32c3, so it could never be taken — it read as esp32c3 support that does not
  exist.

- **New check: `tests/ci/check_target_guard_symmetry.py`.** Fixing five sites
  does nothing about a sixth, and the archive-member hole above means the sixth
  would be just as invisible. This evaluates every `#if`/`#elif`/`#else`
  condition in the first-party tree once per target and, for each target
  separately, requires that every call and every prototype left active has a
  definition that is also active. 495 files; 26 of them carry target guards
  across 33 distinct conditions.

  Conditions are *evaluated*, not pattern-matched, because set algebra over
  target names cannot express the shapes actually in the tree —
  `#if !defined(A) && !defined(B)`, `#ifndef`, and `#elif` chains. There is a
  small recursive-descent evaluator over `||`, `&&`, `!`, parentheses,
  `defined()` and integer literals, with unknown symbols propagating as UNKNOWN
  and UNKNOWN treated as active, so a non-target `#if` never causes a finding.

  **Five bugs in the check were found and fixed before it was trusted**, and they
  are recorded in the script because each would have made it quietly wrong rather
  than noisy: calls on a one-line definition's own line were excluded along with
  the definition (fixed by excluding by name offset, not by line); a block of
  static forward declarations counted as calls; `static` definitions were pooled
  tree-wide, so an unrelated file's static could stand in as a definition;
  `return readDHT11(...);` matches a prototype regex perfectly (fixed by
  rejecting statement keywords); and backslash continuations were not joined, so
  the check read the new three-line LED guard as its first line only, found no
  target mentioned, and silently discarded the restriction.

  **Then mutation-tested against the real tree, not just its own fixtures** —
  its `SELF_TEST` cases are strings in the same file, which is not evidence that
  it can fail. Five of the fixed sites were reintroduced into a scratch copy one
  at a time — the DHT11 wrapper, the DHT11 header, the uart prototype, the
  hall-effect prototype and the LED guard, with the uart case narrowed by exactly
  one target rather than unguarded, since that is how it originally read. All
  five were caught, each naming the right file, and baseline and restored both
  passed at 495 files. The LED mutation is the one that exercises the
  continuation-line handling, which is why it is applied by script rather than
  `sed`. `sensor_0016_oneWire_DHT22.h` is the one fixed site not covered this
  way; it is the same shape as the DHT11 header, which is.

  Its limits are in the script. The main one: it cannot expand object-like
  macros, so a guard hidden behind a helper macro reads as UNKNOWN and is
  skipped. That is why the LED condition is written out in all three places it
  appears rather than factored into a macro — the preprocessor alone should show
  which targets have an implementation.

### Testing — [infra]

- **New `ezlopi-util-sanitize` component** holding the untrusted-input helpers
  (`EZPI_util_json_value_end`, `EZPI_util_redact_secrets_in_place`,
  `EZPI_util_clamp_copy_len`, `EZPI_util_copy_str_bounded`). It is deliberately
  dependency-free — `idf_component_register` is called with an empty `REQUIRES`
  — so the firmware and the host tests compile *the same source*, rather than
  the tests exercising a copy that can drift.

- **New host test suite, [`tests/host/`](tests/host/)**: 4747 assertions across
  twenty-two binaries, no framework, no hardware. `make -C tests/host` builds and runs
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

  That has stayed the practice for tests added since, and the entries say so
  where it applies — `test_i2c_shared_bus.c` was run against a copy of the HAL
  with the fix removed and fails there on 4 of 30 checks, with the untouched
  paths still passing. Where a counterfactual was *not* run, the entry says that
  instead.

- **New host test for the GPIO pin bound,
  [`tests/host/test_cjsongpio.c`](tests/host/test_cjsongpio.c)** — 59 checks
  compiling the real `ezlopi_core_cjson_macros.c`, not a copy of it. See *Device
  configuration* for the defect. Two things about this one are worth recording
  beyond the usual.

  **It measures both directions.** The obvious direction is that out-of-range
  pin numbers are refused: remove the range clause and 30 checks fail. The
  direction that actually needed measuring is the opposite one — that ordinary
  configurations still work — because the changelog already records an earlier
  attempt at pin validation that was withdrawn for refusing a normal two-sensor
  I²C config. So the `255`-means-unused sentinel has its own mutation, and
  removing that clause fails exactly one check. A bound that is too tight breaks
  working hardware, and a test that only pushes from one side cannot see it.

  **The table is the verdict; the sanitizer is a second opinion.** The first
  version of the comments in this file claimed UBSan would abort on the
  out-of-range shift. It would not have: gcc does not include
  `-fsanitize=float-cast-overflow` in `-fsanitize=undefined`, gcc's default on a
  finding is to print and continue, and nothing in this tree sets
  `UBSAN_OPTIONS` — so a finding would have been reported on stderr by a process
  that then exited 0. The rule now passes both `-fsanitize=float-cast-overflow`
  and `-fno-sanitize-recover=`, scoped to this one binary, and both mutations
  were confirmed to fail with sanitizers switched off entirely. Which is the
  property that matters: the assertions catch the defect on their own, and the
  sanitizer is there to catch a *different* defect than the one being tested
  for.

  A side effect of making a finding fatal was worth a line in `main()`: the
  first run of the deliberately-broken subject printed the sanitizer message and
  none of the thirty `FAIL` lines before it, because `make` reads stdout through
  a pipe and the immediate abort discarded the buffer. The failure was visible;
  *which* checks failed was not. `setvbuf(stdout, NULL, _IOLBF, 0)`.

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

- **New CI check that device-configuration pin numbers stay bounded,
  [`tests/ci/check_gpio_accessor.py`](tests/ci/check_gpio_accessor.py).** Same
  reason as the chunk-clamp check: the fix was right and nothing kept it right.
  Sixteen pin reads across five drivers were on the general-purpose integer
  readers rather than the GPIO accessor, so the bound that makes an
  out-of-range pin fail instead of aliasing onto a real one did not apply to
  them — and every driver in this tree was visibly written by copying another,
  so a seventeenth is a matter of time. See *Device configuration pins* above
  for the aliasing itself.

  It enforces two rules, because either one failing alone removes the protection
  from all of them: a pin key is read through `CJSON_GET_VALUE_GPIO`, and
  `EZPI_core_cjson_get_value_gpio()` still bounds against `GPIO_NUM_MAX`. The
  second is the quieter failure — delete that comparison and all 144 call sites
  keep compiling while protecting nothing.

  Two deliberate design choices, each because the obvious version would have
  missed a real site. **The pin keys are derived, not listed**, from the
  `ezlopi_gpio*_str` declarations in
  [`ezlopi_cloud_keywords.c`](ezlopi-cloud/ezlopi-cloud-constants/ezlopi_cloud_keywords.c)
  — 15 of them today — so a pin key added later is covered without editing the
  check. **Bare string literals count too**, because `pms_dev_items_prepare.c`
  read its fourth pin as a literal `"gpio4"` rather than through
  `ezlopi_gpio4_str`, and a check keyed only on the keyword variables would have
  passed over precisely the least protected site in the tree. Conversely the 26
  reads whose *destination* mentions gpio but whose key is a flag
  (`ezlopi_logic_inv_str` into `item->interface.gpio.gpio_in.invert`, and
  friends) must not be flagged, and `SELF_TEST['flag_not_a_pin']` pins that down.

  Verified by mutation against the real tree in a scratch copy, not only by its
  self-tests: undoing one of the sixteen reroutes is reported at the right line,
  undoing the bare-literal one is reported, and replacing the accessor's
  `GPIO_NUM_MAX` comparison is reported — the last one while `GPIO_NUM_MAX` still
  appears twice elsewhere in that file, which is what shows the check is reading
  the function body rather than grepping the file. Baseline and restored copies
  both pass. A literal bound is rejected as well as a missing one, because
  `GPIO_NUM_MAX` is 40 on esp32 and 22 on esp32c3, so a hard-coded 40 would be
  wrong on three targets out of four.

- **New CI check that one item `user_arg` has one owner,
  [`tests/ci/check_user_arg_owner.py`](tests/ci/check_user_arg_owner.py).** The
  bug, the four drivers and the reason neither heap configuration could report
  it are under *Four drivers asked for one allocation to be freed up to six
  times* above; what belongs here is how the check was validated.

  It was run against the tree **as committed before the fix**, with nothing
  altered but the check dropped into `tests/ci/`, and it named exactly the four
  offending drivers with their owner counts — 6, 5, 3 and 2 — plus the 2-axis
  joystick's redundant pair. That is the test that decides whether a green run
  on the fixed tree means anything, and it is stronger than a self-test,
  because a self-test only shows the scanner matches text its author chose.

  Three more mutations, each breaking one invariant on the fixed tree: putting
  a second owner back into BME280 is reported at the right line; making
  `ezlopi_device_free_item()` stop honouring the flag is reported, which matters
  because that deletion leaves every driver compiling while every shared
  `user_arg` leaks; and removing the duplicate-ownership collapse from
  `ezlopi_device_free_single()` is reported, which nothing else would notice
  since no driver can see that loop. Restored, it passes again.

  The scanner strips comments and string literals before counting, and the
  self-test covers that specifically — the fixes landed with explanatory
  comments that quote the literal text `is_user_arg_unique = true` while
  describing what used to be there, so a naive counter would have failed on the
  very files it had just corrected.

- **[`tests/ci/check_ui_frame_redaction.py`](tests/ci/check_ui_frame_redaction.py)
  was written, committed, and never wired into a workflow.** It ran nowhere. It
  is in `ci.yml` now. Found by asking, for each of the 13 `tests/ci/check_*.py`,
  whether any workflow file names it — twelve did. A check that exists and never
  runs is worse than no check, because it reads as coverage in exactly the place
  someone would look for it: this one is the only thing standing between
  `hub.network.get`'s reply and the Wi-Fi PSK appearing in the local
  configuration page's traffic log.

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
  | `hygiene` | the static checks, 17 of them, each a `tests/ci/check_*.py` named in the job rather than a glob — so a check that exists and is never invoked shows up as an orphan. Started as five subjects (no hard-coded Ezlo telemetry endpoint; telemetry not enabled in committed defaults; base and per-target `sdkconfig.defaults` all present; JSON string destinations initialised; no pinned `sdkconfig.h` includes) and every entry added since is described where its fix is, not here, so that this row cannot drift out of date again |
  | `firmware` | builds all four targets in `espressif/idf:v4.4.6`, asserts the generated config was actually inherited from the per-target defaults, merges and verifies a flashable image, uploads `bin`/`elf`/`map`/`sdkconfig` |
  | `firmware-outdir` | builds esp32 with `idf.py -B /tmp/...`, outside the workspace, and asserts the image landed there and that **no `./build` was created at all** — so the tree cannot regress to a build directory it only builds in by default |
  | `firmware-ota` | builds esp32 with `CONFIG_EZPI_ENABLE_OTA=y`, which no committed default sets, and asserts the symbol really got enabled, that the pinned header is still the default CA source, and that `ca_mios` reached the ELF |
  | `firmware-ota-bundle` | builds esp32 with OTA on and `CONFIG_EZPI_OTA_CA_BUNDLE=y`, from a tree with **no `ca_mios.h`**, and asserts the header is absent, `ca_mios` is *not* in the ELF, and `esp_https_ota` still is |
  | `firmware-improv` | builds esp32 with `CONFIG_EZPI_BLE_ENABLE_IMPROV=y` and asserts both that the symbol got enabled and that the service is in the ELF |
  | `firmware-mqtt` | builds esp32 with `CONFIG_EZPI_ENABLE_MQTT=y` and **both websocket transports off** — the local-only Home Assistant configuration — and asserts the broadcast queue is still installed and that no mDNS service is advertised on a port with no listener |
  | `firmware-mdns-extra` | builds esp32 with all five `CONFIG_EZPI_MDNS_ENABLE_*` symbols on, the only configuration that compiles the manufacturer, brand, model and name TXT blocks of `ezlopi_core_mdns.c` at all, and asserts each symbol in both `sdkconfig` and the generated header. Described under *Ownership of an mDNS TXT value* above |
  | `install-page` | assembles and verifies [`install/`](install/) from the firmware artifacts |
  | `deploy-install-page` | publishes it, once a maintainer opts in |

- **Several jobs exist only to compile code the other builds skip.**
  `ezlopi_core_ota.c` is wrapped in `#ifdef CONFIG_EZPI_ENABLE_OTA`, the Improv
  service and advertisement sit behind `CONFIG_EZPI_BLE_ENABLE_IMPROV`, and four
  of the TXT-record blocks in `ezlopi_core_mdns.c` sit behind
  `CONFIG_EZPI_MDNS_ENABLE_EXTRA_SERVICES`. None of those symbols is set in any
  committed `sdkconfig.defaults.*`, so the four target builds compile *none* of
  them and a green run said nothing about them. Each job turns one feature on
  for one target.

  The mDNS one is the clearest demonstration that this matters rather than being
  tidiness: the free-of-a-string-literal fixed under *Ownership of an mDNS TXT
  value* above lived in those four blocks specifically, and could not be reached
  by the two blocks that every build does compile. The defect sat in exactly the
  code CI did not compile, for as long as the file had existed.

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

- **Every fix in the hilink settings file was proved by reversing it, and the
  five reversals that prove nothing are recorded as such.** Each fix in
  `hilink_presence_sensor_setting.c` was undone on its own, the suite rebuilt
  and rerun against it, and the subject restored and checked byte-identical by
  sha256 before the next reversal. 30 reversals; after the third round 25 of
  them turn [`tests/host/test_hilinkset.c`](tests/host/test_hilinkset.c) red
  and 5 do not.

  The three rounds are the point, because the first one was mostly a report on
  the test file rather than on the subject:

  | Round | Checks | Reversals | Leave the suite green |
  |---|---|---|---|
  | 1 | 58 | 25 | 9 |
  | 2 | 72 | 30 | 6 |
  | 3 | 75 | 30 | 5 |

  Round 2 added 14 checks and 5 further reversals: the new checks closed six of
  the nine blind spots — the apply-path and reset write results, the radar's
  refusal of a custom template, the no-payload apply path, the reset guard
  testing the wrong pointer, and a leaked reset string — while the five new
  reversals opened three more. Round 3 added 3 checks to close one of those,
  the predefined set path overwriting its own write result, which had been
  reported as success with nothing noticing.

  Of the 25 that do fail, 16 fail a named check and 9 are caught only by a
  sanitizer or by the stub that aborts: 5 LeakSanitizer leaks with a 0-failure
  tally, 3 AddressSanitizer faults that print no tally at all, and the
  `ESP_ERROR_CHECK`-restoring reversal, which exits on `SIGABRT`. A suite that
  only read the tally would score those 9 as passes, which is the argument for
  running the host binaries under sanitizers rather than treating the printed
  count as the result.

  The five that stay green are not written off: they are listed with their
  reasons at the foot of that test file, and three of them are one underlying
  defect, recorded under Known gaps. The harness that drives all this is a
  local scratch script and is deliberately not committed, so the evidence here
  is the record of what it measured rather than something a reader can re-run.

- **The guard-after-use scanner was skipping the directory most of this
  branch's defects were found in.**
  [`tests/ci/check_check_after_use.py`](tests/ci/check_check_after_use.py)'s
  `DIRS` named seven directories and `ezlopi-sensors-devices` was not one of
  them, leaving 88 driver sources outside the scan entirely. Adding it takes
  the scan from 163 files to 251 and reports **zero** new findings, so it costs
  nothing and uncovers no backlog.

  It is recorded anyway, because the honest reading of that zero is
  unflattering. The one defect of exactly this class known to have existed in
  that tree — the `__add_value_to_cjson()` guard in sensor 0067, described
  above — would still not be reported, since the scanner's precision anchor
  requires the pointer be assigned from a *call* and that one is a cast of a
  parameter's field. Checked by running the scanner's own `ASSIGN_CALL` pattern
  against that exact line: no match. So this widens coverage to future code of
  a shape the tree does not presently contain; it does not detect the thing
  that prompted it. The payload-cast shape stays in Known gaps as needing a
  check of its own.

  Self-test unchanged at 16 fixtures, with the 8 must-fire ones still flagged
  and the 8 must-be-silent ones still clean.

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
- **The citations in these documents are now checked**, by
  [`check_doc_citations.py`](tests/ci/check_doc_citations.py) — the twentieth
  check and the seventeenth the `hygiene` job invokes. This file is the main
  thing this fork produces, and these documents carry 459 markdown links,
  320 of them with a `#L<n>` fragment. (This said 310 *links* when the entry
  was written; it is re-measured whenever a paragraph below quotes it, and
  whenever an entry added above adds citations of its own.) Line
  numbers move whenever anything
  above them is edited, and nothing in the toolchain notices: a link to a
  renamed file, or to a line past the end of one, renders as ordinary text and
  reads as evidence. Entries here turn on specific lines — "returns `NULL` on
  three paths", "the guard is the next block down" — so a citation that points
  nowhere is worse than no citation at all.

  It found one, which is why it exists rather than being deferred a fifth time:
  `ezlopi_core_factory_info.c:140` linked to `#L135`. The prose meant the
  `arg = NULL;` statement and the link pointed at the function signature five
  lines above it. Each number was right about something; the pair was wrong,
  and that is the one shape a reader has no way to spot. The link now says
  `#L140`.

  Four mechanical failures are caught — the path is gone, the file is shorter
  than the cited line, the link text ends `foo.c:123` while its own fragment
  says `#L456`, or the link text is one backticked code token that is not on
  the cited line. What it still cannot do is judge a citation whose text is
  prose: "appended", "the per-chunk dump" and the like have nothing to check
  against. **78** of the **320** citations with a line fragment are in that
  class — the fourth rule bears on the **140** that are a single backticked
  token and the third on the **102** whose text ends `:N`. This said "150 of
  the 200" when the rule was added; the proportion improved because citing by
  token became the habit here, not because the rule was loosened.
  The docstring says so, so that a green run is not read as more than it is.
  Links to directories are skipped, because `tests/host/` is linked that way
  deliberately.

  **That fourth rule was declined once, on measurement, and then added when the
  measurement changed.** The HTTP body-logging change under *Credentials*
  inserted 63 lines into `ezlopi_core_http.c` and moved four citations in older
  entries; two then pointed at a blank line and at an unrelated
  `mbedtls_net_free()`. The check passed on all four — correctly, by its own
  rules — and the four were repaired by hand. The strengthening considered then
  was a *window*: the token within ±12 lines. Over the 47 citations that
  qualified, 42 were exact, 4 within twelve lines and 1 twenty-one away, so the
  window flagged exactly one citation and that one was right — `GATED_SWITCHES`
  was cited at the table entry, not at the table. The window that spared it was
  twenty-five lines, which also spared the fourteen-line error that prompted
  the exercise, and no ambiguity filter separated them: the token to catch
  appeared 7 times in its file, the token to spare 10. Declined on those
  numbers.

  The next change into the same file — the `target_page` fix under *Scene HTTP
  requests* — rotted **six** more citations in the same older entries, and that
  is what changed the answer. Re-measured after repairing them by hand, all
  **50** qualifying citations sit on the **exact** line. No window, so no
  tolerance to tune and no exception to carve out. The one citation that had
  forced a window was fixed at the source rather than exempted: it now links
  twice, once at `GATED_SWITCHES` for the name and once at the entry for the
  pin, so each link text names what its own line contains. A rule with no
  exceptions is a different proposition from a rule with one, and that is the
  shape that was worth gating on. Two refinements are about notation and not
  tolerance — a trailing `()` is dropped, and a `CONFIG_` prefix is dropped
  because `Kconfig.projbuild` writes `config EZPI_FOO` while the prose cites
  the name the generated `sdkconfig` uses.

  **Seen to catch the real thing, not just a fixture.** On a full copy of the
  repaired tree the check exits 0 with no `ANCHOR` findings; with the six
  rotted numbers put back it exits 1 and names all six, each by file, line and
  missing token. The three older rules report **nothing** on those same six —
  which is the point, since every one of them resolved to a real line inside a
  real file, and that is precisely why they shipped unnoticed twice.

  Proved able to fail four ways before being trusted: against the committed
  `CHANGELOG.md` it exits 1 on the original mismatch; against a shadow tree
  where one cited source file has been renamed away it reports the three links
  that cited it; against the six-rot copy above it reports all six; and a copy
  with its `return problems` replaced by `return []` exits 1 with
  `SELF-TEST FAILED` rather than passing silently. **Fourteen** fixtures run
  first, **ten** of them correct links — including an external URL and a bare
  `#anchor`, neither of which is this check's business, and a two-letter
  backticked token, which is too short to be read as a citation claim. The two
  `ANCHOR` fixtures look their own line numbers up at import time instead of
  hardcoding them: a self-test for citation rot that could itself rot would be
  testing something weaker than the rule, and a needle that stops matching
  raises rather than quietly turning the fixture into a no-op.

  **It then went red a third time, on 24 citations, and they are repaired.**
  All 24 pointed into
  [`ezlopi_core_scenes_when_methods_helper_functions.c`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c),
  from entries written before the later scenes work edited that file. Two
  distinct shifts — **−44** and **−9** — so two separate commits moved them,
  and neither re-read what rested on the lines it was moving. The check's own
  docstring already recorded this happening twice; this is the third and the
  largest, which is the argument for the rule existing rather than against it.

  **Three of the 24 had moved file, not line**, and that is the interesting
  class. `s_sunstate_data_t` was relocated from the `.c` into the header so a
  host test could construct it, taking
  [`moment_tm_day`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.h#L143),
  [`moment_valid`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.h#L144)
  and
  [`backoff`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.h#L155)
  with it. Nothing but the fourth rule can see that: the `.c` still exists, so
  MISSING passes, and it is 2338 lines long, so SHORT passes on a citation of
  line 108. The `.c` carries a comment at
  [`line 72`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L72)
  saying where the type went, which is how the new lines were found.

  **Four more citations into the same file were invisible to the check and were
  re-read by hand.** 28 lines carried a citation into it when the gate went
  red, and 24 of the 28 were flagged. Two of
  the four are worth naming, because they show what the third rule is and is
  not: their link text ended `:547` and `:597` and their own fragments said
  `#L547` and `#L597`, so MISMATCH compared the pair, found them consistent,
  and passed. MISMATCH asks whether a citation agrees with itself, not whether
  it is true, and two numbers that rot together rot invisibly. They are now
  `:503` — the
  [`EZPI_core_suntime_should_attempt`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L503)
  gate the per-second `TRACE_S` moved behind — and `:553`, the comment the
  paragraph above quotes. The other two were a prose link text and a path used
  as a label, which no rule touches.

  **Each of the 17 distinct targets behind those 28 citations was re-derived
  from the token it names, not from a shift applied to the old number.** That
  is not pedantry here: the shifts
  were −44, −9, and two file moves, in one file, so any citation renumbered by
  assuming a uniform offset would have been wrong in a way the check cannot
  distinguish from right — the token would simply be somewhere else.

  After the repair the gate exits **0**: 14 fixtures behaving as declared — the
  4 that must fire firing, the 10 that must not staying clean — and **446**
  links examined across six documents with no finding. Both halves are read,
  because `main()` returns 1 for either and a quiet run proves neither.

  **One limitation was measured and left alone.** `main()` lists `*.md` in a
  single directory, so `docs/` is unscanned — and widening that glob would
  cover almost nothing, because `LINK` requires a repo-root-relative path and
  every citation in a subdirectory document is written `../ezlopi-core/…`,
  which begins with a dot. The three files in `docs/` carry **23** local links
  and the regex can see exactly **1**. Covering them means resolving each link
  against its own document's directory. That is a real change, not a wider
  glob, and it is recorded rather than attempted here.
- Added this file and [SECURITY.md](SECURITY.md).

---

## Known gaps

Listed because a changelog that only mentions what was finished is misleading.

- **The `ESP_ERROR_CHECK`-on-plain-`int` class is now closed, but the wider
  policy of aborting on a return value is not.** The census behind *Device
  settings* found 102 `ESP_ERROR_CHECK` call sites tree-wide with a direct
  function call, 39 of them on a callee whose only return type anywhere in the
  tree is plain `int`. Ten were fixed in
  `sensor_0068_ENS160_gas_sensor_settings.c`, nineteen in
  `hilink_presence_sensor_setting.c`, one in
  `sensor_0067_hilink_presence_sensor.c`, and the last **9** in
  `pms_dev_items_prepare.c` — see *Particulate matter sensor*, where the nine
  turned out to abort on success rather than only on failure. All four files
  are now at zero, re-confirmed by counting call sites with comments stripped.

  Re-derived repo-wide over the 700 tracked `.c` and `.h` files: **60**
  `ESP_ERROR_CHECK` call sites remain, in 14 files, 58 of them passing a call
  directly. The drop from the 69/15/67 recorded before the PMS5003 fix is
  exactly those nine, and `pms_dev_items_prepare.c` leaves the list entirely,
  which is where the one-file difference comes from. The figure is measured each
  time rather than carried forward by subtraction. It still does not reconcile
  with 102 less the 39, and the 102 is not reproducible from here because this
  entry never recorded the scope it was taken over; 60 is the current
  measurement and the 102 is kept only as the history of how the 39 was arrived
  at. The two largest holdings are *not* the plain-`int` class — `TCS230.c` has
  20 and `sensor_bme280.c` 17, on genuine `esp_err_t` returns, which is the
  wrong policy correctly typed. The same is true of the two remaining sites in
  the PMS5003 directory, `pms5003.c:220` and `:224`, on `esp_timer_create()`
  and `esp_timer_start_periodic()`: both really do return `esp_err_t`, so they
  are a policy question about whether a failed timer should take the hub down,
  not the type confusion that the nine were.

  Counting by name rather than by call site inflates all of this. Both hilink
  files still contain the string `ESP_ERROR_CHECK` — 5 times and 6 times
  respectively — and every one of those is inside a comment explaining why the
  macro was removed. With comments stripped, the call-site count in each file
  is zero. A bare `grep -c` on either file reports otherwise and is wrong.
  `pms_dev_items_prepare.c` has now joined them: one occurrence of the string,
  zero call sites, because the comment above the two new return-code macros
  names the macro it exists to keep out. Repo-wide the same gap is 71 against
  60 — eleven of the apparent sites are prose about the other sixty.

  This entry previously read *"Twenty-nine more"* and counted the nineteen
  hilink settings sites among them, then *"Ten more"* while one
  `sensor_0067_hilink_presence_sensor.c` site was still open, then *"Nine
  more"* for the PMS5003 set. All are now fixed, so the figure for this class
  is zero and the heading above no longer counts anything.

- **The census was never going to find five of the six `ESP_ERROR_CHECK` sites
  in `sensor_0067_hilink_presence_sensor.c`, and that blind spot is still
  open.** Those six are fixed — see *UART sensor dispatch* — but the reason only
  one of them appeared in the count above is worth keeping, because the
  criterion is still how any future census of this would be taken. The
  criterion is "callee whose only return type anywhere in the tree is plain
  `int`", which in that file matched exactly one site,
  `HILINK_presence_sensor_apply_settings()`. Counting every `ESP_ERROR_CHECK`
  in the file instead gave **6**: one on `int`, three on `ezlopi_error_t`
  (whose failure value is `-1` and is not an `esp_err_t` code at all), and two
  on `ld2410_get_data`, which *does* return `esp_err_t` and so was correctly
  typed while still being the wrong policy — one of those sat on the 1-second
  item-update path. A return-type census cannot see the last three classes, so
  the 60 above is a lower bound on the remaining work, not a total.

  The PMS5003 nine added a fourth thing the criterion cannot see, and it is the
  worst of them: the *direction* of the convention. Those nine matched the
  criterion perfectly and were duly counted, but what the count recorded was
  "aborts on a value meaning failure" when the truth was "aborts on every value,
  including the one meaning success". The census tells you a site is suspect; it
  cannot tell you which way round the callee's convention runs, and the two
  readings differ by whether the sensor is merely fragile or completely
  unusable. Reading the callee is not optional.

- **`check_check_after_use.py` would not catch the hilink guard-after-use even
  though it now scans the driver tree.** This entry originally reported two
  independent gaps. The first is closed: `ezlopi-sensors-devices` has been added
  to `DIRS`, taking the scan from 163 files to 251 with zero new findings, and
  that change is described under Testing above.

  The second remains, and it is the one that mattered. The scanner's precision
  anchor requires the pointer be assigned from a *call* inside the function, and
  `ld2410_outputs_t *hilink_data = (ld2410_outputs_t *)item->user_arg;` is a cast
  of a parameter's field, while `item` itself is a parameter. Checked by running
  the scanner's own `ASSIGN_CALL` pattern against that exact line: no match.
  Widening the anchor to any assignment is not attempted, because the
  40-false-positives-in-41 history recorded at the top of that file is what the
  anchor exists to prevent; the payload-cast shape needs a check aimed at
  `item->user_arg` instead, which is the one scoped under Device settings above
  — three implementations, 30 payload-read sites.

- **A second hilink presence sensor on one hub is broken by design.**
  `hilink_presence_sensor_setting.c` keeps all three setting nodes in
  file-scope statics, so initializing a second sensor overwrites the first's.
  One consequence was fixed here —
  `__setting_update_user_defined_setting` now reads the node it was handed
  rather than the global — but the general problem is untouched and is **not**
  claimed as fixed.

  How much this costs in evidence is now measured rather than asserted. Of the
  30 one-at-a-time reversals of this file's fixes, 25 turn the suite red and
  **5 leave it green**, and three of those five are this defect: two restore
  the NULL guards on the file-scope nodes, and one puts back the global read in
  `__setting_update_user_defined_setting`. All three are invisible to a
  single-device test, because with one sensor the global and the handed-in node
  *are* the same node. The other two unproven reversals are the `false` arm of
  the apply path — which differs only when the global's payload is missing
  while the caller's is not, the same one-device limitation — and the missing
  `else` on an unserializable setting, which needs `cJSON_PrintBuffered` to
  fail while `cjext.c` is compiled without the allocation-failure stubs on its
  include path. All five are listed with their reasons at the foot of
  [`tests/host/test_hilinkset.c`](tests/host/test_hilinkset.c), which is where
  a reader looking at a green 75-check run would have to go to learn that five
  known defects would not disturb it.

- **No driver can report an initialisation failure, because every error code is
  the wrong sign.** `__EZPI_initialize_devices_v3()` drops a device when an
  item's `EZLOPI_ACTION_INITIALIZE` returns `< 0`
  ([`ezlopi_core_ezlopi.c#L239`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L239),
  freed at
  [`#L307`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L307)).
  `ezlopi_error_t` is a `typedef int32_t` whose only non-positive values are
  `EZPI_SUCCESS` (0), `EZPI_FAILED` (-1) and `EZPI_NOT_AVAILABLE` (-2); every
  named `EZPI_ERR_*` code is a positive bit flag. Brace-matching every
  `static ezlopi_error_t __init*` body under `ezlopi-sensors-devices/` gives
  **64** init functions, of which **32** return
  [`EZPI_ERR_INIT_DEVICE_FAILED`](ezlopi-core/ezlopi-core-errors/ezlopi_core_errors.h#L73)
  — `0x200`, positive — and **none** returns `EZPI_FAILED`,
  `EZPI_NOT_AVAILABLE`, or a negative literal.

  So the gate is dead, and the constant documented as the way to get a device
  "deleted from the tree" is numerically incapable of doing it. A sensor whose
  I²C bus never came up, or whose radar never answered, is kept in the device
  list and published to the cloud as a working device that reports nothing.
  That is worse than the drop it was meant to cause, because a missing device
  at least tells the owner to try again.

  Not fixed here, and deliberately so: changing the test to `!= 0`, or the
  constants' sign, would begin dropping devices from 32 drivers simultaneously
  on the first boot after the change, including on hardware nobody here can
  test. It needs a decision about which drivers *should* be fatal, taken by
  someone who can watch a hub come up. The logging that names a dropped device
  is already in place for when it does fire — see the corrected entry under
  *Core device init* above.

- **Seven `int32_t *` pointer-type mismatches are invisible on every target this
  project ships.** Building the tree for `esp32c3` while verifying the hilink
  change surfaced **7** `-Wincompatible-pointer-types` sites that produce
  **zero** warnings on `esp32`, `esp32s2` and `esp32s3`:
  `ezlopi_core_log.c:109` and `:110`, `ezlopi_hal_uart.c:282`,
  `ezlopi_service_system_temperature_sensor.c:201`, `ezlopi_service_uart.c:707`
  and `:715`, and `hilink_presence_sensor_setting.c:430`.

  The cause is one typedef: `int32_t` is `int` on xtensa and `long int` on
  riscv32, so an `int *` handed to a parameter declared `int32_t *` is a
  same-type pointer on the three xtensa targets and a different-type pointer on
  the one riscv target. Nothing misbehaves at runtime — both are 32-bit with
  identical representation — so this is a portability and strictness fault, not
  a live bug, which is exactly why it has survived: the warning cannot appear
  unless somebody builds the riscv target.

  Not fixed here. The hilink one sits in this commit's file but predates it and
  is a separate concern from the error-handling this commit is about; the other
  six are in files with no host coverage. Distinct from the
  `-Wdiscarded-qualifiers` sites, which do fire on all four targets and are also
  still open — the two were briefly conflated by a grep loose enough to match
  both tags, and are counted separately here.

  **Both counts in this entry have since been superseded, and the way they were
  taken is the reason.** They came from incremental builds, and an incremental
  build cannot produce a tree total: a file that is not recompiled emits no
  diagnostics, so the figure is "warnings among whatever happened to rebuild".
  Worse, the esp32c3 build tree had drifted **77 of 663** tracked C and header
  files behind the audited tree, so recompiling the files named here measured a
  mixture of current and stale sources — at one point resurrecting the hilink
  site that commit `1629ec0` had already fixed, purely because the build tree
  still held the old copy of that file.

  Re-derived properly, from a `fullclean` build of a freshly synced tree
  (**1491** objects compiled, `rc=0`, so every file in the tree was asked):

  - `-Wincompatible-pointer-types`: **4** distinct sites in `.c` files on
    esp32c3 at that measurement — `ezlopi_hal_uart.c`,
    `ezlopi_service_uart.c` twice, and
    `ezlopi_service_system_temperature_sensor.c`. The hilink site was already
    gone, and the two in `ezlopi_core_log.c` are fixed under *Log severity*
    above. **All four are now fixed as well**, so this figure is **0** on both
    esp32c3 and esp32 — see *The last four pointer-type mismatches* under
    *Build and configuration* above, which carries the current whole-tree
    numbers for both ABIs. The line numbers quoted in the list further up this
    entry are pre-fix and have since moved; they are left as they were taken,
    since they are the record of a measurement rather than a pointer into
    today's files.
  - `-Wdiscarded-qualifiers`: **9** distinct sites, not 8. Eight are the ones
    named here in `ezlopi_service_uart.c` (`:443-448`, `:534-535`); the ninth is
    [`ezlopi_core_wifi.c:189`](ezlopi-core/ezlopi-core-wifi/ezlopi_core_wifi.c#L189),
    which no entry in this document had ever named because no incremental
    measurement ever recompiled that file. Still open, and since confirmed as
    the same nine sites on esp32 as well, so it is not a riscv-only count.

- **Five markdown links in the root documents are still not examined by
  `check_doc_citations.py`, down from nine, and its "links examined" line
  cannot show that.** Its `LINK` regex matches a href by shape, so a link that
  does not fit is skipped outright rather than reported — not even the MISSING
  rule reaches it. There were **9** such links in three classes; two of the
  classes are now covered and the third is not.

  Fixed here:

  - Three hrefs beginning with a dot — all of them
    `.github/workflows/ci.yml`, in `CHANGELOG.md`, `CONTRIBUTING.md` and
    `README.md`. The leading `.` was not in the first-character class. That CI
    workflow could have been renamed or deleted with the gate staying green in
    three documents at once, which is the one failure mode it exists to
    prevent. The class now admits a leading dot.
  - One line *range*, `ezlopi_core_devices.h#L110-L129`. The fragment now
    accepts a `-L<digits>` tail and checks the first number, which is the one
    that has to exist.

  Still open:

  - Five cross-document anchors, `README.md#6-troubleshooting` and similar. The
    `#` is read as the start of a line fragment and `6-troubleshooting` is not
    digits. Checking these means slugifying the target document's headings the
    way the markdown renderer does, which is a different kind of rule from
    anything else in the file and is left for a change of its own.

  Found while writing the *UART sensor dispatch* entry above, after noticing
  that a `path.c:332` href — the colon form, not `#L332` — had passed the gate
  silently for the same reason.

  **A third class, found by the gate catching its sibling.** Inserting a
  nineteen-line comment into `ezlopi_service_uart.c` moved everything below it,
  and two citations in this document pointed into that region. The gate caught
  one — the citation whose link text was `ezpi_service_uart_set_config()` and
  whose fragment was `#L1399`, because the text names a token it could look for
  and the token was no longer on that line. It could not catch the other, whose
  text was `ezlopi_service_uart.c:1402` against a `#L1402` fragment: the
  text agrees with the fragment by construction, line 1402 still exists, and
  nothing in the citation says what is supposed to be there. It had silently
  come to point at a `break;` in an unrelated `switch`. Both are fixed, at
  `#L1418` and `#L1421`.

  So the gate detects a line anchor that points past the end of a file, or one
  that disagrees with a token named in its own link text — and cannot detect a
  line anchor that has merely drifted onto a different line. The cure is to
  write citations in the token form wherever a token exists, which makes them
  self-describing and checkable; the bare `file.c:NNN` form should be treated
  as unverified by anyone relying on it. Not enforced, because a rule that
  banned the bare form would fire on citations that have no token to name, such
  as a pointer into the middle of a function body.

- **A citation written as `[line 898]` pointing at `file.c#L898` was checked by
  nothing, and one had rotted in both halves at once.** (Written unlinked here
  on purpose: as a real link the gate would read `file.c` as a path and report
  it MISSING — which it did, on the first draft of this entry.)
  `TEXT_LINE` is anchored at the end of the
  link text, so it only ever saw the `` `file.c:898` `` form; a citation
  phrased as prose matched neither it nor `TOKEN`, leaving the line number
  asserted and never tested. Eight such citations exist in `CHANGELOG.md`.
  `CHANGELOG.md:2126` was one of them, and when the file it cites shifted by
  ten lines, the text and the fragment moved together — they agreed with each
  other while both pointed at the wrong line, which is precisely the state the
  gate cannot see.

  Two rules close this as far as it can be closed:

  - `PROSE_LINE` reads a link text beginning "line N" or "lines N" as the same
    claim `TEXT_LINE` reads at the end, and requires it to agree with the
    fragment. This catches the halves disagreeing. It does **not** catch them
    agreeing and both being wrong — that is not detectable from the link alone,
    and saying otherwise would overstate what the check does.
  - `TOKEN` now accepts a trailing `:123` after the closing backtick, so a
    citation whose link text is `` `foo`:123 `` and whose href ends `#L123` is
    verified on both axes at once: the line number against the fragment, and
    the fragment against the line's actual contents. That is the only
    fully-checkable citation form in the file, and the one to reach for when
    writing a new one. (Spelled out in words rather than shown as a link, for
    the same reason as the note above.)

  The eight existing prose citations all currently agree with their fragments,
  so nothing in the documents changed. Converting them to the token form would
  make them content-checked, and is deliberately left alone here: it rewrites
  eight sentences for a reason that has nothing to do with what they say.

- **The device loop discards every driver's 1-second notify return, for every
  driver in the tree.** `__device_loop()` calls
  `item_node->func(EZLOPI_ACTION_NOTIFY_1000_MS, item_node, NULL, item_node->user_arg)`
  at
  [`ezlopi_core_ezlopi.c#L332`](ezlopi-core/ezlopi-core-ezlopi/ezlopi_core_ezlopi.c#L332)
  and does not assign the result to anything. So a driver that correctly reports
  "I could not read my sensor this second" — which
  `sensor_0067_hilink_presence_sensor.c` now does, rather than panicking — is
  indistinguishable to the core from one that succeeded. Nothing is logged, no
  counter advances, and a sensor that has gone silent produces no evidence
  anywhere above the driver.

  Not fixed here for the same reason as the settings-callback entry below: the
  call site is shared by every device in the tree, and giving it behaviour means
  deciding what the hub should *do* about a failing item — log once, log every
  tick, mark the item unavailable to the cloud — on hardware that cannot be
  tested here. Returning the honest value from the driver is the precondition
  for any of those, which is why it was done first.

- **The cloud discards every settings callback's return value.** All four
  `curr_setting->func(...)` call sites in `ezlopi-cloud-settings`, and the one
  in `ezlopi_core_device_value_updated.c`, ignore the `int` the callback
  returns. So a device driver that correctly reports a settings operation as
  failed gets the same treatment as one that succeeded: the cloud sends back a
  settings object with no value in it rather than an error. Propagation was
  added inside the ENS160 file so its own `ret` values are no longer dead, but
  it stops at that boundary. Fixing it means changing shared code on behalf of
  every device that implements settings, which is a larger change than this
  release makes.

- **`ENS160_gas_sensor_settings_init`'s return is discarded by its only
  caller.** `sensor_0068_ENS160_gas_sensor.c:364` calls it and ignores the
  result. The value of making it return a failure instead of rebooting is that
  the hub stays up; nobody acts on the code yet.

- **The addressable strip is configurable now, but two things about it are
  unfinished.** This entry listed three. The colour-order gap — the parser
  accepting all six permutations while `device-0009` could act on exactly one —
  is closed: the driver packs the frame buffer itself with `EZPI_aled_fill()`
  rather than going through `led_strip_set_pixel()` and the strip-type table
  behind it, so the schema no longer promises more than the firmware delivers.
  See *Addressable LED configuration*. It is listed here as removed rather than
  deleted silently, because this section is the one a reader checks to find out
  whether a limitation they hit is known.

  **A changed `pixel_count` needs a reboot, and one of the three routes in does
  not say so.** `__init()` is guarded by
  `if (0 == dimmer_args->sk6812_led_strip_initialized)`, and `led_strip_init()`
  allocates the frame buffer from `length`, so re-shaping a running strip would
  mean `led_strip_free()` plus a re-init rather than an assignment. The config
  is only read in `__prepare()`, which runs at boot. Editing a provisioned
  device's pixel count therefore takes effect on the next restart.

  This entry said "nothing in the firmware says so to the user", and that was
  wrong for two of the three routes: `hub.ezlopi.config.set` has always
  answered `rebootRequired: true`, and the BLE dynamic-config characteristic
  restarts the hub itself about 1.5 s after a config it accepted. It is true of
  serial `cmd:3` and only of serial `cmd:3`, which answers `status_write: 1`
  and leaves a hub storing one pixel count while driving another — the route a
  production jig would use, and the one with no signal at all. Narrowed rather
  than deleted because the gap is real; it was the scope that was wrong.

  **No board has driven a multi-pixel strip.** The parser is host-tested and
  the driver compiles clean on all four CI targets, c3 included, but the
  actuation — RMT timing at length, `is_rgbw` producing a fourth byte per
  pixel, the `>=50 µs` reset gap at 2048 pixels — has never run on hardware.
  Compiling on four targets says nothing about any of that; it only removes
  "it might not even build there" from the list. The 2048 cap in particular is
  derived from the wire protocol on paper and has not been observed to be
  drivable.

  **No strip has been lit in any colour order, `grb` included.** That closed
  gap above is closed *in source*. The permutation is swept on the host and the
  calls are in all four ELFs; which physical LED changes colour is unobserved,
  and so is whether the previously-unreachable fourth byte of an RGBW strip
  does anything.

  **A latent off-by-one in the vendored `led_strip` component is recorded and
  not patched.** `led_strip_set_pixel()` validates `num <= strip->length` while
  its own header documents the range as `0..length-1`, so `num == length` would
  write `COLOR_SIZE` bytes past the frame buffer. Every caller was checked:
  only `led_strip_set_pixels` and `led_strip_fill` reach it and both bound the
  index correctly, so it is **unreachable today** — and `device-0009` no longer
  calls it at all. Left alone because changing vendored MIT code to fix a
  defect nothing can reach is the trade this work declined to make everywhere
  else, and recorded because "unreachable today" is a property of the current
  callers rather than of the function.

- **Two target gaps are now stated by the build instead of being fixed, because
  both need hardware to close.** The target-guard work under *Build and
  configuration* made the guards honest; it did not add the missing support, and
  these are the two cases where "the guard is right" and "the feature works
  everywhere" are not the same thing.

  **Neither DHT sensor works on esp32c3.** `dht11.c` and `dht22.c` both bit-bang
  the 1-wire protocol with `ets_delay_us` out of `<target>/rom/ets_sys.h`, and
  the timing is written against the three Xtensa parts. Both drivers and both
  device-list entries are guarded accordingly, so an esp32c3 hub simply does not
  offer device 15 or 16 — consistently now, rather than offering a prototype with
  no definition behind it. Adding C3 support is not an include change: it means
  validating the pulse timing on a C3 board, which no build or host test can
  stand in for. Deliberately not attempted blind.

  **The indicator LED has no esp32s2 implementation.** The effects in
  `ezlopi_service_led_indicator.c` exist for esp32, esp32s3 and esp32c3 only.
  Since `CONFIG_EZPI_ENABLE_LED_INDICATOR` is `default y` in Kconfig and off in
  all four shipped defaults, no shipped image is affected — but anyone turning it
  on for esp32s2 now gets a one-line `#error` naming the limitation instead of
  five link errors and warnings that name the symbols and not the cause. Writing
  the arm is a small change; confirming the LED timing on an esp32s2 board is the
  part that is missing, so the arm is not written on guesswork.

- **`cppcheck`'s recall on this class is 1 of 2 on the cases we can measure
  against; a second check now covers the half it misses, and combined recall is
  2 of 2 — but the size of the remainder is still unknown rather than small.**
  `cppcheck` found 33 instances of the class and they are fixed (see *Null
  dereferences* above). The shape it is blind to is diagnosed at the foot of
  this entry, and the check written for it is
  [`tests/ci/check_check_after_use.py`](tests/ci/check_check_after_use.py),
  described in *A 35th null dereference* above. Two measured checks agreeing on
  two known cases is still two cases.

  What is known: of the two check-after-use defects fixed earlier in this
  branch, the analyser finds
  [`ezlopi_core_factory_info.c:564`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L564)
  (`isprint(ret[0])` before `if (ret)`) and does not find
  `ezpi_provisioning_info_read_func()` (two `strlen()`s before
  `if (NULL != g_provisioning_info_base64)`), even at `--enable=all
  --check-level=exhaustive`. One of two is a useful check and a poor guarantee.

  It was also recorded here, and in the check's own docstring, that the
  analyser missed a thirtieth trace site at `ezlopi_core_scenes_v2.c:2091`
  which a grep caught afterwards — making the score two of three. That was
  wrong, and the correction runs in the analyser's favour: line 2091 is inside
  an `#if 0` opened at
  [line 1924](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_v2.c#L1924)
  and commented `[for future use]`, so it is not compiled into any image and
  cppcheck was right to say nothing about it. The same file holds five live
  copies of that exact idiom — lines 220, 398, 453, 635 and 708 — and all five
  are reported, so the analyser is not weak on this shape here. The one it
  skipped is the one that does not survive preprocessing.

  That refines the headline number too. Of the 69 hunks the fix commit touched,
  exactly two lines fall inside a non-compiled region, and both are that single
  site; every other line it changed is live. So 33 of the 34 fixed defects were
  reachable in a shipped image, 33 is also the number cppcheck reported, and
  the one site it did not report is the one site that was never built. The fix
  there is still worth keeping — the block is labelled for future use and the
  bug would arrive with it — but it should not be counted as a defect that
  shipped, and the analyser should not be marked down for it.

  The reason is **not** the missing include paths, which is worth saying
  outright because that was the first explanation written here and it is wrong.
  It has now been tested instead of reasoned about. Each defect was extracted at
  the commit before its own fix and scanned both ways — bare, and with the exact
  `-I`/`-D` flags for that translation unit taken from a real compilation
  database, 250 of them for the provisioning unit and 70 for the factory-info
  one. Nothing moves: the provisioning defect is missed **with and without**
  headers, and the factory-info one is found **both** ways. A header-aware scan
  of all 159 first-party translation units reports no findings of this class
  anywhere, so the compilation database that was the "obvious next step" buys no
  recall at all.

  What the analyser is actually blind to is a guard sitting in a **sibling
  block** of the one that holds the dereference. Isolated with eight fixtures,
  changing one variable at a time:

  | Dereference | Guard position | Reported |
  |---|---|---|
  | `strlen(p)`, local | same block | yes |
  | `strlen(p)`, function-static | same block | yes |
  | `p[0]`, local | same block | yes |
  | `strlen(p)`, local, nested one level | same block | yes |
  | `strlen(p)`, function-static, nested | same block | yes |
  | `strlen(p)`, local | sibling block | **no** |
  | `strlen(p)`, function-static | sibling block | **no** |
  | `p[0]`, local | sibling block | **no** |

  So none of the things that look like they ought to matter do: not the storage
  class, not the nesting depth, and not whether the dereference goes through a
  library call or a plain subscript. The single predictor is whether the test
  shares a block with the use. That is exactly the shape of the one it misses —
  `g_provisioning_info_base64` is assigned and handed to `strlen()` inside
  `if (NULL == ...)`, and the `if (NULL != ...)` meant to guard it is the next
  block down. It is the only known instance in the tree, and the only measured
  cause of a miss.

  The useful consequence is that this says what to search for, which "pass
  include paths" never did: a pointer tested in one block and used in its
  sibling. `cppcheck` will not be taught that shape, so it meant a check of our
  own, and that check is now written and in CI — it reports the provisioning
  defect, stays silent on both fixed files, and brought the tree to zero
  findings over the same 159 translation units. What it does not do is make the
  remainder measurable: it is line-oriented, so a guard expressed through data
  flow rather than a `NULL` test is invisible to it, and the one such site in
  this tree is suppressed by hand with its reason stated. The honest position is
  that two tools now agree on two known cases, and neither can bound what is
  left.

  The alternatives remain worse. The compiler cannot see this class:
  `-Wnull-dereference` needs the `NULL` to be provable on a path, and here it
  depends on whether a malloc failed. A grep cannot either — the use may be
  `p[0]`, `strlen(p)`, `p->field` or a call taking `p`, and the test below may
  be `if (p)`, `if (NULL != p)` or an early return. And reading every site by
  hand is larger than it looks: the `EZPI_core_factory_info_v3_get_*` family
  alone is called from 116 places, and most of those getters return a freshly
  allocated string that is `NULL` on four separate paths.

- **The isSunState call path still has no test that can fail — but less of it
  than before.** This entry used to open with *a failing sunrise/sunset lookup
  is retried with no backoff*, which is no longer true: see *A failed suntime
  lookup is no longer retried once a second* under *Scene HTTP requests*. What
  survives of the gap is the narrower half, and the boundary has moved rather
  than disappeared.

  The query, the reply parsing, the clock arithmetic, the offset parser, the
  shift and now the retry policy were all extracted so that they could be
  tested, and they have **277** assertions between them. The two functions that
  call them still have none.
  [`__issunsate_update_sunstate_tm`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L1893)
  needs a radio: a test for it means a seam at `EZPI_core_http_mbedtls_req` — a
  function pointer or a weak symbol — which is a larger change to the HTTP
  component than this defect warranted.
  [`ISSUNSTATE_get_offs_tmval`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L586)
  needs a populated scene, a field list and a `struct tm`, so the once-per-day
  recomputation and the `e_type` gate are likewise argued rather than
  demonstrated.

  **The backoff is the sharpest example of the split, and worth stating as
  one.** Its *policy* — the 60 s to 32 min ladder, the day rollover, the
  saturation, the inclusive deadline — is three pure functions with 71
  assertions and a 28-mutant sweep behind them. Its *wiring* — that
  [`ISSUNSTATE_get_suntime`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L470)
  asks before the lookup, records the outcome after it, and keeps the `user_arg`
  block instead of deleting it — is in the untestable half, and is supported by
  reading, by an argument about which fields each reader gates on, and by a
  four-target build. A mutation of the policy fails a check. A mutation of the
  wiring fails nothing. That asymmetry is the shape of this whole gap, and it is
  stated here rather than left to be inferred, because the entry below reports a
  lot of green and only half of it is the kind that can go red.

  [`ISSUNSTATE_eval_range`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L662)
  **was** a third function in this half, and this paragraph used to end: *"Mutating
  the layout fails a check; deleting the guard fails nothing."* **That is no
  longer true, and the correction is the useful part.** The premise was always
  measured — three checks in `test_scenefields.c` that write a bool into the
  union and read a non-null `char *` back out, with the de-unioned layout as a
  control that makes them fail — but the guard itself was supported only by a
  four-target build, because the translation unit holding it was believed
  un-hostable. It was not: it needed a `freertos/task.h` stub and three warning
  suppressions. With `tests/host/test_issun.c` in place, deleting the
  discriminant clause now fails loudly — an AddressSanitizer SEGV on address
  0x1, from `strlen()` reading a small integer out of the union as a pointer.
  See *Scene triggers*.

  The general lesson is about this gap's own framing rather than about scenes:
  "untestable" was a conclusion inherited and repeated across several entries
  here, and in this instance it was never checked. One stub file moved a
  function from the half that cannot go red into the half that can. The rest of
  the functions listed in this gap have **not** been re-examined in that light,
  and should be before anything else here is described as untestable.

- **`ISSUNSTATE_get_suntime` has no test, and a mutant proves it.** Of the 18
  mutants run against
  [`ezlopi_core_scenes_when_methods_helper_functions.c`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c),
  one survivor is a genuine coverage gap rather than an equivalent mutant:
  changing which bit `ISSUNSTATE_get_suntime` sets is undetected, because no
  check calls it. The function is the wiring half described above — it asks for
  a suntime, records the outcome, and keeps the `user_arg` block — so testing
  it needs the suntime lookup stubbed, which `test_issun.c` does not do. The
  29 checks there cover the *decision*, not the *fetch*.

- **Ten `-Wsign-compare` sites in the scene timing family are suppressed, not
  fixed.** Making the above file hostable required `-Wno-sign-compare`, and the
  ten sites it hides are all in
  `WHEN_function_for_opr`/`_repeat`/`_follow`/`_pulse`: a `uint32_t` tick count
  compared against an `int` threshold. None are on the `isSunState` path — I
  measured that wrongly once, with a `grep -B20` that caught unrelated "In
  function" lines, and the corrected attribution by `awk` state-tracking puts
  all ten in the timing family. The comparison is wrong at the point a tick
  count exceeds `INT_MAX`, which on a 1 kHz tick is about 25 days of uptime.
  Left alone because fixing it is a behavioural change to scene timing with no
  test behind it, and the file only became host-testable in this same change.
  That file is now hostable, so these are reachable by a test for the first
  time — which is the argument for fixing them next rather than later.

- **`flag_check == 0x89` — a range present together with an exact moment — is
  not pinned by any check.** The 512-pair sweep in `test_issun.c` asserts the
  firing set matches the 16 combinations derived from the case labels, and 0x89
  is outside that set, so the sweep agrees with the switch without either one
  being shown correct for this input. Whether a scene that specifies both an
  `isSunState` range and an exact moment *should* fire is a product question
  about what the app can actually save, and there is no sample payload in this
  tree that produces the combination.

- **The wire shape of `hms_interval` is assumed, not verified.** The isSunState
  offset parser described under *Scene triggers* takes a string — `"30"`,
  `"5:30"`, `"1:05:30"` — because that is what the shipped code tried to parse
  and what the field's name implies. But there is no sample payload for an
  `isSunState` block anywhere in this tree, and field population names
  [`EZLOPI_VALUE_TYPE_HMS_INTERVAL`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_populate.c#L578)
  explicitly in its `cJSON_Array` arm, which stores a duplicated `cJSON *`
  rather than a string. So the controller may well send an array or an object,
  and which it sends is undetermined here.
  The consequence is bounded on purpose rather than by luck: the caller gates
  on `e_type`, so a non-string arrives as a refusal and a trace line naming the
  type it got, not as a cJSON node handed to `snprintf("%s")`. An offset the
  hub cannot read means the scene does not fire and says so. What is *not*
  known is whether that is the branch a real controller takes — in which case
  every isSunState offset is refused and the correct fix is to read the fields
  out of the object. One captured payload settles it, and that needs a hub
  paired to the cloud.

  **The same union-without-its-tag read is still present two lines away, and
  was deliberately not fixed in that change.**
  [`ISSUNSTATE_eval_range`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L662)
  gates on [`value_string`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_when_methods_helper_functions.c#L699)
  being non-NULL for its `EZLOPI_VALUE_TYPE_TOKEN` field, with
  no `e_type` check, and the `cJSON_Object` arm of field population stores
  `VALUE_TYPE_CJSON` for any value_type it does not name — `TOKEN` is not among
  the ones it names. So an object-valued range field puts a `cJSON *` where a
  string is expected here too. It is recorded rather than fixed because the
  consequence is different in kind: the pointer is only ever handed to
  `EZPI_STRNCMP_IF_EQUAL` against a fixed set of tokens, so it fails to match
  and the range check does not fire — no `snprintf("%s")` of a node address. A
  sweep of every `u_value.value_string` read in the scenes component against
  its `e_type` is the right shape for this, rather than one more site at a
  time, and it wants the captured payload above first so the cases can be
  ordered by which ones a controller actually produces.

- **An unbracketed IPv6 literal in a scene URL still has no answer.** The two
  gap entries that stood here — arm nine's leading slash and 199-character
  truncation, and the ladder's treatment of IPv6 literals and query strings —
  are **closed**: the ladder they described no longer exists. See *The URL
  ladder is gone, and with it twenty-three more measured defects* under *Scene
  HTTP requests*. What that change did not answer is this one case, and it is
  left open deliberately rather than guessed at.

  `http://::1:8080/x` cannot be told from a host named `::1` on port 8080
  without a full RFC 3986 authority grammar. The replacement takes the port
  from the last `:` when the tail is all digits in 1..65535, so it reads this
  as the host `::1` on port 8080 — which is one of the two possible readings
  and happens to be the useful one, but it is not a decision the code is making
  on purpose. The **bracketed** forms are decided on purpose and are tested:
  `http://[::1]/x`, `http://[::1]:8080/x` and `https://[2001:db8::1]/x` all
  parse, and `http://[::1/x` with an unclosed bracket is refused rather than
  becoming the host `[:` it used to.

  The cost is bounded the same way it was before: a mis-read authority produces
  a `Host:` header and a `web_server` that do not resolve, so the request fails
  rather than arriving somewhere unintended. Writing the grammar out properly is
  the fix, and it is a larger change than the one that was needed here.

- **The scene HTTP transport is TLS-only, so `http://` means "TLS on port
  80".** This is not introduced by the ladder's replacement — it is what the
  transport has always done — but the replacement is what made it visible
  enough to be worth recording, because it is the argument the replacement
  rests on.

  [`EZPI_core_http_mbedtls_req()`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L164)
  dispatches every request to
  [`__ezlopi_http_req_via_mbedTLS`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L463),
  which always performs an unconditional
  [`mbedtls_ssl_handshake`](ezlopi-core/ezlopi-core-http/ezlopi_core_http.c#L549).
  There is no plaintext path, and `web_port` only ever reaches the connect
  string. So the scheme
  in a scene URL never selected a transport; it only ever selected a **default
  port**, 80 or 443. An `http://` scene therefore sends a TLS ClientHello to
  port 80 and fails against any ordinary HTTP server, and has done so all along.

  That asymmetry is exactly why refusing an unknown scheme is the right answer
  rather than parsing it more carefully: an `ftp://` URL was never going to be
  served however well it parsed, and the old ninth arm's response was to send a
  TLS handshake to whatever port the URL named. Fixing the *scheme* question
  properly means either implementing a plaintext path or refusing `http://`
  too, and both are behaviour changes to a shipped feature rather than
  corrections to it.

- **A byte above 0x7E is not refused in a scene URL.** The guard added with the
  ladder's replacement refuses every byte at or below 0x20 and 0x7F, because
  those are the ones that can split a request line or start a new header. A
  high byte cannot do either, and refusing one would break a UTF-8 host that
  some deployment may be relying on — so `http://exémple.com/café` parses, and
  that is
  [asserted as the current answer](tests/host/test_httpurl.c) rather than left
  unstated.

  It is still not a valid URI: RFC 3986 is ASCII and percent-encoding is what
  exists for everything else. The right fix is to percent-encode on the way
  out, which is a feature rather than a refusal. The assertion exists mainly to
  hold the `(unsigned char)` cast in the guard honest — plain `char` is signed
  on the x86 host and unsigned on xtensa, so without the cast a high byte would
  be refused on the host and pass on the device, and only a check that asserts
  the *accepting* answer can catch that divergence. A mutation that removes the
  cast is detected by exactly those three checks.

- **`url_maxlen` and `targe_page_maxlen` are `uint16_t` against an uncapped
  URL.** The `url` field of a `sendHttpRequest` action is request-supplied and
  has no length limit, and the page is no longer truncated at 199 characters —
  it is stored whole, which is the fix. Both lengths are returned by
  `EZPI_core_http_mem_malloc` into `uint16_t` fields, so a URL whose path
  exceeds 65535 characters would wrap the recorded length while the allocation
  itself succeeded.

  Not fixed here because nothing downstream was measured to read these lengths
  in a way that matters, and widening them is a change to a struct that several
  components share. What *is* asserted is the structural claim that keeps the
  request buffer consistent: `url_maxlen > targe_page_maxlen`, so an unbounded
  page cannot under-size the buffer the request is generated into.

- **The sweep's other 18 findings were read and deliberately not acted on, and
  one more category appears only once the platform model is right.** The sweep
  that answered the include-path question above ran over all 159 first-party
  translation units and reported 21 findings, every one of them outside the
  null class. Two were real and are fixed — the unbounded `sscanf` conversions
  (`invalidscanf`) and the dead `NULL` assignment in the websocket kill
  (`uselessAssignmentPtrArg`), both above. A second
  `uselessAssignmentPtrArg`, at
  [`ezlopi_core_factory_info.c:140`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L140),
  is recorded with that fix as unobservable across its 39 call sites. The
  remaining 18 break into three groups, and none is worth a change:

  Sixteen `literalWithCharPtrCompare`, all of them
  [`EZPI_STRNCMP_IF_EQUAL`](ezlopi-cloud/ezlopi-cloud-constants/ezlopi_cloud_constants.h#L73)
  invoked with a string literal as one of its two arguments — four in
  `scenes_then_methods.c`, eight in `scenes_operators.c`, three in
  `scenes_when_methods.c`, one in `scenes_when_methods_helper_functions.c`.
  The macro opens with `(NULL == STR1) ? false : (NULL == STR2) ? false : ...`,
  so a literal argument expands to `NULL == "length"`, which cppcheck reads as
  a string comparison written with `==`. It is a tautology, not a mistake: the
  guard exists for the call sites that pass a variable, and the arm it creates
  for a literal is constant-folded. Removing it would mean either dropping the
  NULL guard the variable call sites rely on, or keeping two macros that differ
  only in whether they trust their arguments. Suppressing it would mean sixteen
  inline comments on correct code.

  One `comparePointers` at
  [`ezlopi_service_ws_server.c:554`](ezlopi-services/ezlopi-service-ws-server/ezlopi_service_ws_server.c#L554),
  `config_ui_html_end - config_ui_html_start`. cppcheck is right about the
  language and wrong about the program: those are two separately declared
  `extern const uint8_t[]` objects, so by ISO C the subtraction is undefined,
  but they are the linker-provided bounds of one embedded blob and ESP-IDF
  offers no other way to ask how long it is. There is no rewrite that keeps the
  idiom and loses the finding. The arithmetic was checked rather than assumed
  while reading it: the component embeds the page with `EMBED_TXTFILES`, which
  appends a NUL, so the `- 1` is load-bearing, and `config_ui.html` is 36554
  bytes — exactly the length the handler computes.

  One `invalidPrintfArgType_uint` at
  [`ezlopi_core_mdns.c:226`](ezlopi-core/ezlopi-core-mdns/ezlopi_core_mdns.c#L226),
  `"%llu"` given `id_val`, reported as needing `unsigned long long` while the
  argument is `unsigned long`. That one is the sweep's own fault, not the
  code's: cppcheck was left on its default platform, which here is a 64-bit
  host, and on a 64-bit host `uint64_t` *is* `unsigned long`. Re-run with
  `--platform=unix32` the finding disappears, and so does it on `win32A` and
  `avr8`, while `native` and `unix64` both report it. The targets agree with
  the source: a `_Static_assert` on
  `__builtin_types_compatible_p(uint64_t, unsigned long long)` compiles clean
  under both `xtensa-esp32-elf-gcc` and `riscv32-esp-elf-gcc`, and
  `EZPI_core_factory_info_v3_get_id()` is declared `unsigned long long`. So
  `%llu` is right for every target this builds for.

  That artefact is worth more than the finding, because it means the sweep as
  run had a systematic bias, and the fix was to re-run it under
  `--platform=unix32`. Doing so changes nothing for the null class — the two
  recall subjects still score 0 and 1, the same-block fixture is still found,
  the sibling-block one still missed, and the tree still reports none, so the
  conclusion above is platform-independent — but it does surface a category
  the 64-bit run never showed: six `invalidPrintfArgType_sint`, all in
  [`ezlopi_core_heap.c`](ezlopi-core/ezlopi-core-heap/ezlopi_core_heap.c#L214)
  at lines 214, 215, 219, 223 and 231 (twice), each a `%d` given a `uint32_t`
  — a node counter and a `line_number`. Left alone twice over. The widths
  match on a 32-bit target, so the only possible effect is a value above
  `INT_MAX` printing negative, which a line number and a list index do not
  reach. And the code is not in any image: the whole file body sits inside
  `#ifdef CONFIG_EZPI_HEAP_ENABLE`, which is `default n` in
  `ezlopi-main/Kconfig.projbuild` and explicitly `is not set` in all four
  committed `sdkconfig.defaults.*`, as is its one caller at
  `ezlopi_main.c:296`. It is an opt-in leak tracer for a developer who turns
  it on deliberately.

  Two things were checked before leaving it, because "nothing compiles it" is
  a reason to look rather than a reason to stop. It does still build: flipping
  the symbol to `=y` in `sdkconfig.defaults.esp32` and building esp32 succeeds,
  `ezlopi_core_heap.c.obj` is produced, `EZPI_core_util_heap_trace` is in the
  ELF, and the image goes from 1683856 to 1698720 bytes, +14864. So the tracer
  has not rotted while unbuilt, which was the thing actually worth knowing.
  And the compiler is no help on the class: that build emits zero diagnostics
  at any line of the file and zero `-Wformat` anywhere, despite `-Wall
  -Wextra` on the translation unit, because a signed/unsigned format mismatch
  needs `-Wformat-signedness`, which neither of those implies. The six are
  left as they are on the strength of having no effect, then — not because
  anything would catch them.

  None of these categories is in CI. The committed check matches
  `nullPointer*` only, so they do not gate anything today, and adding
  suppressions for findings no gate reads would be noise. The platform model
  is the part worth carrying forward: any future sweep should pass
  `--platform=unix32`, or it will keep inventing `%llu` findings and hiding
  `%d` ones.

- **324 reads of the scene field union are not gated on the union's own
  discriminant, and two have been fixed one at a time.** The `strlen()` on
  address `0x1` under *Memory safety* was not a one-off, and the honest thing is
  to say how large the surrounding class is rather than let two fixes imply it
  is closed.

  A sweep over the nine `.c` files in `ezlopi-core-scenes` finds **369** reads
  of a `u_value` member — excluding writes, frees, and `populate.c`/`delete.c`,
  where the discriminant is *established* rather than consumed, so demanding a
  guard there is backwards. Of those, **324** have no `if` or `switch` on
  `e_type` anywhere in the chain of blocks enclosing them: `when_methods.c` 103,
  `operators.c` 69, `then_methods.c` 59,
  `when_methods_helper_functions.c` 47, `cjson.c` 14,
  `then_methods_helper_func.c` 12, `scenes_v2.c` 8, `expressions.c` 6,
  `print.c` 6.

  **The split inside that number is the part that matters.** 171 of the 324 have
  no type test of any kind. The other **153 sit under a test on `value_type`** —
  the declared schema type — which is the exact shape of the defect that was
  fixed: code that looks type-safe, reads as type-safe in review, and is
  checking the wrong field. Those 153 are more dangerous than the 171, not less.

  **Why this is documented rather than fixed.** The two sites corrected on this
  branch were corrected because each was traced to a specific unsafe
  consumer — a `strlen()` and an `snprintf("%s")`. The remaining sites have not
  been traced that way, and `when_methods_helper_functions.c` alone is 2300-odd
  lines with no host harness and no stub set that would give it one
  (`tests/host/stubs-scenes/` contains a single `sdkconfig.h`). Bulk-editing 324
  guards into a file nothing can test would trade a known, bounded defect for an
  unknown number of unverified changes, several of which would silently disable
  working scenes. So the two that were proven are fixed and the rest is counted.

  **The sweep is a heuristic in one direction, and its own docstring says so.**
  It walks brace depth backwards from each read to collect every dominating
  condition, so a guard twenty lines up counts and a sibling `if` in a previous
  block does not — but it does not understand early `return`/`continue` guards,
  or a helper that validates on the caller's behalf. A finding therefore means
  "no dominating `e_type` test was found here", not "this is reachable". It also
  cannot tell a guard from a consumer: the fixed site still reports one finding,
  because the `!= NULL` clause *of the new guard* is itself a union read. The
  count went 325 → 324 and a diff of the two finding lists shows the one that
  disappeared is the read passed to `ISSUNSTATE_check_mdrn` — the dangerous
  one — with every other line a uniform shift from the comment added above.

  **The structural fix, named because it is not the one taken.** The cross-check
  belongs once, at
  [`EZPI_scenes_populate_assign_field`](ezlopi-core/ezlopi-core-scenes/ezlopi_core_scenes_populate.c#L633),
  which is the one place that has both values in hand and runs once per field
  per scene edit rather than once per second: a field whose declared `type`
  disagrees with the `e_type` its `value` produced could be rejected at the door
  and every downstream reader would be entitled to its assumption. That is a
  change to the scene-accept path with a compatibility question attached — it
  would start refusing scene JSON that today is accepted, and nobody here knows
  what the controller actually sends — so it is not made on a guess. It is also
  the place a diagnostic belongs, which is the second half of this gap: the
  guard that was added is silent, because the function it is in runs about once
  a second, so a mistyped `range` field now yields a scene that never fires and
  no log line saying why.

- **Whether removing a PMS5003's parent device frees `pms_object` out from
  under its eight children is not established.** The sensor builds nine devices
  whose items all point at one heap allocation, with ownership claimed exactly
  once, on the parent — see *Particulate matter sensor*. That is the correct
  shape for the free path as written: `ezlopi_device_free_item()` frees only on
  the flag, so removing a child frees nothing shared. The untested direction is
  the other one. If the parent can be removed while children 1–8 survive, its
  item's flag fires, `pms_object` goes back to the heap, and eight items keep a
  dangling `user_arg` which their `func()` casts and dereferences on every
  update. The alternative is that device removal is always parent-and-children
  together, in which case there is nothing here at all.

  Which of those holds is a question about device-removal semantics in
  `ezlopi-core-devices`, not about this sensor, and it was not chased because
  the abort fixed above made the whole file unreachable in practice and so there
  was never a live PMS5003 to observe. Stated as an open question rather than a
  defect: calling it a use-after-free would assert a removal order that has not
  been read, and calling it safe would assert the same thing in the other
  direction.

  Worth separating from the five drivers in *Memory safety*, because the shape
  is not the same one. Those share a `user_arg` across several items *of one
  device*, which is why `ezlopi_device_free_item()` walking that device's item
  list is the function that matters for them. PMS5003 gives each of its nine
  devices exactly one item and shares the allocation *across the devices* — so
  it is correctly absent from the six multi-item drivers counted elsewhere in
  this section, and the walk that would have to be reasoned about is device
  removal rather than item removal. A driver can therefore be entirely sound
  item-wise and still hold this question open, which is also why the
  one-owner-per-item check that passes on this file cannot answer it.

- **`EZPI_hal_i2c_master_deinit()` is dead code with two defects in it, and was
  left alone deliberately.** It is declared in
  [`ezlopi_hal_i2c_master.h`](ezlopi-hal/ezlopi-hal-i2c/ezlopi_hal_i2c_master.h#L100)
  and called from nowhere in the tree — found while fixing the shared-bus init
  bug above, which is in the same function's counterpart. Two things are wrong
  with it:

  ```c
  if (1 == i2c_master_conf_ptr[i2c_master_conf->channel]->channel)
  ```

  That compares a channel *number* against 1, so on channel 0 — the only
  channel any driver uses — it is `1 == 0` and the driver is never deleted. And
  it dereferences the table entry without a NULL check, so calling it for a
  channel that was never brought up is a null dereference. Separately, the
  semaphore is deleted outside the `if (NULL != i2c_master_conf)` block, so one
  device's teardown would destroy the lock every other I²C device is using.

  Not fixed because the correct version is a design decision, not a
  correction: tearing down a *shared* bus safely needs reference counting of
  the devices on each channel, and there is no caller to tell us what the
  intended lifecycle is. Writing a host test for it, as the shared-bus fix got,
  would freeze a guessed lifecycle into place and make the guess look decided.
  The honest state is that this function does not work and nothing calls it; the
  moment something needs to call it, it needs designing. Recorded here so that
  the next person to reach for it knows it has never run.

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

- **The shadow guard covers the local-shadows-local class only, so a local that
  hides a global, a typedef or a function name is still unguarded.** This is
  the residue of a gap that used to be much larger: all nine shadowed
  declarations are fixed and `-Wshadow=local -Werror=shadow` is now an error on
  our components, recorded under *Build and configuration* above. What that
  spelling does not report is the class `ld2410_driver.c` was in, where a
  parameter named `byte` hid that component's own `typedef unsigned char byte`.
  That site is fixed; a tenth of the same shape would not be caught.

  The narrowing is deliberate rather than an oversight, and the reason is in
  the resolved entry: plain `-Wshadow` fires when one of our locals happens to
  share a name with anything an included header declares, so with `-Werror` it
  would hand upstream header churn the power to break this build for a
  non-defect. Closing this properly means plain `-Wshadow` plus a mechanism
  that pins the accepted set — a suppression list, or `-Wno-error=shadow` with
  a CI step that compares the warning count against a recorded baseline —
  neither of which is a flag change, so neither is bundled into the commit that
  enabled the narrow version.

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

  35 is a count of sites where a failure *changes what happens next*, not of
  sites that merely assign the result somewhere, and the distinction costs one:
  a recount by the shape of the call returns 36. The two that do nothing with
  the failure are

  | site | what happens to the return |
  | --- | --- |
  | `device_0001_digitalOut_generic.c:325` | discarded on the spot |
  | `pms5003.c:189` | `ret \|=`, propagated up through `pms_setup_control_gpio()`, then dropped by `pms_init()` at `pms5003.c:92` — `pms_init()` is `void`, so there is nowhere for it to go |

  `pms5003.c:189` is the one a grep gets wrong: it looks like a checked call
  for two stack frames. The other 35 either compare the return in place
  (`if (0 == gpio_config(...))`, `ret = (0 == gpio_config(...)) ? ... : ...`)
  or store it and test it on the next line, as `jsn_sr04t.c:114` and `:127` do.

  **One part of this gap is now closed, and it was the dangerous part.** A pin
  number outside `[0, GPIO_NUM_MAX)` used to be *accepted* — it aliased onto a
  real pin, because the valid-pin macro is a shift and not a range test — and is
  now refused at the single accessor all 144 `CJSON_GET_VALUE_GPIO` sites read
  through. See *Device configuration* above. What that entry does **not** close,
  and what the rest of this entry is about, is pins that are perfectly real and
  still the wrong choice.

  The sixteen pin reads that were outside the accessor are now inside it, and
  `tests/ci/check_gpio_accessor.py` is what keeps them there. They had gone
  through `CJSON_GET_VALUE_INT`/`UINT32`: device-0038 RGB (3), sensor-0037
  pms5003 (4), sensor-0040 TCS230 (6), sensor-0047 HX711 (2), sensor-0070 (1).
  Those two macros are general integer readers used by hundreds of non-pin keys,
  so the bound does not belong in them.

  **This cost no destination-type changes, which is why it was worth doing now
  rather than as its own project.** The earlier version of this paragraph said
  routing the sixteen "means changing their destination types"; that was wrong
  twice over. `CJSON_GET_VALUE_GPIO` *assigns* its result
  (`gpio_num = EZPI_core_cjson_get_value_gpio(...)`) where every other accessor
  writes through a pointer (`EZPI_core_cjson_get_value_int(..., &item_val)`), so
  the destination is converted by the compiler rather than aliased, and the
  three destination types present — `int`, `uint32_t`, and `gpio_num_t` — all
  take the assignment. And the ten `uint32_t` destinations do not need widening
  to hold `-1`: measured against all four target masks, `-1` and
  `(uint32_t)4294967295` are *indistinguishable* to `GPIO_IS_VALID_GPIO`,
  because both give `1ULL << n == 0x8000000000000000` and every mask rejects
  bit 63. The "no pin" sentinel survives the unsigned round trip.

  One of the sixteen was worse than the other fifteen and is the reason this
  moved up the list. `pms_dev_items_prepare.c` read its fourth pin as a bare
  `"gpio4"` string literal rather than through `ezlopi_gpio4_str`, and
  `pms_gpio_config_output()` in `pms5003.c` builds `pin_bit_mask = (1ULL << pin)`
  with **no** `GPIO_IS_VALID_GPIO` call at all, while `pms_init()` is `void` so
  the failing `gpio_config()` return has nowhere to go (it is one of the two
  exceptions tabulated above). Nothing anywhere in that path would have refused
  pin 70. For pms5003 the reroute converts "silently reconfigure the SPI flash
  clock" into "`gpio_config()` returns `ESP_ERR_INVALID_ARG` and that pin does
  not work", which is the same trade this entry already makes elsewhere. The
  other four drivers do gate their pins on `GPIO_IS_VALID_GPIO`, so for them a
  refused pin was already handled.

  **The reroute removes none of the `-Wincompatible-pointer-types` warnings, and
  an earlier draft of the HX711 comment claimed it removed two.** Built both
  ways on esp32c3 from the same mirror with the same grep: the count and the set
  of sites were identical before and after. (It was 92 in both arms when that
  A/B was run; the current figure is 90, for the unrelated reason given below.
  Both arms moved together, so the comparison is unaffected.) HX711's two pin fields
  *are* `gpio_num_t`, an enum, and the old code did take their address as an
  `int *` — but `gpio_num_t` declares `GPIO_NUM_NC = -1`, so its compatible type
  is `int` and `gpio_num_t *` genuinely is compatible with `int *`. The enums
  that warn, such as the `e_ezlopi_device_interface_type_t` on the line above,
  have no negative enumerator, so the compiler picks `unsigned int` for them.
  Confirmed with a two-enum probe: the one with a `-1` member passes `&x` to an
  `int *` parameter silently; the all-non-negative one warns. HX711 contributes
  exactly one pointer-type warning, on that `interface_type` read, before and
  after.

  Finding that out corrected the count this entry had been quoting. The warnings
  are **87 on esp32, esp32s2 and esp32s3 and 90 on esp32c3**, not 87
  everywhere. On the three xtensa targets all 87 are reported at
  `ezlopi_core_cjson_macros.h` and none anywhere else. esp32c3 has 83 there, and
  the four it is missing account for themselves exactly: two in
  `sensor_0016_oneWire_DHT22.c` and two in `sensor_0015_oneWire_DHT11.c`, both of
  which now sit entirely inside
  `#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2)`
  and compile to 1152-byte objects containing nothing on the C3. DHT22 was always
  guarded that way; DHT11 was not until the target-guard entry under *Build and
  configuration*, which is why this figure was 92/85 when first measured and is
  90/83 now. That the difference from 87 is
  twice the per-file warning count, for exactly the two files the preprocessor
  removes, is the arithmetic working out rather than a coincidence.

  The mechanism was established by replaying the build's own compile command for
  the file with a control (`device_0004_digitalIn_generic.c`, same macro, same
  destination field, 2 warnings on the same compiler) rather than by reading the
  `#if`, because the first three explanations tried — not compiled, `ccache`
  replaying a cached object, a per-component `-w` — were all wrong, and the flags
  are byte-identical between the two files.

  **An earlier version of this entry drew the wrong conclusion from that.** It
  said that because
  `SENSOR_0015_ONEWIRE_DHT11` and `SENSOR_0016_ONEWIRE_DHT22` are unconditionally
  `1` in `ezlopi_core_device_macros.h`, "the device list offers both on every
  target" while the drivers are absent from esp32c3 firmware. That is false.
  Those macros are not the registration: `ezlopi_core_devices_list_modules.h:116`
  wraps both `EZLOPI_DEVICE_LIST` lines in the same three-target `#if`, so
  esp32c3 does not offer either device. Checked against the built image rather
  than re-read from the header — the strings `sensor-DHT11` and `sensor-DHT22`
  appear 0 times in the esp32c3 ELF and neither wrapper symbol is in its symbol
  table. The real defect was narrower and in the opposite place — not a device
  offered without a driver, but a driver compiled without the function it calls —
  and is fixed in the target-guard entry under *Build and configuration*.

  There were 7 on the C3 at the time, in five files that every target builds —
  `ezlopi_service_uart.c` and
  `ezlopi_core_log.c` (2 each), `ezlopi_hal_uart.c`,
  `hilink_presence_sensor_setting.c` and
  `ezlopi_service_system_temperature_sensor.c`. **4 remain**: the hilink one was
  fixed by `1629ec0` and the two in `ezlopi_core_log.c` under *Log severity*
  above, re-derived from a `fullclean` build rather than an incremental one, for
  the reason given in that entry. Those 7 are an ABI difference,
  not a c3-only bug: `__UINT32_TYPE__` is `unsigned int` on
  `xtensa-esp32-elf-gcc` and `long unsigned int` on `riscv32-esp-elf-gcc`, so
  `&some_enum` passed to a `uint32_t *` parameter is compatible on xtensa and
  incompatible on riscv32. Probed directly on both compilers with the same four
  lines. The type confusion is equally real on all four targets; only one
  toolchain is in a position to say so.

  Beyond those sixteen, three real holes are left, and a fourth one this entry
  used to claim turned out not to exist. The third was found while costing out
  the second and belongs here rather than in its own entry, because it is the
  same list walk from the other end:

  - **Pins that are "valid" but already in use by the system — the half that
    depends on `sdkconfig` rather than on efuse.** On esp32
    `SOC_GPIO_VALID_GPIO_MASK` excludes only 24 and 28–31
    (`soc/esp32/include/soc/soc_caps.h:131`), so a pin the system has already
    claimed passes `GPIO_IS_VALID_GPIO` and `gpio_config()` succeeds on it.

    The flash and PSRAM pins are no longer part of this gap. They are refused
    at the accessor, against the set `esp_mspi_get_io()` reports for the chip
    that is running — see *Pins the chip is using for flash or PSRAM are no
    longer accepted as sensor pins* under *Device configuration* above. On esp32
    that set is 6–11 for an external-flash part and 6, 7, 8, 11, 16, 17 for an
    embedded-flash one, which is the reason it is read at runtime instead of
    typed into a table.

    What is left is the console UART pin and the pins of a configured I²C or
    SPI bus. Nothing in this tree knows about either. The difference that
    matters is where the answer lives: the flash pins are in efuse and the
    bootloader, so a running chip can be asked; these are in `sdkconfig`, so
    the answer is per-build and would have to be derived at compile time from
    `CONFIG_ESP_CONSOLE_UART_TX_GPIO` and friends. That is a different
    mechanism from the one above, not an extension of it.

  - **"Two devices claiming the same pin" was the wrong check, and an earlier
    version of this entry had it as a hole to close.** Two devices sharing a pin
    is how I²C works: every sensor on a bus shares `gpio_sda` and `gpio_scl` and
    is distinguished by `slave_addr`. The in-tree example configuration at
    [`ezlopi_core_factory_info.h:225`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.h#L225)
    is exactly that — a BME280 and a TSL2561 both on `gpio_sda:21, gpio_scl:22`,
    `slave_addr` 118 and 57 — though that one is commented out, so the stronger
    evidence is the live HAL:
    [`EZPI_hal_i2c_master_init()`](ezlopi-hal/ezlopi-hal-i2c/ezlopi_hal_i2c_master.c#L90)
    guards installation on `NULL == i2c_master_conf_ptr[channel]` precisely so
    that the second device on a channel reuses the bus the first one brought up,
    and reads and writes then select the device by `->address`. Sharing is
    designed in. A pre-store check that refused repeated pin numbers would
    reject that document, so it would have broken the most ordinary
    multi-sensor setup there is while reporting that it was protecting the user.
    What is left of the original concern is narrower and needs per-bus
    knowledge, not a uniqueness test: two devices claiming the same pin *in
    incompatible modes*. The related I²C case — two devices on one channel with
    *different* SDA/SCL — is no longer part of this gap; it is caught in the HAL
    now, and investigating it turned up the reason the second sensor on a bus
    never worked at all (see *I²C sensor bus* above).

    **That narrower check was costed out rather than left vague, and it is a
    project rather than an addition.** The flash-pin fix above took one function
    because the question had a chip-wide answer and one place to ask it. This one
    has neither, and the measurements say so:

    | what a mode check needs | what is actually there |
    |---|---|
    | one place that sees every pin claim | **38 `gpio_config()` call lines in 30 files** — 37 in `ezlopi-sensors-devices`, 1 in `ezlopi_core_ethernet.c`, **0 in the HAL** |
    | a GPIO HAL to put it in | `ezlopi_hal_gpio.c`'s two `GPIO_IS_VALID_*` tests are commented out at lines 76 and 96, and the init body with them |
    | every claimed pin visible in `item->interface` | **16 pin reads in 5 drivers** store the pin in a driver-private struct instead |
    | a mode vocabulary | this part exists: `e_ezlopi_device_interface_type_t` has 10 values and `l_ezlopi_item_t` carries `interface_type` beside the union |

    The third row is the one that decides it. A check at the top of
    `__EZPI_initialize_devices_v3()` would be the clean design — every device is
    *prepared* before any is *initialised*, so the whole list and its
    `interface_type` values are in memory before the first `gpio_config()`, and
    no driver cooperation is needed. But TCS230 (6 pins), pms5003 (4), RGB (3),
    HX711 (2) and R307's interrupt pin (1) put theirs in `user_arg` structs, so
    such a check would be **blind to 16 real claims while looking complete** —
    and a conflict checker with holes in it is worse than none, because it
    licenses the belief that the configuration was checked. Those reads are all
    bounded, since they go through the accessor like everything else; it is their
    *destination* that is private, not their value.

    Two further traps, both found by looking rather than by reasoning:

    - **The sentinels have to be excluded first or everything collides.** Most
      devices leave unused pins at `-1` or `255`, so a naive pass over the list
      would report every device as conflicting with every other on pin 255. This
      is the four-spellings-of-absent problem again, and the answer is the same
      one the flash-pin check used — test the pin only after it is known to be a
      real index.
    - **1-Wire is a bus in the protocol and not in this tree.** Two DS18B20 on
      one pin is the textbook use of 1-Wire, which would make it look like
      another designed-in sharing case. It is not: the driver defines
      `DS18B20_ROM_COMMAND_MATCH_ROM` and `…_SEARCH_ROM` and uses **neither** —
      the only ROM command it ever issues is `SKIP_ROM`, at
      `sensor_0030_oneWire_DS18B20.c:277` and `:343`, which addresses the whole
      bus at once and therefore only works with exactly one device on it. So two
      onewire items on one pin *is* a conflict here, for a protocol reason rather
      than a mode reason.

    Until this is done, the mitigation is that the consequence is at least
    visible: a device dropped at initialisation now says which device, which
    item and why (see *A device that fails to initialise no longer disappears
    without saying so* above). That does not detect the conflict, it just stops
    the result from being a silent absence.

  - **Items that were already initialised are not undone when their device is
    dropped.** `__EZPI_initialize_devices_v3()` initialises a device's items in
    order and `break`s on the first failure, then frees the device. The items
    *before* the failing one have already run their
    `EZLOPI_ACTION_INITIALIZE` — `gpio_config()`, a LEDC channel, a UART — and
    nothing reverses that, so the pins stay configured with no device owning
    them. No deinit action is dispatched on this path.

    What remains is a leak of *hardware state* — `EZPI_core_device_free_device()`
    handles the structures. The reason it has not caused visible trouble is that
    an unowned pin left as an input or a driven output is inert until something
    else claims it, which is the mode-conflict gap above from the other
    direction.

    **How reachable this is has now been measured, and it is narrower than the
    bullet above implies.** The gap needs a device with two or more items,
    because the loop `break`s on the first failure: with one item per device
    there is nothing to undo. Of 127 `EZPI_core_device_add_item_to_device()`
    call sites across the drivers, 104 give a device exactly one item. Six
    drivers build multi-item devices — SK6812 (6 items), the dimmable lamp (5),
    RGB (3), BME680 and BME280 (2 each on their parent device), and R307, which
    adds its items in a loop. Those six are the whole exposure, and all six are
    `PREPARE`-time shapes rather than anything a configuration can vary, so the
    list does not grow with the device catalogue — only with new drivers.

    That measurement is also what turned up the double free fixed in *Four
    drivers asked for one allocation to be freed up to six times* above: four of
    those same multi-item drivers shared one `user_arg` between all the items
    and flagged every one of them as its owner. So the memory half of this path
    was **not** sound when this bullet first claimed it was, and the claim has
    been corrected rather than left standing. The hardware-state half below is
    what is still open.

  Two things a future attempt needs, both of which looked expensive and are now
  paid for:

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

  Both of those were the reason the flash-pin half stayed deferred for as long
  as it did, and neither turned out to block it. The fifteen keys cost nothing
  because all fifteen already read through one accessor, and
  `check_gpio_accessor.py` derives that key set from the
  `ezlopi_gpio*_str` declarations rather than listing it, so a sixteenth key
  would be covered the day it is added. The four spellings of absent cost
  nothing because the check was placed *after* the in-range test, so it never
  sees a sentinel. Both of those answers are reusable: a console-UART or
  bus-pin check lands at the same line and inherits them.

  **An earlier version of this paragraph named the wrong source, and the
  correction is the interesting part.** It proposed `soc/spi_pins.h` plus
  `esp_rom_efuse_get_flash_gpio_info()` as "the shape a correct check would
  take". `spi_pins.h` is a compile-time header and gives the wrong answer on
  embedded-flash parts, which use a second set of pins in the same header
  (`SPI_D2WD_PIN_NUM_*`) selected by efuse at boot; and the efuse call is the
  raw strapping, below the decoding the bootloader does. The implementation uses
  `esp_mspi_get_io()` instead, which is where IDF itself already combines the
  two, and which also knows about the esp32 WP pad coming from the image header
  rather than from efuse. The principle the old paragraph stated was right —
  never a table typed from memory — but the specific source it named would have
  produced one of the wrong answers it was warning against.

  The intent to check at the HAL level is visible and disabled: the
  `GPIO_IS_VALID_OUTPUT_GPIO` and `GPIO_IS_VALID_GPIO` tests at
  [`ezlopi_hal_gpio.c:76`](ezlopi-hal/ezlopi-hal-gpio/ezlopi_hal_gpio.c#L76)
  and [line 96](ezlopi-hal/ezlopi-hal-gpio/ezlopi_hal_gpio.c#L96) are commented
  out. That is still true and still where a mode check would go, since the HAL
  is the only layer that knows what mode a pin is being configured for.

  What remains deferred is the `sdkconfig`-derived half above and the
  incompatible-mode case, and the original reason still holds for both: the cost
  of being wrong is refusing setups that work today, which is worse than the gap
  itself. The first attempt at this gap named three things to check and one of
  them would have refused a normal two-sensor I²C configuration. The flash-pin
  half became safe to do precisely because its answer is "the pins this chip is
  using right now" — a set that cannot describe a working configuration. Neither
  of the two left has that property.

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

  **The provenance is upstream's, and it was checked rather than assumed,
  because the answer decides who can act on it.** Exactly one commit in the
  entire history touches this file: `f2a74e5`, *Merge pull request #236 from
  ezloteam/development*, authored by an Ezlo developer on 2025-01-31. The blob
  is byte-identical at `origin/master`, at `fork/master` and at this branch
  (`4db010f`), so it is already published in `ezloteam/Ezlo_Pi` and was already
  published in the fork before any work recorded in this changelog began. No
  commit on this branch touches the file at all.

  Two consequences, and the second is the one that was initially got wrong
  here. The first: this is an **upstream disclosure**, so the useful action is
  reporting it to Ezlo, who hold the devices and the cloud account — not
  deleting the file in a fork, which would retract nothing and would make the
  fork diverge from upstream for no security gain. The second: because the
  material is already public and untouched by this branch, **pushing this
  branch does not publish it**. An earlier note treated the push as the
  exposure event and advised holding it on that basis; that was wrong, and the
  distinction matters because "a push would leak the keys" is an argument for
  never pushing, while "the keys are already upstream and public" is an
  argument for filing a report. Only the second is true.

- **Nothing invalidates the three TLS-material caches when provisioning
  overwrites them, so a hub can go on using the certificate and keys it held
  before it was provisioned.**
  [`EZPI_core_factory_info_v3_get_ca_certificate()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L513),
  [`EZPI_core_factory_info_v3_get_ssl_private_key()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L531)
  and
  [`EZPI_core_factory_info_v3_get_ssl_shared_key()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L549)
  each read flash on first call and keep the result in the file statics
  `__ca_cert`, `__pvt_key` and `__pub_key`
  ([`__ca_cert`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L90)–92),
  returning the cached pointer on every later call. The four setters that write
  those same flash regions —
  [`EZPI_core_factory_info_v3_set_ca_cert()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L919),
  [`EZPI_core_factory_info_v3_set_ssl_private_key()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L851),
  [`EZPI_core_factory_info_v3_set_ssl_public_key()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L881)
  and
  [`EZPI_core_factory_info_v3_set_ssl_shared_key()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L900)
  — all go through `__EZPI_core_factory_info_v3_set_4kb()` and none of them
  touches a static. So the order within one uptime is: read, and the cache
  fills; write, and flash changes while the cache does not; read again, and the
  pre-write value comes back.

  Three functions exist to clear exactly these —
  [`EZPI_core_factory_info_v3_free_ca_certificate()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L505),
  [`EZPI_core_factory_info_v3_free_ssl_private_key()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L523)
  and
  [`EZPI_core_factory_info_v3_free_ssl_shared_key()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L541)
  — and a tree-wide search finds **no caller for any of the three**. They are
  declared in the header and called from nowhere, which is the clearest
  statement available that the invalidation was meant to exist.

  The stale value is not merely held, it is handed to the client that connects
  to the cloud. All three provisioning transports write: BLE at
  [`EZPI_core_factory_info_v3_set_ca_cert()`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L530)–532,
  UART at
  [`EZPI_core_factory_info_v3_set_ca_cert()`](ezlopi-services/ezlopi-service-uart/ezlopi_service_uart.c#L658)–660,
  and
  [`__provision_update()`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L664)
  at `:745`, `:751`, `:757` and `:763`. Two of them read first, which is what
  arms the cache:
  [`ezpi_provisioning_info_jsonify()`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L826)
  reads all three at `:841`–843 to answer a read of provisioning info, so an
  app that inspects the current configuration before writing a new one fills
  the cache a few hundred milliseconds before invalidating it on flash. And
  [`__fetch_wss_endpoint()`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L228)
  reads them at `:230`–232 to build the TLS connection itself, with
  [`__provision_check()`](ezlopi-services/ezlopi-service-webprov/ezlopi_service_webprov.c#L550)
  reading them again at `:564`–566.

  No reboot closes the window. The tree contains exactly two live
  `esp_restart()` calls,
  [`esp_restart()`](ezlopi-core/ezlopi-core-api/ezlopi_core_api.c#L329)
  and
  the one at the end of
  [`EZPI_core_reset_reboot()`](ezlopi-core/ezlopi-core-reset/ezlopi_core_reset.c#L84),
  and neither is on the provisioning-success path. The first is a partial mask
  rather than a fix, and worth stating because it shapes the symptom: it
  reboots the hub on the sixth response whose method is `register` — attempts,
  not failures, as pinned by
  `test_register_counter_counts_attempts_not_failures()` in
  [`tests/host/test_api_envelope.c`](tests/host/test_api_envelope.c#L712) — and
  that reboot clears the statics. So what an onboarding user sees is not a
  permanent failure but "it did not take, the hub restarted by itself, now it
  works", with nothing in any log naming a cause.

  Not fixed here, and the reason is specific. Calling the three existing
  invalidators from the four setters is the small change, but the getters hand
  out the cached pointer and their call sites are explicitly told not to free
  it — the `// allocated once for all, do not free` comments at
  [`EZPI_core_factory_info_v3_free`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L203)–205
  and
  [`ezpi_provisioning_info_jsonify()`](ezlopi-services/ezlopi-service-ble/ezlopi_service_ble_provisioning.c#L826)
  `:868`–870.
  Freeing from a setter on the BLE or UART task while the webprov task is
  holding that same pointer inside `__fetch_wss_endpoint()` turns a stale read
  into a use-after-free during TLS setup, which is the worse of the two. An
  in-place update is not available either:
  `__EZPI_core_factory_info_v3_read_string()` allocates `s_length + 1`, sized
  to the content rather than to the field, so a longer replacement certificate
  would not fit the existing buffer. The real fix is to stop caching and give
  each caller its own copy, which rewrites the ownership contract at all six
  read sites and the comments documenting it, and which wants hardware to
  verify. Recorded rather than attempted.

  Two smaller things fell out of the same reading. The field lengths are
  `0x1000` each
  ([`EZLOPI_FINFO_LEN_CA_CERTIFICATE`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.h#L136)–138),
  and because an erased region is all `0xFF` with no zero byte in it, `strlen`
  on an unprovisioned field returns the full 4096 — so on a factory-fresh hub
  these three caches hold about 12 KB of `0xFF`, permanently, whereas on a
  provisioned one they hold only the PEM lengths. And three of the five
  `-Wunused-variable` sites in the tree are `ca_certificate`,
  `ssl_private_key` and `ssl_shared_key` at
  [`EZPI_print_factory_info_v3()`](ezlopi-core/ezlopi-core-factory-info/ezlopi_core_factory_info.c#L144)
  `:163`–165, whose three `TRACE_W` lines are commented out at `:185`–187. They
  are not a leak — the caching makes the "do not free" correct — but they are
  three reads whose only effect is to populate the caches, inside
  `#if (1 == ENABLE_TRACE)`. A trace build therefore arms this staleness at
  boot and a release build does not, which is a difference in behaviour between
  the two builds hiding behind an unused-variable warning.

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
- **RESOLVED, and this entry was wrong about which half was reachable.** It read
  *"One latent free-of-a-literal in the mDNS path, unreachable in every shipped
  configuration"*, and said the fix was unverifiable without a configuration
  nothing currently builds. The free-of-a-literal part was right: it needs
  `CONFIG_EZPI_MDNS_ENABLE_EXTRA_SERVICES`, which is `default n` in Kconfig and
  `is not set` in all four committed defaults, and the one unconditional node
  whose value could in principle be the `"null"` literal takes it from
  `EZPI_core_factory_info_v3_get_device_type()`, which is a
  `return "ezlopi_generic"` and cannot return `NULL`.

  The rest of the entry was filed under that same heading and is not gated on
  anything. The free routine is reached from the *success* path — not only from
  retry exhaustion — so every boot that advertises `_ezlo._tcp` ran it, and it
  never freed the list nodes and never cleared the head. A fifth defect this
  entry did not mention was in the same function's callers: each TXT value was
  assigned by `NULL == getter() ? "null" : getter()`, and every one of those
  getters reads the factory partition into a fresh `ezlopi_malloc`, so the first
  call's string was allocated, tested and dropped unreferenced.

  Fixed under *Ownership of an mDNS TXT value was decided by comparing the
  string* above. The reason the deferral held for as long as it did is the
  claim in its own last sentence, and that claim was not true of the parts of
  the defect that ship.
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
