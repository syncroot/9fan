# Security and safety policy

`9fan` is a local hardware-control utility. A defect can affect cooling even
when it is not a conventional security vulnerability.

## Supported version

Only the latest commit on `main` is supported. This repository distributes
source code, not trusted prebuilt binaries.

## Reporting

Report suspected security, SMC, watchdog, temperature-sensor, or restoration
failures through a
[private GitHub security advisory](https://github.com/syncroot/9fan/security/advisories/new).
Do not include serial numbers, usernames, home-directory paths, or unrelated
system logs.

## Safety invariants

- Every active curve is raised to the verified manual maximum at 82 C. A
  temperature sensor failure, a 90 C hotspot, or a serious, critical, or
  unknown Apple thermal state surrenders to Apple's thermal controller, which
  can command beyond the reported manual RPM range. A temperature handoff
  keeps the terminal monitor open and locks normal-curve re-entry until 80 C.
- An explicit Maximum selection during that lockout is restricted to one
  15-second hot-start boost per lockout and cannot be repeated until the
  hotspot reaches 80 C. It is refused if Apple is already targeting or
  achieving above the verified manual ceiling, if any fan is outside Apple
  mode, or if Apple's system thermal state is unsafe. The independent guard
  applies a separate 20-second limit.
- A fan telemetry failure surrenders control to Apple's thermal controller.
- Active control detects repeated physical fan under-response and unexplained
  target changes.
- Every automatic/manual mode transition is read back and verified.
- Manual mode refuses zero or invalid RPM minima and never writes a zero
  target.
- Restoration never writes a zero target; failed automatic restoration makes a
  best-effort maximum-target request.
- The separately installed guard uses an independent minimal SMC recovery
  implementation, requires periodic heartbeats, and retries restoration after
  a controller death or six-second timeout.
- The terminal frontend runs as the invoking user and has no SMC write path.
  The on-demand root engine accepts only fixed commands over a fixed-size,
  versioned protocol on a one-use private Unix socket; it accepts no raw SMC
  keys or RPM values.
- The socket directory is mode `0700`, the socket is mode `0600`, both are
  owned by the invoking user, and their path has one exact constrained shape.
  The engine verifies the frontend UID with kernel peer credentials; the
  frontend independently requires a root peer. The pathname is unlinked
  immediately after acceptance.
- Session descriptors are nonblocking and close-on-exec, so protocol I/O has a
  deadline and the independent guard cannot retain the engine connection.
- The guard has explicit arm and disarm states. It is armed before the first
  manual-mode write and disarmed only after 9fan verifies Apple automatic mode,
  preventing a later clean shutdown from overwriting another controller.
- The engine and guard independently enforce a non-extendable control lease
  using a clock that includes time asleep. A sleep/scheduling gap restores
  Apple control, Maximum shortens the session to ten minutes, and a hot-start
  maximum has separate 15-second engine and 20-second guard limits.
- Ordinary lease expiration restores Apple control and terminates the
  privileged engine while the normal-user frontend continues as a read-only
  monitor. A later profile selection creates a new sudo process, new private
  channel, new guard, and new fixed lease.
- Startup explicitly unblocks handled termination signals and checks every
  handler installation call; control is refused if installation fails.
- Only one privileged controller or self-test may run at a time.
- A newly detected external manual controller is not overwritten, and an
  unexpected mode change during active control is not reasserted.
- A matching root-owned hardware/OS/key-schema validation record is required
  before custom control and is rechecked after the independent guard starts.
- A compiled exact-match allowlist is checked before self-test or curve writes
  and after guard startup. A local self-test cannot enroll unknown hardware.
- Read-only commands do not take the control lock or write SMC keys.
- Builds, tests, analysis, and hashing run unprivileged. Installation invokes
  only fixed absolute-path installation utilities through sudo;
  root never interprets a user-writable checksum manifest, and `sudo make` is
  unsupported and unsafe.
- Installation requires an interactive terminal, refuses the known
  user-local duplicate path and any PATH shadow, and completes only after the
  guarded self-test and final read-only status both succeed. It invalidates the
  old validation marker before replacement, so a failed, interrupted, or
  cancelled install leaves custom curves disabled.
- `make verify` performs a clean rebuild and records checksums for both the
  binaries and every tracked source file. Installation rechecks both before
  and during the transaction, refusing stale build output after a source edit.
- A retry deletes only fixed-name transaction staging files and replaces an
  inconsistent installed set from the freshly verified build. It does not run
  the inconsistent binaries, and custom curves remain disabled until the
  guarded self-test succeeds.

Recovery and limitations are documented in `SAFETY.md`.
