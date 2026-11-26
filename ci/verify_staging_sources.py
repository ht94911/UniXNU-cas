#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail-closed integrity and frozen-contract check for Phase 2 Mac CI staging sources."""
import argparse, hashlib, json, platform, subprocess, tempfile
from pathlib import Path

def check_contract_headers(contracts_dir: Path, cache_dir: Path) -> int:
    headers = sorted(p.name for p in contracts_dir.glob("*.h") if p.is_file())
    if not headers:
        raise SystemExit("NO_CONTRACT_HEADERS")
    cache_dir.mkdir(parents=True, exist_ok=True)
    compilers = (
        (["xcrun", "clang"], "c11", "c"),
        (["xcrun", "clang++"], "c++17", "c++"),
    ) if platform.system() == "Darwin" else (
        (["gcc"], "c11", "c"),
        (["g++"], "c++17", "c++"),
    )
    with tempfile.TemporaryDirectory(prefix="contracts-", dir=cache_dir) as scratch:
        for idx, header in enumerate(headers):
            src = f'#include "{header}"\nint main(void) {{ return 0; }}\n'
            for cc, std, lang in compilers:
                out_obj = Path(scratch) / f"{idx}-{lang.replace('+', 'p')}.o"
                cmd = [*cc, f"-std={std}", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-I", str(contracts_dir), "-x", lang, "-c", "-", "-o", str(out_obj)]
                res = subprocess.run(cmd, input=src, text=True, capture_output=True, check=False)
                if res.returncode != 0:
                    raise SystemExit(f"CONTRACT_COMPILE_FAILED {' '.join(cc)} {header}: {res.stderr.strip()}")
    return len(headers)

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--check-contracts", action="store_true")
    ap.add_argument("--cache-dir", type=Path)
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
        digest = hashlib.sha256(data).hexdigest()
        if len(data) != meta["bytes"] or digest != meta["sha256"]:
            raise SystemExit(f"STAGING_HASH_MISMATCH: {rel}")
    extra = ""
    if args.check_contracts:
        cache = args.cache_dir.resolve() if args.cache_dir else (root.parent / ".contracts-cache")
        n_hdr = check_contract_headers(root / "contracts", cache)
        extra = f" + {n_hdr} frozen contract headers (strict C11/C++17)"
    print(f"PASS: verified {len(files)} committed staging files (0 drift){extra}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
