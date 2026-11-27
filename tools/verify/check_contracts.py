#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Byte-for-byte contract, strict C/C++ header, and Git path gates.

All generated files, including compiler temporaries, must live below
adreno840-native/.cache/w00-l4*. Git and compiler commands never use a shell.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
import time

DEFAULT_DOCS_CONTRACTS = Path(
    "/mnt/i/Code/UniXNU-nezha/docs/adreno840-native/contracts"
)
CACHE_PARENT = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache")
DEFAULT_CACHE = CACHE_PARENT / "w00-l4"


class GateError(ValueError):
    """A failed gate or an unsafe output location."""


def checked_cache_path(path: str | Path) -> Path:
    raw = str(path)
    if re.match(r"^[a-zA-Z]:", raw):
        raise GateError("Windows output paths are forbidden; use adreno840-native/.cache/<subdir>")
    resolved = Path(path).expanduser().resolve()
    try:
        relative = resolved.relative_to(CACHE_PARENT)
    except ValueError as exc:
        raise GateError(f"output outside approved cache: {resolved}") from exc
    if not relative.parts:
        raise GateError(f"output must be a subdirectory inside approved cache: {resolved}")
    return resolved


def cache_directory(path: str | Path) -> Path:
    directory = checked_cache_path(path)
    directory.mkdir(parents=True, exist_ok=True)
    return checked_cache_path(directory)


def write_json(path: str | Path, value: object) -> Path:
    target = checked_cache_path(path)
    cache_directory(target.parent)
    target.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return target


def cache_environment(directory: str | Path, base: dict[str, str] | None = None) -> dict[str, str]:
    directory = cache_directory(directory)
    env = (os.environ if base is None else base).copy()
    env.update(
        TMPDIR=str(directory), TEMP=str(directory), TMP=str(directory),
        PYTHONPYCACHEPREFIX=str(directory / "pycache"),
        PYTHONDONTWRITEBYTECODE="1", XDG_CACHE_HOME=str(directory / "xdg"),
        CCACHE_DISABLE="1", GIT_OPTIONAL_LOCKS="0",
    )
    return env


def run_command(command: list[str], *, cwd: str | Path, env: dict[str, str],
                source: str | None = None, timeout: int = 120) -> dict:
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=cwd, env=env, input=source,
                                text=True, capture_output=True, timeout=timeout,
                                check=False)
        returncode, stdout, stderr = result.returncode, result.stdout, result.stderr
    except (OSError, subprocess.TimeoutExpired) as exc:
        returncode, stdout, stderr = 127, "", str(exc)
        if isinstance(exc, subprocess.TimeoutExpired):
            stdout = exc.stdout or ""
            stderr = exc.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode("utf-8", errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode("utf-8", errors="replace")
            stderr += f"\nTIMEOUT after {timeout}s"
    return {"command": command, "returncode": returncode, "stdout": stdout,
            "stderr": stderr, "seconds": round(time.monotonic() - started, 4)}


def inventory(directory: Path) -> dict[str, str]:
    if not directory.is_dir() or directory.is_symlink():
        raise GateError(f"missing or symlinked contracts directory: {directory}")
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise GateError(f"symlinked contract: {path}")
        if path.is_file():
            files[path.relative_to(directory).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    if not files:
        raise GateError(f"empty contracts directory: {directory}")
    return files


def normalized_prefix(prefix: str) -> str:
    value = prefix.rstrip("/")
    parts = PurePosixPath(value).parts
    if not value or value.startswith("/") or "\\" in value or ":" in value or \
            any(part in (".", "..") for part in value.split("/")) or not parts:
        raise GateError(f"invalid allowed prefix: {prefix!r}")
    return PurePosixPath(value).as_posix()


def path_allowed(path: str, prefixes: list[str]) -> bool:
    return any(path == prefix or path.startswith(prefix + "/") for prefix in prefixes)


def porcelain_paths(output: str) -> set[str]:
    """Parse -z porcelain, including both sides of renames/copies and spaces."""
    records, paths, index = output.split("\0"), set(), 0
    while index < len(records):
        record = records[index]
        index += 1
        if not record:
            continue
        if len(record) < 4 or record[2] != " ":
            raise GateError("malformed git status --porcelain output")
        paths.add(record[3:])
        if "R" in record[:2] or "C" in record[:2]:
            if index >= len(records) or not records[index]:
                raise GateError("missing rename source in git status")
            paths.add(records[index])
            index += 1
    return paths


def changed_paths(repo: Path, base: str, env: dict[str, str]) -> set[str]:
    if base.startswith("-"):
        raise GateError("base must be a Git commit, not an option")
    resolved = run_command(["git", "-C", str(repo), "rev-parse", "--verify", base + "^{commit}"],
                           cwd=repo, env=env)
    if resolved["returncode"]:
        raise GateError("invalid Git base: " + resolved["stderr"].strip())
    commit = resolved["stdout"].strip()
    diff = run_command(["git", "-C", str(repo), "diff", "--no-renames", "--name-only", "-z",
                        commit + "..HEAD", "--"], cwd=repo, env=env)
    status = run_command(["git", "-C", str(repo), "status", "--porcelain=v1", "-z",
                          "--untracked-files=all"], cwd=repo, env=env)
    for result in (diff, status):
        if result["returncode"]:
            raise GateError("Git path audit failed: " + result["stderr"].strip())
    return set(filter(None, diff["stdout"].split("\0"))) | porcelain_paths(status["stdout"])


def verify_contracts(repo: str | Path, docs_contracts: str | Path = DEFAULT_DOCS_CONTRACTS,
                     cache_dir: str | Path = DEFAULT_CACHE / "contracts",
                     base: str | None = None, allowed_prefixes: list[str] | None = None) -> dict:
    repo, docs = Path(repo).resolve(), Path(docs_contracts).resolve()
    cache = cache_directory(cache_dir)
    env = cache_environment(cache)
    report = {"status": "PASS", "repo": str(repo), "contracts": [], "compiles": [],
              "changed_paths": [], "violations": [], "errors": []}
    try:
        local, reference = inventory(repo / "contracts"), inventory(docs)
        for path in sorted(local.keys() | reference.keys()):
            if path not in local:
                report["errors"].append(f"CONTRACT_MISSING {path}")
            elif path not in reference:
                report["errors"].append(f"CONTRACT_EXTRA {path}")
            elif local[path] != reference[path]:
                report["errors"].append(f"CONTRACT_MISMATCH {path}: {local[path]} != {reference[path]}")
            report["contracts"].append({"path": path, "repo_sha256": local.get(path),
                                        "docs_sha256": reference.get(path)})
        if bool(base) != bool(allowed_prefixes):
            raise GateError("--base and --allowed-prefix must be supplied together")
        if base:
            prefixes = [normalized_prefix(p) for p in allowed_prefixes or []]
            report["changed_paths"] = sorted(changed_paths(repo, base, env))
            report["violations"] = [p for p in report["changed_paths"] if not path_allowed(p, prefixes)]
            report["errors"].extend("PATH_VIOLATION " + p for p in report["violations"])
        if not report["errors"]:
            headers = sorted(p for p in local if p.endswith(".h"))
            if not headers:
                raise GateError("no contract headers to compile")
            with tempfile.TemporaryDirectory(prefix="headers-", dir=cache) as scratch:
                for number, header in enumerate(headers):
                    if '"' in header or "\n" in header or "\r" in header:
                        raise GateError(f"invalid header filename: {header!r}")
                    source = f'#include "{header}"\nint main(void) {{ return 0; }}\n'
                    for compiler, standard, language in (("gcc", "c11", "c"), ("g++", "c++17", "c++")):
                        command = [compiler, "-std=" + standard, "-Wall", "-Wextra", "-Werror", "-pedantic",
                                   "-I", str(repo / "contracts"), "-x", language, "-c", "-", "-o",
                                   str(Path(scratch) / f"{number}-{language.replace('+', 'p')}.o")]
                        result = run_command(command, cwd=scratch, env=env, source=source)
                        result["header"] = header
                        report["compiles"].append(result)
                        if result["returncode"]:
                            report["errors"].append(f"COMPILE_FAILED {compiler} {header}: {result['stderr'].strip()}")
    except (GateError, OSError) as exc:
        report["errors"].append(str(exc))
    if report["errors"]:
        report["status"] = "FAIL"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--docs-contracts", type=Path, default=DEFAULT_DOCS_CONTRACTS)
    parser.add_argument("--base", help="audit committed and staged/unstaged/untracked paths since this commit")
    parser.add_argument("--allowed-prefix", action="append", help="exact file or directory prefix; repeatable")
    parser.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE / "contracts")
    args = parser.parse_args(argv)
    try:
        report = verify_contracts(args.repo, args.docs_contracts, args.cache_dir, args.base, args.allowed_prefix)
    except (GateError, OSError) as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
