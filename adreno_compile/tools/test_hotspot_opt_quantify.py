#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests for Wave 10 / Lane 2 hotspot_opt_quantify.py (pure stdlib)."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import json
from pathlib import Path
import tempfile
import unittest

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import hotspot_opt_quantify as hq  # noqa: E402


def make_stats(
    *,
    total_instrs: int = 100,
    cat0_ctrl: int = 4,
    cat1_mov: int = 12,
    cat234_alu: int = 60,
    cat5_tex: int = 4,
    cat6_mem: int = 12,
    cat7_sync: int = 4,
    nop_count: int = 4,
    full_regs: int = 8,
    half_regs: int = 2,
    reg_spills: int = 0,
    reg_fills: int = 0,
    const_dwords: int = 16,
    est_waves_per_sp: int = 16,
    robust_policy_flags: int = hq.POLICY_FLAG_PRESERVE_XR_OOB,
    merged_regs: bool = True,
) -> dict:
    return {
        "total_instrs": total_instrs,
        "cat0_ctrl": cat0_ctrl,
        "cat1_mov": cat1_mov,
        "cat234_alu": cat234_alu,
        "cat5_tex": cat5_tex,
        "cat6_mem": cat6_mem,
        "cat7_sync": cat7_sync,
        "nop_count": nop_count,
        "full_regs": full_regs,
        "half_regs": half_regs,
        "reg_spills": reg_spills,
        "reg_fills": reg_fills,
        "const_dwords": const_dwords,
        "est_waves_per_sp": est_waves_per_sp,
        "robust_policy_flags": robust_policy_flags,
        "merged_regs": merged_regs,
    }


def make_opt_report(**overrides) -> dict:
    base = {
        "fp16_roundtrip_elided": 0,
        "fp16_alu_narrowed": 0,
        "fp16_rtz_aligned": 0,
        "fp16_xr10_guarded": 0,
        "push_const_loads_merged": 0,
        "push_const_offsets_folded": 0,
        "uniform_widens_elided": 0,
        "sink_moves_applied": 0,
        "peak_live_vec4_before": 8,
        "peak_live_vec4_after": 8,
        "xr10_detected": False,
        "xr10_safe": True,
        "push_layout_preserved": True,
    }
    base.update(overrides)
    return base


def make_shader(
    name: str,
    *,
    category: str = "liquid-glass",
    opt_group: str = "fp16",
    stage: str = "fragment",
    is_xr10: bool = False,
    expects_improvement: bool = False,
    opt_off: dict | None = None,
    opt_on: dict | None = None,
    opt_report: dict | None = None,
) -> dict:
    return {
        "name": name,
        "category": category,
        "opt_group": opt_group,
        "source": "unit_fixture",
        "stage": stage,
        "is_xr10": is_xr10,
        "expects_improvement": expects_improvement,
        "opt_off": opt_off if opt_off is not None else make_stats(),
        "opt_on": opt_on if opt_on is not None else make_stats(),
        "opt_report": opt_report if opt_report is not None else make_opt_report(),
    }


def make_payload(shaders: list[dict]) -> dict:
    return {
        "schema_version": 1,
        "mesa_commit": "85fda3b36405bcaea8bffc64aa4f2651dc90770d",
        "target_chip_id": "0x44050a31",
        "device_verified": False,
        "hardware_acceptance": False,
        "shaders": shaders,
    }


class HotspotOptQuantifyTests(unittest.TestCase):
    def test_01_percentile_empty_and_singleton(self) -> None:
        self.assertEqual(hq.percentile([], 95.0), 0.0)
        self.assertEqual(hq.percentile([42.5], 50.0), 42.5)
        self.assertEqual(hq.percentile([42.5], 95.0), 42.5)

    def test_02_percentile_interpolation(self) -> None:
        vals = [0.0, 10.0, 20.0, 30.0, 40.0]
        self.assertAlmostEqual(hq.percentile(vals, 50.0), 20.0)
        self.assertAlmostEqual(hq.percentile(vals, 95.0), 38.0)

    def test_03_effective_reg_vec4_merged_and_non_merged(self) -> None:
        self.assertEqual(hq.effective_reg_vec4({"full_regs": 4, "half_regs": 6, "merged_regs": True}), 4)
        self.assertEqual(hq.effective_reg_vec4({"full_regs": 2, "half_regs": 8, "merged_regs": True}), 4)
        self.assertEqual(hq.effective_reg_vec4({"full_regs": 2, "half_regs": 8, "merged_regs": False}), 6)

    def test_04_valid_multi_group_aggregation(self) -> None:
        payload = make_payload([
            make_shader(
                "fp16_blur",
                category="liquid-glass",
                opt_group="fp16",
                expects_improvement=True,
                opt_off=make_stats(total_instrs=80, cat1_mov=20, full_regs=6, half_regs=2),
                opt_on=make_stats(total_instrs=56, cat1_mov=4, full_regs=3, half_regs=4),
                opt_report=make_opt_report(fp16_roundtrip_elided=8, fp16_alu_narrowed=6),
            ),
            make_shader(
                "push_dup",
                category="compositor",
                opt_group="uniform_push",
                expects_improvement=True,
                opt_off=make_stats(total_instrs=40, cat6_mem=8, const_dwords=32),
                opt_on=make_stats(total_instrs=30, cat6_mem=4, const_dwords=16),
                opt_report=make_opt_report(push_const_loads_merged=4, push_const_offsets_folded=2),
            ),
            make_shader(
                "pressure_conv",
                category="liquid-glass",
                opt_group="reg_pressure",
                expects_improvement=True,
                opt_off=make_stats(total_instrs=120, full_regs=22, est_waves_per_sp=10),
                opt_on=make_stats(total_instrs=120, full_regs=12, est_waves_per_sp=16),
                opt_report=make_opt_report(sink_moves_applied=14, peak_live_vec4_before=22, peak_live_vec4_after=10),
            ),
            make_shader(
                "xr10_guard",
                category="xr10",
                opt_group="xr10_guard",
                is_xr10=True,
                opt_off=make_stats(total_instrs=40, full_regs=6),
                opt_on=make_stats(total_instrs=40, full_regs=6),
                opt_report=make_opt_report(xr10_detected=True, xr10_safe=True, fp16_xr10_guarded=3),
            ),
        ])
        rep = hq.quantify(payload)
        self.assertEqual(rep["status"], "PASS")
        self.assertFalse(rep["device_verified"])
        self.assertFalse(rep["hardware_acceptance"])
        self.assertEqual(rep["overall"]["shader_count"], 4)
        self.assertEqual(rep["overall"]["improved_count"], 3)
        self.assertEqual(rep["overall"]["regressed_count"], 0)
        self.assertEqual(rep["overall"]["total_saved_instrs"], 34)
        self.assertEqual(rep["overall"]["total_delta_cat1_mov"], -16)
        self.assertEqual(rep["overall"]["total_delta_full_regs"], -13)
        self.assertEqual(rep["overall"]["max_delta_waves"], 6)
        self.assertEqual(rep["overall"]["xr_preservation_rate_pct"], 100.0)
        self.assertIn("fp16", rep["by_opt_group"])
        self.assertIn("uniform_push", rep["by_opt_group"])
        self.assertIn("reg_pressure", rep["by_opt_group"])
        self.assertIn("xr10_guard", rep["by_opt_group"])
        self.assertIn("liquid-glass", rep["by_category"])
        self.assertIn("compositor", rep["by_category"])
        self.assertIn("xr10", rep["by_category"])

    def test_05_fp16_full_to_half_reg_trade_allowed_when_effective_drops(self) -> None:
        payload = make_payload([
            make_shader(
                "fp16_trade",
                expects_improvement=True,
                opt_off=make_stats(total_instrs=50, full_regs=6, half_regs=0),
                opt_on=make_stats(total_instrs=40, full_regs=2, half_regs=6),
            )
        ])
        rep = hq.quantify(payload)
        self.assertEqual(rep["status"], "PASS")
        self.assertEqual(rep["shaders"][0]["delta_effective_vec4"], -3)

    def test_06_rejects_instruction_regression(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_instr",
                opt_off=make_stats(total_instrs=40),
                opt_on=make_stats(total_instrs=41),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "instruction regression"):
            hq.quantify(payload)

    def test_07_rejects_full_register_regression(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_reg",
                opt_off=make_stats(total_instrs=50, full_regs=6),
                opt_on=make_stats(total_instrs=45, full_regs=7),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "register pressure regression"):
            hq.quantify(payload)

    def test_08_rejects_effective_register_regression_from_half_bloat(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_half_bloat",
                opt_off=make_stats(total_instrs=50, full_regs=4, half_regs=4),
                opt_on=make_stats(total_instrs=45, full_regs=4, half_regs=12),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "register pressure regression"):
            hq.quantify(payload)

    def test_09_rejects_spill_regression(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_spill",
                opt_off=make_stats(total_instrs=60, reg_spills=0),
                opt_on=make_stats(total_instrs=55, reg_spills=2),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "register spill regression"):
            hq.quantify(payload)

    def test_10_rejects_wave_occupancy_regression(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_wave",
                opt_off=make_stats(total_instrs=60, est_waves_per_sp=16),
                opt_on=make_stats(total_instrs=55, est_waves_per_sp=14),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "wave occupancy regression"):
            hq.quantify(payload)

    def test_11_rejects_xr10_policy_flag_drop(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_xr_flag",
                is_xr10=True,
                opt_off=make_stats(total_instrs=50, robust_policy_flags=hq.POLICY_FLAG_PRESERVE_XR_OOB),
                opt_on=make_stats(total_instrs=40, robust_policy_flags=0),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "XR10 safety boundary violated"):
            hq.quantify(payload)

    def test_12_rejects_xr10_unsafe_report(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_xr_report",
                is_xr10=True,
                opt_off=make_stats(total_instrs=50),
                opt_on=make_stats(total_instrs=45),
                opt_report=make_opt_report(xr10_detected=True, xr10_safe=False),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "XR10 safety boundary violated"):
            hq.quantify(payload)

    def test_13_rejects_xr10_alu_narrowing_violation(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_xr_narrow",
                is_xr10=True,
                opt_off=make_stats(total_instrs=50),
                opt_on=make_stats(total_instrs=40),
                opt_report=make_opt_report(xr10_detected=True, xr10_safe=True, fp16_alu_narrowed=2),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "XR10 safety boundary violated"):
            hq.quantify(payload)

    def test_14_rejects_push_layout_violation(self) -> None:
        payload = make_payload([
            make_shader(
                "bad_push_layout",
                opt_off=make_stats(total_instrs=50),
                opt_on=make_stats(total_instrs=40),
                opt_report=make_opt_report(push_layout_preserved=False),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "push-constant layout contract violated"):
            hq.quantify(payload)

    def test_15_rejects_expected_improvement_with_zero_gain(self) -> None:
        payload = make_payload([
            make_shader(
                "no_gain_hotspot",
                expects_improvement=True,
                opt_off=make_stats(total_instrs=50),
                opt_on=make_stats(total_instrs=50),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "zero measurable improvement"):
            hq.quantify(payload)

    def test_16_rejects_zero_overall_improvement(self) -> None:
        payload = make_payload([
            make_shader(
                "flat_shader",
                expects_improvement=False,
                opt_off=make_stats(total_instrs=50),
                opt_on=make_stats(total_instrs=50),
            )
        ])
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "no shader demonstrated measurable"):
            hq.quantify(payload)

    def test_17_rejects_bad_schema_and_empty_shaders(self) -> None:
        with self.assertRaises(hq.HotspotQuantifyError):
            hq.quantify("not_an_object")
        with self.assertRaises(hq.HotspotQuantifyError):
            hq.quantify({"schema_version": 2, "shaders": []})
        with self.assertRaises(hq.HotspotQuantifyError):
            hq.quantify({"schema_version": 1, "shaders": []})

    def test_18_rejects_duplicate_names_and_invalid_stage(self) -> None:
        s1 = make_shader("dup", opt_off=make_stats(total_instrs=50), opt_on=make_stats(total_instrs=40))
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "duplicate shader name"):
            hq.quantify(make_payload([s1, s1]))
        bad_stage = make_shader("bad_st", stage="geometry")
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "invalid stage"):
            hq.quantify(make_payload([bad_stage]))

    def test_19_rejects_bool_negative_and_zero_instrs(self) -> None:
        bad_bool = make_stats()
        bad_bool["total_instrs"] = True
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "must be an integer"):
            hq.quantify(make_payload([make_shader("b", opt_off=bad_bool)]))

        bad_neg = make_stats()
        bad_neg["cat1_mov"] = -1
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "must be >= 0"):
            hq.quantify(make_payload([make_shader("n", opt_off=bad_neg)]))

        bad_zero = make_stats(total_instrs=0)
        with self.assertRaisesRegex(hq.HotspotQuantifyError, "total_instrs must be > 0"):
            hq.quantify(make_payload([make_shader("z", opt_off=bad_zero)]))

    def test_20_hardcodes_device_verified_false_even_if_input_lies(self) -> None:
        payload = make_payload([
            make_shader(
                "s1",
                opt_off=make_stats(total_instrs=60),
                opt_on=make_stats(total_instrs=45),
            )
        ])
        payload["device_verified"] = True
        payload["hardware_acceptance"] = True
        rep = hq.quantify(payload)
        self.assertIs(rep["device_verified"], False)
        self.assertIs(rep["hardware_acceptance"], False)

    def test_21_deterministic_json_and_markdown_rendering(self) -> None:
        s_a = make_shader("a_shader", opt_group="fp16", opt_off=make_stats(total_instrs=60), opt_on=make_stats(total_instrs=48))
        s_b = make_shader("b_shader", opt_group="uniform_push", opt_off=make_stats(total_instrs=40), opt_on=make_stats(total_instrs=36))
        r1 = hq.quantify(make_payload([s_b, s_a]))
        r2 = hq.quantify(make_payload([s_a, s_b]))
        self.assertEqual(json.dumps(r1, sort_keys=True), json.dumps(r2, sort_keys=True))
        md1 = hq.render_markdown(r1)
        md2 = hq.render_markdown(r2)
        self.assertEqual(md1, md2)
        self.assertIn("device_verified**: `false`", md1)
        self.assertIn("hardware_acceptance**: `false`", md1)

    def test_22_cli_input_pass_and_write_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            inp = root / "in.json"
            out_json = root / "out.json"
            out_md = root / "out.md"
            inp.write_text(json.dumps(make_payload([
                make_shader("s1", opt_off=make_stats(total_instrs=50), opt_on=make_stats(total_instrs=40)),
            ])), encoding="utf-8")
            rc = hq.main(["--input", str(inp), "--out-json", str(out_json), "--out-md", str(out_md)])
            self.assertEqual(rc, 0)
            self.assertTrue(out_json.is_file())
            self.assertTrue(out_md.is_file())
            loaded = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertEqual(loaded["status"], "PASS")
            self.assertIs(loaded["device_verified"], False)

    def test_23_cli_no_fail_on_gate_writes_fail_report_and_returns_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            inp = root / "bad.json"
            out_json = root / "out.json"
            out_md = root / "out.md"
            inp.write_text(json.dumps(make_payload([
                make_shader("reg", opt_off=make_stats(total_instrs=50), opt_on=make_stats(total_instrs=55)),
            ])), encoding="utf-8")
            rc = hq.main([
                "--input", str(inp), "--out-json", str(out_json), "--out-md", str(out_md),
                "--no-fail-on-gate",
            ])
            self.assertNotEqual(rc, 0)
            loaded = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertEqual(loaded["status"], "FAIL")
            self.assertTrue(len(loaded["violations"]) >= 1)

    def test_24_cli_runner_mode_and_missing_dump_arg(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out_json = root / "out.json"
            out_md = root / "out.md"
            fake_runner = root / "fake_runner.py"
            payload_json = json.dumps(make_payload([
                make_shader("r1", opt_off=make_stats(total_instrs=60), opt_on=make_stats(total_instrs=48))
            ]))
            fake_runner.write_text(
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "out = sys.argv[2]\n"
                f"open(out, 'w', encoding='utf-8').write({payload_json!r})\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            # Missing --dump-ab-json fails closed.
            rc_missing = hq.main(["--runner", str(fake_runner), "--out-json", str(out_json), "--out-md", str(out_md)])
            self.assertNotEqual(rc_missing, 0)
            # Valid --runner + --dump-ab-json succeeds.
            ab_path = root / "ab.json"
            rc_ok = hq.main([
                "--runner", str(fake_runner),
                "--dump-ab-json", str(ab_path),
                "--out-json", str(out_json),
                "--out-md", str(out_md),
            ])
            self.assertEqual(rc_ok, 0)
            self.assertTrue(ab_path.is_file())


if __name__ == "__main__":
    unittest.main()
