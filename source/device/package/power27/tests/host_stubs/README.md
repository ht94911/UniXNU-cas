# Host contract fixtures

These are deliberately minimal host-only models of locks, kernel threads and VFS
interfaces. They are used by `scripts/check-power27-control.py` to execute the
actual runtime, logger and diagnostic source with simulated IO and fault cases.
They are not XNU headers, hardware emulation or an ABI/hardware qualification.
Production compilation uses the selected native XNU compilation records and must
never include this directory.
