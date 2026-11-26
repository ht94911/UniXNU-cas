#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build pinned Mesa 85fda3b36405 non-Vulkan ir3+fdl archives and libadreno_compile_ir3.a."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

MESA_COMMIT = "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
MESON = Path("/home/hua/nezha-work/tools/meson-venv/bin/meson")
REPO_ROOT = Path(__file__).resolve().parents[2]
CACHE_ROOT = REPO_ROOT.parent / ".cache"
DEFAULT_MANIFEST = REPO_ROOT / "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json"
EXCLUDED = "src/freedreno/vulkan"

MESON_OPTIONS = [
    "--backend=ninja", "--wrap-mode=nodownload", "-Dplatforms=[]", "-Dvulkan-drivers=[]",
    "-Dgallium-drivers=softpipe", "-Dtools=freedreno", "-Dopengl=true", "-Dglx=disabled",
    "-Degl=disabled", "-Dgbm=disabled", "-Dgles1=disabled", "-Dgles2=disabled",
    "-Dllvm=disabled", "-Dspirv-tools=disabled", "-Dbuild-tests=false",
]

# Helper sub-archives linked by mesa_util (built via the same non-Vulkan Meson config).
MESA_UTIL_SUBLIBS = (
    "mesa_util_clflush",
    "mesa_util_clflushopt",
    "mesa_util_simd",
    "blake3",
    "mesa_util_c11",
)

IGNORE_DIRS = {".git", "__pycache__", "target", "build", "out", ".cache"}


def checked_cache_dir(path: Path, label: str) -> Path:
    raw = str(path)
    if re.match(r"^[a-zA-Z]:", raw) or raw.startswith("/mnt/c"):
        raise ValueError(f"{label} must not point to C: or Windows paths: {raw}")
    resolved = path.expanduser().resolve()
    if resolved == CACHE_ROOT or not resolved.is_relative_to(CACHE_ROOT):
        raise ValueError(f"{label} must resolve strictly beneath {CACHE_ROOT}: {resolved}")
    return resolved


def run_cmd(cmd: list[str | Path], *, cwd: Path | None = None,
            env: dict[str, str] | None = None, log_path: Path | None = None,
            quiet: bool = False) -> str:
    argv = [str(x) for x in cmd]
    if not quiet:
        print(f"$ {shlex.join(argv)}", flush=True)
    result = subprocess.run(
        argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace",
    )
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0:
        tail = "\n".join(result.stdout.splitlines()[-40:])
        raise RuntimeError(f"exit {result.returncode}: {shlex.join(argv)}\n{tail}")
    return result.stdout


def materialize_pinned_mesa(mesa: Path, cache: Path, env: dict[str, str]) -> Path:
    source = cache / f"mesa-{MESA_COMMIT[:12]}"
    marker = source / ".pinned-commit"
    if marker.is_file() and marker.read_text(encoding="ascii").strip() == MESA_COMMIT:
        return source
    if source.exists():
        shutil.rmtree(source)
    source.mkdir(parents=True, exist_ok=True)
    cmd = [
        "git", "-C", str(mesa), "archive", MESA_COMMIT,
        "VERSION", "meson.build", "meson.options", "bin", "include", "src", "subprojects",
    ]
    print(f"$ {shlex.join(cmd)} | tar -xf - -C {shlex.quote(str(source))}", flush=True)
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, env=env) as arch:
        assert arch.stdout is not None
        ext = subprocess.run(["tar", "-xf", "-", "-C", str(source)], stdin=arch.stdout, env=env)
        arch.stdout.close()
        if arch.wait() != 0 or ext.returncode != 0:
            raise RuntimeError("failed to extract pinned Mesa Git archive")
    marker.write_text(MESA_COMMIT + "\n", encoding="ascii")
    return source


def build_mesa_archives(mesa: Path, manifest_path: Path, cache: Path, jobs: int,
                        env: dict[str, str]) -> tuple[Path, Path, dict[str, Path], list[Path]]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest["mesa_commit"] != MESA_COMMIT:
        raise ValueError(f"manifest commit mismatch: {manifest['mesa_commit']}")
    head = run_cmd(["git", "-C", mesa, "rev-parse", "HEAD"], env=env, quiet=True).strip()
    if head != MESA_COMMIT:
        raise ValueError(f"Mesa HEAD is {head}; expected {MESA_COMMIT}")
    if not MESON.is_file():
        raise RuntimeError(f"missing Meson executable: {MESON}")

    source = materialize_pinned_mesa(mesa, cache, env)
    build = cache / f"build-{MESA_COMMIT[:12]}"
    targets_cache = cache / "meson-targets.json"
    if not (build / "build.ninja").is_file():
        run_cmd([MESON, "setup", build, source, *MESON_OPTIONS],
                env=env, log_path=cache / "meson-setup.log")
        run_cmd([MESON, "introspect", "--targets", build],
                env=env, log_path=targets_cache, quiet=True)
    elif not targets_cache.is_file():
        run_cmd([MESON, "introspect", "--targets", build],
                env=env, log_path=targets_cache, quiet=True)

    inventory = json.loads(targets_cache.read_text(encoding="utf-8"))
    static_targets = {
        t["name"]: Path(t["filename"][0]).relative_to(build).as_posix()
        for t in inventory if t["type"] == "static library"
    }

    core_names = [*manifest["libraries"].keys(), "mesa_util"]
    missing_core = [n for n in core_names if n not in static_targets]
    if missing_core:
        raise RuntimeError(f"missing 10 core Mesa static targets: {missing_core}")

    sub_names = [n for n in MESA_UTIL_SUBLIBS if n in static_targets]
    all_names = core_names + [n for n in sub_names if n not in core_names]
    rel_archives = [static_targets[n] for n in all_names]

    # Hard verification: zero src/freedreno/vulkan inputs in Ninja graph.
    commands = run_cmd(["ninja", "-C", build, "-t", "commands", *rel_archives],
                       env=env, log_path=cache / "ninja-commands.log", quiet=True)
    if EXCLUDED in commands or "libvulkan_freedreno" in commands:
        raise RuntimeError("Ninja graph contains forbidden Turnip/Vulkan inputs")
    if "src/compiler/nir/nir.c" not in commands or "nir_stub.c" in commands:
        raise RuntimeError("Ninja graph did not select full NIR")
    absent_sources = sorted(
        p for lib in manifest["libraries"].values() for p in lib["sources"] if p not in commands
    )
    if absent_sources:
        raise RuntimeError(f"manifest compile sources absent from Ninja: {absent_sources[:8]}")

    t0 = time.monotonic()
    run_cmd(["ninja", "-C", build, f"-j{jobs}", *rel_archives],
            env=env, log_path=cache / "ninja-build.log")
    dt = time.monotonic() - t0
    print(f"Mesa {len(core_names)} core (+{len(sub_names)} mesa_util sub) static archives ready in {dt:.2f}s (zero {EXCLUDED} inputs)",
          flush=True)

    archive_map = {n: build / static_targets[n] for n in all_names}
    for n, p in archive_map.items():
        if not p.is_file():
            raise RuntimeError(f"expected archive missing after ninja: {p}")
    link_list = [archive_map[n] for n in all_names]
    return source, build, archive_map, link_list


def mesa_include_flags(source: Path, build: Path) -> list[str]:
    repo_dirs = [
        REPO_ROOT / "contracts",
        REPO_ROOT / "kits/Adreno840",
        REPO_ROOT / "adreno_compile/ir3",
    ]
    mesa_dirs = [
        source / "include",
        source / "src",
        source / "src/compiler",
        source / "src/compiler/isaspec",
        source / "src/compiler/nir",
        source / "src/compiler/spirv",
        source / "src/freedreno",
        source / "src/freedreno/common",
        source / "src/freedreno/ir3",
        source / "src/freedreno/fdl",
        source / "src/freedreno/isa",
        source / "src/util",
        source / "src/util/format",
        build / "src",
        build / "src/compiler",
        build / "src/compiler/nir",
        build / "src/compiler/spirv",
        build / "src/freedreno/common",
        source / "src/freedreno/registers",
        build / "src/freedreno/registers",
        build / "src/freedreno/registers/adreno",
        build / "src/freedreno/isa",
        build / "src/util",
        build / "src/util/format",
    ]
    flags: list[str] = []
    for d in repo_dirs:
        if d.is_dir():
            flags.extend(["-I", str(d)])
    for d in mesa_dirs:
        if d.is_dir():
            flags.extend(["-isystem", str(d)])
    return flags


def mesa_compile_defines(cache: Path) -> list[str]:
    targets_cache = cache / "meson-targets.json"
    inventory = json.loads(targets_cache.read_text(encoding="utf-8"))
    mesa_defs: list[str] = []
    for target in inventory:
        if target.get("name") == "freedreno_ir3":
            for src in target.get("target_sources", []):
                params = src.get("parameters", [])
                defs = [p for p in params if p.startswith("-D")]
                if defs:
                    mesa_defs = defs
                    break
            break
    if not mesa_defs:
        raise RuntimeError("failed to extract Mesa compile defines from meson-targets.json")
    return ["-DA840_BUILD_MESA_IR3_BACKEND=1", *mesa_defs]


def build_ir3_archive(source: Path, build: Path, cache: Path,
                      env: dict[str, str]) -> tuple[Path, list[Path]]:
    ir3_dir = REPO_ROOT / "adreno_compile/ir3"
    c_sources = sorted(
        p for p in ir3_dir.glob("*.c")
        if p.is_file() and not p.name.startswith("test_")
    )
    if not c_sources:
        raise RuntimeError(f"no backend C sources found in {ir3_dir}")

    obj_dir = cache / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    inc = mesa_include_flags(source, build)
    defs = mesa_compile_defines(cache)

    objs: list[Path] = []
    for src in c_sources:
        obj = obj_dir / (src.stem + ".o")
        run_cmd([
            "gcc", "-std=c11", "-O2", "-g", "-fPIC",
            "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter",
            *defs, *inc, "-c", src, "-o", obj,
        ], env=env)
        objs.append(obj)

    archive = cache / "libadreno_compile_ir3.a"
    if archive.exists():
        archive.unlink()
    run_cmd(["ar", "rcs", archive, *objs], env=env)
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(f"Built {archive} ({archive.stat().st_size} bytes, sha256={sha[:16]})", flush=True)
    return archive, c_sources


def cargo_program() -> str:
    found = shutil.which("cargo")
    if found:
        return found
    local = Path("/home/hua/.cargo/bin/cargo")
    if local.is_file():
        return str(local)
    raise RuntimeError("cargo not found in PATH or ~/.cargo/bin/cargo")


def ensure_translate_archive(cache: Path, jobs: int, env: dict[str, str]) -> Path:
    translate_src = REPO_ROOT / "translate"
    out_dir = cache / "translate-build"
    target_dir = out_dir / "cargo-target"
    archive = target_dir / "release/libnative_metal_translate.a"

    # Check if already built and newer than translate sources.
    if archive.is_file():
        arch_mtime = archive.stat().st_mtime
        src_files = [
            p for p in translate_src.rglob("*")
            if p.is_file() and not any(part in IGNORE_DIRS for part in p.relative_to(translate_src).parts)
        ]
        if src_files and all(p.stat().st_mtime <= arch_mtime for p in src_files):
            print(f"Reusing cached {archive}", flush=True)
            return archive

    out_dir.mkdir(parents=True, exist_ok=True)
    copied = out_dir / "cargo-source"
    if copied.exists():
        shutil.rmtree(copied)
    shutil.copytree(translate_src, copied, ignore=shutil.ignore_patterns(*IGNORE_DIRS))

    cargo_home = out_dir / "cargo-home"
    registry_dst = cargo_home / "registry"
    registry_src = (Path.home() / ".cargo/registry").resolve()
    if not registry_dst.exists() and registry_src.is_dir():
        cargo_home.mkdir(parents=True, exist_ok=True)
        shutil.copytree(registry_src, registry_dst)

    cargo = cargo_program()
    c_env = env.copy()
    c_env.update(
        CARGO_TARGET_DIR=str(target_dir),
        CARGO_HOME=str(cargo_home),
        CARGO_NET_OFFLINE="true",
        CARGO_BUILD_JOBS=str(jobs),
    )
    if Path(cargo).is_absolute():
        c_env["PATH"] = str(Path(cargo).parent) + os.pathsep + c_env.get("PATH", "")

    run_cmd([cargo, "build", "--offline", "--release", "--manifest-path", copied / "Cargo.toml"],
            cwd=copied, env=c_env, log_path=out_dir / "cargo-build.log")
    if not archive.is_file():
        raise RuntimeError(f"translate release archive missing: {archive}")
    print(f"Built {archive} ({archive.stat().st_size} bytes)", flush=True)
    return archive


def build_and_run_e2e(source: Path, build: Path, cache: Path, archive: Path,
                      c_sources: list[Path], mesa_libs: list[Path],
                      translate_lib: Path, dump_disasm: Path | None,
                      env: dict[str, str]) -> None:
    test_src = REPO_ROOT / "adreno_compile/ir3/test_ir3_e2e.c"
    if not test_src.is_file():
        raise RuntimeError(f"missing E2E test source: {test_src}")

    inc = [
        *mesa_include_flags(source, build),
        "-I", str(REPO_ROOT / "translate/include"),
    ]
    defs = mesa_compile_defines(cache)
    common_sys_libs = ["-lz", "-lpthread", "-ldl", "-lm"]

    # 1. Link & run against the packaged libadreno_compile_ir3.a
    e2e_bin = cache / "test_ir3_e2e"
    e2e_obj = cache / "obj/test_ir3_e2e.o"
    run_cmd([
        "gcc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        *defs, *inc, "-c", test_src, "-o", e2e_obj,
    ], env=env)
    run_cmd([
        "g++", "-O2", "-g", e2e_obj,
        "-Wl,--start-group", archive, *mesa_libs, translate_lib, "-Wl,--end-group",
        *common_sys_libs, "-o", e2e_bin,
    ], env=env)

    e2e_args = [e2e_bin]
    if dump_disasm is not None:
        dump_disasm.mkdir(parents=True, exist_ok=True)
        e2e_args.extend(["--dump-disasm", dump_disasm])
    out = run_cmd(e2e_args, env=env)
    print(out, end="", flush=True)

    # 2. Compile & run ASan + UBSan over adreno_compile/ir3/*.c + test_ir3_e2e.c
    asan_dir = cache / "obj-asan"
    asan_dir.mkdir(parents=True, exist_ok=True)
    asan_flags = [
        "-std=c11", "-O1", "-g", "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter",
    ]
    asan_objs: list[Path] = []
    for src in [*c_sources, test_src]:
        obj = asan_dir / (src.stem + ".o")
        run_cmd(["gcc", *asan_flags, *defs, *inc, "-c", src, "-o", obj], env=env)
        asan_objs.append(obj)
    asan_bin = cache / "test_ir3_e2e_asan"
    run_cmd([
        "g++", "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
        *asan_objs,
        "-Wl,--start-group", *mesa_libs, translate_lib, "-Wl,--end-group",
        *common_sys_libs, "-o", asan_bin,
    ], env=env)

    asan_env = env.copy()
    asan_env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    asan_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    asan_out = run_cmd([asan_bin], env=asan_env)
    print("[ASan/UBSan] " + asan_out.splitlines()[-1] if asan_out.strip() else "[ASan/UBSan] PASS",
          flush=True)

    # 3. Run no_fallback_test.py on libadreno_compile_ir3.a
    nf_out = run_cmd([
        sys.executable, REPO_ROOT / "tools/verify/no_fallback_test.py",
        "--profile", "compiler",
        "--artifact", archive,
    ], cwd=REPO_ROOT, env=env)
    print(nf_out, end="", flush=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--jobs", type=int, default=32)
    parser.add_argument("--run-e2e", action="store_true")
    parser.add_argument("--dump-disasm", type=Path, default=None)
    parser.add_argument("--mesa-only", action="store_true",
                        help="stop after building and verifying the 10 Mesa static libraries")
    args = parser.parse_args(argv)

    try:
        cache = checked_cache_dir(args.cache_dir, "--cache-dir")
        dump_disasm = (
            checked_cache_dir(args.dump_disasm, "--dump-disasm")
            if args.dump_disasm is not None else None
        )
        if args.jobs < 1:
            raise ValueError("--jobs must be >= 1")
        cache.mkdir(parents=True, exist_ok=True)
        (cache / "tmp").mkdir(parents=True, exist_ok=True)

        env = os.environ.copy()
        env.update(
            PATH=str(MESON.parent) + os.pathsep + env.get("PATH", ""),
            TMPDIR=str(cache / "tmp"),
            TEMP=str(cache / "tmp"),
            TMP=str(cache / "tmp"),
            PYTHONDONTWRITEBYTECODE="1",
            PYTHONPYCACHEPREFIX=str(cache / "pycache"),
            MESA_GIT_SHA1_OVERRIDE=MESA_COMMIT,
            CCACHE_DISABLE="1",
        )

        source, build, _archive_map, mesa_libs = build_mesa_archives(
            args.mesa.resolve(), args.manifest.resolve(), cache, args.jobs, env
        )
        if args.mesa_only:
            print("Mesa-only build completed cleanly.")
            return 0

        archive, c_sources = build_ir3_archive(source, build, cache, env)
        if args.run_e2e:
            translate_lib = ensure_translate_archive(cache, args.jobs, env)
            build_and_run_e2e(
                source, build, cache, archive, c_sources, mesa_libs,
                translate_lib, dump_disasm, env,
            )
    except (OSError, RuntimeError, ValueError, KeyError) as exc:
        print(f"build_adreno_compile_ir3: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
