#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Qualify the existing locationd snapshot's bounded queue reads and fields."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiled-source', type=Path)
parser.add_argument('--compiled-header', type=Path)
parser.add_argument('--out', type=Path)
args = parser.parse_args()
if bool(args.compiled_source) != bool(args.compiled_header):
    parser.error('both actual compiled sources are required together')
source = ROOT / 'package/BootUiTraceProbe.c'
header = ROOT / 'package/BootUiTraceQueue.h'
if args.compiled_source:
    for actual, expected in [(args.compiled_source, source), (args.compiled_header, header)]:
        if actual.read_bytes() != expected.read_bytes():
            raise SystemExit('actual queue sampler source differs: ' + str(actual))
body = source.read_text()
copied = body.index('q->tid=thread->thread_id')
unlocked = body.index('task_unlock(task);\n // No thread pointers')
sampled = body.index('btq_capture(')
if not copied < unlocked < sampled:
    raise SystemExit('user queue reads must follow task and scheduler unlock')
if 'const bool sample_location_queue=!strcmp(name,"locationd")' not in body:
    raise SystemExit('queue reads must stay scoped to the existing locationd role')
if args.out:
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    temporary = None
else:
    temporary = tempfile.TemporaryDirectory(prefix='gnss-queue-snapshot-')
    work = Path(temporary.name)
try:
    command = [os.environ.get('CC', 'clang'), '-std=c11', '-O1', '-g', '-Wall',
               '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    if platform.system() != 'Darwin':
        command += ['-fno-pie', '-no-pie']
    command += [str(ROOT / 'tests/gnss27/queue_snapshot_test.c'), '-o', str(work / 'queue-test')]
    subprocess.run(command, check=True)
    result = subprocess.check_output([str(work / 'queue-test')], text=True)
    (work / 'execution.txt').write_text(result)
    report = dict(passed=True, actual_compiled_sources_verified=bool(args.compiled_source),
                  ASan_UBSan=True, user_reads_after_locks=True, bounded_records=8,
                  missing_reads_preserved=True, repeated_values_not_atomic=True,
                  original_waiter_return_frame_required=True, nonproducer_marker_rejected=True,
                  actual_target_execution=False, phone_operations=False,
                  source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                  test_sha256=hashlib.sha256((ROOT / 'tests/gnss27/queue_snapshot_test.c').read_bytes()).hexdigest(),
                  command=command)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.strip())
    print(json.dumps({k: v for k, v in report.items() if k != 'command'}, indent=2))
finally:
    if temporary:
        temporary.cleanup()
