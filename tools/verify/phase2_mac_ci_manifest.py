#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministic public-source inventory for a later encrypted Mac build.

This tool only inventories local source bytes; it never uploads or packages.
Source roots and extensions form an explicit allowlist. Forbidden paths,
binary/private-input suffixes, symlinks, key material, and the configured
device identifier fail closed. Existing build/__pycache__/target directories
are excluded from source inputs. All output paths use the shared cache guard.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path

from check_contracts import GateError, checked_cache_path, write_json
from stack_key import compute_stack_key

PUBLIC_ROOTS = ("contracts", "kits", "translate", "adreno_compile", "drivers",
                "apps/NezhaGpuTest", "tools/verify")
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".inc", ".m", ".mm", ".metal",
                   ".rs", ".toml", ".tsv", ".json", ".py", ".md", ".txt", ".xml", ".plist",
                   ".xcconfig", ".cmake", ".def"}
SOURCE_NAMES = {"Makefile", "CMakeLists.txt", "meson.build", "meson.options", "Cargo.lock"}
FORBIDDEN_SUFFIXES = {".metallib", ".ipsw", ".dmg", ".img", ".dtb", ".mbn", ".elf", ".bin",
                      ".pem", ".key", ".p12", ".mobileprovision", ".log"}
FORBIDDEN_SEGMENTS = {"private", "evidence", "runs", ".cache", "receipts"}
BUILD_SEGMENTS = {".git", "__pycache__", "target", "build", "out"}
# Keep actual sensitive tokens out of this public verification source.
DEVICE_IDENTIFIER = b"c7fe" + b"a50c"
PRIVATE_HEADERS = tuple(b"BEGIN " + kind + b"PRIVATE KEY" for kind in
                        (b"", b"RSA ", b"OPENSSH ", b"EC ", b"ENCRYPTED "))


def validate_public_path(relative: str) -> None:
    path = Path(relative)
    segments = relative.replace("\\", "/").split("/")
    if path.is_absolute() or any(part in {"", ".", ".."} for part in segments):
        raise GateError(f"INVALID_PUBLIC_PATH: {relative}")
    if any(part.lower() in FORBIDDEN_SEGMENTS for part in segments):
        raise GateError(f"FORBIDDEN_INPUT_PATH: {relative}")
    if set(suffix.lower() for suffix in path.suffixes) & FORBIDDEN_SUFFIXES:
        raise GateError(f"FORBIDDEN_INPUT_SUFFIX: {relative}")


def validate_source_bytes(relative: str, data: bytes) -> None:
    if DEVICE_IDENTIFIER in data.lower():
        raise GateError(f"DEVICE_IDENTIFIER_IN_SOURCE: {relative}")
    if any(header in data.upper() for header in PRIVATE_HEADERS):
        raise GateError(f"PRIVATE_KEY_IN_SOURCE: {relative}")
    if b"\0" in data:
        raise GateError(f"BINARY_CONTENT_IN_SOURCE: {relative}")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise GateError(f"NON_UTF8_SOURCE: {relative}") from exc


def public_sources(repo: str | Path) -> list[tuple[str, bytes]]:
    raw_repo = Path(repo)
    if raw_repo.is_symlink():
        raise GateError("SYMLINK_INPUT: repository root")
    repo = raw_repo.resolve()
    if not repo.is_dir():
        raise GateError(f"MISSING_REPOSITORY: {repo}")
    # Audit forbidden inputs even when they are outside the selected roots;
    # the only skipped trees are Git metadata and ordinary build caches.
    for directory, directories, names in os.walk(repo, followlinks=False):
        directories.sort()
        for name in sorted(directories + names):
            path = Path(directory) / name
            relative = path.relative_to(repo).as_posix()
            if path.is_symlink():
                raise GateError(f"SYMLINK_INPUT: {relative}")
            if name in BUILD_SEGMENTS:
                continue
            validate_public_path(relative)
        directories[:] = [name for name in directories if name not in BUILD_SEGMENTS]
    records = {}
    for relative_root in PUBLIC_ROOTS:
        root = repo / relative_root
        parent = repo
        for part in Path(relative_root).parts:
            parent /= part
            if parent.is_symlink():
                raise GateError(f"SYMLINK_INPUT: {relative_root}")
        if not root.is_dir():
            continue
        for directory, directories, names in os.walk(root, followlinks=False):
            directories.sort()
            for name in sorted(directories + names):
                path = Path(directory) / name
                relative = path.relative_to(repo).as_posix()
                if path.is_symlink():
                    raise GateError(f"SYMLINK_INPUT: {relative}")
                validate_public_path(relative)
            directories[:] = [name for name in directories if name not in BUILD_SEGMENTS]
            for name in sorted(names):
                path = Path(directory) / name
                relative = path.relative_to(repo).as_posix()
                if name == ".gitkeep":
                    continue
                if not path.is_file():
                    raise GateError(f"NONREGULAR_SOURCE_INPUT: {relative}")
                if path.suffix not in SOURCE_SUFFIXES and name not in SOURCE_NAMES:
                    raise GateError(f"UNLISTED_SOURCE_TYPE: {relative}")
                data = path.read_bytes()
                validate_source_bytes(relative, data)
                records[relative] = data
    if not records:
        raise GateError("EMPTY_PUBLIC_SOURCE_MANIFEST")
    return sorted(records.items())


def generate_manifest(repo: str | Path) -> dict:
    sources = public_sources(repo)
    files = [{"path": relative, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
             for relative, data in sources]
    manifest = {"schema_version": 1, "kind": "PHASE2_PUBLIC_SOURCE", "roots": list(PUBLIC_ROOTS),
                "file_count": len(files), "total_bytes": sum(row["bytes"] for row in files),
                "files": files, "stack_key": compute_stack_key(repo)}
    serialized = json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    manifest["manifest_sha256"] = hashlib.sha256(serialized).hexdigest()
    return manifest


def verify_manifest(repo: str | Path, path: str | Path) -> dict:
    path = Path(path)
    if path.is_symlink():
        raise GateError("SYMLINK_MANIFEST_INPUT")
    try:
        saved = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise GateError(f"INVALID_MANIFEST: {exc}") from exc
    if not isinstance(saved, dict) or not isinstance(saved.get("files"), list) or \
            any(not isinstance(row, dict) or not isinstance(row.get("path"), str) for row in saved["files"]):
        raise GateError("INVALID_MANIFEST_SCHEMA")
    current = generate_manifest(repo)
    if saved != current:
        prior = {row["path"]: row for row in saved["files"]}
        present = {row["path"]: row for row in current["files"]}
        changed = sorted(str(path) for path in prior.keys() | present.keys() if prior.get(path) != present.get(path))
        raise GateError("PUBLIC_MANIFEST_MISMATCH: " + (", ".join(changed) or "manifest metadata or stack key changed"))
    return current


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--verify-manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.json_out:
            checked_cache_path(args.json_out)
        report = verify_manifest(args.repo, args.verify_manifest) if args.verify_manifest else generate_manifest(args.repo)
        if args.json_out:
            write_json(args.json_out, report)
        print(json.dumps(report, indent=2, sort_keys=True))
    except (GateError, OSError, ValueError) as exc:
        print(json.dumps({"status": "FAIL", "error": str(exc)}, indent=2, sort_keys=True))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
