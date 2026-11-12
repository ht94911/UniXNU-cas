#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build and run the combined power contracts against explicit host models.
Native ABI and actual hardware remain separate qualification gates.
"""
from pathlib import Path
import argparse,subprocess,json,zlib,re
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--usb',type=Path);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1];src=root/'package/power27';stubs=src/'tests/host_stubs';report={}
flags=['-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
inc=['-I'+str(src/'include'),'-I'+str(src/'vendor/qcom'),'-I'+str(src/'vendor/usb')]
usb=a.usb.resolve() if a.usb else root.parent/'usb'
assert (usb/'include/QcomXnuUsbDebug.h').is_file(),'Selected USB source is required for the public cached status API'
inc.append('-I'+str(usb/'include'))
def call(command):
 r=subprocess.run(list(map(str,command)),cwd=root,capture_output=True,text=True);return dict(command=list(map(str,command)),exit=r.returncode,stdout=r.stdout,stderr=r.stderr)
def run(name,files,extra=(),cpp=False,modes=None,libraries=()):
 exe=out/name;command=['g++' if cpp else 'gcc','-std=c++17' if cpp else '-std=gnu11',*flags,*extra,*inc,*[src/f for f in files],*libraries,'-o',exe]
 b=call(command);result=dict(build_exit=b['exit'],build=b,runs=[]);report[name]=result
 if b['exit']:raise RuntimeError(b['stderr'])
 for args in modes or [()]:
  r=call([exe,*args]);result['runs'].append(r)
  if r['exit']:raise RuntimeError(r['stderr'] or r['stdout'])
 result['run_exit']=0;print('PASS',name,flush=True)
try:
 run('core',['tests/core_cases.c','core/NezhaMca.c','core/NezhaPowerMeasurements.c','core/NezhaCharge.c'])
 run('transport',['tests/transport_cases.c','core/NezhaMca.c','core/NezhaSc8585.c','vendor/qcom/QcomSmem.c','vendor/qcom/QcomGlink.c'])
 run('soccp',['tests/soccp-readiness.c','platform/NezhaSoccp.c','core/NezhaMca.c','vendor/qcom/QcomSmem.c','vendor/qcom/QcomSmp2p.c','vendor/qcom/QcomGlink.c'])
 control_files=['core/NezhaPd.c','core/NezhaBuck.c','core/NezhaBuckPolicy.c','core/NezhaJeita.c','core/NezhaPowerControl.c','core/NezhaMca.c']
 run('control',['tests/control_cases.c',*control_files])
 run('fcc-control',['tests/fcc_control_cases.c',*control_files])
 run('fixed-pd',['tests/fixed_pd_cases.c',*control_files])
 run('jeita',['tests/jeita_cases.c','core/NezhaJeita.c'])
 run('stock-policy',['tests/stock_policy_cases.c','core/NezhaBuck.c','core/NezhaBuckPolicy.c','core/NezhaPd.c','core/NezhaMca.c'])
 run('stock-firmware',['tests/stock_firmware_cases.c','core/NezhaBuck.c','core/NezhaBuckPolicy.c','core/NezhaPd.c','core/NezhaMca.c'])
 run('stock-votes',['tests/stock_vote_cases.c',*control_files])
 run('auth',['tests/auth_cases.c','core/NezhaAuth.c','core/NezhaMca.c'],libraries=['-lcrypto'])
 run('pump',['tests/pump_io_cases.c','platform/NezhaGaugeBus.c','core/NezhaSc8585.c','core/NezhaPowerMeasurements.c'],['-DKERNEL','-I'+str(stubs/'bus')])
 run('battery-MAC',['tests/auth_io_cases.c','platform/NezhaGaugeBus.c','core/NezhaSc8585.c','core/NezhaPowerMeasurements.c'],['-DKERNEL','-I'+str(stubs/'bus')],modes=[(str(i),) for i in range(12)])
 objects=[]
 for name in ['NezhaMca','NezhaPd','NezhaBuck','NezhaBuckPolicy','NezhaJeita','NezhaPowerControl','NezhaPowerMeasurements','NezhaAuth']:
  obj=out/(name+'.o');r=call(['gcc','-std=c11',*flags,*inc,'-c',src/'core'/(name+'.c'),'-o',obj]);assert not r['exit'],r;objects.append(obj)
 run('runtime',['tests/runtime_control.cpp'],['-I'+str(stubs/'runtime'),*map(str,objects)],cpp=True,modes=[(str(i),) for i in range(32)])
 run('runtime-snapshot',['tests/runtime_snapshot.cpp'],['-I'+str(stubs/'runtime'),*map(str,objects)],cpp=True)
 run('runtime-auth',['tests/runtime_auth.cpp'],['-I'+str(stubs/'runtime'),*map(str,objects)],cpp=True,modes=[(str(i),) for i in range(21)],libraries=['-lcrypto'])
 run('runtime-identity',['tests/runtime_identity.cpp'],['-I'+str(stubs/'runtime'),*map(str,objects)],cpp=True,libraries=['-lcrypto'])
 run('log',['tests/log_cases.c'],['-I'+str(stubs/'log')],modes=[(str(i),str(out/('log-model-'+str(i)+'.txt'))) for i in range(13)])
 for mode in [0,6,8,9]:
  data=(out/('log-model-'+str(mode)+'.txt')).read_bytes();prefix,end=data.rsplit(b'END_NEZHA_POWER_LOG_V1 ',1)
  footer=re.fullmatch(rb'records=(\d+) dropped=(\d+) crc32=([0-9a-f]{8}) capped=([01])\n',end);assert footer
  assert zlib.crc32(prefix)==int(footer[3],16)
  records=prefix.splitlines()[1:];assert len(records)==int(footer[1])
  assert len(data)<=262144
  assert int(footer[4])==(mode==8)
  if mode!=8:
   assert len([r for r in records if r.startswith(b'CPREG ')])==1
   assert len([r for r in records if r.startswith(b'P1 ')])==(31 if mode==6 else 1)
 report['log']['independent_zlib_crc_passed']=True
 run('diagnostic',['tests/diagnostic_cases.cpp'],['-I'+str(stubs/'runtime')],cpp=True,modes=[(str(out/'diagnostic.json'),)])
 j=json.loads((out/'diagnostic.json').read_text());assert len(j['meta'])==14 and len(j['pdos'])==7
 stock=json.loads((out/'diagnostic.json-stock.json').read_text());assert stock['mode']==5 and len(stock['policy'])==14
 report['diagnostic']['JSON_parse_passed']=True
 r=call(['python3','-B',str(src/'tests/log_format_cases.py')]);report['log-format']=dict(build_exit=0,run_exit=r['exit'],runs=[r])
 if r['exit']:raise RuntimeError(r['stderr'] or r['stdout'])
 print('PASS log-format',flush=True)
finally:
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
