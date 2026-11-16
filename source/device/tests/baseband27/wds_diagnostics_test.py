#!/usr/bin/env python3
"""Exercise diagnostic ambiguity using the existing bridge's console formats."""
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/baseband27"))
from wds_diagnostics import parse


def qmi(direction="host", channel=1, client=4, kind=0, message=0x20,
        transaction=10, error=0):
    return (
        f"[  130.000001]: COACHZ qmi: {direction} ch {channel} svc 0x1 cid {client} "
        f"type {kind} msg {hex(message)} txn {transaction} len 14 err {hex(error)} n 1 t 130000"
    )


class WDSDiagnosticsTest(unittest.TestCase):
    def test_native_pairing_separates_internal_clients_and_channels(self):
        report = parse("\n".join((
            qmi(), qmi(channel=2),
            qmi(direction="int-tx", channel=0), qmi(direction="int-rx", channel=0, kind=2),
            qmi(direction="to-host", channel=2, kind=2, error=14),
            qmi(direction="to-host", kind=2),
        )))
        start = report["commands"]["Start Network"]
        self.assertEqual((start["request_count"], start["response_count"], start["paired_response_count"]),
                         (2, 2, 2))
        self.assertEqual(start["response_error_counts"], {"14": 1, "0": 1})
        self.assertEqual(len(report["internal_WDS_events"]), 2)
        for pair in report["paired_exchanges"]:
            self.assertEqual(pair["request"]["channel"], pair["response"]["channel"])

    def test_unpaired_response_is_retained_with_no_invented_request(self):
        report = parse(qmi(direction="to-host", kind=2))
        self.assertEqual(len(report["responses_without_captured_request"]), 1)
        self.assertFalse(report["WDS_Start_request_observed"])
        self.assertTrue(report["WDS_Start_success_response_observed"])
        self.assertEqual(report["paired_exchanges"], [])
        self.assertNotIn("cellular_HTTPS_request_passed", report)

    def test_call_end_presence_and_negative_verbose_reason(self):
        report = parse("\n".join((
            "COACHZ pdp: start failed cid 4 channel 0 mux 1 family 4 "
            "reason-present 1 reason 17 verbose-present 1 type 2 code -32768",
            "COACHZ pdp: disconnected cid 4 channel 0 mux 1 family 6 "
            "reason-present 0 reason 0 verbose-present 0 type 0 code 0",
        )))
        failed, disconnected = report["call_end_reason_events"]
        self.assertEqual((failed["reason"], failed["verbose_type"], failed["verbose_code"]),
                         (17, 2, -32768))
        self.assertEqual(failed["header_channel"], 1)
        self.assertEqual(disconnected["family"], 6)
        self.assertIsNone(disconnected["reason"])
        self.assertIsNone(disconnected["verbose_type"])
        self.assertIsNone(disconnected["verbose_code"])

    def test_runtime_fields_require_success_and_remain_family_specific(self):
        report = parse("\n".join((
            "COACHZ pdp: WDS type 2 message 0x2d txn 13 mux 1 family 4 "
            "active 0 error 14 address-fields 1",
            "COACHZ pdp: WDS type 2 message 0x2d txn 14 mux 1 family 6 "
            "active 1 error 0 address-fields 2",
            "COACHZ pdp: pdp_ip0 active families 3",
            "COACHZ pdp: pdp_ip0 active families 2",
        )))
        self.assertFalse(report["IPv4_runtime_address_field_observed"])
        self.assertTrue(report["IPv6_runtime_address_field_observed"])
        self.assertEqual(report["interface_family_changes"][-1]["families"], 2)
        self.assertEqual(report["native_WDS_events"], [])

    def test_packet_directions_belong_to_the_same_interface(self):
        report = parse("\n".join((
            "COACHZ pdp: 140 s unit 0 families 1 blocked 0 queued 0 tx 4 rx 0 drop 0 busy 0",
            "COACHZ pdp: 140 s unit 1 families 1 blocked 0 queued 0 tx 0 rx 7 drop 0 busy 0",
            "COACHZ pdp: 150 s unit 0 families 1 blocked 0 queued 0 tx 8 rx 9 drop 0 busy 0",
            "COACHZ pdp stack: 150 s unit 0 requested 8 flags 0x41 mtu 1492",
        )))
        self.assertEqual(report["PDP_units_with_bidirectional_packets"], [0])
        self.assertEqual(report["latest_PDP_counters"]["0"]["report_seconds"], 150)
        self.assertEqual(report["latest_PDP_stack"]["0"]["flags"], 0x41)
        self.assertFalse(report["WDS_Start_success_response_observed"])

    def test_budget_and_empty_capture_do_not_imply_data_success(self):
        report = parse("COACHZ qmi: 220 s: 6000 trace lines, 4 held back by the budget")
        self.assertTrue(report["QMI_trace_budget_held_back_observed"])
        empty = parse("")
        self.assertFalse(empty["WDS_Start_request_observed"])
        self.assertFalse(empty["IPv4_runtime_address_field_observed"])
        self.assertFalse(empty["IPv6_runtime_address_field_observed"])
        self.assertEqual(empty["PDP_units_with_bidirectional_packets"], [])
        self.assertFalse(empty["phone_operations"])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WDSDiagnosticsTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(json.dumps(dict(passed=result.wasSuccessful(), cases=result.testsRun,
                          input="Synthetic console fixtures; hardware acceptance is separate",
                          phone_operations=False)))
    raise SystemExit(0 if result.wasSuccessful() else 1)
