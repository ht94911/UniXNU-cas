#!/usr/bin/env python3
"""Generate a bounded cas Apple-format ADT and its host audit in a new directory.

Fixture mode omits entropy and is never a boot input. Private mode obtains
fresh OS entropy; the resulting file still requires X-05/X-06 runtime gates.
No device, storage, firmware or network operation is performed.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path

from adt_codec import decode
from cas_adt import ROOT, audit_sources, audit_tree, build_tree, load_json, load_schema, private_directory, private_write, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--facts", type=Path, default=ROOT / "config/cas-adt-facts.json")
    parser.add_argument("--schema", type=Path, default=ROOT / "config/cas-adt-schema.json")
    parser.add_argument("--cpu-profile", choices=("cpu0", "all"), default="cpu0")
    parser.add_argument("--mode", choices=("fixture", "private"), default="fixture")
    parser.add_argument("--evidence-root", type=Path, help="Verify source bytes/values against the local management workspace")
    parser.add_argument("--out", type=Path, required=True, help="New output directory; existing paths are refused")
    args = parser.parse_args()
    try:
        facts, schema = load_json(args.facts), load_schema(args.schema)
        source_audit = audit_sources(facts, evidence_root=args.evidence_root) if args.evidence_root else {"result": "not_run", "reason": "source originals not supplied; fixed facts/schema hashes retained"}
        tree, trace = build_tree(facts, cpu_profile=args.cpu_profile, mode=args.mode)
        blob = tree.encode()
        decoded = decode(blob)
        if decoded.encode() != blob:
            raise ValueError("ADT round-trip changed bytes")
        report = audit_tree(decoded, facts, schema, cpu_profile=args.cpu_profile, mode=args.mode)
        actual = {path + "::" + key for path, node in decoded.walk() for key in ("name", *node.properties)}
        if actual != trace.keys():
            raise ValueError("property provenance is incomplete")
        report.update({"observed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
                       "format": "Apple Device Tree; not Android FDT", "bytes": len(blob),
                       "adt_sha256": hashlib.sha256(blob).hexdigest(),
                       "facts_sha256": sha256(args.facts), "schema_sha256": sha256(args.schema),
                       "source_audit": source_audit, "roundtrip_verified": True,
                       "trace_property_count": len(trace), "producer_files": {p.name: sha256(p) for p in
                         [Path(__file__), ROOT / "scripts/cas_adt.py", ROOT / "scripts/adt_codec.py"]}})
        # All validation finishes before creating any output. O_EXCL and mkdir
        # refuse reuse of a previous candidate, including symlinks.
        if args.mode == "private":
            private_directory(args.out)
        else:
            args.out.mkdir(mode=0o755, parents=True, exist_ok=False)
        files = {"cas.adt": blob, "audit.json": (json.dumps(report, indent=2) + "\n").encode(),
                 "provenance.json": (json.dumps(trace, indent=2) + "\n").encode()}
        for name, raw in files.items():
            if args.mode == "private":
                private_write(args.out / name, raw)
            else:
                fd = os.open(args.out / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
                with os.fdopen(fd, "wb") as stream:
                    stream.write(raw)
        print(json.dumps({"out": str(args.out), "mode": args.mode, "cpu_profile": args.cpu_profile,
                          "bytes": len(blob), "sha256": report["adt_sha256"], "result": report["result"],
                          "source_audit": source_audit["result"], "boot_ready": False}))
        return 0
    except (ValueError, KeyError, TypeError, OSError, UnicodeError) as error:
        parser.exit(2, f"cas ADT refused: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
