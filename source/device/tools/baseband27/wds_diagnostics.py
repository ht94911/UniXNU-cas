#!/usr/bin/env python3
"""Read existing WDS and PDP console observations without accessing the phone."""
import argparse
from collections import Counter, defaultdict, deque
import hashlib
import json
from pathlib import Path
import re

COMMANDS = {
    0x20: "Start Network",
    0x21: "Stop Network",
    0x22: "Packet Service Status",
    0x27: "Create Profile",
    0x28: "Modify Profile",
    0x2B: "Get Profile Settings",
    0x2D: "Get Current Settings",
    0x4D: "Set IP Family",
    0xA2: "Bind Mux Data Port",
}
QMI = re.compile(
    r"COACHZ qmi: (\S+) ch (\d+) svc (0x[0-9a-f]+|\d+) cid (\d+) "
    r"type (\d+) msg (0x[0-9a-f]+|\d+) txn (\d+) len (\d+) "
    r"err (0x[0-9a-f]+|\d+) n (\d+) t (\d+)"
)
LIFECYCLE = re.compile(
    r"COACHZ pdp: WDS type (\d+) message (0x[0-9a-f]+|\d+) txn (\d+) "
    r"mux (\d+) family (\d+) active (\d+) error (\d+) address-fields (\d+)"
)
CALL_END = re.compile(
    r"COACHZ pdp: (start failed|disconnected) cid (\d+) channel (\d+) "
    r"mux (\d+) family (\d+) reason-present (\d+) reason (\d+) "
    r"verbose-present (\d+) type (\d+) code (-?\d+)"
)
COUNTERS = re.compile(
    r"COACHZ pdp: (\d+) s unit (\d+) families (\d+) blocked (\d+) "
    r"queued (\d+) tx (\d+) rx (\d+) drop (\d+) busy (\d+)"
)
STACK = re.compile(
    r"COACHZ pdp stack: (\d+) s unit (\d+) requested (\d+) flags (0x[0-9a-f]+) mtu (\d+)"
)
FAMILIES = re.compile(r"COACHZ pdp: pdp_ip(\d+) active families (\d+)")
BUDGET = re.compile(
    r"COACHZ qmi: (\d+) s: (\d+) trace lines, (\d+) held back by the budget"
)


def integer_fields(match, names, offset=0):
    return {name: int(value, 16 if value.startswith("0x") else 10)
            for name, value in zip(names, match.groups()[offset:])}


def parse(text):
    native, internal, lifecycle, call_end = [], [], [], []
    counters, stack, families, budgets = [], [], [], []
    for number, line in enumerate(text.splitlines(), 1):
        stamp = re.search(r"\[\s*(\d+\.\d+)\]:", line)
        position = dict(line=number, seconds=float(stamp[1]) if stamp else None)
        match = QMI.search(line)
        if match:
            event = dict(position, direction=match[1])
            event.update(integer_fields(match, (
                "channel", "service", "client", "type", "message", "transaction",
                "bytes", "error", "kind_count", "transport_ms",
            ), offset=1))
            if event["service"] == 1:
                if event["channel"] in (1, 2) and event["direction"] in ("host", "to-host"):
                    native.append(event)
                else:
                    internal.append(event)
        match = LIFECYCLE.search(line)
        if match:
            lifecycle.append(dict(position, **integer_fields(match, (
                "type", "message", "transaction", "mux", "family", "active",
                "error", "address_fields",
            ))))
        match = CALL_END.search(line)
        if match:
            values = [int(value) for value in match.groups()[1:]]
            client, channel, mux, family, present, reason, verbose, kind, code = values
            call_end.append(dict(
                position, event=match[1], client=client, channel=channel, mux=mux,
                header_channel=channel + 1 if channel in (0, 1) else None,
                family=family, reason_present=bool(present),
                reason=reason if present else None, verbose_present=bool(verbose),
                verbose_type=kind if verbose else None,
                verbose_code=code if verbose else None,
            ))
        for pattern, names, target in (
            (COUNTERS, ("report_seconds", "unit", "families", "blocked", "queued",
                        "tx", "rx", "dropped", "busy"), counters),
            (STACK, ("report_seconds", "unit", "requested", "flags", "mtu"), stack),
            (FAMILIES, ("unit", "families"), families),
            (BUDGET, ("report_seconds", "trace_lines", "held_back"), budgets),
        ):
            match = pattern.search(line)
            if match:
                target.append(dict(position, **integer_fields(match, names)))

    requests, responses, indications, pairs, unpaired = [], [], [], [], []
    pending = defaultdict(deque)
    for event in native:
        key = tuple(event[name] for name in ("channel", "client", "message", "transaction"))
        if event["direction"] == "host" and event["type"] == 0:
            requests.append(event)
            pending[key].append(event)
        elif event["direction"] == "to-host" and event["type"] == 2:
            responses.append(event)
            if pending[key]:
                pairs.append(dict(request=pending[key].popleft(), response=event))
            else:
                unpaired.append(event)
        elif event["direction"] == "to-host" and event["type"] == 4:
            indications.append(event)
    commands = {}
    for message, name in COMMANDS.items():
        sent = [event for event in requests if event["message"] == message]
        received = [event for event in responses if event["message"] == message]
        matched = [pair for pair in pairs if pair["response"]["message"] == message]
        commands[name] = dict(
            message_id=hex(message), request_count=len(sent), response_count=len(received),
            paired_response_count=len(matched),
            response_error_counts=dict(Counter(str(event["error"]) for event in received)),
        )
    latest = {str(event["unit"]): event for event in counters}
    latest_stack = {str(event["unit"]): event for event in stack}
    return dict(
        phone_operations=False, native_WDS_events=native, internal_WDS_events=internal,
        requests=requests, responses=responses, indications=indications, paired_exchanges=pairs,
        responses_without_captured_request=unpaired,
        requests_without_captured_response=[event for queue in pending.values() for event in queue],
        commands=commands, WDS_lifecycle=lifecycle, call_end_reason_events=call_end,
        PDP_counters=counters, latest_PDP_counters=latest,
        PDP_stack=stack, latest_PDP_stack=latest_stack, interface_family_changes=families,
        QMI_trace_budget=budgets,
        QMI_trace_budget_held_back_observed=any(event["held_back"] for event in budgets),
        WDS_Start_request_observed=any(event["message"] == 0x20 for event in requests),
        WDS_Start_success_response_observed=any(
            event["message"] == 0x20 and event["error"] == 0 for event in responses),
        IPv4_runtime_address_field_observed=any(
            event["type"] == 2 and event["message"] == 0x2D and event["error"] == 0
            and event["address_fields"] & 1 for event in lifecycle),
        IPv6_runtime_address_field_observed=any(
            event["type"] == 2 and event["message"] == 0x2D and event["error"] == 0
            and event["address_fields"] & 2 for event in lifecycle),
        PDP_units_with_bidirectional_packets=[
            int(unit) for unit, event in latest.items() if event["tx"] > 0 and event["rx"] > 0],
        evidence_scope=(
            "Existing control headers, address-field presence and per-interface counters. "
            "Lifecycle rows lack client IDs and remain separate from header pairs. "
            "Call-end rows use the bridge's zero-based channel index. "
            "Missing header pairs do not establish missing requests. "
            "Internet qualification also requires the native helper's actual TLS/HTTP result."
        ),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("console", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raw = args.console.read_bytes()
    report = parse(raw.decode("utf8", "replace"))
    report.update(source=str(args.console), source_bytes=len(raw),
                  source_sha256=hashlib.sha256(raw).hexdigest())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in (
        "commands", "call_end_reason_events", "latest_PDP_counters",
        "QMI_trace_budget_held_back_observed",
    )}, indent=2))


if __name__ == "__main__":
    main()
