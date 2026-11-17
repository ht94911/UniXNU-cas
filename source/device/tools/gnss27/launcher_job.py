#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Use one source job for the GNSS LaunchDaemon and service-cache entry."""
from pathlib import Path
import plistlib

LABEL = "org.unixnu.nezha.location-launch"
DAEMON_MEMBER = "daemons/" + LABEL + ".plist"
CACHE_KEY = "/System/Library/LaunchDaemons/" + LABEL + ".plist"


def apply(files, source):
    """Update the daemon member and return that exact job for cache merging."""
    raw = Path(source).read_bytes()
    job = plistlib.loads(raw)
    if job.get("Label") != LABEL or job.get("ProgramArguments") != ["/usr/libexec/nezha-location-launch"]:
        raise ValueError("Expected the GNSS launcher job")
    if job.get("UserName") != "mobile" or job.get("LimitLoadToSessionType") != "System":
        raise ValueError("The launcher must use the existing mobile user in the System session")
    if job.get("RunAtLoad") is not True or job.get("LaunchOnlyOnce") is not True or job.get("KeepAlive") is not False:
        raise ValueError("The launcher must run once at load")
    if "StartInterval" in job:
        raise ValueError("The helper owns its startup delay")
    if DAEMON_MEMBER not in files:
        raise ValueError("The selected addon has no GNSS daemon member")
    files[DAEMON_MEMBER] = (0o644, raw)
    return job


def check(files):
    """Reject the observed old-daemon/new-cache mismatch before packaging."""
    daemon = plistlib.loads(files[DAEMON_MEMBER][1])
    cache = plistlib.loads(files["xpc/launchd.plist"][1])
    if cache["LaunchDaemons"].get(CACHE_KEY) != daemon:
        raise ValueError("GNSS daemon and service cache describe different jobs")
    return daemon
