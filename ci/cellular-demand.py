"""Build one ordinary Network.framework cellular request and seal its outputs."""
from pathlib import Path
import argparse
import hashlib
import json
import plistlib
import re
import shutil
import struct
import subprocess

from artifacts import seal

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--sealed", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
args.output.mkdir(parents=True, exist_ok=False)
logs = args.output / "logs"
logs.mkdir()
commands = []


def run(name, command):
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=180)
    (logs / (name + ".stdout")).write_bytes(result.stdout)
    (logs / (name + ".stderr")).write_bytes(result.stderr)
    commands.append(dict(name=name, command=list(map(str, command)), exit_code=result.returncode))
    (args.output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    if result.returncode:
        raise RuntimeError("Inspect the sealed original build diagnostics: " + name)
    return result.stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


passed = False
try:
    sdk = run("sdk", ["xcrun", "--sdk", "iphoneos", "--show-sdk-path"]).decode().strip()
    sdk_version = run("sdk-version", ["xcrun", "--sdk", "iphoneos", "--show-sdk-version"]).decode().strip()
    compiler = run("compiler", ["xcrun", "--sdk", "iphoneos", "clang", "--version"]).decode().strip()
    source = root / "source/CellularDemand.mm"
    entitlements = root / "source/entitlements.plist"
    expected = {
        "application-identifier": "org.unixnu.nezha.cellular-demand",
        "com.apple.private.security.no-container": True,
        "com.apple.security.exception.sysctl.read-write": ["debug.qcom_usb_userlog"],
    }
    assert plistlib.loads(entitlements.read_bytes()) == expected
    binary = args.output / "cellular-demand021"
    run("compile", [
        "xcrun", "--sdk", "iphoneos", "clang++", "-target", "arm64e-apple-ios27.0",
        "-isysroot", sdk, "-std=c++17", "-fobjc-arc", "-fblocks",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-O2",
        "-fstack-protector-strong", "-framework", "Network", "-framework", "IOKit",
        "-framework", "CoreFoundation", source, "-o", binary,
    ])
    run("sign", [
        "codesign", "--force", "--sign", "-", "--identifier", expected["application-identifier"],
        "--entitlements", entitlements, "--generate-entitlement-der", "--pagesize", "4096",
        "--digest-algorithm", "sha256", binary,
    ])
    run("verify-sign", ["codesign", "--verify", "--strict", binary])
    signed = run("entitlements", ["codesign", "-d", "--entitlements", ":-", binary])
    assert plistlib.loads(signed) == expected
    detail = subprocess.run(["codesign", "-d", "--verbose=4", str(binary)],
                            capture_output=True, text=True, check=True).stderr
    (logs / "signature-details.txt").write_text(detail)
    cdhash = re.search(r"^CDHash=([0-9a-f]{40})$", detail, re.M)[1]
    assert re.search(r"^Page size=4096$", detail, re.M)
    imports = run("imports", ["xcrun", "nm", "-u", binary]).decode()
    required = [
        "_nw_parameters_create_secure_tcp", "_nw_parameters_set_required_interface_type",
        "_nw_parameters_set_prohibit_expensive", "_nw_connection_create",
        "_nw_connection_set_state_changed_handler", "_nw_connection_start", "_nw_connection_send",
        "_nw_connection_receive", "_nw_connection_copy_current_path", "_nw_path_uses_interface_type",
        "_nw_path_enumerate_interfaces", "_nw_interface_get_name", "_nw_interface_get_type",
        "_IORegistryEntryCreateCFProperty", "_sysctlbyname",
    ]
    assert all(name in imports.split() for name in required)
    dependencies = run("dependencies", ["xcrun", "otool", "-L", binary]).decode()
    build = run("build-platform", ["xcrun", "vtool", "-show-build", binary]).decode()
    data = binary.read_bytes()
    magic, cpu, subtype, kind, count, length = struct.unpack_from("<6I", data)
    assert magic == 0xfeedfacf and cpu == 0x0100000c and subtype & 0xff == 2 and kind == 2
    cursor, targets = 32, []
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, cursor)
        assert 8 <= size and cursor + size <= 32 + length
        if command == 0x32:
            targets.append(struct.unpack_from("<4I", data, cursor + 8))
        cursor += size
    assert cursor == 32 + length and len(targets) == 1 and targets[0][:2] == (2, 27 << 16)
    for path in (root / "source").iterdir():
        shutil.copyfile(path, args.output / path.name)
    report = {
        "passed": True, "target": "arm64e-apple-ios27.0", "sdk_version": sdk_version,
        "compiler": compiler, "source_sha256": sha(source),
        "source_files": {p.name: sha(p) for p in (root / "source").iterdir()},
        "binary_sha256": sha(binary), "binary_bytes": len(data), "cdhash": cdhash,
        "entitlements": expected, "required_imports": required, "dependencies": dependencies,
        "platform_report": build, "network_interface_required": "cellular",
        "successful_path_must_include_pdp_ip": True,
        "endpoint": "https://www.gstatic.com/generate_204", "TLS_default_validation": True,
        "maximum_request_count": 1, "request_timeout_seconds": 180,
        "request_and_response_only_no_account_or_identity_payload": True,
        "native_kernel_rebuilt": False, "device_executed": False,
        "cellular_runtime_verified": False, "phone_operations": False,
    }
    (args.output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    passed = True
finally:
    (args.output / "build-state.json").write_text(json.dumps({
        "passed": passed, "phone_operations": False, "native_kernel_rebuilt": False,
        "purpose": "One selected-payload ordinary cellular Network.framework demand",
    }, indent=2) + "\n")
    names = sorted(str(p.relative_to(args.output)) for p in args.output.rglob("*") if p.is_file())
    seal(args.output, names, root / "ci/artifact-recipient.pem", args.sealed)
