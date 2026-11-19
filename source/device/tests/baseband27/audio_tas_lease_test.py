#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the existing TAS claim/release and identity sequencer with IPA live."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOC = Path(os.environ.get("BB_SOC_SOURCE", str(ROOT.parent / "soc-shared")))
source = ROOT / "package/audio27/os/NezhaAudioTasBoard.cpp"
text = source.read_text()


def function(signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (text[cursor] == "{") - (text[cursor] == "}")
        cursor += 1
    return text[start:cursor]


constants = re.search(r"constexpr unsigned resource=\d+,owner=0x[0-9a-f]+;", text)[0]
declarations = "\n".join((
    constants,
    function("int error(int r)"),
    function("int claim(void *v,uint32_t kind,uint64_t *lease)"),
    function("int release(void *v,uint32_t kind,uint64_t lease)"),
))
fixture = r'''
#include "NezhaAudioTasBoard.h"
#include "NbPower.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static bool locked;
static unsigned maps;
static int lock(void *) {assert(!locked);locked=true;return 0;}
static void unlock(void *) {assert(locked);locked=false;}
static int read32(void *,unsigned,uint32_t,uint32_t *) {abort();}
static int write32(void *,unsigned,uint32_t,uint32_t) {abort();}
static int read64(void *,unsigned,uint32_t,uint64_t *) {abort();}
static int fence(void *) {return 0;}
// Trace and CPU aliases are the external providers; no hardware read/write
// function is exercised. The production ledger calls remain unchanged.
static int trace_before(NaNativeTasBoard *,const char *,uint64_t,uint64_t) {
    assert(!locked);return 1;
}
static void na_xpu_trace_after(const char *,uint64_t,int,uint32_t,uint64_t=0) {
    assert(!locked);
}
extern "C" int na_native_map(NaNativeWindow *window,NaRange range,uint64_t lease) {
    assert(!locked && lease);++maps;
    window->mapping=reinterpret_cast<void *>(1);
    window->va=reinterpret_cast<uint8_t *>(1);
    window->physical=range;window->lease=lease;return NA_OK;
}
constexpr uint64_t geni=0x1980000,tlmm=NA_TLMM_GPIO_BASE;
/*PRODUCTION_CLAIM_AND_RELEASE*/
static int identity_claim(void *context,uint64_t *lease) {
    return claim(context,NaBoardTas,lease);
}
static int identity_release(void *context,uint64_t lease) {
    return release(context,NaBoardTas,lease);
}
static int reset(void *,uint64_t lease,int) {assert(lease && !locked);return NA_OK;}
static int power(void *,uint64_t lease,int) {assert(lease && !locked);return NA_OK;}
static int revision(void *,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *out) {
    assert(lease && !locked && reg==0x78);
    *out=address==na_nezha_facts.amp_address[0] ? 0x51:0x52;return NA_OK;
}
static void run(unsigned ipa_resource,bool audio_first,bool legacy) {
    Sm8850Resources common={};NaNativeTasBoard board={};NaTasIdentity identity={};
    Sm8850ResourceIo io={nullptr,lock,unlock,read32,write32,read64,fence};
    assert(!sm8850_resources_init(&common,&io));board.common=&common;
    NaTasPowerOps ops={&board,identity_claim,power,reset,identity_release,{nullptr,revision,nullptr},nullptr};
    assert(!na_tas_identity_init(&identity,&ops));
    maps=0;uint64_t ipa=0,adsp=0,mic=0;
    assert(!sm8850_resource_acquire(&common,8,0x4e41,&adsp));
    assert(!sm8850_resource_acquire(&common,10,0x4e41,&mic));
    if(audio_first)assert(na_tas_identity_poll(&identity,100)==NA_PENDING && identity.lease && maps==2);
    int acquired=sm8850_resource_acquire(&common,ipa_resource,NB_OWNER,&ipa);
    if(legacy && audio_first) {
        assert(acquired==Sm8850Busy && !ipa && identity.lease);
        assert(na_tas_identity_poll(&identity,120)==NA_PENDING);
        assert(!na_tas_identity_poll(&identity,140));
        return;
    }
    assert(!acquired && ipa);
    if(!audio_first)assert(na_tas_identity_poll(&identity,100)==NA_PENDING);
    if(legacy) {
        assert(!identity.lease && !identity.claimed && maps==0);
        assert(na_tas_identity_poll(&identity,5100)==NA_TIMEOUT);
        assert(identity.phase==4 && common.owner[9]==NB_OWNER);
        return;
    }
    assert(identity.lease && maps==2);
    assert(na_tas_identity_poll(&identity,120)==NA_PENDING);
    assert(!na_tas_identity_poll(&identity,140));
    assert(identity.phase==3 && identity.revision[0]==0x51 && identity.revision[1]==0x52);
    assert(!identity.lease && !board.lease && !common.owner[9]);
    assert(common.owner[ipa_resource]==NB_OWNER && common.lease[ipa_resource]==ipa);
    // Playback reacquires the same real TAS adapter after identity cleanup.
    uint64_t playback=0;assert(!claim(&board,NaBoardTas,&playback));
    assert(playback && maps==2 && common.owner[ipa_resource]==NB_OWNER);
    assert(!release(&board,NaBoardTas,playback));
    assert(!sm8850_resource_release(&common,ipa_resource,NB_OWNER,ipa,1));
    assert(common.owner[8]==0x4e41 && common.lease[8]==adsp);
    assert(common.owner[10]==0x4e41 && common.lease[10]==mic);
}
int main() {
    run(9,false,true);run(9,true,true);
    run(NB_RES_SMMU,false,false);run(NB_RES_SMMU,true,false);
    puts("PASS: original047 ledger9 blocks real TAS claim/identity; selected IPA ledger permits both startup orders and playback reacquisition");
}
'''
fixture = fixture.replace("/*PRODUCTION_CLAIM_AND_RELEASE*/", declarations)
with tempfile.TemporaryDirectory(prefix="baseband-audio-tas-lease-") as directory:
    out = Path(directory)
    cpp = out / "lease.cpp"
    cpp.write_text(fixture)
    objects = []
    sources = [SOC / "src/Sm8850Resources.c",
               ROOT / "package/audio27/platform/Tas25xxEndpoint.c",
               ROOT / "package/audio27/platform/NezhaAudioPlatform.c", cpp]
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
             "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
             "-I", str(SOC / "include"), "-I", str(ROOT / "package/audio27/os"),
             "-I", str(ROOT / "package/baseband27/platform")]
    if sys.platform != "darwin":
        flags.append("-fno-pie")
    for index, path in enumerate(sources):
        cpp_source = path.suffix == ".cpp"
        obj = out / (str(index) + ".o")
        compiler = os.environ.get("CXX" if cpp_source else "CC", "g++" if cpp_source else "gcc")
        subprocess.run([compiler, "-std=c++17" if cpp_source else "-std=gnu11", *flags,
                        "-c", str(path), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = out / "lease-test"
    link = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-no-pie", "-Wl,--gc-sections"]
    subprocess.run([os.environ.get("CXX", "g++"), "-fsanitize=address,undefined",
                    *link, *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
