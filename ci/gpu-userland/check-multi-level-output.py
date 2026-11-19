"""Bind the camera imageblock backport and retained GPU051 allocator/tiled source."""
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
fixtures = Path(__file__).parent / "fixtures/retained-runtime096"
report = json.loads((fixtures / "report.json").read_text())
assert report["passed"] and report["runtime_tree"] == expected["patched_tree"]
assert report["source_manifest_sha256"] == sha(Path(__file__).parent / "fixtures/multi-level-output-sources.json")
for name, required in (("planner.log", "tiled_move_plans_keep_unsupported_levels_on_the_generic_path ... ok"),
                       ("allocator.log", "8 passed; 0 failed"),
                       ("imageblock.log", "a_cell_holds_the_widest_access_through_its_pointer ... ok")):
    content = (fixtures / name).read_text()
    assert required in content
    (out / name).write_text(content)
report.update(current_run_host_tests_executed=False, current_Mac_imageblock_interpreter_passed=False,
              current_Mac_planner_tests_passed=False, current_Mac_allocator_tests_passed=False,
              prior_Mac_runtime_checks_passed=True, original_runtime_check_run=44478010517,
              original_runtime_check_head="71dd1b4c9f7b70f2f597b01c113b87ead042e953",
              retained_exact_runtime_sources=True)
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
