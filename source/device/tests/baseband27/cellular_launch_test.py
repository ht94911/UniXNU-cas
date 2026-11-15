#!/usr/bin/env python3
"""Execute the actual selected helper launch predicate without phone access."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
patch = ROOT / "patches/xnu-12377/zzzzzz-nezha-cellular-launch-identity.patch"
added = "\n".join(line[1:] for line in patch.read_text().splitlines()
                  if line.startswith("+") and not line.startswith("+++"))
start = added.index("static bool\nnezha_cellular_request_launch")
predicate = added[start:added.index("\n}\n", start) + 3]
fixture = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
using DTEntry=void*;
using proc_t=void*;
constexpr int kSuccess=0;
constexpr unsigned IMGPF_SPAWN=0x10, IMGPF_EXEC=0x100;
constexpr unsigned POSIX_SPAWN_SETEXEC=0x40, CS_VALID=1, CS_CDHASH_LEN=20;
constexpr unsigned CS_LAUNCH_TYPE_SYSTEM_SERVICE=1;
struct _posix_spawnattr { unsigned psa_launch_type,psa_flags; };
struct image_params {
    _posix_spawnattr *ip_px_sa;
    void *ip_scriptvp,*ip_vp;
    unsigned ip_flags,ip_csflags;
    unsigned long ip_arch_offset;
};
struct cs_blob {};
static unsigned scenario,queries,policy_calls;
static uint32_t launch=29,demand=21;
static cs_blob blob;
static const uint8_t expected[20]={
    0xb2,0x8e,0xb5,0xcc,0x62,0x9c,0xc9,0x11,0x2b,0xf7,
    0x01,0x34,0xa0,0x1e,0x4d,0x57,0xc2,0xee,0x3b,0x41
};
static uint8_t supplied_hash[20];
static int SecureDTLookupEntry(void*,const char *path,DTEntry *out) {
    assert(!strcmp(path,"/chosen"));
    *out=(void*)1;
    return scenario==1 ? -1 : 0;
}
static int SecureDTGetProperty(DTEntry entry,const char *key,
                              const void **data,unsigned *length) {
    assert(entry==(void*)1);
    ++queries;
    bool first=!strcmp(key,"nezha-cellular-launch");
    assert(first || !strcmp(key,"nezha-cellular-demand"));
    *data=(scenario==2 && first) || (scenario==18 && !first)
        ? nullptr : first ? &launch : &demand;
    *length=(scenario==3 && first) || (scenario==19 && !first) ? 8 : 4;
    return (scenario==4 && first) || (scenario==20 && !first) ? -1 : 0;
}
static unsigned proc_original_ppid(proc_t) { return scenario==10 ? 99 : 1; }
static cs_blob *csvnode_get_blob(void*,unsigned long) {
    return scenario==12 ? nullptr : &blob;
}
static const uint8_t *csblob_get_cdhash(cs_blob*) {
    return scenario==13 ? nullptr : supplied_hash;
}
static const char *csblob_get_identity(cs_blob*) {
    return scenario==14 ? nullptr : scenario==15 ? "other.helper"
        : "org.unixnu.nezha.cellular-demand";
}
"""
main = r"""
static int dispatch(proc_t p,image_params *image) {
    if (nezha_cellular_request_launch(p,image)) return 0;
    ++policy_calls;
    return 91;
}
int main() {
    for (unsigned i=0;i<29;++i) {
        scenario=i; queries=policy_calls=0;
        launch=i==5 ? 23 : 29;
        demand=i==21 ? 0 : 21;
        memcpy(supplied_hash,expected,sizeof expected);
        _posix_spawnattr attr={i==9 ? 3u : 1u,0};
        image_params image={i==8 ? nullptr : &attr,i==7 ? (void*)1 : nullptr,
                            (void*)1,i==6 ? 0u : IMGPF_SPAWN,
                            i==11 ? 0u : CS_VALID,0};
        if (i==16) supplied_hash[19]^=1;
        if (i==17 || i>=22) {
            image.ip_flags=IMGPF_EXEC;
            attr.psa_flags=POSIX_SPAWN_SETEXEC;
        }
        if (i==23) attr.psa_flags=0;
        if (i==24) image.ip_flags=0;
        if (i==25) image.ip_px_sa=nullptr;
        if (i==26) attr.psa_launch_type=3;
        if (i==27) image.ip_csflags=0;
        if (i==28) supplied_hash[0]^=1;
        bool accepted=i==0 || i==17 || i==22;
        assert(dispatch(nullptr,&image)==(accepted ? 0 : 91));
        assert(policy_calls==(accepted ? 0u : 1u));
    }
    return 0;
}
"""
with tempfile.TemporaryDirectory() as folder:
    source, binary = Path(folder) / "launch.cpp", Path(folder) / "launch"
    source.write_text(fixture + predicate + main)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)
assert "if (nezha_cellular_request_launch(curp, imgp))" in added
entry = (ROOT / "patches/xnu-12377/zzzzzz-nezha-cellular-cached-entry.patch").read_text()
assert '+\tif (cellular_launch_version == 29)' in entry
assert '"/usr/libexec/DumpPanic"' in entry
print(json.dumps(dict(
    passed=True, phone_operations=False, actual_predicate_executed=True,
    source_patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
    cases=29, plain_spawn_and_typed_SETEXEC=True,
    exact_signed_helper_hash_and_identity=True,
    original_parent_and_service_type_required=True,
    launch29_and_demand21_required=True, other_executions_fall_through=True,
    ASan_UBSan=True), indent=2))
