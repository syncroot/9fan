# Hardware safety and recovery

`9fan` uses an undocumented AppleSMC interface. Apple can change this interface
in a macOS or firmware update. Do not treat this software as a substitute for
Apple's firmware thermal protections.

## Before selecting a curve

1. Run `make verify`, then unprivileged `make install`. Never run `sudo make`.
   Confirm the frontend, engine, and guard have the ownership documented in
   `README.md`.
2. Run `9fan status`; confirm every fan has a valid minimum and maximum.
3. Confirm `Temp keys` does not report `(INCOMPLETE)`.
4. Stop any other fan-control utility.
5. Run the guarded self-test while the Mac is cool and idle.

The self-test refuses to run above an 80 C hotspot. Non-interactive runs require
the explicit `self-test --yes` command. It raises fans to their reported maximum
and verifies target and actual response; it never lowers cooling. It refuses
pre-existing manual mode, and normal controller startup will not take over a
fan that is already manual. A later external manual-mode change stops 9fan
without overwriting that controller, while a mode change during active 9fan
control is treated as an Apple override and is not reasserted.

Custom curves require the root-owned validation marker created by a passing
self-test. The marker is tied to the app version, Mac model and chip, macOS
build, fan count, mode-key variant, fan limits, and SMC sensor/key schema, so
relevant changes require a new test.

Starting self-test invalidates the prior record before its first guarded write.
Cancellation or failure leaves custom curves disabled until a later test
passes.

Manual writes are additionally limited to the exact compiled platform profile.
Unknown models, OS builds, fan layouts, key formats, and SMC schemas cannot
self-enroll; only read-only status and Apple-default recovery remain available.

The SMC-reported maximum is not necessarily the physical maximum. A custom
curve exits and restores Apple control if temperature telemetry is lost, the
hotspot reaches 90 C, or Apple's system thermal state becomes serious, critical,
or unknown, allowing macOS to use its full emergency cooling policy.

After an eight-second spin-up allowance, active control continuously verifies
that each fan physically follows a conservative fraction of its requested
target. Three consecutive under-response samples or an unexplained target
change causes Apple handoff.

Quiet, Balanced, and Performance have a non-extendable 30-minute session lease.
Maximum shortens the remaining session to no more than 10 minutes, and
self-test has a two-minute lease. Both the engine and independent guard enforce
time using a clock that includes sleep. Expiration or a sleep/scheduling gap
restores Apple control. Restarting control requires fresh authorization and all
preflight checks.

## Recovery

Normal exit, frontend disconnect, `q`, Ctrl-C, SIGTERM, SIGHUP, and SIGQUIT
restore Apple control.
The separately installed `9fan-guard` executable has an independent minimal SMC
restore implementation. It begins recovery if the controller dies or stops
sending heartbeats for six seconds and retries for at least 60 seconds.
The recovery path does not depend on temperature discovery or complete normal
telemetry, and it never writes a zero target.

The engine control lock is released if the engine crashes while the independent
guard may still be completing recovery. A newly started engine will refuse the
remaining manual mode rather than fight that recovery.

Manual control also refuses a zero SMC minimum or any computed zero target.
Long legacy mode transitions continue sending watchdog heartbeats; a failed
interactive request to restore Apple control terminates with an error instead
of continuing without watchdog coverage.

When an active curve falls below its manual-control threshold, 9fan first
verifies Apple automatic mode and then disarms the guard. If another controller
subsequently selects manual mode, 9fan exits without having a clean guard
shutdown overwrite that controller. Any unexpected guard-channel failure still
uses the fail-safe Apple restoration path.

If the display is unresponsive, run the explicit recovery command from another
terminal:

```sh
/usr/local/bin/9fan default
```

If the frontend is unavailable:

```sh
sudo /usr/local/libexec/9fan-engine --default
```

If another controller is still running, stop that controller normally; it may
otherwise select manual mode again on its next sample.

`killall -9 9fan-engine` does not match the separately named `9fan-guard`, so
the guard can recover from a forcibly killed privileged engine. Never kill
`9fan-guard` while a curve is active.

After a kernel panic, power interruption, or forced termination of both
processes, run the same recovery command after login. If status still reports
manual mode, shut down the Mac completely and seek service rather than
continuing under load.

## Preset status

The built-in curves are monotonic, bounded by each fan's SMC-reported limits,
and covered by automated interpolation and state-machine tests. Hardware
acceptance is checked locally by `self-test`. The presets are operational
policies, not Apple-approved thermal specifications, and should not be
described as universally validated across Mac models.
