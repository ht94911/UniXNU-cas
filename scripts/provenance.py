#!/usr/bin/env python3
"""Read-only source and file identities shared by the local build/pack tools."""
import hashlib
import subprocess
from pathlib import Path


def file_ref(path):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def source_state(uefi, device):
    result = {}
    for name, root in (("uefi-cas", uefi), ("UniXNU-cas", device), ("UniXNU", device.parent / "UniXNU")):
        root = Path(root).resolve()

        def git(*args):
            return subprocess.check_output(["git", "-C", str(root), *args])

        status = git("status", "--porcelain=v1", "--untracked-files=all").decode()
        untracked = git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
        result[name] = {"path": str(root), "commit": git("rev-parse", "HEAD").decode().strip(),
                        "dirty": bool(status), "status": status,
                        "diff_sha256": hashlib.sha256(git("diff", "--binary", "HEAD")).hexdigest(),
                        "untracked": [file_ref(root / p.decode()) for p in untracked if p],
                        "submodules": git("submodule", "status", "--recursive").decode()}
    return result


def require_clean(state):
    dirty = [name for name, repo in state.items() if repo["dirty"]]
    if dirty:
        raise ValueError("candidate packaging requires committed clean sources: " + ", ".join(dirty))


def check_ref(ref):
    actual = file_ref(Path(ref["path"]))
    if actual != ref:
        raise ValueError("build input/product changed: " + ref["path"])



def firmware_reuse(previous, current, device):
    """Reuse accepted bytes only when every actual firmware input is unchanged.

    The device checkout contains separate loader/kernel tools. Their commits may
    advance without recompiling the firmware. Unknown changed paths fail closed.
    """
    require_clean(previous)
    require_clean(current)
    for name in ('uefi-cas', 'UniXNU'):
        if previous[name] != current[name]:
            raise ValueError('Firmware reuse source changed: ' + name)
    before, after = previous['UniXNU-cas'], current['UniXNU-cas']
    allowed = {
        'package/CasHandoff/Trace.h', 'package/CasHandoff/TraceAbi.h',
        'package/CasHandoff/TraceView.c', 'config/cpu0-contract.json',
        'scripts/cas_cpu0.py', 'scripts/cpu0_payload.py',
        'scripts/package_cpu0.py', 'scripts/check_cpu0_host.py',
        'scripts/check_trace_view_host.py', 'scripts/provenance.py',
        'tests/cpu0_host.c',
    }
    changed = subprocess.check_output(['git', '-C', str(device), 'diff', '--name-only',
                                       before['commit'], after['commit'], '--']).decode().splitlines()
    unknown = [p for p in changed if p not in allowed and not p.startswith('ci/xnu/') and
               p != '.github/workflows/xnu-source.yml' and not p.startswith('docs/')]
    if unknown:
        raise ValueError('Unclassified firmware reuse changes: ' + ', '.join(unknown))
    return {'result': 'pass', 'firmware_rebuilt': False,
            'uefi_and_framework_sources_identical': True,
            'previous_device_commit': before['commit'], 'current_device_commit': after['commit'],
            'changed_paths_outside_firmware_inputs': changed,
            'reason': 'Only separate kernel/loader/build-evidence tools changed; firmware, shim, menu overlay, DTB and memory inputs unchanged.'}
