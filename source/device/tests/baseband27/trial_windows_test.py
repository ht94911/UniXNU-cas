#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check the actual trial's mutation order, no replay and normal-return identity."""
import contextlib
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import types

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("trial", ROOT / "tools/baseband27/trial-windows.py")
trial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trial)

def require(ok, message):
    if not ok:
        raise ValueError(message)

def save(path, value):
    Path(path).write_text(json.dumps(value))

def read(path):
    return json.loads(Path(path).read_text())

def run_case(directory, failure=None):
    directory.mkdir()
    calls, observations = [], []
    task = object.__new__(trial.Trial)
    task.run = directory
    task.project = directory.parent
    task.candidate_id = "baseband003"
    task.a = types.SimpleNamespace(candidate=directory, port=5076)
    task.ready = {"boot_sha256": "boot-fixture", "payload_sha256": "payload-fixture"}
    script_sha = hashlib.sha256(Path(trial.__file__).read_bytes()).hexdigest()
    task.b = types.SimpleNamespace(require=require, read=read,
                                  digest=lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest())
    save(directory/"host-staged.json", {"boot_sha256": "boot-fixture", "source_sha256": script_sha})
    prefix = b"synthetic-prefix"
    prior = {"Data_prefix_sha256": {v: hashlib.sha256(prefix).hexdigest() for v in ("17", "27")},
             "current_disk_payload_sha256_reference": "old-payload"}
    def claim():
        calls.append("claim")
        save(directory/"window-claim.json", {"nonce": "synthetic"})
        save(directory/"handoff-reference.json", {"record": prior})
    task.claim = claim
    task.save = lambda n, v: save(directory/n, v)
    task.update_owner = lambda phase, pid=None: observations.append(phase)
    task.bind = lambda: calls.append("bind")
    task.device = "/dev/block/sdb34"
    task.maps = lambda: calls.append("maps")
    task.head = lambda version: prefix
    task.installer = types.SimpleNamespace(head_info=lambda p: {"generation": 1})
    class Transport:
        def idle(self):
            calls.append("inventory")
        def android(self):
            calls.append("Android")
        def adb(self, *args, **kwargs):
            calls.append(("adb", args))
            return ""
        def root(self, command, **kwargs):
            calls.append(command)
            if command.startswith("getprop"):
                return "none"
            if "rawhash " in command:
                return "old-payload file"
            if command.startswith("test "):
                return ""
            if command.startswith("sha256sum"):
                return "payload-fixture file"
            if "rawwrite " in command:
                if failure == "write":
                    raise RuntimeError("uncertain write")
                return "wrote 28672 blocks (117440512 bytes); payload-fixture"
            if command.startswith("rm "):
                return ""
            raise AssertionError(command)
    task.t = Transport()
    task.remote = types.SimpleNamespace(owned_forward=lambda *a, **kw: contextlib.nullcontext())
    task.remote_candidate = "C:/Users/Hua/NezhaTrials/baseband27-baseband003"
    def api(**kwargs):
        calls.append(("api", kwargs))
        assert kwargs["action"] == "launch" and not kwargs["fake"]
        return {"pid": 123}
    task.api = api
    def fastboot(*args, **kwargs):
        calls.append(("fastboot", args))
        if args == ("devices",):
            return "c7fea50c fastboot"
        if args == ("getvar", "product"):
            return "nezha"
        if args == ("getvar", "is-userspace"):
            return "no"
        if args[0] == "boot":
            if failure == "boot":
                raise RuntimeError("uncertain boot")
            return "OKAY"
        raise AssertionError(args)
    task.fastboot = fastboot
    error = None
    try:
        task.start()
    except RuntimeError as e:
        error = str(e)
    writes = [x for x in calls if isinstance(x, str) and "rawwrite " in x]
    reboots = [x for x in calls if isinstance(x, tuple) and x[0] == "adb" and x[1] == ("reboot", "bootloader")]
    boots = [x for x in calls if isinstance(x, tuple) and x[0] == "fastboot" and x[1][0] == "boot"]
    assert len(writes) == 1 and "/data/unixnu/ios27/payload.img" in writes[0]
    assert len(reboots) == len(boots) == (0 if failure == "write" else 1)
    assert error is not None if failure else error is None
    before = list(calls)
    try:
        task.start()
    except ValueError:
        pass
    else:
        raise AssertionError("Repeated start accepted")
    assert calls == before
    return {"case": failure or "success", "writes": len(writes), "boots": len(boots), "no_replay": True}

def normal_return(directory):
    directory.mkdir()
    task = object.__new__(trial.Trial)
    task.run = directory
    task.b = types.SimpleNamespace(require=require, read=read)
    task.save = lambda n, v: save(directory/n, v)
    save(directory/"collector-launch.json", {"pid": 23})
    calls = []
    def api(**args):
        calls.append(args)
        return {"pid": 23, "device_session": "0123456789abcdef", "stale": False} if args["action"] == "state" else {"request_id": "one"}
    task.api = api
    task.finish_close = lambda: calls.append("postflight")
    task.close()
    assert calls[1] == {"action": "request", "kind": "reboot", "session": "0123456789abcdef"}
    before = list(calls)
    try:
        task.close()
    except ValueError:
        pass
    else:
        raise AssertionError("Repeated normal return accepted")
    assert not any(x.get("action") == "request" for x in calls[len(before):] if isinstance(x, dict))
    return {"normal_return": "session-bound once", "phone_operations": False}

def main():
    with tempfile.TemporaryDirectory(prefix="bb-trial-contract-") as p:
        root = Path(p)
        cases = [run_case(root/n, failure) for n, failure in (("success", None), ("write", "write"), ("boot", "boot"))]
        cases.append(normal_return(root/"return"))
    print(json.dumps({"passed": True, "cases": cases, "phone_transport": False}, indent=2))

if __name__ == "__main__":
    main()
