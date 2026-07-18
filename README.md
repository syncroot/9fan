# 9fan

`9fan` is a tiny, dependency-free terminal fan controller for Apple Silicon
Macs. Version 1.2 was built and hardware-validated on one Mac17,9 M5 Pro
MacBook Pro. It probes the hardware at runtime instead of assuming fixed RPM
values or fan-mode key casing.

> [!CAUTION]
> `9fan` uses an undocumented AppleSMC interface and is not Apple-supported.
> A passing self-test validates only the current app version, Mac, and macOS
> build. Do not run it alongside another fan controller.

The app is conservative by design:

- Apple automatic control is always available with `a` or `9fan default`.
- Every curve uses each fan's own SMC-reported minimum and maximum.
- Quiet and Balanced leave Apple in control below their activation temperature,
  preserving the Mac's true 0 RPM idle.
- The hottest valid CPU, GPU, or memory sensor drives both fans.
- Temperature rise is applied immediately; release is damped to prevent hunting.
- A missing or unreadable temperature sensor immediately surrenders to Apple's
  controller.
- A 95 C hotspot immediately surrenders to Apple instead of keeping a manual
  ceiling.
- A fan telemetry failure immediately surrenders control to Apple.
- Normal exit and common termination signals restore Apple control.
- A small watchdog uses an independent minimal SMC connection and retries
  restoration if the UI process crashes, is killed, or stops sending
  heartbeats. Long legacy mode transitions emit progress heartbeats too.
- A root-owned lock prevents two privileged controllers from fighting.
- Starting a curve refuses pre-existing manual fan mode, which protects upgrades
  from competing with an older controller that does not use the lock.
- A later external manual-mode change stops 9fan without overwriting that
  controller; a mode override during active 9fan control stops reassertion and
  surrenders to Apple.
- Hardware validation and complete temperature discovery are rechecked after
  the watchdog reconnects to AppleSMC and before a curve is armed.

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
an emergency mode and 9fan hands control back to Apple at 95 C or on sensor
loss.

These are bounded presets, not Apple-approved thermal specifications. Their
math and controller state transitions are tested, while hardware acceptance
must be checked with the guarded self-test on each Mac before a custom curve is
allowed.

## Verified hardware

On 2026-07-18, the guarded self-test passed on one Mac17,9 Apple M5 Pro running
macOS 26.5.2 (build 25F84). Both fans accepted their SMC-reported 7,826 RPM
maximum target, accelerated to 6,895 and 6,801 RPM, and returned to Apple
automatic mode with a zero target after the test.

This result documents one hardware and OS combination; it is not an
Apple endorsement or a guarantee for another Mac. `9fan` still requires every
machine to pass its own guarded self-test, and invalidates that result after an
app or macOS build change.

## Build and install

Requirements: macOS, Apple Silicon, and Xcode Command Line Tools.

```sh
make
make test
make analyze
sudo make install
```

The default install path is `/usr/local/bin/9fan`. Installation with `sudo`
makes the executable root-owned so an unprivileged process cannot replace it
before a later privileged launch. The install refuses symlinked intermediate
or final path components and requires existing directories to already be
root-owned mode `0755`; it does not silently change ownership of shared
directories. Confirm the complete path:

```sh
ls -ld /usr/local /usr/local/bin
ls -l /usr/local/bin/9fan
```

Do not run a copy from `build/`, `~/.local/bin`, or another user-writable
directory with `sudo`. Fan writes require root:

```sh
sudo /usr/local/bin/9fan
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
sudo /usr/local/bin/9fan balanced
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
sudo /usr/local/bin/9fan self-test
```

Scripts and non-TTY sessions must express intent explicitly:

```sh
sudo /usr/local/bin/9fan self-test --yes
```

The self-test refuses to run when the hotspot is unavailable or above 80 C,
fan telemetry or temperature discovery is incomplete, or another controller
already has a fan outside Apple-controlled mode. It repeats those checks after
the watchdog reconnects and before its first write. Runtime target writes are
also read back together with manual mode and must be accepted before control
continues.

A passing test writes `/var/db/9fan.validation` as a root-owned `0600` file.
The record contains only the 9fan version, Mac model identifier, macOS build,
fan count, and detected mode-key variant. A custom curve refuses to start if
that record is missing or stale, so an app update, OS update, or hardware change
requires the guarded test again.

At any time, force restoration of macOS control with:

```sh
sudo /usr/local/bin/9fan default
```

Recovery uses only the minimal fan-mode keys, so it does not depend on normal
temperature discovery or complete fan telemetry. Restoration never writes a
zero target; after verified Apple-controlled mode (`0` auto or `3` system),
macOS alone chooses whether 0 RPM is appropriate.

Do not use `killall -9 9fan`: it can kill the independent watchdog along with
the controller. See [SAFETY.md](SAFETY.md) for failure and recovery procedures.

## Efficiency

`9fan` is a single native C binary. It has no package dependencies, config
daemon, analytics, or network access. A small watchdog child exists only while
a custom curve is active. SMC sensors are discovered once and sampled every
two seconds; fan targets are rewritten only when the requested speed changes
materially.

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
