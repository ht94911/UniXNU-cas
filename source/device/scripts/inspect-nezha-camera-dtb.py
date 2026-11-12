#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inventory camera wiring in a local stock DTB without assuming sensor models."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


CAMERA_NODES = re.compile(
    r"^qcom,(?:cam-sensor\d+|cci\d+|csiphy\d+|csid(?:-lite)?\d+|"
    r"ife(?:-lite)?\d+|vfe(?:-lite)?\d+|tpg\d+|eeprom\d+|"
    r"actuator\d+|ois\d+|zoom\d+|cam_smmu|cam-cpas|cpas)(?:@.*)?$"
)
CONTROLLER_COMPATIBLE = (
    "qcom,canoe-camcc", "qcom,canoe-cambistmclkcc", "qcom,cam-cpas"
)
CELLS = (
    "cell-index", "cci-master", "csiphy-sd-index", "clock-rates",
    "interrupts", "power-domains", "clocks", "iommus", "regulator-min-microvolt",
    "regulator-max-microvolt", "rgltr-min-voltage", "rgltr-max-voltage",
    "data-lanes", "lane-polarities", "bus-type",
)
STRINGS = ("compatible", "clock-names", "interrupt-names", "regulator-names", "reg-names")


def node_path(node):
    names = []
    while node.parent is not None:
        names.append(node.name)
        node = node.parent
    return "/" + "/".join(reversed(names))


def inventory(blob, read_fdt, registers):
    root = read_fdt(blob)
    nodes = list(root.walk())
    phandles = {
        node.scalar("phandle"): node for node in nodes if "phandle" in node.properties
    }
    entries = []
    for node in nodes:
        compat = node.strings("compatible")
        if not (CAMERA_NODES.fullmatch(node.name) or set(compat) & set(CONTROLLER_COMPATIBLE)):
            continue
        entry = {"path": node_path(node), "enabled": node.enabled()}
        for key in STRINGS:
            if key in node.properties:
                entry[key] = node.strings(key)
        for key in CELLS:
            if key in node.properties:
                entry[key] = node.cells(key)
        if "reg" in node.properties:
            entry["reg_in_parent_bus"] = [
                {"address": f"0x{address:x}", "bytes": size}
                for address, size in registers(node)
            ]
        links = {}
        for key in node.properties:
            if key.endswith(("-supply", "-src")) or key == "remote-endpoint":
                targets = []
                for ref in node.cells(key):
                    target = phandles.get(ref)
                    targets.append({"phandle": ref, "path": node_path(target) if target else None})
                links[key] = targets
        if links:
            entry["links"] = links
        entries.append(entry)
    return {
        "dtb_sha256": hashlib.sha256(blob).hexdigest(),
        "root_compatible": root.strings("compatible"),
        "nodes": sorted(entries, key=lambda entry: entry["path"]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dtb", type=Path, help="locally collected stock FDT")
    parser.add_argument("--framework", type=Path, required=True, help="UniXNU checkout")
    args = parser.parse_args()
    sys.path.insert(0, str(args.framework / "scripts"))
    from unixnu.devicetree import read_fdt, registers

    result = inventory(args.dtb.read_bytes(), read_fdt, registers)
    if not result["nodes"]:
        parser.exit(1, "No supported camera nodes in this device tree\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
