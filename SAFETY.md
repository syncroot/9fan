# Hardware safety and recovery

`9fan` uses an undocumented AppleSMC interface. Apple can change this interface
in a macOS or firmware update. Do not treat this software as a substitute for
Apple's firmware thermal protections.

## Before selecting a curve

1. Install the executable into `/usr/local/bin` as a root-owned file.
2. Run `9fan status`; confirm every fan has a valid minimum and maximum.
3. Confirm `Key cap` reports `complete`.
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
self-test. The marker is tied to the app version, Mac model, macOS build, fan
count, and mode-key variant, so relevant changes require a new test.

The SMC-reported maximum is not necessarily the physical maximum. A custom
curve exits and restores Apple control if temperature telemetry is lost or the
hotspot reaches 95 C, allowing macOS to use its full emergency cooling policy.

## Recovery

Normal exit, `q`, Ctrl-C, SIGTERM, SIGHUP, and SIGQUIT restore Apple control.
The independent watchdog retries restoration if the controller process dies
or stops sending heartbeats for 15 seconds.
The recovery path does not depend on temperature discovery or complete normal
telemetry, and it never writes a zero target.

Manual control also refuses a zero SMC minimum or any computed zero target.
Long legacy mode transitions continue sending watchdog heartbeats; a failed
interactive request to restore Apple control terminates with an error instead
of continuing without watchdog coverage.

If the display is unresponsive, do not use `killall -9 9fan`, because that can
kill both the controller and its watchdog. From another terminal, run:

```sh
sudo /usr/local/bin/9fan default
```

If another controller is still running, stop that controller normally; it may
otherwise select manual mode again on its next sample.

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
