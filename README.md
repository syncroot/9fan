# 9fan

`9fan` is a small, dependency-free terminal fan controller for one explicitly
verified Apple Silicon platform. Version 1.4.1 separates the normal-user
terminal frontend from a minimal, on-demand root control engine. The only
currently allowlisted profile is the locally tested Mac17,9 M5 Pro and macOS
build documented below.

> [!CAUTION]
> `9fan` uses an undocumented AppleSMC interface and is not Apple-supported.
> Unknown models, chips, macOS builds, fan layouts, mode-key formats, or SMC
> schemas are read-only. A local self-test cannot promote an unknown platform
> into the allowlist. Do not run 9fan alongside another fan controller.

The app is conservative by design:

- Apple automatic control is always available with `a` or `9fan default`.
- Every curve uses each fan's own SMC-reported minimum and maximum.
- Quiet and Balanced leave Apple in control below their activation temperature,
  preserving the Mac's true 0 RPM idle.
- The hottest valid sensor from a conservative set of SMC thermal families
  drives both fans. This includes CPU/GPU/memory and enclosure/proximity-style
  sensors, so an unexpectedly hot peripheral sensor can only request earlier
  cooling or Apple handoff.
- Temperature rise is applied immediately; release is damped to prevent hunting.
- A missing or unreadable temperature sensor immediately surrenders to Apple's
  controller.
- A 90 C hotspot immediately surrenders to Apple instead of keeping a manual
  ceiling.
- Apple's supported
  [`ProcessInfo.thermalState`](https://developer.apple.com/documentation/foundation/processinfo/thermalstate-swift.property)
  is checked before control and every 200 ms while active. Serious, critical,
  or unknown state surrenders to Apple.
- A fan telemetry failure immediately surrenders control to Apple.
- Active control continuously checks target ownership and physical fan
  response. An unexplained target change or repeated under-response surrenders
  to Apple.
- Normal exit and common termination signals restore Apple control.
- The terminal frontend never runs as root and contains no SMC write path.
- The on-demand root engine accepts only fixed curve/default commands over a
  versioned, fixed-size protocol on a one-use private Unix socket. Both sides
  verify the peer UID; the engine also validates the socket's constrained
  pathname, owner, and modes. It does not parse terminal input, arbitrary
  paths, configuration files, arbitrary RPM values, or arbitrary SMC keys.
- The private socket keeps the binary protocol independent of sudo's
  pseudo-terminal behavior. Its directory and pathname are removed as soon as
  the root engine connects, while the accepted connection remains available
  for crash detection.
- A separately installed `9fan-guard` executable has its own minimal SMC
  recovery implementation. It restores after six missed seconds and retries
  for at least 60 seconds if the controller crashes, is killed, or stops
  sending heartbeats. Long legacy transitions emit progress heartbeats too.
- A root-owned lock prevents two privileged controllers from fighting.
- Starting a curve refuses pre-existing manual fan mode, which protects upgrades
  from competing with an older controller that does not use the lock.
- A later external manual-mode change stops 9fan without overwriting that
  controller; a mode override during active 9fan control stops reassertion and
  surrenders to Apple.
- After 9fan verifies a return to Apple automatic mode, it explicitly disarms
  the guard before allowing another controller to take ownership. A later
  clean guard shutdown therefore cannot overwrite that controller.
- Hardware validation and complete temperature discovery are rechecked after
  the guard starts and before a curve is armed.
- Every session has a non-extendable, sleep-aware lease. Quiet, Balanced, and
  Performance are limited to 30 minutes; selecting Maximum shortens the
  remaining session to at most 10 minutes. Self-test is limited to two minutes.
  Expiration or a sleep/scheduling gap restores Apple control.

## Curves

Percentages below mean a position between the fan's reported minimum and
maximum, not a percentage of absolute RPM.

| Curve | Behavior |
| --- | --- |
| Apple default | macOS `thermalmonitord` controls the fans, including 0 RPM |
| Quiet | Apple auto below 65 C; 65 C min, 75 C 35%, 82 C 70%, 90 C max |
| Balanced | Apple auto below 55 C; 55 C min, 67 C 35%, 78 C 70%, 88 C max |
| Performance | Apple auto below 40 C; 40 C min, 55 C 45%, 68 C 75%, 82 C max |
| Maximum | SMC-reported maximum at every temperature |

The presets intentionally never request a manual RPM below the SMC minimum or
above its reported maximum. This matters on M5, whose firmware behavior differs
from earlier Apple Silicon generations. A zero or otherwise invalid reported
manual minimum is rejected rather than written as a target.

The reported maximum is a conservative manual ceiling, not the fan's physical
limit. Apple may command higher emergency cooling, so the Maximum preset is not
an emergency mode and 9fan hands control back to Apple at 90 C or on sensor
loss.

These are bounded presets, not Apple-approved thermal specifications. Their
math and controller state transitions are tested. Manual writes are compiled
to default-deny every hardware/OS/schema combination except an exact verified
profile, and that machine must also have a current local validation record.

## Verified hardware

On 2026-07-18, the guarded self-test passed on one Mac17,9 Apple M5 Pro running
macOS 26.5.2 (build 25F84). Both fans accepted their SMC-reported 7,826 RPM
maximum target, sustained 7,336 and 7,331 RPM under the stricter response test,
and returned to Apple automatic mode with a zero target after the test. The
validated SMC schema fingerprint was `651d1eadd3e88f2a`.

This result documents one hardware and OS combination; it is not an Apple
endorsement or a guarantee for another Mac. Version 1.4.1 permits manual
control only when the complete observed identity matches this compiled profile:
`Mac17,9`, `Apple M5 Pro`, `25F84`, two fans, `F%dmd`, and schema
`651d1eadd3e88f2a`. It also requires that machine to pass the guarded self-test
again after an app change. A macOS build change remains read-only until a new
9fan release explicitly adds evidence for it.

## Build and install

Requirements: macOS, Apple Silicon, and Xcode Command Line Tools.

```sh
make verify
make install
```

Never run `sudo make`, including `sudo make install`. `make verify` builds,
tests, analyzes, and hashes all three binaries as your normal user. The
interactive, unprivileged `make install` target verifies those hashes and
the source-tree checksum from a clean `make verify`, checks directory safety,
invalidates prior hardware validation, verifies root-owned temporary copies,
and invokes fixed absolute-path system utilities through `sudo` for atomic
per-file replacement. Root never interprets a user-writable checksum manifest.
It then requires confirmation for the guarded hardware self-test and finishes
with read-only status. Cancellation, interruption, or failure leaves custom
curves disabled and makes the target fail safely. The resulting installed
digest is root-owned at `/usr/local/libexec/9fan.SHA256SUMS`.

Installation refuses a duplicate `~/.local/bin/9fan`, any other `9fan` that
shadows the installed path, or a PATH without `/usr/local/bin`. This prevents
an older executable from shadowing or confusing the verified
`/usr/local/bin/9fan`. Run `make verify` again after every source or
documentation edit; installation refuses a stale source checksum. A retry
removes fixed-name staging leftovers and safely replaces a partially installed
or checksum-inconsistent set from the newly verified build, while hardware
validation remains disabled until the new self-test succeeds.

The installed paths are:

- `/usr/local/bin/9fan` — normal-user frontend
- `/usr/local/libexec/9fan-engine` — on-demand root controller
- `/usr/local/libexec/9fan-guard` — independent root recovery process

Control sessions and self-test refuse to run unless the engine, its directory,
and its guard are root-owned mode `0755`. The narrow `--default` recovery path
still works if the separate guard is missing, because recovery must have fewer
dependencies. Confirm the paths:

```sh
ls -ld /usr/local /usr/local/bin /usr/local/libexec
ls -l /usr/local/bin/9fan /usr/local/libexec/9fan-{engine,guard}
```

Run the frontend without `sudo`. It requests authorization only when starting
the fixed control engine:

```sh
/usr/local/bin/9fan
```

Keys in the interactive screen:

| Key | Action |
| --- | --- |
| `a` or `0` | Restore Apple automatic control |
| `1` | Quiet |
| `2` | Balanced |
| `3` | Performance |
| `4` | Maximum |
| `q` or `Ctrl-C` | Restore Apple automatic control and quit |

You can also start directly on a curve:

```sh
/usr/local/bin/9fan balanced
```

Use `--duration` to shorten a lease. Scripts and non-TTY curve sessions must
provide it explicitly:

```sh
/usr/local/bin/9fan balanced --duration 15
```

## Verification and recovery

Read-only hardware discovery does not need sudo:

```sh
9fan status
```

To verify that this Mac accepts the M5 fan-control sequence, run the safe
self-test. It only raises cooling: each fan is briefly sent to its reported
maximum while 9fan verifies manual mode, target readback, and actual RPM
response, then immediately restores Apple control:

```sh
/usr/local/bin/9fan self-test
```

Scripts and non-TTY sessions must express intent explicitly:

```sh
/usr/local/bin/9fan self-test --yes
```

The self-test refuses to run when the hotspot is unavailable or above 80 C,
Apple reports an unsafe thermal state, fan telemetry or temperature discovery
is incomplete, or another controller already has a fan outside
Apple-controlled mode. It repeats those checks after the guard starts and
before its first write. Each fan must remain above 50% of its usable RPM span
for three consecutive samples. Runtime target writes are read back, and active
curves continue checking physical response after an eight-second spin-up
allowance.

A passing test writes `/var/db/9fan.validation` as a root-owned `0600` file.
The record contains only the 9fan version, Mac model and chip identifiers,
macOS build, fan count, detected mode-key variant, and a fingerprint of tested
fan limits and SMC key schemas. A custom curve refuses to start if that record
is missing or stale, so an app update, OS update, sensor/key change, or hardware
change requires the guarded test again.

Self-test removes the previous validation record before its first guarded
write. A cancelled or failed test therefore leaves curves disabled until a
later test passes; this is intentionally fail-safe.

Self-test is refused before any write if the full compiled platform profile
does not match. It is not an experimental enrollment mechanism for other Macs.

At any time, force restoration of macOS control with:

```sh
/usr/local/bin/9fan default
```

If the frontend itself is unavailable, the narrow emergency recovery entry
point is:

```sh
sudo /usr/local/libexec/9fan-engine --default
```

Recovery uses only the minimal fan-mode keys, so it does not depend on normal
temperature discovery or complete fan telemetry. Restoration never writes a
zero target; after verified Apple-controlled mode (`0` auto or `3` system),
macOS alone chooses whether 0 RPM is appropriate.

For fault testing, `killall -9 9fan-engine` targets the privileged controller
but not the separately named `9fan-guard`; the guard treats the closed
heartbeat pipe as a crash and restores Apple control. Closing or killing the
normal-user frontend also closes the engine channel and causes restoration.
Never kill `9fan-guard` while a curve is active. See [SAFETY.md](SAFETY.md).

## Efficiency

`9fan` installs three small native binaries: an unprivileged frontend, an
on-demand privileged engine, and its independent recovery guard. It has no
package dependencies, persistent daemon, config file, analytics, or network
access. A tiny Objective-C bridge reads Apple's supported thermal-state
property; all fan control remains native C. The engine and guard exist only
while a control or self-test request is active. SMC sensors are discovered once
and sampled every two seconds; fan targets are rewritten only when the
requested speed changes materially.

## Private API and safety notice

Apple does not publish a supported macOS API for direct fan control. `9fan`
uses the AppleSMC user-client interface and therefore may require maintenance
after macOS or hardware changes. Manual fan control can interfere with macOS
thermal management. Use it at your own risk.

The M5 key probing, direct manual-mode transition, RPM encoding, and default
restoration behavior follow the current MIT-licensed
[macos-smc-fan research and implementation](https://github.com/agoodkind/macos-smc-fan).
That project documents successful M5 Max testing, including the lowercase
`F%dmd` mode key and direct mode writes. `9fan` probes both lowercase and
uppercase variants rather than hard-coding a generation.

See [NOTICE](NOTICE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) for
attribution and license text. This repository is source-only; no unsigned
prebuilt release should be treated as trusted.
