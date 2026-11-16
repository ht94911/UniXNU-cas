#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compare a local receiver snapshot with an ordinary Core Location app sample.

Only reads saved JSON. A result is hardware evidence only when its inputs are
from the same real boot/candidate and the application obtained normal permission.
"""
import argparse
import json
import math
from pathlib import Path
import struct


def number_bits(value, size):
    raw = bytes.fromhex(value)
    if len(raw) != size:
        raise ValueError("Invalid IEEE field length")
    return struct.unpack(">d" if size == 8 else ">f", raw)[0]


def correlate(receiver, app):
    if receiver["boot_session_uuid"] != app["boot_session_uuid"] or receiver["candidate_sha256"] != app["candidate_sha256"]:
        raise ValueError("Receiver/app boot or candidate identity differs")
    fix = receiver["fix"]
    location = app["location"]
    if app.get("provider") != "CoreLocation" or app.get("authorization") not in ("authorizedWhenInUse", "authorizedAlways"):
        raise ValueError("An ordinary authorized Core Location sample is required")
    if location.get("simulated_by_software") is not False:
        raise ValueError("The app must record a non-simulated source")
    if fix["status"] or fix["running"] != 1 or not fix["fix_deliveries"] or fix["valid"] & 0x32 != 0x32:
        raise ValueError("No receiver fix was delivered by the PDS provider")
    latitude, longitude = number_bits(fix["lat_bits"], 8), number_bits(fix["lon_bits"], 8)
    accuracy = number_bits(fix["accuracy_bits"], 4)
    values = (latitude, longitude, accuracy, location["latitude"], location["longitude"], location["horizontal_accuracy"])
    if not all(math.isfinite(v) for v in values) or accuracy < 0 or location["horizontal_accuracy"] < 0:
        raise ValueError("Invalid receiver or application position/precision")
    if abs(latitude) > 90 or abs(longitude) > 180 or abs(location["latitude"]) > 90 or abs(location["longitude"]) > 180:
        raise ValueError("Coordinates are outside WGS84 degrees")
    # Preserve distinct Core Location fusion/rounding and label the correlation
    # honestly. Matching coordinates alone cannot prove satellite origin.
    timestamp_delta = abs(location["timestamp_unix_ms"] - fix["utc_ms"])
    dt = math.radians(location["latitude"] - latitude)
    dl = math.radians(location["longitude"] - longitude)
    a = math.sin(dt / 2)**2 + math.cos(math.radians(latitude)) * math.cos(math.radians(location["latitude"])) * math.sin(dl / 2)**2
    distance = 6371000 * 2 * math.asin(min(1, math.sqrt(max(0, a))))
    age = app["received_unix_ms"] - location["timestamp_unix_ms"]
    aligned = timestamp_delta <= 1000 and 0 <= age <= 10000 and distance <= max(1, accuracy, location["horizontal_accuracy"])
    if app.get("accuracy_authorization") != "fullAccuracy":
        aligned = False
    pages = receiver.get("used", [])
    if pages and any(row["cursor"] != fix["cursor"] for row in pages):
        raise ValueError("Used-satellite pages belong to different fixes")
    complete = bool(fix["used_present"]) and len(pages) == fix["used_count"]
    if len({row["index"] for row in pages}) != len(pages) or (complete and set(row["index"] for row in pages) != set(range(len(pages)))):
        raise ValueError("Satellite pages are duplicated or incomplete")
    bds = [row for row in pages if row["system"] == 6 and 201 <= row["svid"] <= 263 and row["used_in_fix"] is True]
    bds_ids = {row["svid"] for row in bds}
    if complete and len(bds_ids) != fix["bds_used"]:
        raise ValueError("Receiver usage count differs from satellite pages")
    masks = 0
    for row in bds:
        masks |= row["signals"]
    if aligned and complete and bds:
        status = "北斗参与解算并关联到该次 Core Location 位置"
    elif receiver.get("bds_seen", fix.get("bds_seen", 0)):
        status = "已观测到北斗，参与解算待证"
    else:
        status = "北斗参与解算及应用关联待证"
    return dict(status=status, application_aligned=aligned, timestamp_delta_ms=timestamp_delta,
                distance_metres=distance, used_pages_complete=complete,
                bds_used_confirmed=len(bds_ids) if complete else 0,
                bds_signals=dict(B1I=bool(masks & 0x200) if complete and fix["signals_present"] else None,
                                B1C=bool(masks & 0x400) if complete and fix["signals_present"] else None,
                                B2a=bool(masks & (0x1000 | 0x80000)) if complete and fix["signals_present"] else None),
                scope="comparison of saved evidence; actual boot and sample provenance remain required")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receiver", required=True, type=Path)
    parser.add_argument("--application", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = correlate(json.loads(args.receiver.read_text()), json.loads(args.application.read_text()))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
