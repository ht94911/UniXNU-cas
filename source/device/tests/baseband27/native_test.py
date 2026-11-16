#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
from pathlib import Path
import importlib.util,os,shutil,struct,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
SOC=Path(os.environ.get('BB_SOC_SOURCE',str(ROOT.parent/'soc-shared')))
SMMU=Path(os.environ.get('BB_SMMU_SOURCE',str(ROOT.parent.parent/'gpu27-astra/framework/kits/ArmSmmu4K')))
spec=importlib.util.spec_from_file_location('packer',ROOT/'tools/baseband27/pack-image.py')
packer=importlib.util.module_from_spec(spec);spec.loader.exec_module(packer)
with tempfile.TemporaryDirectory(prefix='nb-native-test-') as name:
    temp=Path(name);inputs=temp/'input';inputs.mkdir();(inputs/'ram').mkdir()
    for stem in ['ipa_fws','modem','modem_dtb']:
        header=bytearray(64+3*56);header[:6]=b'\x7fELF\x02\x01';struct.pack_into('<Q',header,32,64);struct.pack_into('<HH',header,54,56,3)
        for i in range(3):
            off,size=(0,len(header)) if i==0 else (4096,32) if i==1 else (8192,17)
            kind,flags,mem=(1,1<<27,4096) if i==2 else (0,2<<24 if i==1 else 0,size)
            struct.pack_into('<II6Q',header,64+i*56,kind,flags,off,0,0,size,mem,4096)
        (inputs/(stem+'.mdt')).write_bytes(header+b'H'*32)
        (inputs/(stem+'.b02')).write_bytes(b'F'*17)
    for stem in ['modemst1','modemst2','fsg','fsc']:
        with (inputs/'ram'/stem).open('wb') as out:out.truncate(0x20000 if stem=='fsc' else 0x800000)
    packer.pack(inputs,temp/'test.img',0)
    shared=temp/'soc'
    for sub in ['include','src']:shutil.copytree(SOC/sub,shared/sub)
    if 'sid[4],count' not in (shared/'include/Sm8850Resources.h').read_text():
        subprocess.run(['git','apply','--unsafe-paths','--directory='+str(shared),str(ROOT/'package/baseband27/integration/soc-stream-count.patch')],check=True)
    pkg=ROOT/'package/baseband27'
    c_sources=[pkg/'platform'/n for n in ['NbArchive.c','NbFirmware.c','NbPas.c','NbPower.c','NbMss.c']]+[pkg/'services/NbRmtfs.c']+[
        pkg/'upstream'/n for n in ['QcomMdt.c','QcomRpmh.c','QcomQmp.c','QcomSmem.c','QcomSmp2p.c','QcomGlink.c']]+[shared/'src/Sm8850Resources.c']+[
        SMMU/n for n in ['ArmSmmu4K.c','ArmSmmu4KContext.c','ArmSmmu4KProgram.c','ArmSmmu4KStreams.c']]
    cpp_sources=[pkg/'platform/NbNative.cpp',pkg/'platform/NbBoard.cpp',ROOT/'tests/baseband27/native_binding_test.cpp']
    flags=['-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie',
        '-I',str(ROOT/'tests/baseband27'),'-I',str(shared/'include'),'-I',str(SMMU),'-DNB_HOST_TEST=1']
    objects=[]
    for i,source in enumerate(c_sources+cpp_sources):
        cpp=source.suffix=='.cpp';obj=temp/(str(i)+'.o');objects.append(obj)
        subprocess.run([os.environ.get('CXX' if cpp else 'CC','g++' if cpp else 'gcc'),'-std=c++17' if cpp else '-std=gnu11',*flags,'-c',str(source),'-o',str(obj)],check=True)
    exe=temp/'native-test'
    subprocess.run([os.environ.get('CXX','g++'),'-fsanitize=address,undefined','-no-pie',*map(str,objects),'-lcrypto','-o',str(exe)],check=True)
    subprocess.run([str(exe),str(temp/'test.img')],check=True)
    subprocess.run([str(exe),str(temp/'test.img'),'missing'],check=True)
