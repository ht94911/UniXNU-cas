#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inventory the Vulkan-free compiler targets in the pinned Mesa Git tree."""

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess


MESA_COMMIT = "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
EXCLUDED = "src/freedreno/vulkan"
COMPILER = "src/compiler"
NIR = f"{COMPILER}/nir"
SPIRV = f"{COMPILER}/spirv"
ISASPEC = f"{COMPILER}/isaspec"
FREEDRENO = "src/freedreno"
IR3 = f"{FREEDRENO}/ir3"
FDL = f"{FREEDRENO}/fdl"
COMMON = f"{FREEDRENO}/common"
REGS = f"{FREEDRENO}/registers"
ISA = f"{FREEDRENO}/isa"
UTIL = "src/util"
FORMAT = f"{UTIL}/format"
COMPONENTS = {
    "nir": NIR, "spirv": SPIRV, "ir3": IR3, "fdl": FDL, "common": COMMON,
    "registers": REGS,
}
SUPPORT_MESON = {
    "meson_root": "meson.build",
    "compiler_subdirs": "src/meson.build",
    "freedreno_subdirs": f"{FREEDRENO}/meson.build",
    "idep_mesautil": f"{UTIL}/meson.build",
    "idep_mesautilformat": f"{FORMAT}/meson.build",
    "idep_blake3": f"{UTIL}/blake3/meson.build",
    "idep_mesautilc11": "src/c11/impl/meson.build",
}


def git(mesa, *args):
    return subprocess.run(
        ["git", "-C", str(mesa), *args], check=True, stdout=subprocess.PIPE,
        text=True, encoding="utf-8",
    ).stdout


def meson_path(directory):
    return f"{directory}/meson.build"


def meson_match(text, pattern):
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError(f"expected pinned Meson rule matching {pattern!r}")
    return match.group(1)


def literals(text):
    return re.findall(r"'([^'\n]+)'", text)


def paths(directory, names):
    return [f"{directory}/{name}" for name in names]


def files_rule(text, assignment):
    return literals(meson_match(text, rf"^[ \t]*{assignment}[ \t]*files\(([^)]*)\)"))


def library_files(text, target):
    body = meson_match(text, rf"^{target} = static_library\(\s*'[^']+',\s*\[(.*?)\],")
    return literals(body)


def library(directory, meson_target, names, generator_names, dependencies):
    files = paths(directory, names)
    sources = [path for path in files if PurePosixPath(path).suffix in {".c", ".cc", ".cpp"}]
    return {
        "meson_build": meson_path(directory),
        "meson_target": meson_target,
        "sources": sources,
        "headers": [path for path in files if path not in sources],
        "generators": generator_names,
        "meson_dependencies": dependencies,
    }


def generator(definitions, tool, inputs, outputs):
    return {
        "meson_build": definitions,
        "tool": tool,
        "source_inputs": inputs,
        "outputs": outputs,
    }


def direct_generator(builds, directory, name, extra_inputs=(), tool="python3"):
    # These pinned targets declare same-line literal inputs and outputs.
    body = meson_match(builds[directory], rf"^{name} = custom_target\((.*?)^\)")
    inputs = literals(meson_match(body, r"^[ \t]*input[ \t]*:[ \t]*([^\n]+)"))
    # The optional second output of nir_opt_algebraic is for Mesa tests.
    output_expr = meson_match(body, r"^[ \t]*output[ \t]*:[ \t]*([^\n]+)")
    outputs = literals(output_expr.split(" + ", 1)[0])
    return generator(
        [meson_path(directory)], tool,
        paths(directory, inputs) + list(extra_inputs), paths(directory, outputs),
    )


def register_generators(builds):
    adreno = f"{REGS}/adreno"
    parent_build = builds[REGS]
    adreno_build = builds[adreno]
    register_xml = files_rule(adreno_build, r"xml_reg_files =")
    extra_xml = files_rule(adreno_build, r"xml_files \+=")
    root_xml = literals(meson_match(parent_build, r"^xml_adreno_files = \[(.*?)\]"))
    pm4_xml = files_rule(adreno_build, r"adreno_pm4_xml =")
    counter_pairs = literals(meson_match(adreno_build, r"^perfcntr_files = \[(.*?)^\]"))
    shared = paths(REGS, ["gen_header.py", "rules-fd.xsd", "freedreno_copyright.xml"])
    xml_inputs = paths(REGS, root_xml) + paths(adreno, register_xml + extra_xml + pm4_xml)
    definitions = [meson_path(REGS), meson_path(adreno)]

    defines = [f"{path}.h" for path in paths(REGS, root_xml) +
               paths(adreno, register_xml + extra_xml)]
    pm4_headers = paths(adreno, ["adreno_pm4.xml.h", "adreno-pm4-pack.xml.h"])
    counter_json = counter_pairs[1::2]
    python_defines = paths(adreno, [f"{PurePosixPath(xml).stem}.py" for xml in register_xml])
    counters = generator(
        definitions, "python3", shared + xml_inputs + paths(adreno, counter_json),
        paths(adreno, [f"{name}.h" for name in counter_json]),
    )
    counters["xml_json_pairs"] = [
        paths(adreno, [xml, data]) for xml, data in zip(counter_pairs[::2], counter_json)
    ]
    return {
        "register_c_defines": generator(definitions, "python3", shared + xml_inputs, defines),
        "register_pm4_headers": generator(
            [meson_path(adreno)], "python3", shared + xml_inputs, pm4_headers,
        ),
        "register_c_pack_structs": generator(
            [meson_path(adreno)], "python3", shared + xml_inputs, paths(adreno, ["a6xx-pack.xml.h"]),
        ),
        "register_perfcntrs": counters,
        "register_py_defines": generator(
            definitions, "python3", shared + xml_inputs, python_defines,
        ),
    }


def build_plan(builds):
    nir_inputs = paths(NIR, files_rule(builds[NIR], r"nir_depends ="))
    algebraic_inputs = paths(NIR, files_rule(builds[NIR], r"nir_algebraic_depends ="))
    nir_generators = {
        name: direct_generator(builds, NIR, name, nir_inputs)
        for name in ("nir_builder_opcodes_h", "nir_constant_expressions_c",
                     "nir_opcodes_h", "nir_opcodes_c")
    }
    nir_generators["nir_opt_algebraic"] = direct_generator(
        builds, NIR, "nir_opt_algebraic", algebraic_inputs,
    )
    for name in ("nir_intrinsics_h", "nir_intrinsics_indices_h", "nir_intrinsics_c"):
        nir_generators[name] = direct_generator(builds, NIR, name, paths(NIR, ["nir_intrinsics.py"]))

    spirv_generators = {
        name: direct_generator(builds, SPIRV, name)
        for name in ("vtn_gather_types_c", "spirv_info", "vtn_generator_ids_h")
    }
    ir3_generators = {
        name: direct_generator(builds, IR3, name, algebraic_inputs)
        for name in ("ir3_nir_trig_c", "ir3_nir_imul_c", "ir3_nir_branch_and_or_not_c",
                     "ir3_nir_triop_bitwise_c")
    }
    ir3_generators["ir3_parser"] = direct_generator(builds, IR3, "ir3_parser", tool="bison")
    ir3_generators["ir3_lexer"] = direct_generator(builds, IR3, "ir3_lexer", tool="flex")

    registers = register_generators(builds)
    devices = direct_generator(builds, COMMON, "freedreno_devices_h",
                               paths(COMMON, ["freedreno_dev_info.py"]))
    devices["generated_inputs"] = registers["register_py_defines"]["outputs"]
    isa_xml = literals(meson_match(builds[ISA], r"^isa_depend_files = \[(.*?)\]"))
    isa_inputs = paths(ISA, isa_xml) + paths(ISASPEC, ["isa.py"])
    isa_decode = direct_generator(builds, ISA, "ir3_isa", paths(ISASPEC, ["decode.py"]) + isa_inputs)
    isa_encode = direct_generator(builds, ISA, "encode_h", paths(ISASPEC, ["encode.py"]) + isa_inputs)

    compiler_generators = {
        name: direct_generator(builds, COMPILER, name, paths(COMPILER, ["builtin_types.py"]))
        for name in ("builtin_types_h", "builtin_types_c")
    }
    compiler_generators["ir_expression_operation_h"] = direct_generator(
        builds, COMPILER, "ir_expression_operation_h",
    )
    format_generators = {
        name: direct_generator(builds, FORMAT, name,
                               paths(FORMAT, ["u_format_pack.py", "u_format_parse.py"]))
        for name in ("u_format_gen_h", "u_format_pack_h", "u_format_table_c")
    }
    compiler_generators["builtin_types_h"]["generated_inputs"] = format_generators[
        "u_format_gen_h"
    ]["outputs"]
    generators = {
        **nir_generators, **spirv_generators, **ir3_generators, **registers,
        "freedreno_devices_h": devices, "ir3_isa": isa_decode, "encode_h": isa_encode,
        **compiler_generators, **format_generators,
        "git_sha1_h": generator(["src/meson.build", "bin/meson.build"], "python3",
                                ["bin/git_sha1_gen.py"], ["src/git_sha1.h"]),
        "format_srgb": direct_generator(builds, UTIL, "format_srgb"),
        "shader_stats_h": direct_generator(builds, UTIL, "shader_stats_h"),
    }
    libraries = {
        "nir": library(
            NIR, "_libnir", files_rule(builds[NIR], r"files_libnir \+=(?= files\(\n)"),
            list(nir_generators), ["idep_compiler", "dep_valgrind", "idep_mesautil"],
        ),
        "vtn": library(
            SPIRV, "libvtn", files_rule(builds[SPIRV], r"files_libvtn ="),
            list(spirv_generators), ["dep_spirv_tools", "idep_nir", "dep_valgrind"],
        ),
        "freedreno_ir3": library(
            IR3, "libfreedreno_ir3", files_rule(builds[IR3], r"libfreedreno_ir3_files ="),
            list(ir3_generators), ["libir3encode", "libfreedreno_common", "idep_libir3decode",
                                   "idep_nir_headers", "dep_dl", "idep_mesautil"],
        ),
        "freedreno_layout": library(
            FDL, "libfreedreno_layout", library_files(builds[FDL], "libfreedreno_layout"),
            ["register_c_defines", "register_pm4_headers", "register_c_pack_structs",
             "register_perfcntrs"],
            ["idep_nir_headers", "idep_mesautil", "idep_libfreedreno_common"],
        ),
        "freedreno_common": library(
            COMMON, "libfreedreno_common", library_files(builds[COMMON], "libfreedreno_common"),
            ["freedreno_devices_h", "register_pm4_headers", "git_sha1_h"],
            ["idep_mesautil"],
        ),
        "compiler": library(
            COMPILER, "libcompiler", files_rule(builds[COMPILER], r"files_libcompiler ="),
            list(compiler_generators), ["dep_valgrind"],
        ),
        "ir3encode": library(
            ISA, "libir3encode", literals(meson_match(builds[ISA], r"^encode_files = \[(.*?)\]")),
            ["encode_h"], ["idep_mesautil", "idep_nir"],
        ),
        "ir3decode": library(
            ISA, "_libir3decode", literals(meson_match(builds[ISA], r"^decode_files = \[(.*?)\]")),
            ["ir3_isa"], ["idep_mesautil", "idep_isaspec_decode", "libisaspec"],
        ),
        "isaspec": library(
            ISASPEC, "libisaspec", library_files(builds[ISASPEC], "libisaspec"), [],
            ["idep_mesautil"],
        ),
    }
    return libraries, generators


def probe(mesa):
    head = git(mesa, "rev-parse", "HEAD").strip()
    if head != MESA_COMMIT:
        raise ValueError(f"Mesa HEAD is {head}; expected {MESA_COMMIT}")

    # Use commit objects so patches and untracked files in the shared Mesa checkout
    # cannot change the Wave 1 baseline or require a reset.
    directories = [*COMPONENTS.values(), COMPILER, ISASPEC, ISA, REGS + "/adreno", UTIL, FORMAT]
    builds = {directory: git(mesa, "show", f"{head}:{meson_path(directory)}")
              for directory in directories}
    tracked = set(git(
        mesa, "ls-tree", "-r", "--name-only", "-z", head, "--",
        *directories, *SUPPORT_MESON.values(), "bin/meson.build", "bin/git_sha1_gen.py",
    ).split("\0"))
    tracked.discard("")
    components = {}
    for component, directory in COMPONENTS.items():
        files = sorted(path for path in tracked if path.startswith(f"{directory}/"))
        components[component] = {
            "directory": directory,
            "file_count": len(files),
            "meson_build": [path for path in files if PurePosixPath(path).name == "meson.build"],
            "files": files,
        }

    libraries, generators = build_plan(builds)
    listed = set(SUPPORT_MESON.values())
    for library_info in libraries.values():
        listed.add(library_info["meson_build"])
        listed.update(library_info["sources"] + library_info["headers"])
    for rule in generators.values():
        listed.update(rule["meson_build"] + rule["source_inputs"])
    for info in components.values():
        listed.update(info["files"])
    if listed - tracked:
        raise ValueError(f"paths missing from pinned Git tree: {sorted(listed - tracked)}")
    if any(path == EXCLUDED or path.startswith(f"{EXCLUDED}/") for path in listed):
        raise ValueError(f"compiler inventory includes excluded source prefix {EXCLUDED}")

    return {
        "mesa_commit": head,
        "source_basis": "Pinned Git tree and Meson blobs",
        "configuration": {"with_nir_headers_only": False, "with_tests": False,
                          "with_freedreno_vk": False},
        "excluded_source_prefixes": [EXCLUDED],
        "components": components,
        "libraries": libraries,
        "generators": generators,
        "support_meson": SUPPORT_MESON,
        "generator_host_requirements": {
            "tools": ["python3", "bison", "flex"],
            "python_modules": ["Mako >= 0.8.0", "PyYAML"],
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.out.resolve().is_relative_to(args.mesa.resolve()):
        parser.error("--out must be outside the read-only Mesa checkout")

    try:
        manifest = probe(args.mesa)
        args.out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"mesa_probe: {exc}\n")

    print(f"Mesa HEAD {manifest['mesa_commit']}")
    for name, info in manifest["components"].items():
        print(f"  {name}: {info['file_count']} tracked files, {len(info['meson_build'])} Meson files")
    output_count = sum(len(rule["outputs"]) for rule in manifest["generators"].values())
    print(f"  {len(manifest['libraries'])} static libraries, "
          f"{len(manifest['generators'])} generator groups, {output_count} outputs")
    print(f"  verified zero compiler input paths under {EXCLUDED}")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
