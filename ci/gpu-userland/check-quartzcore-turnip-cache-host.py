#!/usr/bin/env python3
"""Stand-in for the device repository's check-quartzcore-turnip-cache-host.py on the GPU userland runner.

docs/drivers/adreno740.md lists that check among the original-input checks a device repository adds: it needs the target's
QuartzCore library from the OS image, which this runner neither has nor may hold. ci/gpu-userland/prepare.py copies this file
into the UniXNU checkout only when the checkout has no such script, so scripts/check-turnip-darwin-host.py records the check as
skipped and runs its remaining checks."""
import sys

print('SKIP: check-quartzcore-turnip-cache-host.py needs the original QuartzCore library (device repository check)', flush=True)
sys.exit(0)
