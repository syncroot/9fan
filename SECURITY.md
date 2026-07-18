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

- A temperature sensor failure, a 90 C hotspot, or a serious, critical, or
  unknown Apple thermal state surrenders to Apple's thermal controller, which
  can command beyond the reported manual RPM range.
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
  versioned pipe protocol; it accepts no raw SMC keys or RPM values.
- The engine and guard independently enforce a non-extendable control lease
  using a clock that includes time asleep. A sleep/scheduling gap restores
  Apple control, and Maximum shortens the session to ten minutes.
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
  only fixed absolute-path system copy/compare/rename utilities through sudo;
  `sudo make` is unsupported and unsafe.

Recovery and limitations are documented in `SAFETY.md`.
