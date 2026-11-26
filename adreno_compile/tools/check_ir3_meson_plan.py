#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Configure the pinned Mesa compiler archives offline and dry-run their Ninja targets."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


MESA_COMMIT = "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
MESON = Path("/home/hua/nezha-work/tools/meson-venv/bin/meson")
LANE = Path(__file__).resolve().parents[2]
CACHE_ROOT = LANE.parent / ".cache"
EXCLUDED = "src/freedreno/vulkan"

# Desktop GL enables full NIR. Software Gallium gives Meson a frontend
# configuration, but none of its driver targets are selected for the dry-run.
OPTIONS = [
    "--backend=ninja", "--wrap-mode=nodownload", "-Dplatforms=[]", "-Dvulkan-drivers=[]",
    "-Dgallium-drivers=softpipe", "-Dtools=freedreno", "-Dopengl=true", "-Dglx=disabled",
    "-Degl=disabled", "-Dgbm=disabled", "-Dgles1=disabled", "-Dgles2=disabled",
    "-Dllvm=disabled", "-Dspirv-tools=disabled", "-Dbuild-tests=false",
]


def run(command, env, log=None):
    command = [str(part) for part in command]
    print(f"$ {shlex.join(command)}", flush=True)
    result = subprocess.run(
        command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8",
    )
    if log is not None:
        log.write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0:
        detail = "\n".join(result.stdout.splitlines()[-24:])
        raise RuntimeError(f"exit {result.returncode}: {shlex.join(command)}\n{detail}")
    return result.stdout


def pinned_source(mesa, cache, env):
    source = cache / f"mesa-{MESA_COMMIT[:12]}"
    marker = source / ".pinned-commit"
    if marker.is_file() and marker.read_text(encoding="ascii").strip() == MESA_COMMIT:
        return source
    source.mkdir(exist_ok=True)
    # Use Git objects so private working-tree patches do not enter the plan.
    command = [
        "git", "-C", str(mesa), "archive", MESA_COMMIT,
        "VERSION", "meson.build", "meson.options", "bin", "include", "src", "subprojects",
    ]
    print(f"$ {shlex.join(command)} | tar -xf - -C {shlex.quote(str(source))}", flush=True)
    with subprocess.Popen(command, stdout=subprocess.PIPE, env=env) as archive:
        assert archive.stdout is not None
        extracted = subprocess.run(["tar", "-xf", "-", "-C", source], stdin=archive.stdout, env=env)
        archive.stdout.close()
        if archive.wait() != 0 or extracted.returncode != 0:
            raise RuntimeError("could not materialize pinned Mesa Git objects")
    marker.write_text(MESA_COMMIT + "\n", encoding="ascii")
    return source


def check_plan(mesa, manifest_path, cache):
    env = os.environ.copy()
    env.update(
        PATH=str(MESON.parent) + os.pathsep + env.get("PATH", ""),
        TMPDIR=str(cache / "tmp"),
        PYTHONDONTWRITEBYTECODE="1",
        MESA_GIT_SHA1_OVERRIDE=MESA_COMMIT,
    )
    (cache / "tmp").mkdir(exist_ok=True)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest["mesa_commit"] != MESA_COMMIT:
        raise ValueError("manifest does not describe the pinned Mesa commit")
    head = run(["git", "-C", mesa, "rev-parse", "HEAD"], env).strip()
    if head != MESA_COMMIT:
        raise ValueError(f"Mesa HEAD is {head}; expected {MESA_COMMIT}")
    print(f"Mesa HEAD {head}")

    needed = ("git", "tar", "ninja", "pkg-config", "cc", "c++", "bison", "flex")
    missing = [tool for tool in needed if shutil.which(tool, path=env["PATH"]) is None]
    if missing:
        raise RuntimeError(f"missing system tools: {', '.join(missing)}")
    if not MESON.is_file():
        raise RuntimeError(f"missing Meson: {MESON}")
    versions = run(
        [MESON.with_name("python"), "-c",
         "from importlib.metadata import version; import mako, yaml; "
         "print('Mako', version('Mako'), '/ PyYAML', version('PyYAML'))"],
        env,
    )
    print(f"Host prerequisites: {', '.join(needed)}; {versions.strip()}")

    source = pinned_source(mesa, cache, env)
    build = cache / f"build-{MESA_COMMIT[:12]}"
    libraries = manifest["libraries"]
    input_paths = {"include/vulkan/vulkan_core.h", "src/util/format/u_format.yaml"}
    for library in libraries.values():
        input_paths.update(library["sources"])
        input_paths.update(library["headers"])
    for rule in manifest["generators"].values():
        input_paths.update(rule["source_inputs"])
    absent = sorted(path for path in input_paths if not (source / path).is_file())
    if absent:
        raise RuntimeError(f"missing pinned source inputs: {absent}")
    if any(path == EXCLUDED or path.startswith(EXCLUDED + "/") for path in input_paths):
        raise RuntimeError("manifest demands a Turnip source input")
    print(f"Verified {len(input_paths)} pinned sources, headers, and generator inputs")

    reconfigure = ["--reconfigure"] if (build / "build.ninja").is_file() else []
    run([MESON, "setup", *reconfigure, build, source, *OPTIONS], env, cache / "meson-setup.log")
    inventory = json.loads(run(
        [MESON, "introspect", "--targets", build], env, cache / "meson-targets.json",
    ))
    targets = {
        target["name"]: Path(target["filename"][0]).relative_to(build).as_posix()
        for target in inventory if target["type"] == "static library"
    }
    names = [*libraries, "mesa_util"]
    missing_targets = [name for name in names if name not in targets]
    if missing_targets:
        raise RuntimeError(f"missing Meson static targets: {', '.join(missing_targets)}")
    archives = [targets[name] for name in names]
    for name, archive in zip(names, archives):
        print(f"  {name}: {archive}")

    commands = run(["ninja", "-C", build, "-t", "commands", *archives],
                   env, cache / "ninja-commands.log")
    if EXCLUDED in commands or "libvulkan_freedreno" in commands:
        raise RuntimeError("Ninja compiler graph contains a Turnip input")
    if "src/compiler/nir/nir.c" not in commands or "nir_stub.c" in commands:
        raise RuntimeError("Ninja graph does not select full NIR")
    absent_sources = sorted(
        path for library in libraries.values() for path in library["sources"] if path not in commands
    )
    if absent_sources:
        raise RuntimeError(f"manifest compile sources absent from Ninja: {absent_sources}")

    dry_run = run(["ninja", "-C", build, "-n", *archives], env, cache / "ninja-dryrun.log")
    steps = sum(bool(re.match(r"^\[\d+/\d+\] ", line)) for line in dry_run.splitlines())
    print(f"Ninja dry-run passed: {len(archives)} static archives, {steps} scheduled steps")
    print(f"Full NIR and all manifest compile sources selected; zero {EXCLUDED} inputs")
    print("FDL is named libfreedreno_layout.a by the pinned Mesa build")
    print(f"Meson and Ninja logs: {cache}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    args = parser.parse_args()
    cache = args.cache_dir.resolve()
    if cache == CACHE_ROOT or not cache.is_relative_to(CACHE_ROOT):
        parser.error(f"--cache-dir must resolve beneath {CACHE_ROOT}")
    try:
        cache.mkdir(parents=True, exist_ok=True)
        check_plan(args.mesa.resolve(), args.manifest.resolve(), cache)
    except (KeyError, OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"check_ir3_meson_plan: {error}\n")


if __name__ == "__main__":
    main()
