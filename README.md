# 9fan

`9fan` is a tiny, dependency-free terminal fan controller for Apple Silicon
Macs. It was built for the M5 Pro MacBook Pro and probes the hardware at
runtime instead of assuming fixed RPM values or fan-mode key casing.

The app is conservative by design:

- Apple automatic control is always available with `a` or `9fan default`.
- Every curve uses each fan's own SMC-reported minimum and maximum.
- Quiet and Balanced leave Apple in control below their activation temperature,
  preserving the Mac's true 0 RPM idle.
- The hottest valid CPU, GPU, or memory sensor drives both fans.
- Temperature rise is applied immediately; release is damped to prevent hunting.
- A missing temperature sensor forces maximum RPM.
- Normal exit and common termination signals restore Apple control.
- A small watchdog restores Apple control if the UI process crashes or is killed.

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
from earlier Apple Silicon generations.

## Build and install

Requirements: macOS, Apple Silicon, and Xcode Command Line Tools.

```sh
make
make test
make install
```

The default install path is `~/.local/bin/9fan`. If that directory is in your
`PATH`, launch the monitor with:

```sh
9fan
```

Fan writes require root. `sudo` may use a restricted `PATH`, so this form works
reliably:

```sh
sudo "$(command -v 9fan)"
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
sudo "$(command -v 9fan)" balanced
```

## Verification and recovery

Read-only hardware discovery does not need sudo:

```sh
9fan status
```

To verify that this Mac accepts the M5 fan-control sequence, run the safe
self-test. It requests only each fan's reported minimum, verifies that the SMC
accepted manual mode and the target, then immediately restores Apple control:

```sh
sudo "$(command -v 9fan)" self-test
```

At any time, force restoration of macOS control with:

```sh
sudo "$(command -v 9fan)" default
```

## Efficiency

`9fan` is a single native C binary. It has no package dependencies, config
daemon, analytics, network access, or recurring subprocesses. SMC sensors are
discovered once and sampled every two seconds; fan targets are rewritten only
when the requested speed changes materially.

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

See [NOTICE](NOTICE) for attribution.
