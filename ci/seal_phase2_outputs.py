#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Seal Phase 2 Mac CI outputs and logs using only the public CMS recipient certificate."""
import argparse, json, shutil, sys
from pathlib import Path

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--logs", type=Path, required=True)
    ap.add_argument("--work-logs", type=Path, required=True)
    ap.add_argument("--sealed", type=Path, required=True)
    args = ap.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    log_dst = out / "logs"
    log_dst.mkdir(parents=True, exist_ok=True)
    for src_dir in (args.logs, args.work_logs):
        if src_dir.is_dir():
            for item in sorted(src_dir.rglob("*")):
                if item.is_file():
                    rel = item.relative_to(src_dir)
                    dst = log_dst / rel
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(item, dst)

    sys.path.insert(0, str((args.device / "ci/xnu").resolve()))
    import artifacts
    files = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    if not files:
        (out / "empty-run.txt").write_text("no outputs produced\n", encoding="utf-8")
        files = ["empty-run.txt"]
    cert = (args.device / "ci/xnu/artifact-recipient.pem").resolve()
    envelope = artifacts.seal(out, files, cert, args.sealed.resolve())
    print(json.dumps(envelope, indent=2))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
