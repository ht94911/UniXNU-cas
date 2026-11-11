#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the real Format2 archive plan and expanded XNU linker configuration."""
import argparse,json,sys,tomllib
from pathlib import Path
from types import SimpleNamespace


def check(framework,device,board,configuration):
    from unixnu import components,kernel
    ctx=SimpleNamespace(root=framework,board=board,schema_version=2,tree=device/'build/power-link-check',need_config=lambda:configuration)
    ctx.component_specs=components.selected(ctx)
    plan=[('component:'+entry['id']) if kind=='component' else entry for kind,entry in kernel.archive_plan(ctx)]
    if plan.count('build-nezha27-power.py')!=1:raise ValueError('power archive recipe missing or duplicated')
    if not plan.index('build-nezha27-input.py')<plan.index('build-nezha27-power.py')<plan.index('component:Adreno840'):raise ValueError('power archive order differs from selected GPU015 integration')
    flags=kernel.xnu_env(ctx)['XNU_EXTRA_LD_ARCHIVES'].split()
    power='-Wl,-force_load,'+str(ctx.tree/'build/nezha27-power/libNezha27Power.a')
    if flags.count(power)!=1:raise ValueError('compiled power archive must occur exactly once in actual XNU linker input')
    for path in ['sheng-source-storage/libShengSourceStorage.a','nezha27-usb-control/libNezha27UsbControl.a','nezha27-input/libNezha27Input.a','adreno840/libAdreno840.a']:
        if flags.count('-Wl,-force_load,'+str(ctx.tree/'build'/path))!=1:raise ValueError('GPU015 archive dropped or duplicated: '+path)
    return dict(archive_plan=plan,expanded_link_flags=flags)


def main():
    p=argparse.ArgumentParser();p.add_argument('--framework',type=Path,required=True);p.add_argument('--device',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    sys.path.insert(0,str(a.framework/'scripts'))
    from unixnu.config import parse_config
    board=tomllib.loads((a.device/'board.toml').read_text());configuration=parse_config((a.device/'configs'/board['kernel']['defconfig']).read_text())
    result=check(a.framework,a.device,board,configuration)
    token='-Wl,-force_load,{tree}/build/nezha27-power/libNezha27Power.a';base=configuration['XNU_EXTRA_LD_ARCHIVES'];rejected=[]
    for label,replacement in [('missing-power-link',base.replace(token,'')),('duplicate-power-link',base+' '+token),('wrong-power-output',base.replace('libNezha27Power.a','libMissingPower.a'))]:
        test=dict(configuration,XNU_EXTRA_LD_ARCHIVES=replacement)
        try:check(a.framework,a.device,board,test)
        except ValueError as e:rejected.append(dict(case=label,error=str(e)))
        else:raise ValueError('bad linker configuration accepted: '+label)
    result.update(passed=True,rejected_cases=rejected,scope='actual archive planner and XNU environment expansion; native link remains a separate check',phone_operations=False)
    a.out.parent.mkdir(parents=True,exist_ok=True)
    with a.out.open('x') as f:f.write(json.dumps(result,indent=2)+'\n')
    print('PASS: power archive is both built and linked; GPU015 libraries retained; 3 bad configurations rejected')
if __name__=='__main__':main()
