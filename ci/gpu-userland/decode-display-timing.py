"""Decode raw display timing; never equate software copy completion with vblank."""
from collections import Counter, defaultdict
from pathlib import Path
import argparse
import hashlib
import json
import struct

FRAME=struct.Struct('<II4Q8I')
WIRE=struct.Struct('<4I6Q58I')
NAMES=['config','status','timing_enable','frame_line_count_enable','command_trigger',
       'timestamp_control','tear_enable','tear_sync_config','tear_height','autorefresh_config',
       'autorefresh_status','mdp_frame_count','video_frame_count','read_frame_line',
       'read_line_high','write_line','out_line','underruns','misr_control','misr_signature',
       'panel_timestamp_low','panel_timestamp_high','mdp_timestamp_low','mdp_timestamp_high',
       'write_frame_line','write_line_high']


def decode_reply(data):
    values=WIRE.unpack(data)
    names=['version','flags','hardware_version','attempts','sequence','source_cookie',
           'cpu_before_us','cpu_after_us','counter_before','counter_after','surface_handle',
           'mapping','phase','status','cpu_frames','gpu_fallbacks']
    out=dict(zip(names,values[:16]));out['interfaces']=[]
    for i in range(2):
        d=dict(zip(NAMES,values[16+i*26:16+(i+1)*26]))
        d.update(index=i,panel_timestamp=d['panel_timestamp_low']|(d['panel_timestamp_high']<<32),
                 mdp_timestamp=d['mdp_timestamp_low']|(d['mdp_timestamp_high']<<32),
                 read_frame_count=d['read_frame_line']>>16,
                 read_line=(d['read_frame_line']&65535)|(d['read_line_high']<<16),
                 write_frame_count=d['write_frame_line']>>16)
        out['interfaces'].append(d)
    return out


def clock_sample_reasons(sample):
    reasons=[]
    if sample['version']!=1:reasons.append('wire_version')
    if sample['status'] or sample['flags']&7!=7:reasons.append('incomplete_or_incoherent_read')
    if sample['flags']&8:reasons.append('diagnostic_writer_possible')
    if sample['hardware_version']>>28!=13:reasons.append('hardware_profile_not_DPU13')
    if not (sample['cpu_before_us']<=sample['cpu_after_us'] and
            0<sample['counter_before']<=sample['counter_after']):reasons.append('sample_clock_order')
    return reasons


def active_interfaces(sample):
    # CMD/autorefresh has its own frame counter. Video timing-enable/count-enable
    # bits do not substitute for the command-mode register contract.
    return [d for d in sample['interfaces'] if d['autorefresh_config']&0x80000000
            and d['autorefresh_config']&65535 and d['tear_enable']&1
            and d['timestamp_control']&1 and 0<d['panel_timestamp']<=sample['counter_after']]


def read_frames(path):
    if path.stat().st_size%FRAME.size:raise ValueError('Truncated compact frame record')
    pending=defaultdict(dict);duplicates=set();sizes={};copies={};headers=[];digest=hashlib.sha256()
    quality=Counter()
    with path.open('rb') as source:
        while block:=source.read(FRAME.size*16384):
            digest.update(block)
            for row in FRAME.iter_unpack(block):
                kind,index,t0,t1,t2,t3,a,b,c,d,tid,cpu0,cpu1,tid2=row
                if kind==0:headers.append(dict(pid=b,format=d))
                if kind==5:copies[a]=dict(sequence=a,offered_us=t0,copied_us=t1,completed_us=t2,result=b)
                if kind!=26:continue
                phase,part=a>>16,a&65535;key=(t0,phase)
                if phase not in (1,2) or b!=WIRE.size or part>=(WIRE.size+47)//48:
                    quality['invalid_fragment_header']+=1;continue
                if part in pending[key]:duplicates.add(key)
                pending[key][part]=struct.pack('<3Q6I',t1,t2,t3,c,d,tid,cpu0,cpu1,tid2)
                sizes[key]=b
    if len(headers)!=1 or headers[0]['format']!=2:raise ValueError('One format2 session required')
    rows=[]
    for key,parts in sorted(pending.items()):
        if key in duplicates:quality['duplicate_fragment_group']+=1;continue
        if set(parts)!=set(range((WIRE.size+47)//48)):
            quality['incomplete_fragment_group']+=1;continue
        padded=b''.join(parts[i] for i in range(len(parts)))
        if any(padded[WIRE.size:]):quality['nonzero_fragment_padding']+=1;continue
        sample=decode_reply(padded[:WIRE.size])
        if (sample['sequence'],sample['phase'])!=key:quality['reply_identity_mismatch']+=1;continue
        sample['clock_sample_rejection_reasons']=clock_sample_reasons(sample)
        sample['active_interface_indices']=[d['index'] for d in active_interfaces(sample)]
        rows.append(sample)
    return dict(frame_sha256=digest.hexdigest(),header=headers[0],quality=dict(quality),rows=rows,copies=copies)


def unchanged_scanout_intervals(rows,copies):
    samples={(s['sequence'],s['phase']):s for s in rows};result=[]
    for number in sorted(copies):
        before=copies.get(number-1);after=copies[number]
        left=samples.get((number-1,2));right=samples.get((number,1))
        if before is None or left is None or right is None:continue
        reasons=list(left['clock_sample_rejection_reasons']+right['clock_sample_rejection_reasons'])
        if before['result'] or after['result']:reasons.append('copy_failed')
        if left['cpu_frames']!=right['cpu_frames'] or left['gpu_fallbacks']!=right['gpu_fallbacks']:
            reasons.append('non_GPU_writer_or_fallback_changed')
        if not (before['offered_us']<=left['cpu_before_us']<=left['cpu_after_us']<=before['copied_us']
                <=before['completed_us']<=after['offered_us']<=right['cpu_before_us']
                <=right['cpu_after_us']<=after['copied_us']):reasons.append('copy_sample_clock_order')
        active=left['active_interface_indices']
        if len(active)!=1 or active!=right['active_interface_indices']:
            reasons.append('no_unique_stable_active_interface')
        info={}
        if len(active)==1 and active==right['active_interface_indices']:
            a,b=left['interfaces'][active[0]],right['interfaces'][active[0]]
            configuration=['config','timestamp_control','tear_enable','tear_sync_config','tear_height','autorefresh_config']
            if any(a[k]!=b[k] for k in configuration):reasons.append('display_configuration_changed')
            if a['underruns']!=b['underruns']:reasons.append('display_underruns_changed')
            mdp=(b['mdp_frame_count']-a['mdp_frame_count'])&0xffffffff
            tear_frame_field=(b['read_frame_count']-a['read_frame_count'])&0xffff
            # The stock get_frame_count selects INTF_MDP_FRAME_COUNT for
            # autorefresh on this DPU. The auxiliary TE read-pointer high word
            # remains0 with the retained boot configuration (heightfffffff0);
            # it is not an independent required panel-refresh counter.
            if not mdp or b['panel_timestamp']<=a['panel_timestamp']:
                reasons.append('hardware_refresh_did_not_demonstrably_advance')
            low=max(left['cpu_before_us']-left['counter_before']/19.2,
                    right['cpu_before_us']-right['counter_before']/19.2)
            high=min(left['cpu_after_us']+1-left['counter_after']/19.2,
                     right['cpu_after_us']+1-right['counter_after']/19.2)
            if low>high:reasons.append('no_common_hardware_CPU_clock_offset')
            event=[b['panel_timestamp']/19.2+low,b['panel_timestamp']/19.2+high]
            if not (left['cpu_after_us']<=event[0]<=event[1]<=right['cpu_before_us']):
                reasons.append('latest_hardware_event_not_bounded_inside_unchanged_interval')
            info=dict(interface=active[0],mdp_cycles=mdp,tear_read_frame_field_delta=tear_frame_field,
                      frame_counter_source='INTF_MDP_FRAME_COUNT selected by enabled autorefresh and DPU13 capability',
                      panel_timestamp_delta_ticks=b['panel_timestamp']-a['panel_timestamp'],
                      interior_MDP_cycles_lower_bound=max(0,mdp-1),
                      latest_panel_event_CPU_bounds_us=event,
                      write_frame_delta=(b['write_frame_count']-a['write_frame_count'])&0xffff,
                      underrun_count_available=a['underruns']!=0xffffffff and b['underruns']!=0xffffffff,
                      MISR_control_words=[a['misr_control'],b['misr_control']],
                      MISR_signature_unchanged=a['misr_signature']==b['misr_signature'])
        result.append(dict(sequence=number-1,next_sequence=number,
                           source_cookie=left['source_cookie'],surface_handle=left['surface_handle'],
                           start_us=left['cpu_after_us'],end_us=right['cpu_before_us'],
                           unchanged_scanout_ms=(right['cpu_before_us']-left['cpu_after_us'])/1000,
                           hardware_clock_corroborated=not reasons,
                           rejection_reasons=sorted(set(reasons)),**info))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('frames',type=Path)
    parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    result=read_frames(args.frames);rows=unchanged_scanout_intervals(result['rows'],result['copies'])
    result['unchanged_scanout_intervals']=rows
    result['phone_operations']=False;result['optical_pixel_timing_measured']=False
    result['limits']=[
        'Only complete raw groups and one advancing active CMD interface qualify; software copy times are not vblank.',
        'The interval between the preceding after-copy sample and next before-copy sample contains no GOP GPU write.',
        'Non-GPU frames/fallbacks and diagnostic overlay are checked; a hardware cycle is not itself a changed image.',
        'Interior MDP cycles are a lower bound; copy-overlapping refreshes have ambiguous image versions.',
        'Animation intent and the matching producer command must be established before calling an interval a UI stall.',
        'Kernel surface handles are retained separately from mapping-cache IDs; linkage to frontend IOSurface IDs requires validation.',
        'Raw MISR values are not used to claim a fresh per-frame output signature; no MISR control is enabled here.',
        'No physical-frame acceptance is possible when this capture lacks enabled hardware timestamp/counter records.']
    result.pop('copies')
    args.out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(samples=len(result['rows']),quality=result['quality'],intervals=len(rows),
                         corroborated_intervals=sum(r['hardware_clock_corroborated'] for r in rows),
                         largest=sorted(rows,key=lambda r:r['unchanged_scanout_ms'],reverse=True)[:8]),indent=2))


if __name__=='__main__':main()
