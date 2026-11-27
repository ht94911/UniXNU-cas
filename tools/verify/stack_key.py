#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministic source-byte identity for A840 shader/state records.

Files are sorted by POSIX repository-relative path and hashed as UTF-8
FILE <path> NUL <byte length> NUL followed by raw bytes. Revision salts
are MESA_COMMIT NUL <40 hex characters> NUL and M2V_COMMIT NUL <sha> NUL.
The default Mesa revision is read from the included manifest; the default
translator salt is the task's pinned metal2vulkan v0.2.0 snapshot.
No Git HEAD, host paths, timestamps or verification tools enter the key.
Outputs must resolve inside .cache/w02-l4*. Symlinked source entries fail.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from check_contracts import CACHE_PARENT, GateError, checked_cache_path

SOURCE_ROOTS = ("contracts", "kits/Adreno840", "kits/Adreno840Emit", "translate", "adreno_compile")
EXTENSIONS = {".h", ".c", ".cpp", ".rs", ".toml", ".tsv"}
EXCLUDED = {"test", "tests", "docs", "tools", ".git", "__pycache__", "target", "build", "out", ".cache",
            "apps", "receipts"}
MESA_MANIFEST = "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json"
DEFAULT_M2V_COMMIT = "02aa9ec82c26e0b1d2cfe4627696e0fa51e0291e"


def wave2_cache_path(path: str | Path) -> Path:
    resolved = checked_cache_path(path)
    if resolved == CACHE_PARENT or not resolved.relative_to(CACHE_PARENT).parts:
        raise GateError(f"Cache output must live inside a .cache/<subdir>: {resolved}")
    return resolved


def wave2_cache_directory(path: str | Path) -> Path:
    directory = wave2_cache_path(path)
    directory.mkdir(parents=True, exist_ok=True)
    return wave2_cache_path(directory)


def write_bytes(path: str | Path, data: bytes) -> Path:
    output = wave2_cache_path(path)
    wave2_cache_directory(output.parent)
    output.write_bytes(data)
    return output


def write_report(path: str | Path, report: object) -> Path:
    return write_bytes(path, (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8"))


def revision(value: str, name: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise GateError(f"{name} must be a full 40-character Git SHA")
    return value.lower()


def source_inputs(repo: str | Path) -> list[tuple[str, bytes]]:
    repo = Path(repo).resolve()
    if not repo.is_dir():
        raise GateError(f"missing repository: {repo}")
    files = {}
    for relative_root in SOURCE_ROOTS:
        root = repo / relative_root
        if root.is_symlink() or not root.is_dir():
            raise GateError(f"missing or symlinked source directory: {relative_root}")
        for directory, directories, names in os.walk(root, followlinks=False):
            directories.sort()
            for name in sorted(directories + names):
                path = Path(directory) / name
                if path.is_symlink():
                    raise GateError(f"symlinked source: {path.relative_to(repo).as_posix()}")
            # Only this exact docs entry survives pruning.
            directories[:] = [name for name in directories if name not in EXCLUDED or
                               (relative_root == "adreno_compile" and Path(directory) == root and name == "docs")]
            for name in sorted(names):
                path = Path(directory) / name
                relative = path.relative_to(repo).as_posix()
                if name == ".gitkeep" or not path.is_file():
                    continue
                special = relative == MESA_MANIFEST
                if not special and any(part in EXCLUDED for part in path.relative_to(repo).parts):
                    continue
                if special or relative_root == "contracts" or path.suffix in EXTENSIONS:
                    if "\0" in relative:
                        raise GateError("NUL in source filename")
                    files[relative] = path.read_bytes()
    if MESA_MANIFEST not in files:
        raise GateError(f"missing pinned Mesa manifest: {MESA_MANIFEST}")
    if not any(path.startswith("contracts/") for path in files):
        raise GateError("empty contracts input set")
    return sorted(files.items())


def key_from_digest(digest: bytes) -> int:
    if len(digest) != 32:
        raise GateError("stack digest must be SHA-256")
    key = int.from_bytes(digest[:8], "big")
    if not key:
        raise GateError("zero stack_key_u64 is reserved and must not be emitted")
    return key


def compute_stack_key(repo: str | Path, mesa_commit: str | None = None,
                      m2v_commit: str = DEFAULT_M2V_COMMIT) -> dict:
    inputs = source_inputs(repo)
    try:
        manifest = json.loads(dict(inputs)[MESA_MANIFEST])
        manifest_revision = revision(manifest["mesa_commit"], "manifest mesa_commit")
    except (KeyError, TypeError, ValueError) as exc:
        raise GateError(f"invalid pinned Mesa manifest: {exc}") from exc
    mesa_commit = revision(mesa_commit, "MESA_COMMIT") if mesa_commit is not None else manifest_revision
    m2v_commit = revision(m2v_commit, "M2V_COMMIT")
    digest = hashlib.sha256()
    records = []
    for path, data in inputs:
        digest.update(f"FILE {path}\0{len(data)}\0".encode("utf-8"))
        digest.update(data)
        records.append({"path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    for label, value in (("MESA_COMMIT", mesa_commit), ("M2V_COMMIT", m2v_commit)):
        digest.update(f"{label}\0{value}\0".encode("ascii"))
    raw_digest = digest.digest()
    key = key_from_digest(raw_digest)
    return {"schema_version": 1, "stack_key_u64": key, "stack_key_hex": f"0x{key:016x}",
            "stack_digest_sha256": raw_digest.hex(), "file_count": len(records), "files": records,
            "mesa_commit": mesa_commit, "m2v_commit": m2v_commit}


def header_bytes(report: dict) -> bytes:
    return ("/* SPDX-License-Identifier: MIT */\n"
            "/* Generated by stack_key.py; source bytes and pinned revisions only. */\n"
            "#ifndef A840_STACK_KEY_H\n#define A840_STACK_KEY_H\n"
            "#include <stdint.h>\n"
            f"#define A840_STACK_KEY_U64 UINT64_C({report['stack_key_hex']})\n"
            f"#define A840_STACK_DIGEST_HEX \"{report['stack_digest_sha256']}\"\n"
            f"#define A840_STACK_FILE_COUNT {report['file_count']}u\n"
            "#endif /* A840_STACK_KEY_H */\n").encode("ascii")


def check_header(path: str | Path, report: dict) -> None:
    path = Path(path)
    if path.is_symlink() or path.read_bytes() != header_bytes(report):
        raise GateError(f"STACK_HEADER_MISMATCH: {path}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--mesa-commit")
    parser.add_argument("--m2v-commit", "--m2v-rev", default=DEFAULT_M2V_COMMIT)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--header-out", type=Path)
    parser.add_argument("--check-header", type=Path)
    args = parser.parse_args(argv)
    try:
        outputs = [wave2_cache_path(path) for path in (args.json_out, args.header_out) if path is not None]
        if len(outputs) != len(set(outputs)):
            raise GateError("JSON and header outputs must be distinct")
        report = compute_stack_key(args.repo, args.mesa_commit, args.m2v_commit)
        # Check before writing: --header-out must not conceal a stale --check-header.
        if args.check_header:
            check_header(args.check_header, report)
        if args.json_out:
            write_report(args.json_out, report)
        if args.header_out:
            write_bytes(args.header_out, header_bytes(report))
        print(json.dumps(report, indent=2, sort_keys=True))
    except (GateError, OSError, ValueError) as exc:
        print(json.dumps({"status": "FAIL", "error": str(exc)}, indent=2, sort_keys=True))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
