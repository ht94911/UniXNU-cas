#!/usr/bin/env python3
"""Execute the production radio-ready gate with explicit storage/USB fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
D = Path(__file__).resolve().parents[1]
source = D / "package/NezhaWcn7860/NezhaWifiDiagnostic.c"
text = source.read_text()
start = text.index("int nezha_wifi_platform_ready(void)")
end = text.index("\nstruct registry_event", start)
body = text[start:end]
prefix = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define QCOM_USB_DEBUG_ACTIVE 7
struct qcom_usb_debug_status { unsigned phase,dma_lease_active,domain_result; };
static struct qcom_usb_debug_status status;
static unsigned boot_persist,boot_present;
static volatile int nezha_touch_stage,native_owner_data_stage,native_owner_data_status;
static volatile uint32_t native_data_persist_state;
static volatile uint64_t native_data_persist_writes,native_data_persist_errors;
static int ufs_ready;
static int native_ufs_store_ready(void) { return ufs_ready; }
static int PE_parse_boot_argn(const char *n,void *p,int bytes)
{ assert(!strcmp(n,"sheng-data-persist") && bytes==4); memcpy(p,&boot_persist,4); return boot_present; }
static void qcom_usb_debug_get_status(struct qcom_usb_debug_status *out) { *out=status; }
'''
tests = r'''
static void reset(void) {
 status=(struct qcom_usb_debug_status){7,1,0};boot_present=1;boot_persist=2;
 nezha_touch_stage=7;native_data_persist_state=2;native_data_persist_writes=500;
 native_data_persist_errors=0;native_owner_data_stage=4;native_owner_data_status=0;ufs_ready=1;
}
int main(void) {
 reset();assert(nezha_wifi_platform_ready()); /* actual writes must not veto */
 for(unsigned s=0;s<=7;++s) {reset();native_data_persist_state=s;assert(nezha_wifi_platform_ready()==(s==2));}
 reset();native_data_persist_errors=1;assert(!nezha_wifi_platform_ready());
 reset();native_owner_data_stage=3;assert(!nezha_wifi_platform_ready());
 reset();native_owner_data_status=5;assert(!nezha_wifi_platform_ready());
 reset();ufs_ready=0;assert(!nezha_wifi_platform_ready());
 reset();status.phase=0;assert(!nezha_wifi_platform_ready());
 reset();status.dma_lease_active=0;assert(!nezha_wifi_platform_ready());
 reset();status.domain_result=1;assert(!nezha_wifi_platform_ready());
 reset();nezha_touch_stage=6;assert(!nezha_wifi_platform_ready());
 reset();boot_present=0;assert(!nezha_wifi_platform_ready());
 reset();boot_persist=1;assert(!nezha_wifi_platform_ready());
 reset();boot_persist=0;native_data_persist_state=0;native_data_persist_writes=0;
 assert(nezha_wifi_platform_ready());native_data_persist_writes=1;assert(!nezha_wifi_platform_ready());
 puts("PASS 21 radio-ready cases: mounted persistent Data accepts writes, invalid states refuse, original RAM gate and USB/touch gates retained");
}
'''
a.out.mkdir(parents=True, exist_ok=True)
generated = a.out / "actual-radio-ready.c"
generated.write_text(prefix + body + tests)
command = ["gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
           str(generated), "-o", str(a.out / "ready")]
for name, cmd in [("compile", command), ("run", [str(a.out / "ready")])]:
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    (a.out / (name + ".log")).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
(a.out / "report.json").write_text(json.dumps({
    "passed": True, "source": str(source),
    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "actual_function_executed": True, "ASan_UBSan": True,
    "hardware_verified": False, "phone_operations": False
}, indent=2) + "\n")
print(result.stdout, end="")
