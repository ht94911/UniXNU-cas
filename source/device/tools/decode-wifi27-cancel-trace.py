#!/usr/bin/env python3
"""Select the Wi-Fi markers from the existing NEZHA INPUT FLOW text stream."""
import argparse,json,re
from pathlib import Path
CALLERS={0x2b7383e44:'current-network-onChange',0x2b7385f58:'association-state-onChange',0x2b73867c8:'associating-network-onChange',0x2b7382dd0:'isAssociating-didSet',0x2b74ad544:'password-cancelTapped',0x2b7315ca8:'cancel-publish-idle'}
PATTERN=re.compile(r'NEZHA INPUT FLOW: code=2b28e(?:100|200|300) tid=(\d+) ticks=(\d+) a=([0-9a-f]+) b=([0-9a-f]+) c=([0-9a-f]+) d=([0-9a-f]+) drop=(\d+)')
def decode(text,slide):
 records=[]
 for m in PATTERN.finditer(text):
  code=re.search(r'code=([0-9a-f]{8})',m[0])[1]
  tid,ticks=int(m[1]),int(m[2]);ret,obj,flags,comparison=(int(m[i],16) for i in range(3,7));pc=ret-slide;name=CALLERS.get(pc)
  if code=='2b28e300':
   records.append({'tid':tid,'ticks':ticks,'probe':'cancel-force-refresh','interface_object':hex(ret),'forced_mode':obj,'refresh_window_flag':flags,'route':'original-query-only' if flags&1 else 'original-new-window-and-query','reserved':comparison,'marker_code':code,'queue_drop_count':int(m[7])})
   continue
  row={'tid':tid,'ticks':ticks,'return_pc':hex(ret),'unslid_return_pc':hex(pc),'probe':name or 'unknown-callsite','object':hex(obj),'raw_flags':hex(flags),'marker_code':code,'queue_drop_count':int(m[7])}
  if name in ['current-network-onChange','association-state-onChange','associating-network-onChange','isAssociating-didSet']:
   row.update(isAssociating=flags&255,association_state=(flags>>8)&255)
   if code=='2b28e100':
    row.update(actual_display_String_words_match=comparison if comparison in [0,1] else None,actual_state_pointer_absent=comparison==2)
   else:
    absent=bool(flags&(1<<17))
    row.update(actual_state_pointer_absent=absent,actual_display_String_words_match=None if absent else ((flags>>16)&1),actual_representation_tag=None if absent else f'{comparison>>32:08x}',display_representation_tag=None if absent else f'{comparison&0xffffffff:08x}',actual_type_byte=None if absent else f'{(flags>>24)&255:02x}',display_type_byte=None if absent else f'{(flags>>32)&255:02x}',name_identification_requires_live_calibration=True)
  elif name=='cancel-publish-idle':row['published_state']=flags
  records.append(row)
 return {'cache_slide':hex(slide),'records':records,'marker_count':len(records),'unknown_callsites':sum(r['probe']=='unknown-callsite' for r in records),'queue_drops_seen':max((r['queue_drop_count'] for r in records),default=None),'missing_event_exclusion_allowed':False,'notes':['The input must belong to one verified host run/session. Use that run actual cache slide.','Markers expose equality or32-bit String-representation tags/type bytes, never original name words or passwords.','Representation tags can collide or change with heap storage; identify names only with a known live UI baseline and supporting evidence.','Presence records actual execution. Absence alone, queue drops, export gaps or a wrong slide cannot exclude execution.']}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('input',type=Path);p.add_argument('--slide',required=True,type=lambda x:int(x,0));p.add_argument('--out',type=Path);a=p.parse_args();assert a.slide>=0 and a.slide%4096==0
 value=decode(a.input.read_text(errors='replace'),a.slide);text=json.dumps(value,indent=2)+'\n'
 if a.out:a.out.write_text(text)
 else:print(text,end='')
if __name__=='__main__':main()
