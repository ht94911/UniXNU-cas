#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Qualify the existing locationd snapshot's bounded queue reads and fields."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiled-source', type=Path)
parser.add_argument('--compiled-header', type=Path)
parser.add_argument('--thread-source', type=Path)
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
if re.search(r'\bthread_get_runq_locked\s*\(', body):
    raise SystemExit('sampled thread has no processor-set lock; use thread_get_runq')
if len(re.findall(r'\bthread_get_runq\s*\(thread\)', body)) != 2:
    raise SystemExit('both existing sampled-thread getters must use the thread-only API')
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
    # Exercise the exact XNU getter with only sched_lock held, including
    # non-null runq. The old getter must fail the pset-lock assertion.
    runq_verified = False
    if args.thread_source:
        thread_source = args.thread_source.read_text()
        getters = []
        for name in ('thread_get_runq', 'thread_get_runq_locked'):
            match = re.search(r'^processor_t\n' + name + r'\(thread_t thread\)\n\{.*?^\}',
                              thread_source, re.M | re.S)
            if not match:
                raise SystemExit('actual XNU getter is absent: ' + name)
            getters.append(match[0])
        fixture = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define LCK_ASSERT_OWNED 1
#define PROCESSOR_NULL ((processor_t)0)
struct pset {int owned;};
struct processor {struct pset *processor_set;int cpu_id;};
typedef struct processor *processor_t;
struct thread {int locked;struct {processor_t runq;} __runq;};
typedef struct thread *thread_t;
static jmp_buf failed;
static unsigned barriers;
static void thread_lock_assert(thread_t th,int mode) {assert(mode==LCK_ASSERT_OWNED);if(!th->locked)longjmp(failed,1);}
static void pset_assert_locked(struct pset *p) {if(!p->owned)longjmp(failed,2);}
#define os_atomic_thread_fence(order) (++barriers)
'''
        fixture += '\n'.join(getters) + r'''
int main(void) {
 struct pset pset={0};struct processor processor={&pset,2};
 struct thread th={1,{&processor}};
 assert(thread_get_runq(&th)==&processor && barriers==1);
 if(setjmp(failed)==0) {thread_get_runq_locked(&th);abort();}
 th.__runq.runq=0;assert(thread_get_runq(&th)==0 && barriers==2);
 pset.owned=1;th.__runq.runq=&processor;assert(thread_get_runq_locked(&th)==&processor);
 th.locked=0;
 if(setjmp(failed)==0) {thread_get_runq(&th);abort();}
 puts("PASS: actual thread-only getter with sched_lock, non-null runq; old getter rejects missing pset lock");
 return 0;
}
'''
        test = work / 'actual-runq-getter.c'
        test.write_text(fixture)
        getter_command = command[:command.index(str(ROOT / 'tests/gnss27/queue_snapshot_test.c'))]
        subprocess.run([*getter_command, str(test), '-o', str(work / 'actual-runq-getter')], check=True)
        getter_result = subprocess.check_output([str(work / 'actual-runq-getter')], text=True)
        (work / 'runq-execution.txt').write_text(getter_result)
        runq_verified = True
        print(getter_result.strip())
    report = dict(passed=True, actual_compiled_sources_verified=bool(args.compiled_source),
                  ASan_UBSan=True, user_reads_after_locks=True, bounded_records=8,
                  missing_reads_preserved=True, repeated_values_not_atomic=True,
                  original_waiter_return_frame_required=True, nonproducer_marker_rejected=True,
                  actual_target_execution=False, phone_operations=False,
                  actual_XNU_runq_getters_executed=runq_verified,
                  old_missing_pset_lock_rejected=runq_verified,
                  thread_only_getter_required=True,
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
