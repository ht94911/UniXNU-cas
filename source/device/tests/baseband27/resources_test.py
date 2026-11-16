#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
from pathlib import Path
import os,shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
SOC=Path(os.environ.get('BB_SOC_SOURCE',str(ROOT.parent/'soc-shared')))
with tempfile.TemporaryDirectory(prefix='nb-resource-test-') as name:
    temp=Path(name)
    for sub in ['include','src']:
        shutil.copytree(SOC/sub,temp/sub)
    if 'sid[4],count' not in (temp/'include/Sm8850Resources.h').read_text():
        subprocess.run(['git','apply','--unsafe-paths','--directory='+str(temp),str(ROOT/'package/baseband27/integration/soc-stream-count.patch')],check=True)
    sources=[ROOT/'tests/baseband27/resource_binding_test.c',ROOT/'package/baseband27/platform/NbPower.c',ROOT/'package/baseband27/platform/NbScm.c',
        ROOT/'package/baseband27/upstream/QcomRpmh.c',ROOT/'package/baseband27/upstream/QcomQmp.c',temp/'src/Sm8850Resources.c']
    exe=temp/'resources'
    subprocess.run([os.environ.get('CC','gcc'),'-std=gnu11','-Wall','-Wextra','-fsanitize=address,undefined','-no-pie','-I',str(temp/'include'),*map(str,sources),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
