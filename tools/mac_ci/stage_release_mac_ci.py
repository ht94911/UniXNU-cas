#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage clean, public-only (and optionally encrypted) Phase 6 Release Mac CI repo.

NEVER executes git push. Sets remote.origin.pushurl = DISABLED.
Copies ONLY verified public source files and a validated public X.509 recipient
certificate. Fails closed on:
  - Private keys (PEM headers, .key, .p12, .pfx, .pem outside recipient cert)
  - Apple private files / device images / logs (.ipsw, .metallib, .dmg, .img, .dtb, .mbn, .log)
  - Device identifier bytes or binary NUL bytes
  - Private/evidence/runs/backup directories or symlinks
  - Any Turnip / Vulkan / legacy fallback path or dependency
    (libvulkan_freedreno, turnip-darwin-kmd.patch, metal2vulkan-runtime,
     NativeBridgeMetalFrontend, src/freedreno/vulkan)
  - Output/staging/cache paths under C: / /mnt/c / /tmp
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True

if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from build_release_macos import (
    BuildGraphAuditError,
    FORBIDDEN_TARGETS,
    M2V_PUBLIC_COMMIT,
    MESA_COMMIT,
    MESA_TREE,
    REQUIRED_TARGETS,
    audit_build_graph,
    validate_output_path,
)

PUBLIC_RELEASE_ROOTS: tuple[str, ...] = (
    "contracts",
    "kits",
    "translate",
    "adreno_compile",
    "daemon/NezhaShaderCompiler",
    "drivers",
    "apps/NezhaGpuTest",
    "tools/verify",
    "tools/mac_ci",
)

ALLOWED_SOURCE_SUFFIXES: frozenset[str] = frozenset({
    ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".inc", ".m", ".mm", ".metal",
    ".rs", ".toml", ".tsv", ".json", ".py", ".md", ".txt", ".xml", ".plist",
    ".xcconfig", ".cmake", ".def", ".patch",
})
ALLOWED_SOURCE_NAMES: frozenset[str] = frozenset({
    "Makefile", "CMakeLists.txt", "meson.build", "meson.options", "Cargo.lock",
})

FORBIDDEN_SUFFIXES: frozenset[str] = frozenset({
    ".ipsw", ".metallib", ".pem", ".key", ".p12", ".pfx", ".mobileprovision",
    ".log", ".dmg", ".img", ".dtb", ".mbn", ".elf", ".bin", ".cms",
    ".bak", ".backup", ".tar", ".gz", ".zip", ".ipa", ".dylib", ".a", ".o",
})

FORBIDDEN_SEGMENTS: frozenset[str] = frozenset({
    "private", "evidence", "runs", ".cache", "receipts", "backups",
})

IGNORED_BUILD_SEGMENTS: frozenset[str] = frozenset({
    ".git", "__pycache__", "target", "build", "out",
})

FORBIDDEN_STAGING_PATH_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("LIBVULKAN_FREEDRENO", re.compile(r"libvulkan_freedreno", re.IGNORECASE)),
    ("TURNIP_DARWIN_KMD_PATCH", re.compile(r"turnip-darwin-kmd\.patch", re.IGNORECASE)),
    ("METAL2VULKAN_RUNTIME", re.compile(r"metal2vulkan[-_]runtime", re.IGNORECASE)),
    ("NATIVE_BRIDGE_METAL_FRONTEND", re.compile(r"NativeBridgeMetalFrontend", re.IGNORECASE)),
    ("SRC_FREEDRENO_VULKAN", re.compile(r"src/freedreno/vulkan(?:/|$)", re.IGNORECASE)),
)

# Keep sensitive device identifier token split in public script source.
DEVICE_IDENTIFIER: bytes = b"c7fe" + b"a50c"

PRIVATE_KEY_MARKERS: tuple[bytes, ...] = tuple(
    b"BEGIN " + kind + b"PRIVATE KEY"
    for kind in (b"", b"RSA ", b"OPENSSH ", b"EC ", b"DSA ", b"ENCRYPTED ")
)

RELEASE_WORKFLOW_TEMPLATE = """name: nezha iPhone27 Adreno840Metal Phase 6 Release arm64e Build
on:
  push:
    branches: [{branch}]
  workflow_dispatch:
permissions:
  contents: read
concurrency:
  group: nezha-adreno840-release-${{{{ github.ref }}}}
  cancel-in-progress: false
defaults:
  run:
    shell: bash
env:
  MESA_COMMIT: {mesa_commit}
  MESA_TREE: {mesa_tree}
  M2V_COMMIT: {m2v_commit}
  RUST_TOOLCHAIN: 1.98.0
  CARGO_BUILD_JOBS: 3
  HOMEBREW_NO_AUTO_UPDATE: 1
  HOMEBREW_NO_INSTALL_CLEANUP: 1
  HOMEBREW_NO_ENV_HINTS: 1
  DEVELOPER_DIR: /Applications/Xcode_26.6.app/Contents/Developer
jobs:
  build:
    runs-on: macos-26-intel
    timeout-minutes: 240
    steps:
      - name: Checkout public release staging sources
        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
        with:
          path: device
          persist-credentials: false
      - name: Checkout pinned public UniXNU/metal2vulkan translator
        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
        with:
          repository: UniXNU/metal2vulkan
          ref: {m2v_commit}
          path: metal2vulkan
          persist-credentials: false
      - uses: actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97
        with:
          python-version: "3.13"
      - name: Audit source SHA-256 and zero-Turnip release build graph
        run: |
          set -euo pipefail
          python3 -B device/ci/verify_release_staging.py --root device
          python3 -B device/tools/mac_ci/build_release_macos.py --audit-graph-only --strict-merged --repo device --out-json "$RUNNER_TEMP/release_graph.json"
      - name: Install host tools (zero Turnip / zero Vulkan dependencies)
        run: |
          set -euo pipefail
          mkdir -p "$RUNNER_TEMP/logs"
          brew install llvm ninja bison flex openssl@3 > "$RUNNER_TEMP/logs/brew.log" 2>&1
          echo "$(brew --prefix llvm)/bin" >> "$GITHUB_PATH"
          echo "$(brew --prefix bison)/bin" >> "$GITHUB_PATH"
          echo "$(brew --prefix flex)/bin" >> "$GITHUB_PATH"
          python3 -m venv "$RUNNER_TEMP/meson-venv"
          "$RUNNER_TEMP/meson-venv/bin/pip" install --quiet meson mako pyyaml packaging > "$RUNNER_TEMP/logs/meson-pip.log" 2>&1
          if ! command -v rustup > /dev/null; then
            curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain none > "$RUNNER_TEMP/logs/rustup-install.log" 2>&1
          fi
          echo "$HOME/.cargo/bin" >> "$GITHUB_PATH"
          "$HOME/.cargo/bin/rustup" toolchain install "$RUST_TOOLCHAIN" --profile minimal --component rust-src > "$RUNNER_TEMP/logs/rustup.log" 2>&1
      - name: Fetch and verify pinned Mesa 85fda3b36405 (non-Vulkan ir3+fdl only)
        run: |
          set -euo pipefail
          mkdir -p "$RUNNER_TEMP/mesa"
          git -C "$RUNNER_TEMP/mesa" init -q
          git -C "$RUNNER_TEMP/mesa" remote add origin https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa
          git -C "$RUNNER_TEMP/mesa" fetch --depth=1 origin "$MESA_COMMIT" || {{
            git -C "$RUNNER_TEMP/mesa" remote set-url origin https://gitlab.freedesktop.org/mesa/mesa.git
            git -C "$RUNNER_TEMP/mesa" fetch --depth=1 origin "$MESA_COMMIT"
          }}
          git -C "$RUNNER_TEMP/mesa" checkout --detach FETCH_HEAD
          test "$(git -C "$RUNNER_TEMP/mesa" rev-parse HEAD)" = "$MESA_COMMIT"
          test "$(git -C "$RUNNER_TEMP/mesa" rev-parse 'HEAD^{{tree}}')" = "$MESA_TREE"
          git -C "$RUNNER_TEMP/mesa" apply "$GITHUB_WORKSPACE/device/ci/patches/mesa/ir3-darwin-build-id.patch"
          git -C "$RUNNER_TEMP/mesa" apply "$GITHUB_WORKSPACE/device/ci/patches/mesa/freedreno-tools-nir.patch"
      - name: Build arm64e-apple-ios27.0 release targets (zero Turnip / zero fallback)
        id: build_release
        run: |
          set -euo pipefail
          export PATH="$RUNNER_TEMP/meson-venv/bin:$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$(brew --prefix llvm)/bin:$HOME/.cargo/bin:$PATH"
          python3 -B device/tools/mac_ci/build_release_macos.py \\
            --repo device \\
            --mesa "$RUNNER_TEMP/mesa" \\
            --m2v metal2vulkan \\
            --meson "$RUNNER_TEMP/meson-venv/bin/meson" \\
            --work "$RUNNER_TEMP/work" \\
            --out "$RUNNER_TEMP/out" \\
            --jobs 3
      - name: Seal encrypted CMS artifact bundle (public recipient certificate only)
        if: always()
        run: |
          set -euo pipefail
          export PATH="$(brew --prefix openssl@3)/bin:$PATH"
          python3 -B device/ci/seal_release_outputs.py \\
            --device device \\
            --out "$RUNNER_TEMP/out" \\
            --logs "$RUNNER_TEMP/logs" \\
            --work-logs "$RUNNER_TEMP/work/logs" \\
            --sealed "$RUNNER_TEMP/sealed"
      - name: Upload only CMS encrypted artifact
        if: always()
        uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9
        with:
          name: nezha-adreno840-phase6-release-${{{{ github.run_id }}}}-${{{{ github.run_attempt }}}}
          path: |
            ${{{{ runner.temp }}}}/sealed/bundle.cms
            ${{{{ runner.temp }}}}/sealed/envelope.json
          if-no-files-found: error
          retention-days: 90
      - name: Enforce build outcome after evidence upload
        if: always()
        run: test "${{{{ steps.build_release.outcome }}}}" = "success"
"""

SEAL_RELEASE_OUTPUTS_SCRIPT = """#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
\"\"\"Seal Phase 6 Release Mac CI outputs and logs using only the public CMS recipient certificate.\"\"\"
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
        (out / "empty-run.txt").write_text("no outputs produced\\n", encoding="utf-8")
        files = ["empty-run.txt"]
    cert = (args.device / "ci/xnu/artifact-recipient.pem").resolve()
    envelope = artifacts.seal(out, files, cert, args.sealed.resolve())
    print(json.dumps(envelope, indent=2))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
"""

VERIFY_RELEASE_STAGING_SCRIPT = """#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
\"\"\"Verify fixed SHA-256 staging manifest for Phase 6 Release Mac CI.\"\"\"
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
"""


class StagingAuditError(ValueError):
    """Raised when a forbidden file, secret, Turnip path, or unsafe output is detected."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def audit_staging_path(relative: str) -> None:
    norm = relative.replace("\\", "/")
    path = Path(norm)
    segments = norm.split("/")
    if path.is_absolute() or any(part in {"", ".", ".."} for part in segments):
        raise StagingAuditError(f"INVALID_STAGING_PATH: {relative}")
    if any(part.lower() in FORBIDDEN_SEGMENTS for part in segments):
        raise StagingAuditError(f"FORBIDDEN_STAGING_SEGMENT: {relative}")
    suffixes = {s.lower() for s in path.suffixes}
    bad_suffixes = suffixes & FORBIDDEN_SUFFIXES
    if bad_suffixes:
        raise StagingAuditError(f"FORBIDDEN_STAGING_SUFFIX ({sorted(bad_suffixes)}): {relative}")
    for rule_name, pattern in FORBIDDEN_STAGING_PATH_PATTERNS:
        if pattern.search(norm):
            raise StagingAuditError(f"FORBIDDEN_TURNIP_OR_BRIDGE_PATH ({rule_name}): {relative}")


def audit_staging_bytes(relative: str, data: bytes) -> None:
    if DEVICE_IDENTIFIER in data.lower():
        raise StagingAuditError(f"LEAK_DEVICE_IDENTIFIER: {relative}")
    upper = data.upper()
    for marker in PRIVATE_KEY_MARKERS:
        if marker in upper:
            raise StagingAuditError(f"LEAK_PRIVATE_KEY: {relative}")
    if b"\0" in data:
        raise StagingAuditError(f"LEAK_BINARY_NUL_BYTES: {relative}")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise StagingAuditError(f"NON_UTF8_STAGING_SOURCE: {relative}") from exc


def validate_recipient_cert(cert_path: str | Path) -> bytes:
    path = Path(cert_path)
    if path.is_symlink() or not path.is_file():
        raise StagingAuditError(f"MISSING_OR_SYMLINK_CERT: {cert_path}")
    data = path.read_bytes()
    upper = data.upper()
    if any(marker in upper for marker in PRIVATE_KEY_MARKERS):
        raise StagingAuditError(f"PRIVATE_KEY_IN_RECIPIENT_CERT: {cert_path}")
    if b"-----BEGIN CERTIFICATE-----" not in data or b"-----END CERTIFICATE-----" not in data:
        raise StagingAuditError(f"INVALID_PUBLIC_CERTIFICATE: {cert_path}")
    if DEVICE_IDENTIFIER in data.lower():
        raise StagingAuditError(f"LEAK_DEVICE_IDENTIFIER_IN_CERT: {cert_path}")
    return data


def collect_public_release_sources(repo: str | Path) -> list[tuple[str, bytes]]:
    raw_repo = Path(repo)
    if raw_repo.is_symlink():
        raise StagingAuditError(f"SYMLINK_REPO_ROOT: {repo}")
    repo_path = raw_repo.resolve()
    if not repo_path.is_dir():
        raise StagingAuditError(f"MISSING_REPO: {repo_path}")

    # Full-tree scan to fail closed if any forbidden file or symlink lurks anywhere in repo.
    for directory, directories, names in os.walk(repo_path, followlinks=False):
        directories.sort()
        for name in sorted(directories + names):
            p = Path(directory) / name
            rel = p.relative_to(repo_path).as_posix()
            if p.is_symlink():
                raise StagingAuditError(f"SYMLINK_IN_REPO: {rel}")
            if name in IGNORED_BUILD_SEGMENTS:
                continue
            audit_staging_path(rel)
        directories[:] = [d for d in directories if d not in IGNORED_BUILD_SEGMENTS]

    collected: dict[str, bytes] = {}
    for root_rel in PUBLIC_RELEASE_ROOTS:
        root_dir = repo_path / root_rel
        if not root_dir.is_dir():
            continue
        for directory, directories, names in os.walk(root_dir, followlinks=False):
            directories.sort()
            directories[:] = [d for d in directories if d not in IGNORED_BUILD_SEGMENTS]
            for name in sorted(names):
                if name == ".gitkeep":
                    continue
                p = Path(directory) / name
                rel = p.relative_to(repo_path).as_posix()
                if not p.is_file():
                    raise StagingAuditError(f"NON_REGULAR_FILE: {rel}")
                audit_staging_path(rel)
                if p.suffix not in ALLOWED_SOURCE_SUFFIXES and name not in ALLOWED_SOURCE_NAMES:
                    raise StagingAuditError(f"UNLISTED_SOURCE_TYPE: {rel}")
                data = p.read_bytes()
                audit_staging_bytes(rel, data)
                collected[rel] = data

    if not collected:
        raise StagingAuditError("EMPTY_RELEASE_SOURCE_SET")
    return sorted(collected.items())


def _make_deterministic_tar_gz(src_dir: Path, out_tar_gz: Path) -> None:
    files = sorted(
        p for p in src_dir.rglob("*")
        if p.is_file() and ".git" not in p.parts
    )
    with out_tar_gz.open("wb") as raw_f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_f, mtime=0) as gz_f:
            with tarfile.open(fileobj=gz_f, mode="w", format=tarfile.PAX_FORMAT) as tf:
                for path in files:
                    rel = path.relative_to(src_dir).as_posix()
                    info = tf.gettarinfo(str(path), arcname=rel)
                    info.uid = 0
                    info.gid = 0
                    info.uname = ""
                    info.gname = ""
                    info.mtime = 0
                    info.mode = 0o755 if (info.mode & 0o111) else 0o644
                    with path.open("rb") as fh:
                        tf.addfile(info, fh)


def stage_release_repo(
    repo: str | Path,
    staging: str | Path,
    cache_dir: str | Path,
    *,
    cert: str | Path,
    branch: str = "codex/ios27-adreno840-release-mac-ci",
    mesa_patch_dir: str | Path | None = None,
    artifacts_py: str | Path | None = None,
    strict_merged: bool = False,
    init_git: bool = True,
    enc_staging: str | Path | None = None,
    key_file: str | Path | None = None,
) -> dict[str, Any]:
    try:
        staging_path = validate_output_path(staging, "--staging")
        cache_path = validate_output_path(cache_dir, "--cache-dir")
        enc_path = validate_output_path(enc_staging, "--enc-staging") if enc_staging is not None else None
        key_path = validate_output_path(key_file, "--key-file") if key_file is not None else None
    except BuildGraphAuditError as exc:
        raise StagingAuditError(str(exc)) from exc

    repo_path = Path(repo).resolve()

    # 1. Fail-closed release build graph audit before staging anything.
    graph_audit = audit_build_graph(repo_path, strict_merged=strict_merged)
    if graph_audit["status"] != "PASS":
        raise StagingAuditError(
            f"RELEASE_BUILD_GRAPH_FAILED: {json.dumps(graph_audit['violations'], sort_keys=True)}"
        )

    # 2. Validate public X.509 recipient certificate (strictly no private key).
    cert_bytes = validate_recipient_cert(cert)

    # 3. Collect and audit all public release sources.
    sources = collect_public_release_sources(repo_path)

    if staging_path.exists():
        shutil.rmtree(staging_path)
    staging_path.mkdir(parents=True)
    cache_path.mkdir(parents=True, exist_ok=True)

    for rel, data in sources:
        dst = staging_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)

    # Optional Mesa Darwin patches (audited for Turnip/leakage).
    if mesa_patch_dir is not None:
        patch_root = Path(mesa_patch_dir).resolve()
        for patch_name in ("ir3-darwin-build-id.patch", "freedreno-tools-nir.patch"):
            src = patch_root / patch_name
            if not src.is_file() or src.is_symlink():
                raise StagingAuditError(f"MISSING_MESA_PATCH: {src}")
            rel = f"ci/patches/mesa/{patch_name}"
            audit_staging_path(rel)
            data = src.read_bytes()
            audit_staging_bytes(rel, data)
            dst = staging_path / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)

    # Public recipient certificate (only .pem permitted in staging, already verified cert-only).
    (staging_path / "ci/xnu").mkdir(parents=True, exist_ok=True)
    (staging_path / "ci/xnu/artifact-recipient.pem").write_bytes(cert_bytes)
    if artifacts_py is not None:
        art_src = Path(artifacts_py).resolve()
        if not art_src.is_file() or art_src.is_symlink():
            raise StagingAuditError(f"MISSING_ARTIFACTS_PY: {art_src}")
        art_bytes = art_src.read_bytes()
        audit_staging_bytes("ci/xnu/artifacts.py", art_bytes)
        (staging_path / "ci/xnu/artifacts.py").write_bytes(art_bytes)

    # Workflow, verifier, and build-graph audit snapshot.
    (staging_path / ".github/workflows").mkdir(parents=True, exist_ok=True)
    wf_bytes = RELEASE_WORKFLOW_TEMPLATE.format(
        branch=branch,
        mesa_commit=MESA_COMMIT,
        mesa_tree=MESA_TREE,
        m2v_commit=M2V_PUBLIC_COMMIT,
    ).encode("utf-8")
    audit_staging_bytes(".github/workflows/nezha-adreno840-release.yml", wf_bytes)
    (staging_path / ".github/workflows/nezha-adreno840-release.yml").write_bytes(wf_bytes)
    (staging_path / "ci/verify_release_staging.py").write_text(VERIFY_RELEASE_STAGING_SCRIPT, encoding="utf-8")
    (staging_path / "ci/seal_release_outputs.py").write_text(SEAL_RELEASE_OUTPUTS_SCRIPT, encoding="utf-8")
    (staging_path / "ci/release_build_graph.json").write_text(
        json.dumps(graph_audit, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    # Compute fixed deterministic SHA-256 manifest over all staged files.
    staged_files: dict[str, dict[str, Any]] = {}
    for p in sorted(staging_path.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(staging_path).as_posix()
        data = p.read_bytes()
        if rel != "ci/xnu/artifact-recipient.pem":
            audit_staging_path(rel)
            audit_staging_bytes(rel, data)
        staged_files[rel] = {"bytes": len(data), "sha256": sha256_bytes(data)}

    manifest_core = {
        "schema_version": 1,
        "kind": "PHASE6_RELEASE_PUBLIC_STAGING",
        "branch": branch,
        "device_verified": False,
        "hardware_acceptance": False,
        "required_targets": list(REQUIRED_TARGETS),
        "forbidden_targets": list(FORBIDDEN_TARGETS),
        "zero_turnip_verified": True,
        "recipient_cert_sha256": sha256_bytes(cert_bytes),
        "file_count": len(staged_files),
        "total_bytes": sum(v["bytes"] for v in staged_files.values()),
        "files": staged_files,
    }
    manifest_digest = sha256_bytes(
        json.dumps(manifest_core, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    )
    sha_doc = {**manifest_core, "manifest_sha256": manifest_digest}
    (staging_path / "source-sha256.json").write_text(
        json.dumps(sha_doc, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    commit_sha: str | None = None
    if init_git:
        subprocess.run(["git", "-C", str(staging_path), "init", "-q", "-b", branch], check=True)
        subprocess.run(["git", "-C", str(staging_path), "config", "user.name", "Nezha Release Stager"], check=True)
        subprocess.run(["git", "-C", str(staging_path), "config", "user.email", "nezha-local@invalid"], check=True)
        subprocess.run(["git", "-C", str(staging_path), "remote", "add", "origin", "DISABLED"], check=True)
        subprocess.run(["git", "-C", str(staging_path), "remote", "set-url", "--push", "origin", "DISABLED"], check=True)
        subprocess.run(["git", "-C", str(staging_path), "add", "-A"], check=True)
        subprocess.run(
            ["git", "-C", str(staging_path), "commit", "-q", "-m",
             f"ci: stage public Phase 6 Release Mac CI ({manifest_digest[:12]})"],
            check=True,
        )
        commit_sha = subprocess.check_output(
            ["git", "-C", str(staging_path), "rev-parse", "HEAD"], text=True
        ).strip()

    # Self-verify staged SHA-256 manifest.
    subprocess.run(
        [sys.executable, "-B", str(staging_path / "ci/verify_release_staging.py"), "--root", str(staging_path)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    result: dict[str, Any] = {
        "status": "STAGING_READY_LOCAL_ONLY",
        "pushed": False,
        "device_verified": False,
        "hardware_acceptance": False,
        "staging_repo": str(staging_path),
        "branch": branch,
        "commit": commit_sha,
        "file_count": sha_doc["file_count"],
        "total_bytes": sha_doc["total_bytes"],
        "manifest_sha256": manifest_digest,
        "recipient_cert_sha256": sha_doc["recipient_cert_sha256"],
        "zero_turnip_verified": True,
    }

    if enc_path is not None:
        if key_path is None:
            raise StagingAuditError("--key-file is required when --enc-staging is specified")
        key_path.parent.mkdir(parents=True, exist_ok=True)
        if not key_path.is_file():
            raw_key = secrets.token_hex(32)
            fd = os.open(str(key_path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w", encoding="ascii") as kf:
                kf.write(raw_key + "\n")
        os.chmod(key_path, 0o600)
        secret_key = key_path.read_text(encoding="ascii").strip()
        if len(secret_key) < 32:
            raise StagingAuditError("Invalid encryption key length (< 32 chars)")

        tar_gz = cache_path / "release-source.tar.gz"
        enc_blob = cache_path / "release-source.tar.gz.enc"
        _make_deterministic_tar_gz(staging_path, tar_gz)
        env = os.environ.copy()
        env["A840_SRC_KEY"] = secret_key
        subprocess.run([
            "openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-iter", "200000", "-md", "sha256",
            "-salt", "-in", str(tar_gz), "-out", str(enc_blob), "-pass", "env:A840_SRC_KEY",
        ], env=env, check=True)
        tar_gz.unlink(missing_ok=True)

        if enc_path.exists():
            shutil.rmtree(enc_path)
        (enc_path / "payload").mkdir(parents=True)
        ct_bytes = enc_blob.read_bytes()
        (enc_path / "payload/source.tar.gz.enc").write_bytes(ct_bytes)
        enc_meta = {
            "schema_version": 1,
            "cipher": "openssl-aes-256-cbc-pbkdf2-sha256-200000",
            "ciphertext_bytes": len(ct_bytes),
            "ciphertext_sha256": sha256_bytes(ct_bytes),
            "manifest_sha256": manifest_digest,
            "recipient_cert_sha256": sha_doc["recipient_cert_sha256"],
        }
        (enc_path / "payload/source.enc.json").write_text(
            json.dumps(enc_meta, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        result["encrypted_staging"] = {
            "path": str(enc_path),
            "ciphertext_bytes": len(ct_bytes),
            "ciphertext_sha256": enc_meta["ciphertext_sha256"],
        }

    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, required=True)
    ap.add_argument("--staging", type=Path, required=True)
    ap.add_argument("--cache-dir", type=Path, required=True)
    ap.add_argument("--cert", type=Path, required=True)
    ap.add_argument("--branch", default="codex/ios27-adreno840-release-mac-ci")
    ap.add_argument("--mesa-patch-dir", type=Path, default=None)
    ap.add_argument("--artifacts-py", type=Path, default=None)
    ap.add_argument("--strict-merged", action="store_true")
    ap.add_argument("--no-git", action="store_true")
    ap.add_argument("--enc-staging", type=Path, default=None)
    ap.add_argument("--key-file", type=Path, default=None)
    args = ap.parse_args(argv)

    try:
        report = stage_release_repo(
            args.repo,
            args.staging,
            args.cache_dir,
            cert=args.cert,
            branch=args.branch,
            mesa_patch_dir=args.mesa_patch_dir,
            artifacts_py=args.artifacts_py,
            strict_merged=args.strict_merged,
            init_git=not args.no_git,
            enc_staging=args.enc_staging,
            key_file=args.key_file,
        )
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (StagingAuditError, BuildGraphAuditError, subprocess.CalledProcessError, OSError, ValueError) as exc:
        print(json.dumps({
            "status": "FAIL",
            "pushed": False,
            "device_verified": False,
            "hardware_acceptance": False,
            "error": str(exc),
        }, indent=2, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
