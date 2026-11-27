#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reject runtime, Vulkan-driver and (final profile) bridge leakage.

Artifacts use nm -g -C, including defined and undefined global symbols.
Cargo TOML/lock files are parsed, including dependency keys and aliases.
JSON keys and string values are checked recursively. The pinned Mesa
manifest's exact root excluded_source_prefixes declaration is recorded as
an exclusion, never treated as an input source or a general allowlist.
Reports contain sorted inputs and findings, with no timestamps or timings.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import posixpath
import re
import subprocess
import tomllib

from check_contracts import GateError

COMPILER_RULES = (
    ("METAL2VULKAN_RUNTIME", re.compile(r"metal2vulkan[-_]runtime", re.I)),
    ("FREEDRENO_VULKAN_SOURCE", re.compile(r"src/freedreno/vulkan(?:/|$)", re.I)),
    ("FREEDRENO_VULKAN_LIBRARY", re.compile(r"libvulkan_freedreno", re.I)),
    ("VULKAN_DEVICE", re.compile(r"vkCreateDevice", re.I)),
    ("VULKAN_GRAPHICS_PIPELINES", re.compile(r"vkCreateGraphicsPipelines", re.I)),
)
FINAL_RULES = (
    ("NATIVE_BRIDGE_FRONTEND", re.compile(r"NativeBridgeMetalFrontend", re.I)),
    ("NATIVE_BRIDGE_API", re.compile(r"native_bridge_metal_", re.I)),
    ("NB_METAL", re.compile(r"NBMetal")),
    ("NB_BRIDGE", re.compile(r"(?<![A-Za-z0-9])_?NB[A-Za-z_][A-Za-z0-9_]*")),
)
NM_SYMBOL = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z?])\s+(.+?)\s*$")


def string_items(value: object, location: str = "$"):
    """JSON-pointer-like locations are deterministic and identify keys too."""
    if isinstance(value, str):
        yield location, value
    elif isinstance(value, dict):
        for key in sorted(value):
            escaped = key.replace("~", "~0").replace("/", "~1")
            child = location + "/" + escaped
            yield child + "/@key", key
            yield from string_items(value[key], child)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from string_items(item, location + "/" + str(index))


def findings(value: str, profile: str) -> list[tuple[str, str]]:
    normalized = posixpath.normpath(value.replace("\\", "/"))
    rules = COMPILER_RULES + (FINAL_RULES if profile == "final" else ())
    return [(name, match.group()) for name, pattern in rules
            if (match := pattern.search(normalized)) is not None]


def scan_no_fallback(artifacts=(), cargo_manifests=(), json_manifests=(),
                     profile: str = "compiler", env: dict[str, str] | None = None) -> dict:
    if profile not in {"compiler", "final"}:
        raise GateError(f"unknown no-fallback profile: {profile}")
    inputs = {"artifact": {Path(p).resolve() for p in artifacts},
              "cargo": {Path(p).resolve() for p in cargo_manifests},
              "json": {Path(p).resolve() for p in json_manifests}}
    for path in list(inputs["cargo"]):
        lock = path.with_name("Cargo.lock")
        if path.name == "Cargo.toml" and lock.is_file():
            inputs["cargo"].add(lock)
    report = {"status": "PASS", "profile": profile, "scanned_items": 0,
              "strings_scanned": 0, "symbols_scanned": 0, "items": [],
              "violations": [], "errors": [], "exclusion_declarations": []}

    def check(kind: str, path: Path, location: str, value: str) -> None:
        report["strings_scanned"] += 1
        for rule, matched in findings(value, profile):
            report["violations"].append({"kind": kind, "path": str(path), "location": location,
                                          "value": value, "rule": rule, "match": matched})

    for kind in sorted(inputs):
        for path in sorted(inputs[kind]):
            report["scanned_items"] += 1
            item = {"kind": kind, "path": str(path)}
            report["items"].append(item)
            check(kind, path, "@path", str(path))
            try:
                if not path.is_file():
                    raise GateError("input file does not exist")
                if kind == "artifact":
                    command = ["nm", "-g", "-C", "--", str(path)]
                    result = subprocess.run(command, env=env, text=True, errors="replace",
                                            capture_output=True, timeout=60, check=False)
                    item.update(command=command, returncode=result.returncode, symbols=0)
                    if result.returncode:
                        raise GateError("nm failed: " + result.stderr.strip())
                    for line in result.stdout.splitlines():
                        match = NM_SYMBOL.match(line)
                        if match:
                            symbol = match[2]
                            item["symbols"] += 1
                            report["symbols_scanned"] += 1
                            check(kind, path, "symbol:" + symbol, symbol)
                else:
                    with path.open("rb") as source:
                        data = tomllib.load(source) if kind == "cargo" else json.load(source)
                    item["strings"] = 0
                    for location, value in string_items(data):
                        item["strings"] += 1
                        # Only the exact documented root declaration is exempt.
                        if kind == "json" and re.fullmatch(r"\$/excluded_source_prefixes/\d+", location) and \
                                value == "src/freedreno/vulkan":
                            report["strings_scanned"] += 1
                            report["exclusion_declarations"].append(
                                {"path": str(path), "location": location, "value": value})
                        else:
                            check(kind, path, location, value)
            except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
                report["errors"].append({"kind": kind, "path": str(path), "error": str(exc)})
    if not report["scanned_items"]:
        report["errors"].append({"kind": "input", "path": "", "error": "at least one scan input is required"})
    report["violations"].sort(key=lambda r: (r["path"], r["kind"], r["location"], r["rule"], r["value"]))
    if report["violations"] or report["errors"]:
        report["status"] = "FAIL"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", action="append", type=Path, default=[])
    parser.add_argument("--cargo-manifest", action="append", type=Path, default=[])
    parser.add_argument("--json-manifest", action="append", type=Path, default=[])
    parser.add_argument("--profile", choices=("compiler", "final"), default="compiler")
    args = parser.parse_args(argv)
    report = scan_no_fallback(args.artifact, args.cargo_manifest, args.json_manifest, args.profile)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
