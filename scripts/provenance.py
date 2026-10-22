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

