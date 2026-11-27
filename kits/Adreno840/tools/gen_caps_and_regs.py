#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate source-grade A840 caps and variant-aware registers from pinned Mesa.

TSV is the runtime table input. Mesa validates its hardware encodings and the
selected source capability profile. No chip grade is accepted in Wave 1.
The shared read-only pm4_xml_index supplies array/stripe/reg64 expansion.
"""
from __future__ import annotations

import sys
sys.dont_write_bytecode = True

import argparse
import ast
import copy
import hashlib
import importlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from fractions import Fraction

ROOT = Path(__file__).resolve().parents[3]
KIT = ROOT / "kits/Adreno840"
CACHE_PARENT = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache")
MESA_COMMIT = "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
DEFAULT_MESA = Path("/home/hua/nezha-work/ios27/gpu27-astra/private/metal-source-002/.cache/mesa-work")
FD_TABLE = "src/freedreno/fdl/fd6_format_table.c"
TU_FORMATS = "src/freedreno/vulkan/tu_formats.cc"
TU_IMAGE = "src/freedreno/vulkan/tu_image.cc"
TU_DEVICE = "src/freedreno/vulkan/tu_device.cc"
TU_CLEAR = "src/freedreno/vulkan/tu_clear_blit.cc"
DEVICE_PROPS = "src/freedreno/common/freedreno_devices.py"
FORMAT_YAML = "src/util/format/u_format.yaml"
FORMAT_HELPER = "src/util/format/u_format.h"
REGS = "src/freedreno/registers/"
A6XX_XML = REGS + "adreno/a6xx.xml"
COMMON_XML = REGS + "adreno/adreno_common.xml"
PM4_XML = REGS + "adreno/adreno_pm4.xml"
ENUM_XML = REGS + "adreno/a6xx_enums.xml"
PRIMARY = {FD_TABLE, TU_FORMATS, A6XX_XML, COMMON_XML, PM4_XML, ENUM_XML}
FORMAT_COLUMNS = ("mtl_pixel_format mtl_name pipe_format a840_fmt_id swap bpp "
                  "ubwc_supported renderable filterable blendable msaa grade hwtest_id").split()
FEATURE_COLUMNS = "feature_id mechanism supported grade citation hwtest_id".split()
LIMIT_COLUMNS = "limit_id value bound_kind citation notes".split()
GRADES = {"source": 0, "host": 1, "chip": 2}
FLAG_R8G8_QUIRK = 1 << 0
FLAG_XR_CODEC = 1 << 1
FLAG_SRGB = 1 << 2
FLAG_DEPTH = 1 << 3
FLAG_STENCIL = 1 << 4
FLAG_COMPRESSED = 1 << 5
FLAG_SPLIT_DS = 1 << 6
CORE_NAMES = {
    1: "A8Unorm", 10: "R8Unorm", 11: "R8Unorm_sRGB", 12: "R8Snorm",
    13: "R8Uint", 14: "R8Sint", 20: "R16Unorm", 22: "R16Snorm",
    23: "R16Uint", 24: "R16Sint", 25: "R16Float", 30: "RG8Unorm",
    31: "RG8Unorm_sRGB", 32: "RG8Snorm", 33: "RG8Uint", 34: "RG8Sint",
    40: "B5G6R5Unorm", 41: "A1BGR5Unorm", 42: "ABGR4Unorm",
    43: "BGR5A1Unorm", 53: "R32Uint", 54: "R32Sint", 55: "R32Float",
    60: "RG16Unorm", 62: "RG16Snorm", 63: "RG16Uint", 64: "RG16Sint",
    65: "RG16Float", 70: "RGBA8Unorm", 71: "RGBA8Unorm_sRGB",
    72: "RGBA8Snorm", 73: "RGBA8Uint", 74: "RGBA8Sint", 80: "BGRA8Unorm",
    81: "BGRA8Unorm_sRGB", 90: "RGB10A2Unorm", 91: "RGB10A2Uint",
    92: "RG11B10Float", 93: "RGB9E5Float", 94: "BGR10A2Unorm",
    103: "RG32Uint", 104: "RG32Sint", 105: "RG32Float",
    110: "RGBA16Unorm", 112: "RGBA16Snorm", 113: "RGBA16Uint",
    114: "RGBA16Sint", 115: "RGBA16Float", 123: "RGBA32Uint",
    124: "RGBA32Sint", 125: "RGBA32Float", 186: "ASTC_4x4_sRGB",
    190: "ASTC_6x6_sRGB", 194: "ASTC_8x8_sRGB", 204: "ASTC_4x4_LDR",
    208: "ASTC_6x6_LDR", 212: "ASTC_8x8_LDR", 250: "Depth16Unorm",
    252: "Depth32Float", 253: "Stencil8", 260: "Depth32Float_Stencil8",
    552: "BGRA10_XR", 553: "BGRA10_XR_sRGB", 554: "BGR10_XR",
    555: "BGR10_XR_sRGB",
}


class DataError(ValueError):
    pass


def without_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*",
                  lambda m: "\n" * m.group().count("\n"), text, flags=re.S)


def c_value(expression: str, names: dict[str, int]) -> int:
    expression = re.sub(r"UINT(?:32|64)_C\s*\(", "(", expression)
    expression = re.sub(r"\b(0[xX][0-9a-fA-F]+|[0-9]+)(?:[uU](?:ll?|LL?)?|(?:ll?|LL?)[uU]?)\b",
                        r"\1", expression)
    tree = ast.parse("(" + expression.strip() + ")", mode="eval")
    operations = {
        ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b,
        ast.Mult: lambda a, b: a * b, ast.LShift: lambda a, b: a << b,
        ast.RShift: lambda a, b: a >> b, ast.BitOr: lambda a, b: a | b,
        ast.BitAnd: lambda a, b: a & b,
    }

    def value(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, bool)):
            return int(node.value)
        if isinstance(node, ast.Name) and node.id in names:
            return names[node.id]
        if isinstance(node, ast.BinOp) and type(node.op) in operations:
            return operations[type(node.op)](value(node.left), value(node.right))
        raise DataError("unsupported constant expression: " + expression)
    return value(tree.body)


def chip_facts() -> tuple[dict[str, int], dict[str, int]]:
    header = without_comments((KIT / "chip_facts_a840.h").read_text())
    names = {}
    for name, expr in re.findall(r"^#define\s+(\w+)\s+([^\n]+)", header, re.M):
        if name.startswith("A840_"):
            names[name] = c_value(expr, names)
    for body in re.findall(r"\benum\s*\{(.*?)\};", header, re.S):
        next_value = 0
        for item in body.split(","):
            item = item.strip()
            if not item:
                continue
            name, sep, expr = item.partition("=")
            name = name.strip()
            next_value = c_value(expr, names) if sep else next_value
            names[name] = next_value
            next_value += 1
    source = without_comments((KIT / "chip_facts_a840.c").read_text())
    match = re.search(r"static const A840ChipFacts \w+\s*=\s*\{(.*?)\};", source, re.S)
    if not match:
        raise DataError("missing frozen chip facts initializer")
    facts = {name: c_value(expr, names) for name, expr in
             re.findall(r"\.(\w+)\s*=\s*([^,]+),?", match[1])}
    fields = re.findall(r"uint(?:32|64)_t\s+(\w+)\s*;", header)
    if set(fields) != set(facts):
        raise DataError("frozen facts structure/initializer mismatch")
    return facts, names


def xml_tag(element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def xml_enum(text: str, name: str) -> dict[str, int]:
    matches = [e for e in ET.fromstring(text).iter()
               if xml_tag(e) == "enum" and e.get("name") == name]
    if len(matches) != 1:
        raise DataError("expected one XML enum " + name)
    result, next_value = {}, 0
    for e in matches[0]:
        if xml_tag(e) != "value":
            continue
        key = e.get("name")
        value = int(e.get("value"), 0) if e.get("value") else next_value
        if not key or key in result:
            raise DataError("duplicate or unnamed enum value in " + name)
        result[key] = value
        next_value = value + 1
    return result


class Sources:
    def __init__(self, mesa: Path):
        self.mesa = mesa.resolve()
        revision = subprocess.check_output(
            ["git", "-C", str(self.mesa), "rev-parse", "HEAD"], text=True).strip()
        if revision != MESA_COMMIT:
            raise DataError("Mesa revision mismatch: " + revision)
        self.texts, self.hashes = {}, {}
        self.facts, self.fact_constants = chip_facts()
        for path in sorted(PRIMARY):
            self.read(path)
        imports = {e.get("file") for e in ET.fromstring(self.read(A6XX_XML))
                   if xml_tag(e) == "import"}
        if "adreno/a6xx_enums.xml" not in imports:
            raise DataError("a6xx.xml format-enum import missing")
        self.fmts = xml_enum(self.read(ENUM_XML), "a6xx_format")
        self.swaps = xml_enum(self.read(COMMON_XML), "a3xx_color_swap")
        self.table = self.format_table()
        self.yaml_blocks = {}
        yaml = self.read(FORMAT_YAML)
        matches = list(re.finditer(r"^- name:\s+(\S+)\s*$", yaml, re.M))
        for i, match in enumerate(matches):
            end = matches[i + 1].start() if i + 1 < len(matches) else len(yaml)
            self.yaml_blocks[match[1]] = (yaml[match.start():end], yaml[:match.start()].count("\n") + 1)
        self.props, self.prop_lines = {}, {}
        tree = ast.parse(self.read(DEVICE_PROPS))
        templates = {}
        for node in tree.body:
            if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call):
                for target in node.targets:
                    if isinstance(target, ast.Name):
                        templates[target.id] = node.value
        for name in ("a7xx_base", "a7xx_gen3", "a8xx_base", "a8xx_gen2"):
            for kw in templates[name].keywords:
                if isinstance(kw.value, ast.Constant) and isinstance(kw.value.value, bool):
                    self.props[kw.arg] = bool(kw.value.value)
                    self.prop_lines[kw.arg] = kw.value.lineno
        device = self.read(TU_DEVICE)
        mask = re.search(r"static const VkSampleCountFlags sample_counts\s*=\s*(.*?);", device, re.S)
        if not mask:
            raise DataError("missing pinned global sample-count mask")
        self.sample_counts = sum(int(x) for x in
                                 re.findall(r"VK_SAMPLE_COUNT_(\d+)_BIT", mask[1]))
        sys.path.insert(0, str(ROOT / "tools/verify"))
        index_module = importlib.import_module("pm4_xml_index")
        if index_module.MESA_COMMIT != MESA_COMMIT:
            raise DataError("PM4 index revision mismatch")
        self.index = index_module.build_index(self.mesa)
        self.pm4_domains = index_module.PM4_DOMAINS
        self.read(TU_FORMATS)
        self.read(TU_IMAGE)
        self.read(TU_CLEAR)
        helper = self.read(FORMAT_HELPER)
        function = re.search(r"util_format_is_compressed[^\n]*\n\{(.*?)\n\}", helper, re.S)
        if function is None:
            raise DataError("missing pinned compressed-layout predicate")
        self.compressed_layouts = {name.lower() for name in
                                   re.findall(r"case UTIL_FORMAT_LAYOUT_(\w+):", function[1])}

    def read(self, relative: str) -> str:
        if relative not in self.texts:
            data = subprocess.check_output(
                ["git", "-C", str(self.mesa), "show", MESA_COMMIT + ":" + relative])
            if relative in PRIMARY and (self.mesa / relative).read_bytes() != data:
                raise DataError("required source differs from pin: " + relative)
            self.texts[relative] = data.decode("utf-8")
            self.hashes[relative] = hashlib.sha256(data).hexdigest()
        return self.texts[relative]

    def citation(self, relative: str, token: str) -> str:
        data = self.read(relative)
        position = data.find(token)
        if position < 0:
            raise DataError("missing source anchor: " + relative + " / " + token)
        return relative + ":" + str(data[:position].count("\n") + 1)

    def format_table(self) -> dict:
        text = self.read(FD_TABLE).split("formats[PIPE_FORMAT_COUNT] = {", 1)[1].split("};", 1)[0]
        table = {}
        for match in re.finditer(r"\b(VTC|_TC|_T_|VT_|V__|FMT)\s*\(([^)]+)\)", without_comments(text)):
            kind = match[1]
            args = [x.strip() for x in match[2].split(",")]
            pipe = args[0]
            if pipe in table:
                raise DataError("duplicate Mesa format " + pipe)
            if kind == "FMT":
                vtx, tex, rb, swap = args[1:]
            else:
                fmt, swap = args[1:]
                vtx = fmt if kind in {"VTC", "VT_", "V__"} else "NONE"
                tex = fmt if kind in {"VTC", "_TC", "_T_", "VT_"} else "NONE"
                rb = fmt if kind in {"VTC", "_TC"} else "NONE"
            table[pipe] = {"vtx": self.fmts["FMT6_" + vtx],
                           "tex": self.fmts["FMT6_" + tex],
                           "rb": self.fmts["FMT6_" + rb],
                           "swap": self.swaps[swap],
                           "citation": self.citation(FD_TABLE, match.group())}
        return table

    def description(self, pipe: str) -> dict:
        if pipe not in self.yaml_blocks:
            raise DataError("missing pinned format description " + pipe)
        text, line = self.yaml_blocks[pipe]
        block = re.search(r"^\s{2}block:\s*\{width:\s*(\d+),\s*height:\s*(\d+),\s*depth:\s*(\d+)\}", text, re.M)
        layout = re.search(r"^  layout:\s*(\w+)", text, re.M)
        space = re.search(r"^  colorspace:\s*(\w+)", text, re.M)
        channels = re.search(r"^  channels:\s*\[([^]]+)\]", text, re.M)
        if channels is None:
            little = text.split("  little_endian:", 1)
            if len(little) != 2:
                raise DataError("missing LE channel description " + pipe)
            channels = re.search(r"channels:\s*\[([^]]+)\]", little[1].split("  big_endian:", 1)[0])
        if not all((block, layout, space, channels)):
            raise DataError("unsupported YAML description shape " + pipe)
        encodings = [x.strip() for x in channels[1].split(",")]
        bits = sum(int(re.search(r"(\d+)$", x)[1]) for x in encodings)
        width, height, depth = map(int, block.groups())
        return {"bits": bits, "width": width, "height": height, "depth": depth,
                "integer": encodings[0].startswith(("UP", "SP")),
                "snorm": encodings[0].startswith("SN"),
                "layout": layout[1], "colorspace": space[1],
                "bpp": Fraction(bits, width * height * depth),
                "citation": FORMAT_YAML + ":" + str(line)}

    def policy(self, mtl: int, name: str, pipe_full: str) -> dict:
        if mtl not in CORE_NAMES or CORE_NAMES[mtl] != name:
            raise DataError("Metal ID/name mismatch: " + str(mtl))
        if not pipe_full.startswith("PIPE_FORMAT_"):
            raise DataError("expected PIPE_FORMAT_ prefix")
        pipe = pipe_full.removeprefix("PIPE_FORMAT_")
        if pipe not in self.table:
            raise DataError("pipe format not in pinned fd6 table: " + pipe)
        entry = self.table[pipe]
        desc = self.description(pipe)
        linear_fmt, linear_swap = entry["tex"], entry["swap"]
        if pipe in {"A1R5G5B5_UNORM", "A1B5G5R5_UNORM"}:
            linear_fmt = self.fmts["FMT6_1_5_5_5_UNORM"]
            linear_swap = self.swaps["WZYX" if pipe.startswith("A1R") else "WXYZ"]
        depth = pipe.startswith("Z")
        stencil = pipe == "S8_UINT" or "_S8" in pipe
        compressed = desc["layout"] in self.compressed_layouts
        xr = "_XR" in name
        renderable = entry["rb"] != self.fmts["FMT6_NONE"]
        filterable = entry["tex"] != self.fmts["FMT6_NONE"] and not desc["integer"]
        blendable = renderable and not desc["integer"] and not (depth or stencil)
        ubwc = not compressed and pipe not in {"R9G9B9E5_FLOAT", "S8_UINT"}
        if desc["bits"] == 8 and not self.props.get("has_8bpp_ubwc", False):
            ubwc = False
        if desc["snorm"] and not self.props.get("ubwc_unorm_snorm_int_compatible", False):
            ubwc = False
        sample_counts = self.sample_counts if renderable else 1
        flags = 0
        if pipe.startswith("R8G8_") and self.props.get("r8g8_faulty_fast_clear_quirk", False):
            flags |= FLAG_R8G8_QUIRK
        if name.endswith("_sRGB") or desc["colorspace"].upper() == "SRGB":
            flags |= FLAG_SRGB
        if depth:
            flags |= FLAG_DEPTH
        if stencil:
            flags |= FLAG_STENCIL
        if compressed:
            flags |= FLAG_COMPRESSED
        if pipe == "Z32_FLOAT_S8X24_UINT":
            flags |= FLAG_SPLIT_DS
        notes = ["Source profile: 2D optimal, one mip, no sparse/mutable/host-transfer usage.",
                 "fmt/swap are LINEAR sampler encodings; tiled nonmutable swap is WZYX.",
                 "MSAA intersects the pinned global device mask (1/2/4); per-image 8x is not advertised."]
        if flags & FLAG_R8G8_QUIRK:
            notes.append("r8g8_faulty_fast_clear_quirk: retain restriction on generic fast-clear; not a ban on UBWC.")
        if flags & FLAG_SPLIT_DS:
            notes.append("Depth32Float_Stencil8 uses separate depth/stencil images; filtering refers to the depth aspect.")
        if xr:
            flags |= FLAG_XR_CODEC
            ubwc = renderable = filterable = blendable = False
            sample_counts = 0
            notes.append("XR stand-in only: backing bpp/encoding belongs to the chosen pipe format, not native XR storage; "
                         "shader range/transfer/BGRA packing codec is required and NOT implemented/qualified in Wave 1.")
        citation = ";".join((
            entry["citation"], desc["citation"],
            self.citation(FORMAT_HELPER, "util_format_is_compressed("),
            self.citation(TU_FORMATS, "return !vk_format_is_int"),
            self.citation(TU_FORMATS, "if (!vk_format_is_int(vk_format))"),
            self.citation(TU_IMAGE, "/* no UBWC with compressed formats"),
            self.citation(TU_DEVICE, "static const VkSampleCountFlags sample_counts")))
        if flags & FLAG_R8G8_QUIRK:
            citation += ";" + self.citation(TU_CLEAR, "!(info->props.r8g8_faulty_fast_clear_quirk")
        return {"a840_fmt_id": linear_fmt, "swap": linear_swap, "bpp": str(desc["bpp"]),
                "ubwc_supported": int(ubwc), "renderable": int(renderable),
                "filterable": int(filterable), "blendable": int(blendable), "msaa": sample_counts,
                "flags": flags, "tiled_fmt_id": entry["tex"], "tiled_swap": self.swaps["WZYX"],
                "color_fmt_id": entry["rb"], "color_swap": entry["swap"],
                "block_bits": desc["bits"], "block_width": desc["width"], "block_height": desc["height"],
                "citation": citation, "notes": " ".join(notes)}


def local_fact_citation(field: str) -> str:
    text = (KIT / "chip_facts_a840.c").read_text()
    position = text.find("." + field + " =")
    if position < 0:
        raise DataError("missing local fact " + field)
    return "kits/Adreno840/chip_facts_a840.c:" + str(text[:position].count("\n") + 1)


def local_header_citation(constant: str) -> str:
    text = (KIT / "chip_facts_a840.h").read_text()
    position = text.find(constant + " =")
    if position < 0:
        raise DataError("missing local constant " + constant)
    return "kits/Adreno840/chip_facts_a840.h:" + str(text[:position].count("\n") + 1)


def expected_features(sources: Sources) -> dict[str, dict]:
    result = {}
    masks = (
        "has_early_preamble", "has_gmem_fast_clear", "has_coherent_ubwc_flag_caches",
        "supports_uav_ubwc", "ubwc_coherency_quirk", "r8g8_faulty_fast_clear_quirk",
        "prede_nop_quirk", "predtf_nop_quirk", "has_salu_int_narrowing_quirk",
        "has_gmem_save_restore",
    )
    for index, name in enumerate(masks):
        enabled = int(bool(sources.facts["features"] & (1 << index)))
        if index < 9 and enabled != int(sources.props.get(name, False)):
            raise DataError("chip/Mesa quirk mismatch: " + name)
        citation = local_fact_citation("features")
        if name in sources.prop_lines:
            citation += ";" + DEVICE_PROPS + ":" + str(sources.prop_lines[name])
        result[name] = {"feature_id": name, "mechanism": "frozen chip feature bit " + str(index),
                        "supported": str(enabled), "grade": "source",
                        "citation": citation, "hwtest_id": "-"}
    properties = {
        "dual_wave_dispatch": "has_dual_wave_dispatch",
        "lrz_fast_clear": "enable_lrz_fast_clear",
        "lrz_direction_tracking": "has_lrz_dir_tracking",
        "lrz_feedback": "has_lrz_feedback",
        "astc_hdr": "has_astc_hdr",
        "cubic_filter": "has_tex_filter_cubic",
        "ssbo_64b_atomics": "has_64b_ssbo_atomics",
        "image_64b_atomics": "has_64b_image_atomics",
        "generic_clear": "has_generic_clear",
    }
    for name, prop in properties.items():
        if prop not in sources.prop_lines:
            raise DataError("missing pinned feature declaration: " + prop)
        result[name] = {"feature_id": name, "mechanism": prop,
                        "supported": str(int(sources.props[prop])), "grade": "source",
                        "citation": DEVICE_PROPS + ":" + str(sources.prop_lines[prop]), "hwtest_id": "-"}
    for name, field, mechanism in (
        ("tbdr_gmem", "gmem_bytes", "GMEM tile rendering; source profile, not Metal tile-shader qualification"),
        ("ubwc_6_0", "ubwc_version", "UBWC encoder version 6 (version tag is independently 5)"),
        ("sds_64_slots", "max_draw_states", "CP_SET_DRAW_STATE / 64 state groups"),
    ):
        result[name] = {"feature_id": name, "mechanism": mechanism, "supported": "1",
                        "grade": "source", "citation": local_fact_citation(field), "hwtest_id": "-"}
    return result


def expected_limits(sources: Sources) -> dict[str, dict]:
    maximums = {"tile_max_w", "tile_max_h", "cs_shared_mem_size", "max_waves",
                "instr_cache_size", "max_draw_states", "vs_max_inputs_count", "max_sets"}
    byte_fields = {name for name in sources.facts if name.endswith(("_size", "_bytes"))}
    result = {}
    for name, value in sources.facts.items():
        units = "bytes" if name in byte_fields else "exact chip profile field"
        result[name] = {"limit_id": name, "value": str(value),
                        "bound_kind": "max" if name in maximums else "exact",
                        "citation": local_fact_citation(name), "notes": units + "; fact=" + name}
    device = sources.read(TU_DEVICE)
    for name, prop in (
        ("max_texture_size", "maxImageDimension2D"),
        ("max_texture_size_1d", "maxImageDimension1D"),
        ("max_texture_size_3d", "maxImageDimension3D"),
        ("max_texture_size_cube", "maxImageDimensionCube"),
    ):
        match = re.search(r"props->" + prop + r"\s*=\s*\(([^;]+)\);", device)
        if not match:
            raise DataError("missing pinned image limit " + prop)
        result[name] = {"limit_id": name, "value": str(c_value(match[1], {})), "bound_kind": "max",
                        "citation": sources.citation(TU_DEVICE, match.group()),
                        "notes": "source device image dimension in texels; distinct from bin tile_max_w"}
    image_properties = sources.read(TU_FORMATS)
    for name, token, value in (
        ("max_array_layers", "maxArraySize = 2048;", 2048),
        ("max_mip_levels_2d", "maxMipLevels = 15;", 15),
        ("max_mip_levels_3d", "maxMipLevels = 12;", 12),
    ):
        result[name] = {"limit_id": name, "value": str(value), "bound_kind": "max",
                        "citation": sources.citation(TU_FORMATS, token), "notes": "pinned image-property limit"}
    extras = (
        ("max_samples", max(int(x) for x in re.findall(
            r"VK_SAMPLE_COUNT_(\d+)_BIT", re.search(
                r"static const VkSampleCountFlags sample_counts\s*=\s*(.*?);", device, re.S)[1])),
         "max", sources.citation(TU_DEVICE, "static const VkSampleCountFlags sample_counts"),
         "conservative global advertised limit; per-image 8x rule is not promoted"),
        ("sds_slots", sources.facts["max_draw_states"], "exact",
         local_fact_citation("max_draw_states"), "alias of max_draw_states"),
        ("gmem_bytes_alignment", sources.fact_constants["A840_GMEM_BYTES_ALIGNMENT"], "exact",
         local_header_citation("A840_GMEM_BYTES_ALIGNMENT"), "W00 runtime API granularity, not GMEM_PROTECT encoding"),
        ("mesa_highest_bank_bit", sources.fact_constants["A840_MESA_HIGHEST_BANK_BIT"], "exact",
         local_header_citation("A840_MESA_HIGHEST_BANK_BIT"), "Mesa static value; Kaanapali default highest_bank_bit stays 16"),
    )
    for name, value, bound, citation, notes in extras:
        result[name] = {"limit_id": name, "value": str(value), "bound_kind": bound,
                        "citation": citation, "notes": notes}
    return result


def integer(text: str, label: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise DataError("invalid integer " + label + ": " + text) from exc
    if not 0 <= value < 1 << 64:
        raise DataError("out-of-range integer " + label)
    return value


def parse_tsv(text: str, columns: list[str], label: str) -> tuple[list[dict], dict[int, dict]]:
    if "\r" in text or text.startswith("\ufeff") or not text.endswith("\n"):
        raise DataError(label + ": expected UTF-8/LF with final newline")
    rows, notes, header = [], {}, False
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line:
            continue
        if line.startswith("# format-note\t"):
            parts = line.split("\t", 2)
            if len(parts) != 3:
                raise DataError(label + ": malformed format-note")
            key = integer(parts[1], label)
            if key in notes:
                raise DataError(label + ": duplicate format-note ID")
            try:
                note = json.loads(parts[2])
            except ValueError as exc:
                raise DataError(label + ": invalid note JSON") from exc
            if not isinstance(note, dict):
                raise DataError(label + ": format-note must be an object")
            notes[key] = note
            continue
        if line.startswith("#"):
            continue
        cells = line.split("\t")
        if not header:
            if cells != columns:
                raise DataError(label + ": schema mismatch")
            header = True
            continue
        if len(cells) != len(columns):
            raise DataError(f"{label}:{line_number}: expected {len(columns)} columns")
        rows.append(dict(zip(columns, cells)))
    if not header or not rows:
        raise DataError(label + ": missing header or rows")
    return rows, notes


def unique(rows: list[dict], key: str) -> dict[str, dict]:
    result = {}
    for row in rows:
        if not row[key] or row[key] in result:
            raise DataError("empty or duplicate " + key + ": " + row[key])
        result[row[key]] = row
    return result


def grade(row: dict) -> None:
    if row["grade"] not in GRADES:
        raise DataError("invalid grade: " + row["grade"])
    if row["grade"] == "chip":
        raise DataError("Wave 1 has no per-entry chip evidence; chip grade is forbidden")
    if row["grade"] == "host" and not row["hwtest_id"].startswith("host:"):
        raise DataError("host grade requires an explicit host test ID")


def validate_tables(sources: Sources, formats: list[dict], notes: dict[int, dict],
                    features: list[dict], limits: list[dict]) -> list[dict]:
    unique(formats, "mtl_pixel_format")
    unique(formats, "mtl_name")
    enriched, ids = [], set()
    numeric = ("a840_fmt_id", "swap", "ubwc_supported", "renderable", "filterable", "blendable", "msaa")
    for row in formats:
        grade(row)
        key = integer(row["mtl_pixel_format"], "mtl_pixel_format")
        if key in ids:
            raise DataError("duplicate numeric Metal ID")
        ids.add(key)
        policy = sources.policy(key, row["mtl_name"], row["pipe_format"])
        for column in numeric:
            if integer(row[column], column) != policy[column]:
                raise DataError(str(key) + ": pinned capability mismatch for " + column)
        try:
            bpp = Fraction(row["bpp"])
        except (ValueError, ZeroDivisionError) as exc:
            raise DataError("invalid bpp") from exc
        if bpp <= 0 or bpp != Fraction(policy["bpp"]):
            raise DataError(str(key) + ": bpp does not match pinned format block")
        note = notes.get(key)
        expected_note = {k: policy[k] for k in ("flags", "citation", "notes")}
        if note != expected_note:
            raise DataError(str(key) + ": missing/changed source note or fast-clear/XR flags")
        enriched.append({**row, **policy, "mtl_pixel_format": key})
    if ids != set(CORE_NAMES) or set(notes) != ids:
        raise DataError(f"required {len(CORE_NAMES)} Metal core formats or their notes are missing")
    actual_features = unique(features, "feature_id")
    expected = expected_features(sources)
    if set(actual_features) != set(expected):
        raise DataError("required hardware/Metal source feature set mismatch")
    for name, row in actual_features.items():
        grade(row)
        for column in ("mechanism", "supported", "citation"):
            if row[column] != expected[name][column]:
                raise DataError("pinned feature mismatch: " + name + " / " + column)
    actual_limits = unique(limits, "limit_id")
    expected = expected_limits(sources)
    if set(actual_limits) != set(expected):
        raise DataError("required frozen facts/limit set mismatch")
    for name, row in actual_limits.items():
        if row["bound_kind"] not in {"min", "max", "exact"}:
            raise DataError("invalid bound kind")
        if integer(row["value"], name) != int(expected[name]["value"]):
            raise DataError("frozen fact or pinned limit mismatch: " + name)
        for column in ("bound_kind", "citation", "notes"):
            if row[column] != expected[name][column]:
                raise DataError("limit source/units mismatch: " + name + " / " + column)
    return sorted(enriched, key=lambda row: row["mtl_pixel_format"])


def read_tables(sources: Sources) -> tuple[list[dict], list[dict], list[dict]]:
    formats, notes = parse_tsv((KIT / "caps/formats.tsv").read_bytes().decode("utf-8"),
                              FORMAT_COLUMNS, "formats.tsv")
    features, _ = parse_tsv((KIT / "caps/features.tsv").read_bytes().decode("utf-8"),
                            FEATURE_COLUMNS, "features.tsv")
    limits, _ = parse_tsv((KIT / "caps/limits.tsv").read_bytes().decode("utf-8"),
                          LIMIT_COLUMNS, "limits.tsv")
    return validate_tables(sources, formats, notes, features, limits), features, limits


def c_string(text: str) -> str:
    return json.dumps(text, ensure_ascii=True)


def macro_name(text: str) -> str:
    return re.sub(r"_+", "_", re.sub(r"[^A-Za-z0-9_]", "_", text)).strip("_").upper()


def preamble(sources: Sources, guard: str) -> list[str]:
    lines = ["/* SPDX-License-Identifier: MIT */",
             "/* Generated by kits/Adreno840/tools/gen_caps_and_regs.py.",
             " * Mesa commit: " + MESA_COMMIT,
             " * All Mesa path:line citations refer to this committed revision.",
             " * Supplementary source rules are read from Git objects, never local patches.",
             " * Source grade declares source capability, not native Metal/chip qualification."]
    for path, digest in sorted(sources.hashes.items()):
        lines.append(" * SHA256 " + path + " " + digest)
    for name in ("formats.tsv", "features.tsv", "limits.tsv"):
        path = KIT / "caps" / name
        lines.append(" * SHA256 caps/" + name + " " + hashlib.sha256(path.read_bytes()).hexdigest())
    for name in ("chip_facts_a840.h", "chip_facts_a840.c"):
        lines.append(" * SHA256 " + name + " " +
                     hashlib.sha256((KIT / name).read_bytes()).hexdigest())
    lines += [" */", "#ifndef " + guard, "#define " + guard, "", "#include <stdint.h>"]
    return lines


def render_regs(sources: Sources) -> bytes:
    lines = preamble(sources, "ADRENO840_REGS_H")
    lines += ["", "/* Offsets are dwords, not bytes. reg64 high halves/arrays are expanded.",
              " * A840_REG_* resolves A8XX variants; main A6XX domain wins over",
              " * legacy AXXX aliases. Qualified domains preserve every source value. */"]
    priority = {"AXXX": 0, "A6XX": 1, "A7XX": 2, "A8XX": 3}
    qualified, flat = {}, {}
    for record in sources.index["registers"]:
        if record["chip"] != "A8XX" or record["domain"] not in sources.pm4_domains:
            continue
        name = record["name"].removeprefix("A8XX_")
        name = macro_name(name)
        key = "A840_REG_" + record["domain"] + "_" + name
        value = record["offset"]
        if key in qualified and qualified[key] != value:
            raise DataError("conflicting register within domain: " + key)
        qualified[key] = value
        current = flat.get(name)
        rank = priority[record["domain"]]
        if current is None or rank > current[0]:
            flat[name] = (rank, key, value)
        elif rank == current[0] and value != current[2]:
            raise DataError("conflicting A8XX register " + name)
    lines += ["#define A840_REG_COUNT UINT32_C(" + str(len(flat)) + ")",
              "#define A840_REG_QUALIFIED_COUNT UINT32_C(" + str(len(qualified)) + ")", ""]
    for key, value in sorted(qualified.items()):
        lines.append(f"#define {key} UINT32_C(0x{value:05x})")
    lines.append("")
    for name, (_, key, _) in sorted(flat.items()):
        lines.append("#define A840_REG_" + name + " " + key)
    lines += ["", "/* PM4 opcode numbers are 7-bit; availability reflects XML A8XX variants. */",
              "#define A840_CP_OPCODE_COUNT UINT32_C(" + str(len(sources.index["opcodes"])) + ")"]
    availability = {}
    for record in sources.index["opcode_records"]:
        availability[record["name"]] = availability.get(record["name"], False) or "A8XX" in record["chips"]
    seen = set()
    for name, value in sorted(sources.index["opcodes"].items()):
        key = "A840_" + name if name.startswith("CP_") else "A840_CP_" + name
        key = macro_name(key)
        if key in seen:
            raise DataError("opcode identifier collision " + key)
        seen.add(key)
        lines.append(f"#define {key} UINT32_C(0x{value:02x})")
        lines.append(f"#define {key}_A8XX_AVAILABLE UINT32_C({int(availability[name])})")
    lines += ["", "/* a6xx.xml imports these from adreno/a6xx_enums.xml.",
              " * FMT6_NONE is Mesa's internal sentinel, not a hardware format. */"]
    for name, value in sorted(sources.fmts.items()):
        lines.append(f"#define A840_{name} UINT32_C(0x{value:02x})")
    lines += ["", "/* adreno_common.xml a3xx_color_swap; honor per-layout helpers. */"]
    for name, value in sorted(sources.swaps.items()):
        lines.append(f"#define A840_{name} UINT32_C({value})")
    lines += ["", "#endif /* ADRENO840_REGS_H */", ""]
    return "\n".join(lines).encode("utf-8")


def render_caps(sources: Sources, formats: list[dict], features: list[dict], limits: list[dict]) -> bytes:
    lines = preamble(sources, "ADRENO840_CAPS_H")
    lines += ["#include <stddef.h>", '#include "Adreno840Regs.h"', "",
              "enum { A840_CAPS_GRADE_SOURCE = 0, A840_CAPS_GRADE_HOST = 1, A840_CAPS_GRADE_CHIP = 2 };",
              "enum {",
              "    A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK = 1u << 0,",
              "    A840_FORMAT_REQUIRES_XR_CODEC = 1u << 1,",
              "    A840_FORMAT_SRGB = 1u << 2,",
              "    A840_FORMAT_DEPTH = 1u << 3,",
              "    A840_FORMAT_STENCIL = 1u << 4,",
              "    A840_FORMAT_COMPRESSED = 1u << 5,",
              "    A840_FORMAT_SEPARATE_DEPTH_STENCIL_PLANES = 1u << 6",
              "};", "",
              "typedef struct A840FormatCaps {",
              "    uint32_t mtl_pixel_format;",
              "    uint32_t a840_fmt_id; /* linear texture format */",
              "    uint32_t swap; /* linear texture swap */",
              "    uint32_t tiled_fmt_id;",
              "    uint32_t tiled_swap;",
              "    uint32_t color_fmt_id; /* linear render/copy destination format */",
              "    uint32_t color_swap;",
              "    uint32_t bpp_numerator; /* backing pipe bpp, rational for ASTC */",
              "    uint32_t bpp_denominator;",
              "    uint32_t block_bits;",
              "    uint32_t block_width;",
              "    uint32_t block_height;",
              "    uint32_t ubwc_supported; /* eligibility for documented source profile */",
              "    uint32_t renderable; /* depth/stencil requires the flagged attachment path */",
              "    uint32_t filterable;",
              "    uint32_t blendable;",
              "    uint32_t msaa; /* sample-count mask; one mip/2D/optimal/non-storage/non-cube */",
              "    uint32_t flags;",
              "    uint32_t grade;",
              "    const char *mtl_name;",
              "    const char *pipe_format;",
              "    const char *hwtest_id;",
              "    const char *citation;",
              "    const char *notes;",
              "} A840FormatCaps;", "",
              "#define A840_FORMAT_CAPS_COUNT ((size_t)" + str(len(formats)) + ")",
              "static const A840FormatCaps a840_format_caps_data[] = {"]
    for row in formats:
        bpp = Fraction(row["bpp"])
        values = [row[key] for key in ("mtl_pixel_format", "a840_fmt_id", "swap",
                  "tiled_fmt_id", "tiled_swap", "color_fmt_id", "color_swap")]
        values += [bpp.numerator, bpp.denominator]
        values += [row[key] for key in ("block_bits", "block_width", "block_height",
                   "ubwc_supported", "renderable", "filterable", "blendable", "msaa", "flags")]
        values += [GRADES[row["grade"]]]
        numbers = ", ".join("UINT32_C(" + str(v) + ")" for v in values)
        strings = ", ".join(c_string(row[key]) for key in
                            ("mtl_name", "pipe_format", "hwtest_id", "citation", "notes"))
        lines.append("    { " + numbers + ", " + strings + " },")
    lines += ["};", "",
              "static inline size_t a840_format_caps_count(void) { return A840_FORMAT_CAPS_COUNT; }",
              "static inline const A840FormatCaps *a840_format_caps_at(size_t index)",
              "{ return index < A840_FORMAT_CAPS_COUNT ? &a840_format_caps_data[index] : NULL; }",
              "static inline const A840FormatCaps *a840_format_caps_lookup(uint32_t mtl_format)",
              "{",
              "    size_t low = 0, high = A840_FORMAT_CAPS_COUNT;",
              "    while (low < high) {",
              "        size_t middle = low + (high - low) / 2;",
              "        const A840FormatCaps *item = &a840_format_caps_data[middle];",
              "        if (item->mtl_pixel_format == mtl_format) return item;",
              "        if (item->mtl_pixel_format < mtl_format) low = middle + 1;",
              "        else high = middle;",
              "    }",
              "    return NULL;",
              "}",
              "static inline const A840FormatCaps *a840_format_info(uint32_t mtl_format)",
              "{ return a840_format_caps_lookup(mtl_format); }", "",
              "typedef struct A840FeatureCaps {",
              "    const char *feature_id;",
              "    const char *mechanism;",
              "    uint32_t supported;",
              "    uint32_t grade;",
              "    const char *citation;",
              "    const char *hwtest_id;",
              "} A840FeatureCaps;",
              "#define A840_FEATURE_CAPS_COUNT ((size_t)" + str(len(features)) + ")"]
    for row in sorted(features, key=lambda x: x["feature_id"]):
        name = "A840_CAP_" + macro_name(row["feature_id"])
        lines.append("#define " + name + " UINT32_C(" + row["supported"] + ")")
    lines.append("static const A840FeatureCaps a840_feature_caps_data[] = {")
    for row in sorted(features, key=lambda x: x["feature_id"]):
        lines.append("    { " + ", ".join((
            c_string(row["feature_id"]), c_string(row["mechanism"]),
            "UINT32_C(" + row["supported"] + ")", "UINT32_C(" + str(GRADES[row["grade"]]) + ")",
            c_string(row["citation"]), c_string(row["hwtest_id"]))) + " },")
    lines += ["};",
              "static inline size_t a840_feature_caps_count(void) { return A840_FEATURE_CAPS_COUNT; }",
              "static inline const A840FeatureCaps *a840_feature_caps_at(size_t index)",
              "{ return index < A840_FEATURE_CAPS_COUNT ? &a840_feature_caps_data[index] : NULL; }", "",
              "typedef struct A840LimitCaps {",
              "    const char *limit_id;",
              "    uint64_t value;",
              "    const char *bound_kind;",
              "    const char *citation;",
              "    const char *notes;",
              "} A840LimitCaps;",
              "#define A840_LIMIT_CAPS_COUNT ((size_t)" + str(len(limits)) + ")"]
    for row in sorted(limits, key=lambda x: x["limit_id"]):
        lines.append("#define A840_LIMIT_" + macro_name(row["limit_id"]) +
                     " UINT64_C(" + str(integer(row["value"], row["limit_id"])) + ")")
    lines.append("static const A840LimitCaps a840_limit_caps_data[] = {")
    for row in sorted(limits, key=lambda x: x["limit_id"]):
        lines.append("    { " + ", ".join((
            c_string(row["limit_id"]), "A840_LIMIT_" + macro_name(row["limit_id"]),
            c_string(row["bound_kind"]), c_string(row["citation"]), c_string(row["notes"]))) + " },")
    lines += ["};",
              "static inline size_t a840_limit_caps_count(void) { return A840_LIMIT_CAPS_COUNT; }",
              "static inline const A840LimitCaps *a840_limit_caps_at(size_t index)",
              "{ return index < A840_LIMIT_CAPS_COUNT ? &a840_limit_caps_data[index] : NULL; }",
              "", "#endif /* ADRENO840_CAPS_H */", ""]
    return "\n".join(lines).encode("utf-8")


def self_test(sources: Sources) -> int:
    formats, notes = parse_tsv((KIT / "caps/formats.tsv").read_bytes().decode("utf-8"),
                              FORMAT_COLUMNS, "formats.tsv")
    features, _ = parse_tsv((KIT / "caps/features.tsv").read_bytes().decode("utf-8"),
                            FEATURE_COLUMNS, "features.tsv")
    limits, _ = parse_tsv((KIT / "caps/limits.tsv").read_bytes().decode("utf-8"),
                          LIMIT_COLUMNS, "limits.tsv")
    validate_tables(sources, formats, notes, features, limits)
    rejected = 0
    for mutation in ("duplicate", "numeric_duplicate", "grade", "chip", "fmt", "swap",
                     "bpp", "ubwc", "xr", "quirk", "feature", "limit", "bounds"):
        f, n, e, l = copy.deepcopy((formats, notes, features, limits))
        if mutation == "duplicate":
            f.append(copy.deepcopy(f[0]))
        elif mutation == "numeric_duplicate":
            duplicate = copy.deepcopy(f[0])
            duplicate["mtl_pixel_format"] = hex(int(f[0]["mtl_pixel_format"]))
            duplicate["mtl_name"] = "numeric-alias"
            f.append(duplicate)
        elif mutation == "grade":
            f[0]["grade"] = "tested"
        elif mutation == "chip":
            f[0]["grade"] = "chip"
        elif mutation in {"fmt", "swap", "bpp", "ubwc"}:
            key = {"fmt": "a840_fmt_id", "swap": "swap", "bpp": "bpp", "ubwc": "ubwc_supported"}[mutation]
            f[0][key] = str(int(f[0][key]) + 1)
        elif mutation == "xr":
            next(x for x in f if x["mtl_pixel_format"] == "552")["ubwc_supported"] = "1"
        elif mutation == "quirk":
            n[30]["flags"] &= ~FLAG_R8G8_QUIRK
        elif mutation == "feature":
            e[0]["supported"] = "0" if e[0]["supported"] == "1" else "1"
        elif mutation == "limit":
            l[0]["value"] = str(int(l[0]["value"]) + 1)
        elif mutation == "bounds":
            l[0]["bound_kind"] = "approx"
        try:
            validate_tables(sources, f, n, e, l)
        except DataError:
            rejected += 1
        else:
            raise DataError("self-test failed to reject " + mutation)
    for text in ("bad\tcolumns\n", "\ufeff" + "\t".join(FORMAT_COLUMNS) + "\n",
                 "\t".join(FORMAT_COLUMNS) + "\r\n", "\t".join(FORMAT_COLUMNS)):
        try:
            parse_tsv(text, FORMAT_COLUMNS, "bad-fixture")
        except DataError:
            rejected += 1
        else:
            raise DataError("self-test accepted malformed TSV")
    print(f"PASS self-test: source tables valid; {rejected} malformed/schema/grade/quirk/limit cases rejected")
    return rejected


def output_directory(path: Path) -> Path:
    raw = str(path)
    result = path.resolve()
    if re.match(r"^[A-Za-z]:", raw) or result == Path("/mnt/c") or Path("/mnt/c") in result.parents:
        raise DataError("C drive/Windows output forbidden")
    allowed = result == (KIT / "gen").resolve() or CACHE_PARENT in result.parents
    if not allowed:
        raise DataError("output must be kits/Adreno840/gen or approved project cache")
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mesa", type=Path, default=DEFAULT_MESA)
    parser.add_argument("--out-dir", type=Path, default=KIT / "gen")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--tmp-dir", type=Path, default=CACHE_PARENT / "w01-l1")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    try:
        target = output_directory(args.out_dir)
        sources = Sources(args.mesa)
        formats, features, limits = read_tables(sources)
        if args.self_test:
            self_test(sources)
        headers = {"Adreno840Caps.h": render_caps(sources, formats, features, limits),
                   "Adreno840Regs.h": render_regs(sources)}
        if args.check:
            scratch = output_directory(args.tmp_dir)
            if CACHE_PARENT not in scratch.parents:
                raise DataError("--check tmp-dir must be approved cache")
            scratch.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="caps-regs-check-", dir=scratch) as temporary:
                for name, data in sorted(headers.items()):
                    rebuilt = Path(temporary) / name
                    rebuilt.write_bytes(data)
                    checked = target / name
                    if not checked.is_file() or checked.read_bytes() != rebuilt.read_bytes():
                        raise DataError("--check mismatch: " + str(checked))
            print("PASS --check: both generated headers are byte-identical")
        else:
            target.mkdir(parents=True, exist_ok=True)
            for name, data in sorted(headers.items()):
                (target / name).write_bytes(data)
                print("GENERATED " + name + " SHA256=" + hashlib.sha256(data).hexdigest())
        print(f"PASS caps/regs: formats={len(formats)} features={len(features)} limits={len(limits)} "
              f"opcodes={len(sources.index['opcodes'])}; Mesa={MESA_COMMIT}")
        return 0
    except (DataError, OSError, ValueError, subprocess.CalledProcessError) as exc:
        print("FAIL " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
