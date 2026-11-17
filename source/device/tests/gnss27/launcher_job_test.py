# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the actual old daemon and new cache mismatch found in GNSS004."""
import importlib.util
from pathlib import Path
import plistlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gnss_launcher_job", ROOT / "tools/gnss27/launcher_job.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
old_job = dict(Label=module.LABEL, ProgramArguments=["/usr/libexec/nezha-location-launch"],
               UserName="root", RunAtLoad=False, LaunchOnlyOnce=True, KeepAlive=False,
               LimitLoadToSessionType="System", StartInterval=120)
source = ROOT / "package/gnss27/userland/org.unixnu.nezha.location-launch.plist"
new_job = plistlib.loads(source.read_bytes())
files = {
    module.DAEMON_MEMBER: (0o644, plistlib.dumps(old_job)),
    "xpc/launchd.plist": (0o644, plistlib.dumps({"LaunchDaemons": {module.CACHE_KEY: new_job}})),
    "unchanged": (0o755, b"retained"),
}
try:
    module.check(files)
except ValueError:
    pass
else:
    raise AssertionError("The old daemon and new cache mismatch was accepted")
job = module.apply(files, source)
assert job == new_job and module.check(files) == new_job
assert files["unchanged"] == (0o755, b"retained")
assert files[module.DAEMON_MEMBER] == (0o644, source.read_bytes())
print("GNSS launcher daemon/cache mismatch reproduced and corrected")
