// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the real worker sampling/cache path with the existing runtime peer.
#define main runtime_fixture_main
#include "runtime_auth.cpp"
#undef main

static unsigned identity_reads;
static int identity_error;
static int read_identity(void *, NpGaugeIdentity *out) {
    ++identity_reads;
    memset(out, 0, sizeof(*out));
    if (identity_error) return EIO;
    out->valid = 3; out->cell_code = 'F'; strcpy(out->device_name, "BM6N");
    return 0;
}

int main() {
    char program[] = "runtime-identity", mode[] = "9";
    char *args[] = {program, mode};
    assert(!runtime_fixture_main(2, args));
    __atomic_store_n(&lifecycle, 1, __ATOMIC_RELEASE);
    io.read_identity = read_identity;
    begin(ms);
    NpGaugeIdentity out = {};
    assert(np_power_identity_copy(&out) && out.valid == 3 && identity_reads == 1);
    assert(!strcmp(out.device_name, "BM6N") && out.cell_code == 'F');
    ms += 2000; begin(ms);
    assert(identity_reads == 1); // A resource lifetime keeps the completed read.
    invalidate(NP_FAULT_STALE);
    assert(np_power_identity_copy(&out) && !out.valid);
    identity_error = 1; ms += 2000; begin(ms);
    assert(np_power_identity_copy(&out) && !out.valid && out.error == EIO && identity_reads == 2);
    identity_error = 0; ms += 2000; begin(ms);
    assert(np_power_identity_copy(&out) && out.valid == 3 && identity_reads == 3);
    ms += NP_GAUGE_TTL_MS + 1;
    assert(np_power_identity_copy(&out) && !out.valid);
    puts("PASS actual runtime identity: one completed read per resource lifetime, invalidation, failed read, fresh recovery and expired gauge withdraw identity");
}
