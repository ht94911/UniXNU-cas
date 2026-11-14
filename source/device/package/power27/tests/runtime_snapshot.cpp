// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the real-runtime wire fixture to compare the OS and CPU consumers.
#ifndef NP_RUNTIME_FIXTURE
#define NP_RUNTIME_FIXTURE "runtime_control.cpp"
#endif
#define main runtime_fixture_main
#include NP_RUNTIME_FIXTURE
#undef main

int main() {
    char executable[]="snapshot", mode[]="10";
    char *arguments[]={executable,mode};
    assert(runtime_fixture_main(2,arguments)==0);
    NpSnapshot os={}; NpPowerControl cpu={};
    assert(np_snapshot_read(&os)&&os.valid);
    assert(np_power_control_copy(&cpu)&&cpu.sample.valid);
    invalidate(NP_FAULT_STALE);
    assert(np_snapshot_read(&os)&&!os.valid);
    assert(np_power_control_copy(&cpu)&&!cpu.sample.valid&&
        !cpu.sample.gauge.valid&&!cpu.sample.charger.valid&&
        !cpu.pump.valid&&!cpu.pump.controls_valid&&!cpu.pump.switching_valid);
    puts("PASS OS and CPU consumers both withdraw the invalidated power sample");
}
