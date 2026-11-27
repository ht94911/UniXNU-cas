#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify fixed SHA-256 staging manifest for Phase 6 Release Mac CI."""
import argparse, hashlib, json
from pathlib import Path

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    args = ap.parse_args()
    root = args.root.resolve()
    manifest = json.loads((root / "source-sha256.json").read_text(encoding="utf-8"))
    files = manifest["files"]
    actual = sorted(
        p.relative_to(root).as_posix()
        for p in root.rglob("*")
        if p.is_file() and ".git" not in p.parts and p.relative_to(root).as_posix() != "source-sha256.json"
    )
    expected = sorted(files.keys())
    if actual != expected:
        raise SystemExit(f"STAGING_FILE_SET_MISMATCH: extra={set(actual)-set(expected)} missing={set(expected)-set(actual)}")
    for rel, meta in files.items():
        data = (root / rel).read_bytes()
        if len(data) != meta["bytes"] or hashlib.sha256(data).hexdigest() != meta["sha256"]:
            raise SystemExit(f"STAGING_HASH_MISMATCH: {rel}")
    print(f"PASS: verified {len(files)} release staging files (0 drift)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
