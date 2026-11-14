#!/usr/bin/env python3
"""Exercise the actual contact-origin policy; optional local real-event replay."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import os
import re
import runpy
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--console',type=Path)
p.add_argument('--decoder',type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=True)
cc=os.environ.get('CC','cc')
source=root/'tests/touch-edge.c'
header=root/'package/nezha27-input/NezhaTouchEdge.h'
command=[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
         '-fsanitize=address,undefined','-fno-sanitize-recover=all',
         str(source),'-o',str(a.output/'touch-edge')]
subprocess.run(command,check=True)
result=subprocess.run([str(a.output/'touch-edge')],capture_output=True,text=True,check=True)
report=dict(phone_operations=False,actual_policy_source_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
            test_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),command=command,
            sanitizer_result=result.stdout.strip(),hardware_gesture_pass=False)
if a.console or a.decoder:
    assert a.console and a.decoder
    decoder=runpy.run_path(str(a.decoder))
    text=a.console.read_text(errors='replace')
    chunks={};bad_chunks=0
    for match in re.finditer(r'NEZHA INPUT EVENTBYTES: n=(\d+) at=(\d+) ([0-9a-f]+)',text):
        if len(match[3])%2 or len(match[3])>128 or int(match[2])%64:
            bad_chunks+=1;continue
        chunks.setdefault(int(match[1]),{})[int(match[2])]=bytes.fromhex(match[3])
    bridge=a.output/'edge-bridge.c'
    bridge.write_text('#include "'+str(header)+'"\nuint32_t replay(struct nezha_touch_edge_state *s,unsigned i,uint32_t m,int t,int32_t x,int32_t y){return nezha_touch_edge_mask(s,i,m,t,x,y,1200,2608);}\n')
    library=a.output/'edge-bridge.so'
    subprocess.run([cc,'-std=c11','-shared','-fPIC','-O2',str(bridge),'-o',str(library)],check=True)
    api=ctypes.CDLL(str(library)).replay
    api.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint32,ctypes.c_int,ctypes.c_int32,ctypes.c_int32]
    api.restype=ctypes.c_uint32
    state=(ctypes.c_uint32*256)()
    contacts=bottom=patched=middle=bad=0
    for number,parts in sorted(chunks.items()):
        offset=0;blob=bytearray()
        for at,part in sorted(parts.items()):
            if at!=offset:break
            blob+=part;offset+=len(part)
        try:events=decoder['event_records'](bytes(blob))
        except ValueError:bad+=1;continue
        for event in events:
            if event['type']!=11 or event['collection']:continue
            mask=int(event['event_mask'],16)
            x=int(event['x']*65536);y=int(event['y']*65536)
            after=api(state,event['index'],mask,event['touch'],x,y)
            assert (after&~0x43800)==(mask&~0x43800)
            if after!=mask:patched+=1
            if event['touch'] and mask&2:
                contacts+=1
                if 16384<x<49152 and 65536-48*65536//2608<y<=65536:
                    bottom+=1;assert after&0x800
                elif 10000<x<55000 and 10000<y<55000:
                    middle+=1;assert after==mask
    assert bottom>0 and middle>0
    report['recorded_replay']=dict(source_sha256=hashlib.sha256(a.console.read_bytes()).hexdigest(),
        begun_contacts=contacts,bottom_center_begins=bottom,ordinary_center_begins=middle,
        patched_sampled_events=patched,incomplete_or_malformed_records=bad,malformed_chunks=bad_chunks,
        limitation='Recorded transitions are sampled; this is host replay, not hardware gesture acceptance')
(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
