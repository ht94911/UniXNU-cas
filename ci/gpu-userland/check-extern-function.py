"""Execute the production external-function selector and its ownership on macOS."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--unixnu", required=True, type=Path)
p.add_argument("--logs", required=True, type=Path)
a = p.parse_args()
root = a.unixnu.resolve()
kit = root / "drivers/AdrenoMetal"
out = a.logs / "extern-function"
out.mkdir(parents=True, exist_ok=False)
source = kit / "NativeBridgeMetalFrontend.mm"
test = kit / "bridge/extern_function_host.mm"
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
subprocess.run([sys.executable, root / "scripts/build-adreno-metal-bridge.py",
                "--target", "host"], cwd=root, check=True)
archive = root / "build/adreno-metal-bridge/host/libadreno_metal_bridge.a"
flags = ["-std=c++17", "-fobjc-arc", "-O1", "-g0", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-deprecated-declarations", "-I", str(kit),
         "-I", str(kit / "bridge"), "-I", str(root / "drivers/Adreno740"),
         "-I", str(root / ".cache/mesa-work/include")]
frameworks = ["-framework", "Foundation", "-framework", "Metal",
              "-framework", "IOSurface", "-framework", "CoreFoundation"]

def run_case(label, harness):
    exe = out / label
    command = ["xcrun", "clang++", *flags, str(harness), str(archive), *frameworks,
               "-o", str(exe)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (out / (label + "-build.log")).write_text(compiled.stdout + compiled.stderr)
    assert compiled.returncode == 0, f"{label}: target test compilation failed"
    execution = subprocess.run([str(exe)], capture_output=True, text=True,
                               timeout=45, env=dict(os.environ, SHENG_DIAG="0"))
    (out / (label + "-run.log")).write_text(execution.stdout + execution.stderr)
    return execution

new = run_case("current", test)
assert new.returncode == 0 and "PASS extern lookup: 12 " in new.stdout, new.stdout + new.stderr
text = source.read_text()
pattern = r"- \(id<MTLFunction>\)newExternFunctionWithName:\(NSString \*\)name \{\n return \[self newFunctionWithName:name\];\n\}\n"
old, count = re.subn(pattern, "", text)
assert count == 1
with tempfile.TemporaryDirectory(prefix="extern-old-", dir=out) as temporary:
    work = Path(temporary)
    before = work / "frontend-before.mm"
    before.write_text(old)
    harness = work / "extern_function_before.mm"
    harness.write_text(test.read_text().replace(
        '#import "../NativeBridgeMetalFrontend.mm"', '#import "frontend-before.mm"'))
    previous = run_case("previous", harness)
    assert previous.returncode == 1 and "FAIL missing production newExternFunctionWithName:" in previous.stdout
report = dict(
    passed=True, source_sha256=sha(source), test_sha256=sha(test),
    production_ObjectiveC_selector_executed=True, checks=12,
    previous_missing_selector_reproduced=True,
    exact_kernel_bitcode_and_library_lifetime_verified=True,
    real_GPU_created=False, CoreImage_render_verified=False,
    target_iconservice_verified=False, fixture_authored_no_private_inputs=True)
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
