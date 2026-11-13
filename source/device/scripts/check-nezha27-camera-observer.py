#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the camera USB snapshot with the selected protocol and HID snapshot helpers."""

import argparse
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--usb', required=True, type=Path, help='the selected QcomXnuUsbDebug source')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    probe = (ROOT / 'package/camera/NezhaCameraProbe.c').read_text()
    begin = probe.index('enum CameraDiagnosticPhase {')
    end = probe.index('static bool journal_ready;', begin)
    source = '''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "NezhaInputTelemetry.h"
#include "QcomUsbDiagnostic.h"
static unsigned requested_mode=3;
'''
    source += probe[begin:end]
    source += '''
int main(void) {
    char result[2048];size_t bytes=99;unsigned truncated=99;
    diagnostic_words[DiagGdsc]=0xfa22f800;
    diagnostic_words[DiagGdscStatus]=0x04070000;
    diagnostic_words[DiagCoreAhb]=0x08000000;
    record_diagnostic(CameraTop,3);
    diagnostic_words[DiagClockRestore]=0;
    record_diagnostic(CameraClockRestored,0);
    record_diagnostic(CameraFinished,0);
    assert(camera_provider.id==28 && camera_provider.operations==((1u<<QUD_SNAPSHOT)|(1u<<QUD_SUBMIT)));
    struct qud_diag_request request={QUD_SUBMIT,28,1,0x1234,0,0};
    assert(camera_submit(NULL,&request,result,sizeof(result),&bytes,&truncated)==QUD_UNSUPPORTED);
    journal_token=0x1234;usb_gated=1;probe_request=CameraArmed;
    request.arg1=0x5678;
    assert(camera_submit(NULL,&request,result,sizeof(result),&bytes,&truncated)==QUD_INVALID);
    assert(probe_request==CameraArmed);
    request.arg1=0x1234;
    assert(camera_submit(NULL,&request,result,1,&bytes,&truncated)==QUD_FAILED && probe_request==CameraArmed);
    assert(camera_submit(NULL,&request,result,sizeof(result),&bytes,&truncated)==QUD_OK);
    assert(probe_request==CameraQueued && bytes==12 && strstr(result,"queued")!=NULL);
    assert(camera_submit(NULL,&request,result,sizeof(result),&bytes,&truncated)==QUD_BUSY);
    assert(camera_snapshot(NULL,result,sizeof(result),&bytes,&truncated)==QUD_OK);
    assert(bytes==strlen(result) && truncated==0);puts(result);
    uint32_t sequence=nzi_begin(&camera_diagnostic);
    assert(camera_snapshot(NULL,result,sizeof(result),&bytes,&truncated)==QUD_BUSY);
    assert(bytes==0);nzi_end(&camera_diagnostic,sequence);
    /* A commit interrupted between its two records must also return busy. */
    assert(camera_snapshot(NULL,result,sizeof(result),&bytes,&truncated)==QUD_BUSY);
    nzi_end(&camera_physical_diagnostic,sequence);
    assert(camera_snapshot(NULL,result,8,&bytes,&truncated)==QUD_FAILED);
    assert(bytes==0 && truncated==1);
    for(unsigned i=0;i<NZI_WORDS;++i){diagnostic_words[i]=UINT32_MAX;diagnostic_physical[i]=UINT32_MAX;}
    record_diagnostic(CameraFinished,0);
    assert(camera_snapshot(NULL,result,sizeof(result),&bytes,&truncated)==QUD_OK);
    assert(bytes<sizeof(result) && truncated==0);
    requested_mode=4;probe_request=CameraArmed;
    diagnostic_physical[10]=20;diagnostic_physical[30]=384000;diagnostic_physical[31]=0;
    record_diagnostic(CameraFinished,0);
    assert(camera_submit(NULL,&request,result,sizeof(result),&bytes,&truncated)==QUD_OK);
    assert(camera_snapshot(NULL,result,sizeof(result),&bytes,&truncated)==QUD_OK);
    assert(bytes==strlen(result) && truncated==0);puts(result);
    return 0;
}
'''
    path = args.out / 'camera-observer.c'
    path.write_text(source)
    exe = args.out / 'camera-observer'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'package/nezha27-input'), '-I', str(args.usb / 'include'),
                    str(path), '-o', str(exe)], check=True)
    result = subprocess.check_output([str(exe)], text=True)
    record, pattern = map(json.loads, result.splitlines())
    assert record['phase'] == 12 and record['status'] == 0 and record['first_failed_phase'] == 3
    assert record['first_failed_status'] == 3 and record['camcc'][:3] == [0xfa22f800, 0x04070000, 0x08000000]
    assert len(record['gpio']) == 12 and len(record['vrm']) == 12 and len(record['rsc']) == 8
    assert pattern['mode'] == 4 and pattern['request'] == 2 and pattern['frames'][0] == 20
    assert pattern['sample'] == [384000, 0] and len(pattern['smmu']) == 4
    (args.out / 'camera-observer.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Camera snapshot retains the first failure and rejects torn or truncated observations')


if __name__ == '__main__':
    main()
