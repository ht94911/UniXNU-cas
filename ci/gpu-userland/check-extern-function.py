"""Execute the production external-function selector and its ownership on macOS."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
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
bridge_tests = subprocess.run([
    "cargo", "+1.98.0", "test", "--release", "--manifest-path",
    str(root / "build/adreno-metal-bridge/Cargo.toml"),
    "--target", "x86_64-apple-darwin" if platform.machine() == "x86_64" else "aarch64-apple-darwin",
    "--target-dir", str(root / "build/m2v-target"), "--lib", "texture_descriptor_tests::",
    "--", "--nocapture"], cwd=root, capture_output=True, text=True,
    env=dict(os.environ, NMB_RUNTIME_ID=hashlib.sha256(
        b"963f0e64f103d2e682cd0cc454a8fe50f52c8cbb\n6949ff1ac37893ddf59674f79085d0ae3b7d6707").hexdigest()[:32],
        NMB_FIXTURE_AIR=str(root / "build/adreno-metal-bridge/runtime_examples.air"),
        NMB_FC_AIR=str(root / "build/adreno-metal-bridge/function_constant.air"),
        NMB_SCANOUT_AIR=str(root / "build/adreno-metal-bridge/scanout_convert.air")),
    timeout=1500)
(out / "descriptor-Rust-tests.log").write_text(bridge_tests.stdout + bridge_tests.stderr)
assert bridge_tests.returncode == 0 and "1 passed; 0 failed" in bridge_tests.stdout
flags = ["-std=c++17", "-fobjc-arc", "-O1", "-g0", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-deprecated-declarations", "-I", str(kit),
         "-I", str(kit / "bridge"), "-I", str(root / "drivers/Adreno740"),
         "-I", str(root / ".cache/mesa-work/include")]
frameworks = ["-framework", "Foundation", "-framework", "Metal",
              "-framework", "IOSurface", "-framework", "CoreFoundation"]

def run_case(label, harness, trace=None):
    exe = out / label
    command = ["xcrun", "clang++", *flags, str(harness), str(archive), *frameworks,
               "-o", str(exe)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (out / (label + "-build.log")).write_text(compiled.stdout + compiled.stderr)
    assert compiled.returncode == 0, f"{label}: target test compilation failed"
    environment=dict(os.environ, SHENG_DIAG="0")
    environment.pop("SHENG_GPU_TRACE", None)
    if trace:
        environment["SHENG_GPU_TRACE"]="1"
    execution = subprocess.run([str(exe), *(["trace"] if trace is not None else [])],
                               capture_output=True, text=True, timeout=45, env=environment)
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
trace_test=kit / "bridge/icon_library_trace_host.mm"
enabled=run_case("icon-trace-on",trace_test,True)
assert enabled.returncode==0 and "enabled=1 checks=14 records=96" in enabled.stdout,enabled.stdout+enabled.stderr
disabled=run_case("icon-trace-off",trace_test,False)
assert disabled.returncode==0 and "enabled=0 checks=9 records=0" in disabled.stdout,disabled.stdout+disabled.stderr
interfaces_test = kit / "bridge/upstream_interfaces_host.mm"
interfaces = run_case("upstream-interfaces", interfaces_test)
assert interfaces.returncode == 0 and "PASS upstream interfaces: 9;" in interfaces.stdout, interfaces.stdout + interfaces.stderr
report = dict(
    passed=True, source_sha256=sha(source), test_sha256=sha(test),
    production_ObjectiveC_selector_executed=True, checks=12,
    previous_missing_selector_reproduced=True,
    exact_kernel_bitcode_and_library_lifetime_verified=True,
    real_GPU_created=False, CoreImage_render_verified=False,
    target_iconservice_verified=False, fixture_authored_no_private_inputs=True,
    icon_library_trace_test_sha256=sha(trace_test),icon_library_trace_methods_executed=True,
    icon_library_trace_enabled_checks=14,icon_library_trace_disabled_checks=9,
    icon_library_trace_budget=96,icon_library_trace_disabled_records=0,
    upstream_interfaces_checks=9, upstream_interfaces_test_sha256=sha(interfaces_test),
    actual_ObjC_descriptor_policy_forwarding=True, actual_Rust_descriptor_policy_forwarding=True,
    actual_Metal_missing_function_error_compared="originalMetalCompared=1" in interfaces.stdout,
    current_iPhone_texture_layout_or_icon_render_measured=False)
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
