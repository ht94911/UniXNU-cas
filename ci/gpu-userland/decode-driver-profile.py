"""Decode complete driver phase groups from existing72-byte frame records."""
from pathlib import Path
from collections import Counter, defaultdict
import argparse
import hashlib
import json
import struct

RECORD = struct.Struct('<II4Q8I')
API = {1:'BeginRenderPass',2:'EndRenderPass',3:'Draw',4:'DrawIndexed',5:'BindPipeline',6:'UpdateBuffer'}
PHASE = ['summary','attachment allocation','GMEM layout','subpass barrier','LRZ',
    'subpass emission','patchpoint allocation','end subpass','render dispatch','render trace',
    'render GMEM','render system memory','draw state','draw packet emission','CS reserve',
    'BO init','retire mutex wait','retire reap','cache trim','cache mutex wait','cache lookup',
    'VMA mutex wait','VMA allocate','IOKit BO allocate','IOKit BO map','IOKit BO free',
    'IOKit fence wait','IOKit other','frame diagnostics','pipeline state','host memory work']

def decode(path):
    groups = defaultdict(list)
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(RECORD.size * 32768):
            digest.update(block)
            if len(block) % RECORD.size:
                raise ValueError('Incomplete frame record')
            for row in RECORD.iter_unpack(block):
                if row[0] != 25:
                    continue
                _,seq,start,group,command,wall,packed,api,thread,count,cpu_lo,cpu_hi,self_lo,self_hi = row
                phase,flags = packed & 65535,packed >> 16
                groups[group].append(dict(sequence=seq,start_us=start,group=group,command=command,
                    wall_ns=wall,cpu_ns=cpu_lo|(cpu_hi<<32),self_ns=self_lo|(self_hi<<32),
                    phase=phase,flags=flags,api=api,thread=thread,count=count))
    quality = Counter()
    accepted = []
    for key,rows in groups.items():
        summaries = [r for r in rows if r['phase']==0]
        if len(summaries)!=1:
            quality['missing_or_duplicate_summary']+=1
            continue
        total = summaries[0]
        phases = [r for r in rows if r['phase']!=0]
        if total['count'] != len(phases) or len({r['phase'] for r in phases}) != len(phases):
            quality['incomplete_or_duplicate_phase_group']+=1
            continue
        if (total['api'] not in API or total['flags'] & ~15 or
            any(r['phase']>=len(PHASE) or r['flags'] or
                any(r[n]!=total[n] for n in ['start_us','command','api','thread']) for r in phases)):
            quality['invalid_group_identity']+=1
            continue
        measured = sum(r['self_ns'] for r in phases)
        if total['flags'] & 4 or measured + total['self_ns'] != total['wall_ns']:
            quality['invalid_exclusive_time_sum']+=1
            continue
        cpu_valid = bool(total['flags'] & 1) and not total['flags'] & 2 and total['cpu_ns']<=total['wall_ns']
        output = dict(group=key,command=total['command'],thread=total['thread'],start_us=total['start_us'],
            api=API[total['api']],wall_ms=total['wall_ns']/1e6,
            thread_cpu_ms=total['cpu_ns']/1e6 if cpu_valid else None,
            non_running_wall_ms=(total['wall_ns']-total['cpu_ns'])/1e6 if cpu_valid else None,
            unattributed_ms=total['self_ns']/1e6,budget_last_group=bool(total['flags'] & 8),
            phases=sorted([dict(name=PHASE[r['phase']],phase=r['phase'],
                inclusive_ms=r['wall_ns']/1e6,self_ms=r['self_ns']/1e6,count=r['count'])
                for r in phases],key=lambda r:r['self_ms'],reverse=True))
        accepted.append(output)
        quality['complete_groups']+=1
    return dict(source=str(path),source_sha256=digest.hexdigest(),quality=dict(quality),
        calls=sorted(accepted,key=lambda r:r['wall_ms'],reverse=True),phone_operations=False,
        limits=['Only driver calls of at least8ms are retained, at most4096 groups.',
            'Phase inclusive times overlap; self times partition each measured call.',
            'Thread CPU is measured for the whole call, not each phase.',
            'Non-running wall time includes waits and descheduling, not a unique cause.',
            'Clock/CPU-query overhead remains in the measured or unattributed intervals.',
            'Incomplete groups are rejected; missing groups do not prove an absence of stalls.'])

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('frames',type=Path)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    assert not args.out.exists()
    result=decode(args.frames)
    args.out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(quality=result['quality'],top=result['calls'][:5]),indent=2))
