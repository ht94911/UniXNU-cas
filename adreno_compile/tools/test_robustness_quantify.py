#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests (>=20 cases) for robustness_quantify.py (pure Python stdlib)."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import json
from pathlib import Path
import stat
import tempfile
import unittest

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import robustness_quantify as rq


def make_stats(
    total_instrs: int = 100,
    cat0: int = 10,
    cat1: int = 10,
    cat234: int = 60,
    cat5: int = 5,
    cat6: int = 10,
    cat7: int = 5,
    nops: int = 4,
    full_regs: int = 8,
    half_regs: int = 4,
    const_dwords: int = 16,
    waves: int = 16,
    policy_flags: int = 0,
) -> dict:
    return {
        "total_instrs": total_instrs,
        "cat0_ctrl": cat0,
        "cat1_mov": cat1,
        "cat234_alu": cat234,
        "cat5_tex": cat5,
        "cat6_mem": cat6,
        "cat7_sync": cat7,
        "nop_count": nops,
        "full_regs": full_regs,
        "half_regs": half_regs,
        "const_dwords": const_dwords,
        "est_waves_per_sp": waves,
        "robust_policy_flags": policy_flags,
    }


def make_shader(
    name: str,
    category: str,
    stage: str = "compute",
    on_instrs: int = 100,
    off_instrs: int = 80,
    on_full_regs: int = 8,
    off_full_regs: int = 8,
    on_half_regs: int = 4,
    off_half_regs: int = 4,
    on_waves: int = 16,
    off_waves: int = 16,
    is_xr10: bool = False,
    on_flags: int = 0,
    off_flags: int | None = None,
) -> dict:
    if is_xr10:
        on_flags |= rq.POLICY_FLAG_PRESERVE_XR_OOB
    if off_flags is None:
        off_flags = rq.POLICY_FLAG_ELIDE_BUF_BOUNDS | rq.POLICY_FLAG_ZERO_PAGE_GUARD | (
            rq.POLICY_FLAG_PRESERVE_XR_OOB if (is_xr10 or (on_flags & rq.POLICY_FLAG_PRESERVE_XR_OOB)) else 0
        )
    return {
        "name": name,
        "category": category,
        "source": "unit_fixture",
        "stage": stage,
        "is_xr10": is_xr10,
        "robust_on": make_stats(
            total_instrs=on_instrs,
            cat234=on_instrs - 40,
            full_regs=on_full_regs,
            half_regs=on_half_regs,
            waves=on_waves,
            policy_flags=on_flags,
        ),
        "robust_off": make_stats(
            total_instrs=off_instrs,
            cat234=off_instrs - 40,
            full_regs=off_full_regs,
            half_regs=off_half_regs,
            waves=off_waves,
            policy_flags=off_flags,
        ),
    }


def make_payload(shaders: list[dict]) -> dict:
    return {
        "schema_version": 1,
        "mesa_commit": "85fda3b36405bcaea8bffc64aa4f2651dc90770d",
        "target_chip_id": "0x44050a31",
        "zero_page_guard_bytes": 65536,
        "device_verified": False,
        "hardware_acceptance": False,
        "shaders": shaders,
    }


class TestRobustnessQuantify(unittest.TestCase):
    def test_01_percentile_empty(self) -> None:
        self.assertEqual(rq.percentile([], 95.0), 0.0)

    def test_02_percentile_single(self) -> None:
        self.assertEqual(rq.percentile([12.5], 95.0), 12.5)

    def test_03_percentile_interpolated_p95(self) -> None:
        vals = [0.0, 10.0, 20.0, 30.0, 40.0]
        self.assertAlmostEqual(rq.percentile(vals, 50.0), 20.0, places=6)
        self.assertAlmostEqual(rq.percentile(vals, 95.0), 38.0, places=6)

    def test_04_basic_aggregation_and_flags(self) -> None:
        payload = make_payload([
            make_shader("s1", "compute", "compute", 100, 75),
            make_shader("s2", "compositor", "vertex", 200, 180),
            make_shader("s3", "liquid-glass", "fragment", 100, 100),
            make_shader("s4", "camera", "compute", 80, 80),
            make_shader("s5", "xr10", "fragment", 120, 90, is_xr10=True),
        ])
        rep = rq.quantify(payload)
        self.assertEqual(rep["status"], "PASS")
        self.assertFalse(rep["device_verified"])
        self.assertFalse(rep["hardware_acceptance"])
        self.assertEqual(rep["overall"]["shader_count"], 5)
        self.assertEqual(rep["overall"]["total_instrs_on"], 600)
        self.assertEqual(rep["overall"]["total_instrs_off"], 525)
        self.assertEqual(rep["overall"]["total_saved_instrs"], 75)
        self.assertAlmostEqual(rep["overall"]["total_reduction_pct"], 12.5, places=4)
        self.assertAlmostEqual(rep["overall"]["median_reduction_pct"], 10.0, places=4)
        self.assertAlmostEqual(rep["overall"]["max_reduction_pct"], 25.0, places=4)
        self.assertEqual(rep["overall"]["xr_preservation_rate_pct"], 100.0)

    def test_05_category_breakdown_contains_all_five_buckets(self) -> None:
        payload = make_payload([make_shader("only_compute", "compute", "compute", 100, 90)])
        rep = rq.quantify(payload)
        for cat in rq.REQUIRED_CATEGORIES:
            self.assertIn(cat, rep["by_category"])
        self.assertEqual(rep["by_category"]["compute"]["shader_count"], 1)
        self.assertEqual(rep["by_category"]["xr10"]["shader_count"], 0)

    def test_06_rejects_instruction_regression(self) -> None:
        payload = make_payload([make_shader("reg_instr", "compute", "compute", 100, 105)])
        with self.assertRaises(rq.QuantifyError) as ctx:
            rq.quantify(payload)
        self.assertIn("ZERO_INSTR_REGRESSION", str(ctx.exception))

    def test_07_rejects_full_register_regression(self) -> None:
        payload = make_payload([
            make_shader("reg_full", "compute", "compute", 100, 90, on_full_regs=6, off_full_regs=7)
        ])
        with self.assertRaises(rq.QuantifyError) as ctx:
            rq.quantify(payload)
        self.assertIn("ZERO_REG_REGRESSION", str(ctx.exception))

    def test_08_rejects_half_register_regression(self) -> None:
        payload = make_payload([
            make_shader("reg_half", "compute", "compute", 100, 90, on_half_regs=2, off_half_regs=3)
        ])
        with self.assertRaises(rq.QuantifyError) as ctx:
            rq.quantify(payload)
        self.assertIn("ZERO_REG_REGRESSION", str(ctx.exception))

    def test_09_rejects_wave_occupancy_regression(self) -> None:
        payload = make_payload([
            make_shader("reg_wave", "compute", "compute", 100, 90, on_waves=16, off_waves=14)
        ])
        with self.assertRaises(rq.QuantifyError) as ctx:
            rq.quantify(payload)
        self.assertIn("ZERO_WAVE_REGRESSION", str(ctx.exception))

    def test_10_rejects_xr10_oob_flag_dropped(self) -> None:
        payload = make_payload([
            make_shader(
                "xr10_dropped", "xr10", "fragment", 100, 80, is_xr10=True,
                on_flags=rq.POLICY_FLAG_PRESERVE_XR_OOB,
                off_flags=rq.POLICY_FLAG_ELIDE_BUF_BOUNDS,
            )
        ])
        with self.assertRaises(rq.QuantifyError) as ctx:
            rq.quantify(payload)
        self.assertIn("XR10_SAFETY_PRESERVED", str(ctx.exception))

    def test_11_rejects_non_xr10_diff_xr_preserved_false(self) -> None:
        sh = make_shader("bad_diff_xr", "compositor", "fragment", 100, 90)
        sh["diff"] = {"xr_preserved": False}
        with self.assertRaises(rq.QuantifyError) as ctx:
            rq.quantify(make_payload([sh]))
        self.assertIn("XR10_SAFETY_PRESERVED", str(ctx.exception))

    def test_12_no_fail_mode_reports_violations_and_fail_status(self) -> None:
        payload = make_payload([make_shader("reg_soft", "compute", "compute", 100, 110)])
        rep = rq.quantify(payload, enforce_gates=False)
        self.assertEqual(rep["status"], "FAIL")
        self.assertEqual(len(rep["violations"]), 1)
        self.assertEqual(rep["violations"][0]["rule"], "ZERO_INSTR_REGRESSION")

    def test_13_rejects_non_object_root(self) -> None:
        with self.assertRaises(rq.QuantifyError):
            rq.quantify([1, 2, 3])

    def test_14_rejects_bad_schema_version(self) -> None:
        payload = make_payload([make_shader("s", "compute")])
        payload["schema_version"] = 2
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(payload)

    def test_15_rejects_empty_shader_list(self) -> None:
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(make_payload([]))

    def test_16_rejects_duplicate_shader_names(self) -> None:
        payload = make_payload([
            make_shader("dup", "compute"),
            make_shader("dup", "camera"),
        ])
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(payload)

    def test_17_rejects_invalid_stage(self) -> None:
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(make_payload([make_shader("bad_stage", "compute", stage="geometry")]))

    def test_18_rejects_missing_stats_field(self) -> None:
        sh = make_shader("missing_field", "compute")
        del sh["robust_on"]["cat6_mem"]
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(make_payload([sh]))

    def test_19_rejects_boolean_or_negative_stats_values(self) -> None:
        sh_bool = make_shader("bool_val", "compute")
        sh_bool["robust_on"]["full_regs"] = True
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(make_payload([sh_bool]))

        sh_neg = make_shader("neg_val", "compute")
        sh_neg["robust_off"]["cat0_ctrl"] = -1
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(make_payload([sh_neg]))

    def test_20_rejects_zero_total_instrs_or_waves(self) -> None:
        sh_z = make_shader("zero_instr", "compute", on_instrs=100, off_instrs=100)
        sh_z["robust_on"]["total_instrs"] = 0
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(make_payload([sh_z]))

        sh_w = make_shader("zero_wave", "compute")
        sh_w["robust_off"]["est_waves_per_sp"] = 0
        with self.assertRaises(rq.QuantifyError):
            rq.quantify(make_payload([sh_w]))

    def test_21_deterministic_output_ordering_and_markdown(self) -> None:
        p1 = make_payload([
            make_shader("b_shader", "compute", "compute", 100, 80),
            make_shader("a_shader", "camera", "compute", 100, 90),
        ])
        p2 = make_payload([
            make_shader("a_shader", "camera", "compute", 100, 90),
            make_shader("b_shader", "compute", "compute", 100, 80),
        ])
        r1 = rq.quantify(p1)
        r2 = rq.quantify(p2)
        self.assertEqual(json.dumps(r1, sort_keys=True), json.dumps(r2, sort_keys=True))
        md = rq.render_markdown(r1)
        self.assertIn("**device_verified**: `false`", md)
        self.assertIn("**hardware_acceptance**: `false`", md)
        self.assertIn("`a_shader`", md)
        self.assertIn("`b_shader`", md)

    def test_22_cli_end_to_end_input_json_and_bad_json(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            inp = root / "ab.json"
            out_j = root / "report.json"
            out_m = root / "report.md"
            inp.write_text(json.dumps(make_payload([
                make_shader("cli_cs", "compute", "compute", 100, 70),
                make_shader("cli_xr", "xr10", "fragment", 120, 96, is_xr10=True),
            ])), encoding="utf-8")

            rc = rq.main(["--input", str(inp), "--out-json", str(out_j), "--out-md", str(out_m)])
            self.assertEqual(rc, 0)
            rep = json.loads(out_j.read_text(encoding="utf-8"))
            self.assertEqual(rep["status"], "PASS")
            self.assertFalse(rep["device_verified"])
            self.assertFalse(rep["hardware_acceptance"])
            self.assertTrue(out_m.is_file())

            bad = root / "broken.json"
            bad.write_text("{not valid json", encoding="utf-8")
            rc_bad = rq.main(["--input", str(bad), "--out-json", str(out_j), "--out-md", str(out_m)])
            self.assertEqual(rc_bad, 1)

    def test_23_cli_end_to_end_runner_mode(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runner = root / "fake_runner.py"
            payload = make_payload([make_shader("run_s1", "compositor", "vertex", 80, 64)])
            runner.write_text(
                "#!/usr/bin/env python3\n"
                "import json, sys\n"
                "idx = sys.argv.index('--dump-json') + 1\n"
                f"open(sys.argv[idx], 'w').write({json.dumps(json.dumps(payload))})\n",
                encoding="utf-8",
            )
            runner.chmod(runner.stat().st_mode | stat.S_IXUSR)
            dump_p = root / "dumped.json"
            out_j = root / "r.json"
            out_m = root / "r.md"
            rc = rq.main([
                "--runner", str(runner),
                "--dump-ab-json", str(dump_p),
                "--out-json", str(out_j),
                "--out-md", str(out_m),
            ])
            self.assertEqual(rc, 0)
            self.assertTrue(dump_p.is_file())
            self.assertEqual(json.loads(out_j.read_text(encoding="utf-8"))["overall"]["total_saved_instrs"], 16)

    def test_24_wave_improvement_and_zero_regression_accepted(self) -> None:
        payload = make_payload([
            make_shader("wave_up", "compute", "compute", 140, 100,
                        on_full_regs=10, off_full_regs=8, on_waves=12, off_waves=16)
        ])
        rep = rq.quantify(payload)
        self.assertEqual(rep["status"], "PASS")
        self.assertEqual(rep["overall"]["max_delta_waves_per_sp"], 4)
        self.assertEqual(rep["overall"]["max_delta_full_regs"], -2)


if __name__ == "__main__":
    unittest.main()
