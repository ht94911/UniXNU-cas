"""Read bounded thread observations from the existing format2 frame trace."""
from collections import Counter, defaultdict
from pathlib import Path
import argparse
import hashlib
import json
import struct
import uuid

FRAME=struct.Struct('<II4Q8I')
SAMPLE=struct.Struct('<II19Q4I4i4I16Q')
SAMPLE_V2=struct.Struct('<II19Q4I4i4I32Q14Q')
IMAGE=struct.Struct('<IIQq16s128s')
assert SAMPLE.size==336 and SAMPLE_V2.size==576 and IMAGE.size==168
STOPS={0:'not_attempted',1:'end_of_chain',2:'frame_limit',3:'outside_registered_stack',
       4:'unaligned_frame',5:'memory_read_failed',6:'nonincreasing_frame',7:'no_registers',
       8:'thread_registration_full',9:'sample_budget_exhausted'}
STATES={1:'running_or_runnable',2:'stopped',3:'waiting',4:'uninterruptible',5:'halted'}


def sample(data):
    version,length=struct.unpack_from('<II',data)
    assert (version,length,len(data)) in ((1,SAMPLE.size,SAMPLE.size),(2,SAMPLE_V2.size,SAMPLE_V2.size))
    layout,frame_limit=(SAMPLE,8) if version==1 else (SAMPLE_V2,16)
    v=layout.unpack(data)
    keys=['version','bytes','id','began_us','info_ended_us','state_ended_us','ended_us',
          'thread','last_observed_command','previous_copy_us','stack_low','stack_high','cpu_us',
          'pc','lr','fp','sp','last_pc','last_lr','last_fp','last_sp',
          'run_state','flags','suspend_count','policy','basic_kr','state_kr','read_kr','recheck_kr',
          'depth','stop','threshold_us','architecture']
    out=dict(zip(keys,v[:33]));out['frames']=[dict(fp=v[33+i*2],lr=v[34+i*2]) for i in range(min(out['depth'],frame_limit))]
    out['flag_meaning']={key:bool(out['flags']&bit) for bit,key in
        [(1,'basic_valid'),(2,'registers_valid'),(4,'registered_stack_bounds_valid'),
         (8,'frames_read'),(16,'registers_matched_after_read'),(32,'same_copy_at_sample_end'),
         (64,'integer_registers_valid')]}
    integer_valid=version==2 and out['architecture']==1 and bool(out['flags']&64)
    out['arm64_integer_registers']=(dict(zip(['x0','x1','x2','x3']+['x'+str(i) for i in range(19,29)],
        (hex(x) for x in v[65:79]))) if integer_valid else None)
    out['integer_registers_from_first_get_state']=integer_valid
    out['integer_registers_rechecked']=False
    out['state']=STATES.get(out['run_state'],'unknown') if out['flags']&1 else 'unavailable'
    out['stop_reason']=STOPS.get(out['stop'],'unknown')
    out['capture_wall_us']=out['ended_us']-out['began_us']
    out['observation_rejection_reasons']=[]
    if out['depth']>frame_limit:out['observation_rejection_reasons'].append('frame_count')
    if out['flags']&64 and not integer_valid:out['observation_rejection_reasons'].append('integer_register_architecture_or_version')
    if out['stop'] not in (8,9) and not (out['began_us']<=out['info_ended_us']<=out['state_ended_us']<=out['ended_us']):
        out['observation_rejection_reasons'].append('observation_clock_order')
    # Matching registers is a useful check, not a proof of an atomic stack.
    out['atomic_stack_snapshot']=False
    return out


def read_frames(path,require_header=True):
    if path.stat().st_size%72:raise ValueError('Truncated compact frame record')
    pending=defaultdict(dict);sizes={};bad=set();quality=Counter();headers=[];digest=hashlib.sha256()
    with path.open('rb') as f:
        while data:=f.read(72*16384):
            digest.update(data)
            for kind,index,t0,t1,t2,t3,a,b,c,d,tid,cpu0,cpu1,tid2 in FRAME.iter_unpack(data):
                if kind==0:headers.append(dict(pid=b,format=d))
                if kind not in (27,28):continue
                key=(kind,t0);allowed=(SAMPLE.size,SAMPLE_V2.size) if kind==27 else (IMAGE.size,)
                size=b
                if size not in allowed or a>=(size+47)//48 or (key in sizes and sizes[key]!=size):
                    bad.add(key);quality['invalid_fragment_header']+=1;continue
                sizes[key]=size
                if a in pending[key]:bad.add(key);quality['duplicate_fragment']+=1
                pending[key][a]=struct.pack('<3Q6I',t1,t2,t3,c,d,tid,cpu0,cpu1,tid2)
    if require_header and (len(headers)!=1 or headers[0]['format']!=2):raise ValueError('One format2 session required')
    samples=[];images=[]
    for (kind,identity),parts in sorted(pending.items()):
        if (kind,identity) in bad:continue
        size=sizes[kind,identity]
        if set(parts)!=set(range((size+47)//48)):
            quality['missing_fragments']+=1;continue
        padded=b''.join(parts[i] for i in range(len(parts)))
        if any(padded[size:]):quality['nonzero_padding']+=1;continue
        raw=padded[:size];version,length=struct.unpack_from('<II',raw)
        allowed=((1,SAMPLE.size),(2,SAMPLE_V2.size)) if kind==27 else ((1,IMAGE.size),)
        if (version,length) not in allowed:quality['unsupported_wire']+=1;continue
        if kind==27:
            row=sample(raw)
            if row['id']!=identity:quality['sample_identity_mismatch']+=1;continue
            samples.append(row)
        else:
            version,length,base,slide,uid,name=IMAGE.unpack(raw)
            images.append(dict(id=identity,header=base,slide=slide,uuid=str(uuid.UUID(bytes=uid)),
                               path=name.split(b'\0')[0].decode(errors='replace')))
    return dict(frame_sha256=digest.hexdigest(),header=headers[0] if len(headers)==1 else None,
        quality=dict(quality),sample_count=len(samples),image_count=len(images),samples=samples,images=images,
        limits=['A thread observation alone does not identify a physical slow frame.',
            'thread_get_state temporarily stops the target internally; capture_wall_us includes observation work.',
            'RUNNING means running or runnable, not proven on-CPU execution.',
            'Stack reads happen after get_state returns; matching registers do not establish an atomic snapshot.',
            'last_observed_command is a registration-time hint, not an attribution of the observed PC to that command.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('frames',type=Path);p.add_argument('--out',required=True,type=Path)
    a=p.parse_args();report=read_frames(a.frames)
    assert not a.out.exists(),'Preserve prior decoding'
    a.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('frame_sha256','quality','sample_count','image_count')},indent=2))
