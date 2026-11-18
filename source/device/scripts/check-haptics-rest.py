#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the production playback retirement/rest classification."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);args=p.parse_args();args.out.mkdir(parents=True,exist_ok=False)
source=(Path(__file__).resolve().parents[1]/'package/haptics27/NezhaHapticsRuntime.cpp').read_text()
helpers=source[source.index('void restPCM('):source.index('void cacheElectrical(')]
a=source.index('        if(oldState==PGH_ST_DIRECT && core.state==PGH_ST_IDLE)');b=source.index('        NzHapticsSegment segment={};',a);transition=source[a:b]
header='\n#include <cassert>\n#include <cstdint>\n#include <cerrno>\n#include <cstdio>\n#include "NezhaHaptics.h"\n#include <PaganiHaptics/HapticsRegs.h>\nNzHapticsBus bus;\nPghCore core;\nNzHapticsQueue queue;\nuint64_t cooldownUntil;\nvoid note(const char *,uint32_t,int){}\n'
cases='int main(){\n queue.active_operation=NzHapticsPCM;queue.running=queue.ready=1;\n queue.count=4;queue.active_sequence=8;core.state=PGH_ST_IDLE;\n core.t_start=1000;core.deadline=501000;core.budget_us=450000;\n productionTransition(PGH_ST_DIRECT,11000);\n assert(queue.running && queue.ready && !queue.error && !cooldownUntil);\n assert(!queue.active_sequence && queue.completed==1 && queue.count==4);\n // Actual cap gives 500ms rest; it does not poison the endpoint/session.\n queue.active_sequence=9;queue.count=4;\n productionTransition(PGH_ST_DIRECT,501000);\n assert(cooldownUntil==1001000 && queue.rest_until_us==1001000);\n assert(queue.count==0 && queue.running && queue.ready && !queue.error && queue.completed==2);\n core.state=PGH_ST_DIRECT;core.last_play_reg=PGH_PLAY_EN;\n assert(playbackResult(-EBUSY,510000)==-EBUSY);\n core.state=PGH_ST_IDLE;core.last_play_reg=1;bus.bus.pending=1;\n assert(playbackResult(-EBUSY,510000)==-EBUSY);\n bus.bus.pending=0;\n assert(playbackResult(-EBUSY,510000)==0 && queue.rest_until_us==1010000);\n core.state=PGH_ST_FAULT;\n assert(playbackResult(-EBUSY,520000)==-EBUSY);\n assert(playbackResult(-EIO,520000)==-EIO);\n puts("PASS production DIRECT-to-IDLE: protected early stop keeps stream ready; real cap starts bounded rest without fatal EBUSY");\n}\n'
program=args.out/'rest.cpp';program.write_text(header+helpers+'\nvoid productionTransition(unsigned oldState,uint64_t now){\n'+transition+'}\n'+cases)
D=Path(__file__).resolve().parents[1]
command=[os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(D/'package/haptics27'),'-I',str(D/'components'),'-I',str(D/'components/sm8850/kits'),str(program),'-o',str(args.out/'rest')]
subprocess.run(command,check=True);r=subprocess.run([str(args.out/'rest')],check=True,capture_output=True,text=True);(args.out/'execution.log').write_text(r.stdout+r.stderr);print(r.stdout,end='')
