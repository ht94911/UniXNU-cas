#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Read one boot's existing native IMS and SMS logs on the computer."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re

MASK = re.compile(r"RegMasks\[([^]]+)\]:")
MODEL = re.compile(r"5wi\.mdl\.([12])\.([12])")
GATE = re.compile(r"IMSRegistered\[([^]]+)\] qualifiedToBringupPDN\[(\d+)\] qualifiedToRegister\[(\d+)\]")
AVAILABILITY = re.compile(
    r"ConnectionAvailability: size=(\d+), ct=kDataConnectionIM, "
    r"error=(\w+), available=(true|false), contextType=([^,]+)"
)
CONTEXT = re.compile(
    r"updateStateCache: kDataConnectionIM: idByMask=(-?\d+), "
    r"configuredByMask\[BB\]=(true|false), configuredByMask\[iW\]=(true|false), "
    r"activeByMask=(true|false), networkActive=(true|false), activeOver=([^,]+)"
)
FEATURE = re.compile(r"IMS Feature: (ON|OFF), VoLTE Feature support: (\w+), VoNR Feature support: (\w+)")
RAW_SEND_FAILURE = re.compile(r"QMI_WMS_RAW_SEND failed with error (0x[0-9a-fA-F]+) \(([^)]+)\)")


def parse(rows, boot_uuid):
    groups = defaultdict(list)
    masks, operating_masks, features, gates = {}, {}, {}, {}
    sms_ready = {}
    count = 0
    for row in rows:
        count += 1
        if row.get("boot_uuid") != boot_uuid:
            raise ValueError("Native log belongs to another boot UUID")
        seconds = row.get("boot_seconds")
        if not isinstance(seconds, (float, int)) or seconds < 0:
            raise ValueError("Native record has no corrected boot time")
        category, message = row.get("category", ""), row.get("message", "")
        event = {"seconds": seconds, "category": category, "message": message}
        # Slot-bearing category suffixes are retained. The global state-cache
        # message has no slot, so never assign it to the last preceding SIM.
        model, match = MODEL.fullmatch(category), MASK.match(message)
        if model and match:
            values = {key: int(value) for key, value in re.findall(r"(\w+)\[(-?\d+)\]", message)}
            if len(values) >= 15:
                item = dict(event, slot=int(model[1]), instance=int(model[2]),
                            changed_field=match[1], fields=values)
                groups["mask_changes"].append(item)
                masks[category] = item
                if values.get("Power") == 1:
                    operating_masks[category] = item
        if category.startswith("5wi"):
            match = GATE.search(message)
            if match:
                item = dict(event, registration_state=match[1],
                            qualified_to_bringup_PDN=int(match[2]), qualified_to_register=int(match[3]))
                groups["registration_gates"].append(item)
                gates[category] = item
            match = AVAILABILITY.search(message)
            if match:
                groups["connection_availability"].append(dict(
                    event, container_size=int(match[1]), error=match[2],
                    available=match[3] == "true", context_type=match[4]))
            match = FEATURE.search(message)
            if match:
                item = dict(event, feature_enabled=match[1] == "ON", VoLTE_support=match[2], VoNR_support=match[3])
                groups["feature_changes"].append(item)
                features[category] = item
            if "IMS PDN is not active" in message:
                groups["PDN_not_active"].append(event)
            if re.search(r"P-CSCF|PCSCF|\b(?:start|bringup|request)\b.*\bPDN\b|\bPDN\b.*\b(?:activated|failed)\b", message, re.I):
                groups["PDN_and_PCSCF"].append(event)
        if category.startswith("DATA"):
            match = CONTEXT.search(message)
            if match:
                groups["global_IM_context_cache"].append(dict(
                    event, slot=None, context_id=int(match[1]), configured_BB=match[2] == "true",
                    configured_Tech=match[3] == "true", active=match[4] == "true",
                    network_active=match[5] == "true", active_over=match[6]))
            if re.search(r"internal event: S(?:b|w)?f\(", message):
                groups["IM_availability_reasons"].append(event)
            if "settings.name=" in message and re.search(r"type-mask=\[S\]\(0x20000\)", message):
                # Retain the original native redaction; no credentials are decoded.
                groups["original_IMS_APN_settings"].append(event)
        if category.startswith("msg.sms"):
            if "SMS [ready] - over BB" in message or "SMSReadyStatus true" in message:
                groups["SMS_baseband_ready"].append(event)
                sms_ready[category] = event
            if "SMS AWD Send Start" in message:
                match = re.search(r"type: (\w+)", message)
                groups["SMS_send_attempts"].append(dict(event, message_type=match[1] if match else None))
            match = RAW_SEND_FAILURE.search(message)
            if match:
                groups["SMS_raw_send_failures"].append(dict(event, QMI_error=int(match[1], 16), error_name=match[2]))
            if "SMS AWD Send End" in message or "SMS Send Error" in message or "Segment failed" in message:
                groups["SMS_send_results"].append(event)

    return {
        "phone_operations": False, "boot_uuid": boot_uuid, "native_record_count": count,
        "latest_masks": masks, "last_operating_masks": operating_masks,
        "latest_features": features, "latest_registration_gates": gates,
        "group_counts": {key: len(value) for key, value in groups.items()},
        "SMS_raw_send_error_counts": dict(Counter(str(x["QMI_error"]) for x in groups.get("SMS_raw_send_failures", []))),
        "SMS_baseband_ready_categories": sorted(sms_ready), "groups": dict(groups),
        "evidence_scope": (
            "Native feature/readiness, context and registration-gate observations from one corrected boot. "
            "Global IM cache rows have no slot attribution. VoLTE feature ON, WMS ready and a network "
            "registration status do not prove IMS registration, SMS delivery or two-way voice. "
            "Missing events remain unknown; no phone request is generated."
        ),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native_jsonl", type=Path)
    parser.add_argument("--boot-uuid", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9A-F]{32}", args.boot_uuid):
        parser.error("Expected the captured native boot UUID")
    digest = hashlib.sha256()
    with args.native_jsonl.open("rb") as stream:
        def records():
            for line in stream:
                digest.update(line)
                yield json.loads(line)
        report = parse(records(), args.boot_uuid)
    report.update(source=str(args.native_jsonl), source_sha256=digest.hexdigest())
    with args.output.open("x") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps({key: report[key] for key in (
        "boot_uuid", "native_record_count", "group_counts", "SMS_raw_send_error_counts",
    )}, indent=2))


if __name__ == "__main__":
    main()
