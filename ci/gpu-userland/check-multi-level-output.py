"""Bind GPU051 source and run current allocator and tiled-planning checks on macOS."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--unixnu", required=True, type=Path)
p.add_argument("--logs", required=True, type=Path)
a = p.parse_args()
r = a.unixnu.resolve()
t = r / ".cache/gpu-reference/metal2vulkan"
expected = json.loads((Path(__file__).parent / "fixtures/multi-level-output-sources.json").read_text())
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
git = lambda *args, **kwargs: subprocess.check_output(["git", "-C", str(t), *args], **kwargs)
assert git("rev-parse", "HEAD").decode().strip() == expected["original_pin"]
builder = r / "scripts/build-adreno-metal-bridge.py"
assert sha(builder) == expected["builder_sha256"]
patches = re.findall(r"'([0-9]{4}[^']+\.patch)'", builder.read_text())
assert patches == list(expected["patch_hashes"]) and len(patches) == 38
for name, digest in expected["patch_hashes"].items():
    assert sha(r / "drivers/AdrenoMetal/bridge/patches" / name) == digest, name
for name, digest in expected["source_hashes"].items():
    assert sha(t / name) == digest, name
with tempfile.TemporaryDirectory(prefix="multilevel-source-") as temporary:
    env = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / "index"))
    git("read-tree", "HEAD", env=env)
    git("add", "-A", env=env)
    assert git("write-tree", env=env).decode().strip() == expected["patched_tree"]
out = a.logs / "multi-level-output"
out.mkdir(exist_ok=False)
env = dict(os.environ, CARGO_BUILD_JOBS="3",
           CARGO_TARGET_DIR=str(r / "build/multi-level-output-host"), RUSTFLAGS="-Dwarnings")
command = ["cargo", "+1.98.0", "test", "--release", "--manifest-path", str(t / "Cargo.toml"),
           "-p", "metal2vulkan-runtime", "--all-features", "--lib", "tests::heap_tiled::",
           "--", "--nocapture"]
with (out / "planner.log").open("w") as log:
    result = subprocess.run(command, cwd=r, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=1500)
text = (out / "planner.log").read_text()
assert result.returncode == 0 and "tiled_move_plans_keep_unsupported_levels_on_the_generic_path ... ok" in text
allocator_command = command[:]
allocator_command[allocator_command.index("tests::heap_tiled::")] = "memory::latency_tests::"
with (out / "allocator.log").open("w") as log:
    allocator_result = subprocess.run(allocator_command, cwd=r, env=env,
                                      stdout=log, stderr=subprocess.STDOUT, timeout=1500)
allocator_text = (out / "allocator.log").read_text()
assert allocator_result.returncode == 0 and "8 passed; 0 failed" in allocator_text
for name in (
    "foreground_backing_uses_request_size_and_preserves_room_refill",
    "foreground_backing_preserves_existing_pool_and_other_driver_growth",
    "foreground_backing_keeps_alignment_dedicated_sizes_and_allocation_failure",
):
    assert name + " ... ok" in allocator_text
report = dict(passed=True, runtime_tree=expected["patched_tree"],
              source_manifest_sha256=sha(Path(__file__).parent / "fixtures/multi-level-output-sources.json"),
              exact_runtime_source_verified=True, current_Mac_planner_tests_passed=True,
              current_Mac_allocator_tests_passed=True, current_Mac_allocator_cases=8,
              allocator_callbacks_are_model=True, current_iOS_policy_hardware_measured=False,
              current_Mac_Vulkan_execution=False, target_A840_performance_verified=False,
              Linux_Vulkan_execution_reference=expected["linux_execution_reference"],
              old_unrelated_tests_reused=expected["unchanged_test_reference"], phone_operations=False)
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
