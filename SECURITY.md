# Security and safety policy

`9fan` is a local hardware-control utility. A defect can affect cooling even
when it is not a conventional security vulnerability.

## Supported version

Only the latest commit on `main` is supported. This repository distributes
source code, not trusted prebuilt binaries.

## Reporting

Report suspected security, SMC, watchdog, temperature-sensor, or restoration
failures privately through the repository owner's GitHub security contact.
Do not include serial numbers, usernames, home-directory paths, or unrelated
system logs.

## Safety invariants

- A temperature sensor failure or a 95 C hotspot surrenders to Apple's thermal
  controller, which can command beyond the reported manual RPM range.
- A fan telemetry failure surrenders control to Apple's thermal controller.
- Every automatic/manual mode transition is read back and verified.
- Manual mode refuses zero or invalid RPM minima and never writes a zero
  target.
- Restoration never writes a zero target; failed automatic restoration makes a
  best-effort maximum-target request.
- The watchdog uses a minimal connection, requires periodic heartbeats, and
  retries restoration after a controller death or timeout.
- Only one privileged controller or self-test may run at a time.
- A newly detected external manual controller is not overwritten, and an
  unexpected mode change during active control is not reasserted.
- A matching root-owned hardware/OS validation record is required before
  custom control and is rechecked after the watchdog reconnects.
- Read-only commands do not take the control lock or write SMC keys.

Recovery and limitations are documented in `SAFETY.md`.
