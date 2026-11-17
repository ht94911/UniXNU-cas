#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production STOP/cache boundary after diagnostic ownership ends."""
import argparse
import os
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    device = Path(__file__).resolve().parents[1]
    args.out.mkdir(parents=True, exist_ok=False)
    source = (device / 'package/haptics27/NezhaHapticsRuntime.cpp').read_text()
    declarations = re.search(r'struct Electrical \{[^\n]+\} diagnosticElectrical;', source).group()
    body = source[source.index('void cacheElectrical('):source.index('bool allowed(')]
    # The shared hardware model is C. Add only the explicit void-pointer
    # casts required when including the same model in this C++ boundary.
    model_header = (device / 'components/PaganiHaptics/Test/model.h').read_text()
    model_header = model_header.replace('Model *m = ctx;', 'Model *m = (Model *)ctx;')
    model_header = model_header.replace('#include "../HapticsCore.h"', '#include "HapticsCore.h"')
    model_header = model_header.replace('#include "../HapticsRegs.h"', '#include "HapticsRegs.h"')
    (args.out / 'model-cxx.h').write_text(model_header)
    model = args.out / 'electrical-stop.cpp'
    model.write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "NezhaHaptics.h"
extern "C" {
#include "check.h"
#include "model-cxx.h"
}
PghCore core;
NzHapticsQueue queue;
int stopResult;
void *hapticGate;
static Model model;
static bool diagnosticOwner;
static unsigned gateDepth=1;
void IOLockUnlock(void *) {assert(gateDepth==1);gateDepth=0;}
void IOLockLock(void *) {assert(gateDepth==0);gateDepth=1;}
uint64_t nowUs(void *) {return model.now_us;}
int darwinError(int value) {return -value;}
void note(const char *,uint32_t,int) {}
''' + declarations + '\n' + body + r'''
static void prepare() {
    m_init(&model);
    PghConfig cfg=m_config(PGH_MODE_ON);
    static PghFactoryState factory={};
    factory.valid=1;factory.t_lra_us=5726;factory.rc_mode=4;
    cfg.mode=PGH_MODE_ON;cfg.factory=&factory;cfg.vmax_mv=2000;
    cfg.hard_mv=cfg.cap_mv=2000;cfg.max_play_ms=cfg.max_stream_ms=500;
    cfg.brake_len=0;
    cfg.budget_ms=500;cfg.refill_permille=500;cfg.autores=1;
    cfg.ppid[0]=0x7f0;cfg.ppid[1]=0x7f1;cfg.ppid[2]=0x7f2;cfg.ppid[3]=0x09d;
    model.reg[PGH_CFG][PGH_CFG_CAL_EN]=4;
    model.reg[PGH_CFG][PGH_CFG_SPMI_PLAY]=0;
    model.reg[PGH_PTN][PGH_PTN_VISENSE_EN]=0x80;
    static PghSpmiIo io=kModelIo;io.ctx=&model;
    PghOps ops={&model,m_now,m_wait,m_trace,0};
    assert(!pgh_core_init(&core,&cfg,&ops,&io));
    nz_haptics_queue_open(&queue,7);uint32_t epoch=0;
    assert(!nz_haptics_queue_start(&queue,7,&epoch));
    queue.active_sequence=1;queue.active_operation=NzHapticsPulse;
    PghCmd command={PGH_CMD_CONSTANT,500,300,0};
    assert(!pgh_core_command(&core,&command));
    assert(!pgh_core_output(&core));cacheElectrical(true);
    assert(diagnosticElectrical.valid && (diagnosticElectrical.play&128));
    // This is the real007 failure: QUD_CANCEL ends USB ownership first.
    nz_haptics_queue_close(&queue);diagnosticOwner=false;
}
int main() {
    prepare();
    const uint32_t before=diagnosticElectrical.sequence;
    assert(!diagnosticOwner && !stopped("cancel"));
    assert(diagnosticElectrical.valid && diagnosticElectrical.sequence>before);
    assert(!(diagnosticElectrical.play&128) && !diagnosticElectrical.power);
    assert(diagnosticElectrical.stop_reads==1 && core.stops==1);
    assert(!queue.active_sequence && !queue.active_until_us && !stopResult);
    prepare();model.hpwr_until_reset=1;
    assert(!stopped("cancel with power recovery"));
    assert(diagnosticElectrical.valid && !diagnosticElectrical.power && diagnosticElectrical.recoveries==1);
    prepare();model.obs_fail=1;
    assert(stopped("failed readback") && !diagnosticElectrical.valid);
    assert(queue.error && !queue.ready && stopResult);
    puts("PASS production STOP/cache: ownership-ended cancellation, HPWR recovery, failed readback");
}
''')
    include = device / 'components/PaganiHaptics'
    objects = []
    for name, path in [('core', include / 'HapticsCore.c'), ('spmi', include / 'HapticsSpmi.c'),
                       ('queue', device / 'package/haptics27/NezhaHapticsQueue.c')]:
        obj = args.out / (name + '.o')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(device / 'components'), '-I', str(device / 'components/sm8850/kits'),
                        '-c', str(path), '-o', str(obj)], check=True)
        objects.append(obj)
    executable = args.out / 'electrical-stop'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(device / 'package/haptics27'), '-I', str(device / 'components'),
                    '-I', str(device / 'components/sm8850/kits'), '-I', str(include / 'Test'), '-I', str(include),
                    str(model), *map(str, objects), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
