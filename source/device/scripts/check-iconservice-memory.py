#!/usr/bin/env python3
"""Exercise the selected kernel memory-limit function with its unchanged ledger path."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def function(source, signature):
    begin = source.index(signature)
    opening = source.index("{", begin)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
assert not args.out.exists()
args.out.mkdir(parents=True)
source = args.source.read_text()
helper = function(source, "static int32_t\nnezha_icon_inactive_limit(")
setter = function(source, "static int\nmemstat_set_memlimits_locked(")
prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned int memlimit_options_t;
enum { MEMLIMIT_ACTIVE_FATAL=1, MEMLIMIT_INACTIVE_FATAL=2,
       P_MEMSTAT_MEMLIMIT_ACTIVE_FATAL=0x4000, P_MEMSTAT_MEMLIMIT_INACTIVE_FATAL=0x8000 };
struct proc {
 const char *name; int pid;
 int32_t p_memstat_memlimit_active,p_memstat_memlimit_inactive;
 unsigned p_memstat_state;
 bool active;
 int32_t ledger; bool ledger_fatal;
};
typedef struct proc *proc_t;
static bool nezha_icon_memory=true;
static int memorystatus_highwater_enabled=1;
#define LCK_MTX_ASSERT(a,b) ((void)0)
#define memorystatus_log_debug(...) ((void)0)
static const char *proc_best_name(proc_t p) {return p->name;}
static int proc_getpid(proc_t p) {return p->pid;}
static int32_t memorystatus_get_default_task_active_limit(proc_t p) {(void)p;return 512;}
static int32_t memorystatus_get_default_task_inactive_limit(proc_t p) {(void)p;return 512;}
static bool memstat_proc_is_active_locked(proc_t p) {return p->active;}
static bool memstat_update_memlimit_locked(proc_t p,bool active) {(void)p;(void)active;return true;}
static int _memstat_write_memlimit_to_ledger_locked(proc_t p,bool active,bool ignored) {
 (void)ignored;p->ledger=active?p->p_memstat_memlimit_active:p->p_memstat_memlimit_inactive;
 p->ledger_fatal=!!(p->p_memstat_state&(active?P_MEMSTAT_MEMLIMIT_ACTIVE_FATAL:P_MEMSTAT_MEMLIMIT_INACTIVE_FATAL));
 return 0;
}
"""
test = r"""
int main(void) {
 unsigned cases=0;
 const char *names[]={"iconservicesagent","iconservicesagen","backboardd","SpringBoard","iconservicesagent2"};
 for(unsigned name=0;name<5;name++)for(unsigned enabled=0;enabled<2;enabled++)
 for(unsigned active=0;active<2;active++)for(unsigned flags=0;flags<4;flags++) {
  struct proc p={.name=names[name],.pid=100,.active=active};
  nezha_icon_memory=enabled;
  assert(memstat_set_memlimits_locked(&p,200,15,flags)==0);
  const bool change=enabled && name==0 && flags==MEMLIMIT_INACTIVE_FATAL;
  assert(p.p_memstat_memlimit_active==200);
  assert(p.p_memstat_memlimit_inactive==(change?200:15));
  assert(p.ledger==(active?200:(change?200:15)));
  assert(p.ledger_fatal==!!(flags&(active?MEMLIMIT_ACTIVE_FATAL:MEMLIMIT_INACTIVE_FATAL)));
  /* Finite inactive threshold remains fatal: 201 MiB is still over it. */
  if(change && !active)assert(p.ledger_fatal && 201>p.ledger && 114<p.ledger);
  ++cases;
 }
 const int pairs[][2]={{199,15},{201,15},{200,14},{200,16},{-1,15},{200,-1},{0,0},{256,64},{200,200}};
 nezha_icon_memory=true;
 for(unsigned i=0;i<sizeof(pairs)/sizeof(pairs[0]);i++) {
  struct proc p={.name="iconservicesagent",.pid=100,.active=false};
  assert(memstat_set_memlimits_locked(&p,pairs[i][0],pairs[i][1],MEMLIMIT_INACTIVE_FATAL)==0);
  assert(p.p_memstat_memlimit_active==(pairs[i][0]>0?pairs[i][0]:512));
  assert(p.p_memstat_memlimit_inactive==(pairs[i][1]>0?pairs[i][1]:512));
  ++cases;
 }
 assert(cases==89);
 printf("%u actual setter/ledger scenarios passed\n",cases);
 return 0;
}
"""
fixture = args.out / "actual-memory-limit.c"
fixture.write_text(prefix + helper + "\n" + setter + "\n" + test)
binary = args.out / "actual-memory-limit"
subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                str(fixture), "-o", str(binary)], check=True)
result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
(args.out / "execution.txt").write_text(result.stdout + result.stderr)
report = dict(passed=True, cases=89, actual_setter_and_helper_extracted=True,
    source=str(args.source), source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),
    helper_sha256=hashlib.sha256(helper.encode()).hexdigest(),
    setter_sha256=hashlib.sha256(setter.encode()).hexdigest(),
    real_kernel_dependencies_replaced_by_controlled_ledger_for_host=True,
    finite_inactive_fatal_limit_preserved=True, active_limit_and_flags_unchanged=True,
    unrelated_names_pairs_and_disabled_unchanged=True, ASan=True, UBSan=True,
    phone_operations=False, target_ledger_verified=False)
(args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout, end="")
