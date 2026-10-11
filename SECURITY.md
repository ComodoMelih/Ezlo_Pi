# Security Policy

## Reporting a vulnerability

**Please do not open a public GitHub issue for a security problem.**

Use GitHub's private vulnerability reporting on this repository
(**Security → Report a vulnerability**), which opens a private advisory visible
only to the maintainers.

If the issue is in upstream code rather than something this fork introduced,
please also report it to Ezlo Innovation — upstream is
[ezloteam/Ezlo_Pi](https://github.com/ezloteam/Ezlo_Pi), and contact details
are at [ezlopi.com](https://www.ezlopi.com/). This fork cannot ship fixes to
anyone running official EzloPi firmware.

Useful things to include, if you have them:

- Which chip (`esp32`, `esp32s2`, `esp32s3`, `esp32c3`) and which commit.
- The interface involved — BLE, UART, Wi-Fi/cloud, or local HTTP.
- Whether an attacker needs to be provisioned, paired, or merely in range.
- A reproducer. Raw bytes or the exact JSON are ideal.

Please give us a reasonable chance to fix an issue before disclosing it
publicly. This is a hobby-scale fork, not a vendor with an on-call rotation, so
a hard deadline is not promised; what is promised is that reports will be
acknowledged and will not be quietly ignored.

## Scope

### In scope

- Memory-safety defects (overflows, use-after-free, double free) in anything
  that parses input from a remote party.
- Authentication or authorization bypass in the BLE or UART provisioning flow.
- Exposure of credentials or device identity — Wi-Fi PSK, device certificates,
  cloud tokens — in logs, on the air, or to an unauthorized party.
- Anything that lets an unauthenticated party in BLE range reconfigure or brick
  a provisioned device.

### Not in scope

These are known and documented properties of the design, not vulnerabilities.
Reporting them is not useful.

- **BLE provisioning is unauthenticated by design.** Any client in range can
  connect and write Wi-Fi credentials to an unprovisioned device. This is how
  the EzloPi mobile app provisions a board. Proof-of-possession is deliberately
  not implemented in this fork.
- **The serial console is a trusted interface.** Physical access to the UART
  header is full control, including
  [factory reset](README.md#6-troubleshooting). An ESP32 with unencrypted flash
  and no secure boot offers no defence against someone holding the board, and
  this firmware does not pretend otherwise.
- **Flash is not encrypted and secure boot is not enabled** in the shipped
  configuration. Anyone with the board can read the flash, including the Wi-Fi
  PSK and device certificates.
- **The Wi-Fi PSK is sent to the EzloPi cloud.** `hub.network.get` reports it
  as `key` — see
  [`ezlopi_cloud_network.c:174`](ezlopi-cloud/ezlopi-cloud-network/ezlopi_cloud_network.c#L174)
  and the privacy note in the [README](README.md#7-privacy-note). This is
  upstream behaviour, is relied on by the cloud and app, and is deliberately
  left unchanged here. It is documented rather than fixed.
- Denial of service that requires BLE radio proximity, or RF jamming.
- Findings from a static-analysis tool pasted without a reachability argument.

## Supported versions

Only the tip of the default branch. There are no maintained release branches and
no backports.

## Hardening already applied

This fork has had a hardening pass; four memory-safety defects reachable from an
unauthenticated BLE or UART client were fixed. They are described individually
in the [CHANGELOG](CHANGELOG.md#memory-safety--sec), with the reason each one
stayed hidden.

Two pieces of infrastructure exist specifically to stop them coming back:

- The untrusted-input parsing helpers live in one dependency-free component,
  [`ezlopi-util/ezlopi-util-sanitize`](ezlopi-util/ezlopi-util-sanitize/), so
  the firmware and the host tests compile the same source rather than two copies
  that can drift apart.
- [`tests/host/`](tests/host/) regression-tests each defect under
  AddressSanitizer and UndefinedBehaviorSanitizer, and runs on every push. The
  sanitizers matter here: an overrun that happens to land in unused stack
  passes a plain assertion, so a suite without them would have reported these
  bugs as fixed while they were still live.

Run them yourself with:

```sh
make -C tests/host
```

## Deploying this firmware

If you are putting these boards on a network you care about, the two things
worth doing are unrelated to any bug in this repository:

- **Put them on an isolated VLAN or guest network.** The PSK is recoverable from
  the flash of any board someone can physically take, so that PSK should not be
  one that grants access to anything else.
- **Enable flash encryption and secure boot** if the deployment warrants it.
  Neither is on in the shipped configuration, and turning them on is a
  per-deployment decision with irreversible consequences — read the ESP-IDF
  documentation for your chip first.
