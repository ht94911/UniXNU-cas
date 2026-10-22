#!/usr/bin/env python3
"""Fail-closed checks for the deliberately small cas build description.

This is a policy/structure audit, not an EDK II parser or a hardware test.
Only `target` (the default) may precede a build; `inputs` is design validation.
Use the compiler's report and the image audit as additional post-build gates.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path, PurePosixPath

from check_memory import number, validate_map, validate_sizes
from prepare_boot_dtb import prepare as prepare_dtb


class Rejected(ValueError):
    pass


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def contained(root, relative):
    root = Path(root).resolve()
    value = PurePosixPath(relative)
    if value.is_absolute() or ".." in value.parts or "\\" in relative or ":" in relative:
        raise Rejected(f"unsafe input path: {relative}")
    result = (root / relative).resolve()
    if not result.is_relative_to(root):
        raise Rejected(f"input escapes its root: {relative}")
    return result


def verify_file(root, item):
    path = contained(root, item["path"])
    if not path.is_file():
        raise Rejected(f"missing input: {path}")
    if digest(path) != item["sha256"]:
        raise Rejected(f"input hash mismatch: {path}")
    if "json_fields" in item:
        data = json.loads(path.read_text())
        for key, value in item["json_fields"].items():
            if data.get(key) != value:
                raise Rejected(f"input version/identity mismatch: {path}: {key}")
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}


def git(root, *args):
    run = subprocess.run(["git", "-C", str(root), *args], text=True, capture_output=True)
    if run.returncode:
        raise Rejected(f"git {' '.join(args)} failed for {root}: {run.stderr.strip()}")
    return run.stdout.strip()


def check_inputs(policy, roots):
    if policy.get("schema_version") != 1 or policy.get("board") != "cas":
        raise Rejected("unsupported policy version or wrong board")
    if policy.get("firmware_mode") != "source-only" or policy.get("vendor_inputs") != []:
        raise Rejected("this policy admits no vendor binary inputs")
    files = [verify_file(roots[item["root"]], item) for item in policy["inputs"]]
    for item in policy["pinned_includes"]:
        files.append(verify_file(roots["uefi"], item))
    git(roots["uefi"], "merge-base", "--is-ancestor", policy["uefi_baseline"], "HEAD")
    submodules = []
    for item in policy["submodules"]:
        path = contained(roots["uefi"], item["path"])
        # A missing submodule otherwise makes git silently inspect the parent repo.
        if not (path / ".git").exists():
            raise Rejected(f"uninitialized submodule: {path}")
        head = git(path, "rev-parse", "HEAD")
        if head != item["commit"] or git(path, "status", "--porcelain", "--untracked-files=no"):
            raise Rejected(f"submodule version/dirty mismatch: {path}")
        submodules.append({"path": str(path), "commit": head})
    memory = json.loads((roots["device"] / "config/memory-map.json").read_text())
    validate_map(memory)
    return {"files": files, "submodules": submodules}, memory


def source_lines(path):
    # cas descriptions use '#' comments; quoted '#' and line continuations are
    # intentionally unsupported so this audit cannot silently skip new syntax.
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if line:
            yield lineno, line


def resolve_package(uefi, packages, name):
    matches = [contained(uefi, prefix + "/" + name) for prefix in packages]
    matches = [path for path in matches if path.is_file()]
    if len(matches) != 1:
        raise Rejected(f"missing/ambiguous package input: {name}")
    return matches[0]


def audit_descriptions(uefi, policy, entries):
    """Expand only local cas includes plus reviewed, hash-pinned generic rules.

    All references in pinned library includes are checked conservatively,
    including inactive architectures. Binary FDF *rule definitions* are not
    selected inputs and cannot be mistaken for selected vendor modules.
    """
    package_roots = policy["package_roots"]
    allowed = {contained(uefi, item["path"]): item["sha256"] for item in policy["pinned_includes"]}
    local = contained(uefi, "Platforms/Xiaomi/casPkg")
    visited, visiting, references, statements = {}, set(), set(), []
    dsc_components, fdf_components = set(), set()

    def read(path):
        path = path.resolve()
        if path in visiting:
            raise Rejected(f"include cycle: {path}")
        if path in visited:
            return
        if not path.is_file():
            raise Rejected(f"missing description: {path}")
        trusted = path in allowed and digest(path) == allowed[path]
        if not path.is_relative_to(local) and not trusted:
            raise Rejected(f"unreviewed external include: {path}")
        visiting.add(path)
        in_rule = False
        in_apriori = False
        for lineno, line in source_lines(path):
            where = f"{path}:{lineno}"
            if line.startswith("["):
                in_rule = line.lower().startswith("[rule.")
            if line.lower().startswith("!include "):
                name = line.split(None, 1)[1]
                if "$" in name:
                    raise Rejected(f"computed include is not audited: {where}")
                relative = contained(path.parent, name)
                child = relative if relative.is_file() else resolve_package(uefi, package_roots, name)
                read(child)
                continue
            if line.startswith("!") and not trusted:
                raise Rejected(f"conditional/unknown directive requires review: {where}")
            if in_rule:
                if not trusted:
                    raise Rejected(f"unreviewed FDF rule: {where}")
                continue
            if re.fullmatch(r"APRIORI\s+DXE\s*\{", line, re.I):
                in_apriori = True
            elif in_apriori and line == "}":
                in_apriori = False
            if re.search(r"\bRuleOverride\b|\bSECTION\s+(RAW|PE32|TE)\b", line, re.I):
                raise Rejected(f"direct binary input is forbidden: {where}")
            for prefix in policy["forbidden_prefixes"]:
                if prefix.lower() in line.lower():
                    raise Rejected(f"excluded dependency {prefix}: {where}")
            infs = re.findall(r"[A-Za-z0-9_./-]+\.inf\b", line, re.I)
            if ".inf" in line.lower() and (not infs or "$" in line):
                raise Rejected(f"unresolved INF expression: {where}")
            references.update(infs)
            component = re.fullmatch(r"INF\s+([A-Za-z0-9_./-]+\.inf)", line, re.I)
            if component and not in_apriori:
                fdf_components.add(component[1])
            component = re.fullmatch(r"([A-Za-z0-9_./-]+\.inf)(?:\s*\{)?", line, re.I)
            if component:
                dsc_components.add(component[1])
            statements.append(line)
        visiting.remove(path)
        visited[path] = digest(path)

    for entry in entries:
        read(entry)
    modules = []
    for name in sorted(references):
        path = resolve_package(uefi, package_roots, name)
        lines = list(source_lines(path))
        if any(line.lower().startswith("[binaries") for _, line in lines):
            raise Rejected(f"binary INF is forbidden: {path}")
        # BaseFdtLib pins its source subdirectory with a local literal DEFINE.
        # Resolve only these in-file constants; no environment/CLI expressions.
        macros, defines = {}, {}
        section = ""
        for _, line in lines:
            if line.startswith("["):
                section = line.lower()
            if section == "[defines]" and (match := re.fullmatch(r"(BASE_NAME|FILE_GUID|MODULE_TYPE)\s*=\s*(\S+)", line)):
                defines[match[1]] = match[2]
            if section == "[defines]" and (match := re.fullmatch(r"DEFINE\s+(\w+)\s*=\s*([A-Za-z0-9_./-]+)", line)):
                if match[1] in macros:
                    raise Rejected(f"duplicate source macro: {path}: {match[1]}")
                contained(path.parent, match[2])
                macros[match[1]] = match[2]
        sources = []
        section = ""
        for _, line in lines:
            if line.startswith("["):
                section = line.lower()
                continue
            if section.startswith("[sources"):
                member = line.split("|", 1)[0].strip()
                member = re.sub(r"\$\((\w+)\)", lambda m: macros.get(m[1], m[0]), member)
                if "$" in member:
                    raise Rejected(f"generated/computed source requires review: {path}: {member}")
                source = contained(path.parent, member)
                if not source.is_file():
                    raise Rejected(f"missing module source: {source}")
                sources.append({"path": str(source), "sha256": digest(source)})
        if not sources:
            raise Rejected(f"module without auditable source: {path}")
        modules.append({"inf": name, "sha256": digest(path), "defines": defines,
                        "source_macros": macros, "sources": sources})
    if not modules:
        raise Rejected("empty firmware selection")
    return {"descriptions": [{"path": str(p), "sha256": v} for p, v in visited.items()],
            "modules": modules,
            "component_selection": {"dsc": sorted(dsc_components), "fdf": sorted(fdf_components)}}, statements


def check_architectural_protocols(uefi, policy, audit):
    """Check the pinned Core's real requirements against reviewed providers.

    This proves source selection and declarations, not successful installation
    on a device. Explicit DSC components and actual FV entries must agree;
    an APRIORI ordering reference or a library reference is not an FV module.
    """
    selection = audit["component_selection"]
    dsc, fdf = set(selection["dsc"]), set(selection["fdf"])
    if not dsc or dsc != fdf:
        raise Rejected(f"DSC/FDF component mismatch: DSC-only={sorted(dsc-fdf)}, FDF-only={sorted(fdf-dsc)}")
    path = contained(uefi, policy["architecture_protocol_table"])
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
    table = re.search(r"\bmArchProtocols\s*\[\s*\]\s*=\s*\{(.*?)^\s*\};", source, re.S | re.M)
    if not table:
        raise Rejected("cannot read pinned DXE architectural protocol table")
    required = re.findall(r"&\s*(g\w+ProtocolGuid)\b", table[1])
    providers = policy["required_architectural_protocols"]
    if not required or len(required) != len(set(required)) or set(required) != set(providers):
        raise Rejected("architecture provider policy disagrees with pinned DXE Core requirements")
    result = []
    for protocol in required:
        provider = providers[protocol]
        if provider not in fdf:
            raise Rejected(f"missing architectural provider: {protocol}: {provider}")
        inf = resolve_package(uefi, policy["package_roots"], provider)
        declarations, section = set(), ""
        for _, line in source_lines(inf):
            if line.startswith("["):
                section = line.lower()
            elif section.startswith("[protocols"):
                declarations.add(line)
        if protocol not in declarations:
            raise Rejected(f"architectural provider does not declare {protocol}: {provider}")
        result.append({"protocol": protocol, "provider": provider, "inf_sha256": digest(inf)})
    return {"core_table": {"path": str(path), "sha256": digest(path)}, "providers": result,
            "limitation": "Reviewed source declarations and actual component selection; runtime installation still requires hardware evidence."}


def check_target(policy, roots, memory):
    uefi = roots["uefi"]
    target = policy["target"]
    paths = {name: contained(uefi, relative) for name, relative in target.items()}
    for path in paths.values():
        if not path.is_file():
            raise Rejected(f"missing cas target file: {path}")
    config = tomllib.loads(paths["config"].read_text())
    for section, items in policy["config_values"].items():
        for key, value in items.items():
            if config.get(section, {}).get(key) != value:
                raise Rejected(f"cas config mismatch: {section}.{key}")
    regions = {r["name"]: r for r in memory["regions"]}
    if (config["uefi_fd"]["base"], config["uefi_fd"]["size"]) != (
        number(regions["uefi_fd"]["base"]), number(regions["uefi_fd"]["size"])):
        raise Rejected("FD config disagrees with memory design")
    audit, lines = audit_descriptions(uefi, policy, [paths["dsc"], paths["fdf"]])
    audit["architectural_protocols"] = check_architectural_protocols(uefi, policy, audit)
    for pcd, expected in policy["required_pcds"].items():
        values = [s.split("|", 1)[1].strip() for s in lines if s.startswith(pcd + "|")]
        if not values or any(v != expected for v in values):
            raise Rejected(f"missing/unsafe PCD {pcd}")
    for key, value in (("FD_BASE", config["uefi_fd"]["base"]),
                       ("FD_SIZE", config["uefi_fd"]["size"]),
                       ("FD_BLOCKS", config["uefi_fd"]["size"] // 4096)):
        matches = [m.group(1) for s in lines if (m := re.fullmatch(r"(?:DEFINE\s+)?" + key + r"\s*=\s*(\S+)", s))]
        if not matches or any(number(v) != value for v in matches):
            raise Rejected(f"DSC macro inconsistent: {key}")
    return audit


def check_artifacts(memory, files):
    required = {"fd", "dtb", "shim", "boot_image", "ramdisk"}
    if set(files) != required or any(not path.is_file() for path in files.values()):
        raise Rejected("artifact stage requires existing FD, DTB, shim, boot image and ramdisk")
    sizes = {key: path.stat().st_size for key, path in files.items()}
    validate_sizes(memory, sizes)
    return {key: {"path": str(path), "bytes": sizes[key], "sha256": digest(path)} for key, path in files.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--device", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--uefi", type=Path, required=True)
    ap.add_argument("--policy", type=Path)
    ap.add_argument("--stage", choices=("inputs", "target", "artifacts"), default="target")
    ap.add_argument("--stock-dtb", type=Path, help="required private source for artifact-stage DTB provenance")
    for name in ("fd", "dtb", "shim", "boot_image", "ramdisk"):
        ap.add_argument("--" + name.replace("_", "-"), type=Path)
    ap.add_argument("--out", type=Path, help="new JSON result, never overwrites an earlier check")
    args = ap.parse_args()
    result = {"board": "cas", "stage": args.stage, "hardware_proven": False,
              "build_ready": False, "result": "fail"}
    try:
        if args.out and args.out.exists():
            raise Rejected(f"refusing to overwrite report: {args.out}")
        roots = {"uefi": args.uefi.resolve(), "device": args.device.resolve()}
        policy_path = args.policy or roots["device"] / "config/input-policy.json"
        policy = json.loads(policy_path.read_text())
        result["policy_sha256"] = digest(policy_path)
        result["inputs"], memory = check_inputs(policy, roots)
        if args.stage != "inputs":
            result["target"] = check_target(policy, roots, memory)
            result["build_ready"] = True
        if args.stage == "artifacts":
            files = {k: getattr(args, k) for k in ("fd", "dtb", "shim", "boot_image", "ramdisk") if getattr(args, k)}
            result["artifacts"] = check_artifacts(memory, files)
            if not args.stock_dtb or not args.stock_dtb.is_file():
                raise Rejected("artifact stage requires the pinned stock DTB source")
            config = tomllib.loads(contained(roots["uefi"], policy["target"]["config"]).read_text())
            prepared = prepare_dtb(args.stock_dtb.read_bytes(), config["boot_image_dtb"], memory["limits"]["dtb"]["max_bytes"])
            if files["dtb"].read_bytes() != prepared:
                raise Rejected("artifact DTB is not the prepared pinned stock source")
            result["stock_dtb"] = {"path": str(args.stock_dtb.resolve()), "sha256": digest(args.stock_dtb),
                                   "role": "non-executable device-tree data"}
        result["result"] = "pass"
    except (ValueError, OSError, KeyError, TypeError) as exc:
        result.update(result="fail", build_ready=False, error=str(exc))
    output = json.dumps(result, indent=2) + "\n"
    if args.out and not args.out.exists():
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("x") as stream:
            stream.write(output)
    print(json.dumps({k: result[k] for k in ("board", "stage", "result", "build_ready", "hardware_proven", "error") if k in result}, indent=2))
    return 0 if result["result"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
