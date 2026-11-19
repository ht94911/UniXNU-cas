// SPDX-License-Identifier: BSD-2-Clause-Patent
// Only the new shutdown wait and terminal log outcome on the actual worker.
// Retained C objects are linked; no old fixture main or hardware test runs.
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "../platform/NezhaPowerRuntime.cpp"

extern "C" { uint32_t system_inshutdown; }
static uint64_t clock_ms=1000,worker_due,log_stop_ms;
static unsigned clock_reads,sleeps,worker_runs,polls,sends,gauge_reads;
static unsigned log_stops,log_pushes,shutdown_rows,adc_quiet,board_quiet,exit_reports;
static int exit_errno,adc_error,board_error;
static unsigned terminal_phase=4,terminal_errno,terminal_delay;
static bool schedule_worker,worker_running,status_confirmed=true,footer_closed;

void IOLog(const char *format,...) {
    if(strstr(format,"worker exit, error=%d")) {
        va_list args;va_start(args,format);exit_errno=va_arg(args,int);va_end(args);
        ++exit_reports;
    }
}
void IOSleep(unsigned ms) {
    clock_ms+=ms;++sleeps;
    if(schedule_worker&&!worker_running&&clock_ms>=worker_due) {
        schedule_worker=false;worker_running=true;++worker_runs;
        worker(nullptr,0);worker_running=false;
    }
}
extern "C" IOService *np_battery_service_create(void) { static IOService source;return &source; }
extern "C" int np_power_log_start(void) { return 0; }
extern "C" int np_power_log_push(const char *line,size_t bytes) {
    assert(line&&bytes>0&&bytes<1024);++log_pushes;
    if(!memcmp(line,"S1 ",3))++shutdown_rows;
    return 0;
}
extern "C" void np_power_log_stop(void) { assert(!log_stops);++log_stops;log_stop_ms=clock_ms; }
extern "C" int np_power_log_status(NpPowerLogStatus *out) {
    *out=NpPowerLogStatus{};
    out->phase=2;
    if(log_stops&&clock_ms-log_stop_ms>=terminal_delay) {
        out->phase=terminal_phase;out->error=terminal_errno;
        if(status_confirmed&&terminal_phase==4&&!terminal_errno)footer_closed=true;
    }
    return status_confirmed?1:0;
}
extern "C" void qcom_usb_debug_get_status(qcom_usb_debug_status *out) { *out=qcom_usb_debug_status{}; }
extern "C" void np_power_diagnostic_start(void) {}
static uint64_t now(void *) { ++clock_reads;return clock_ms; }
static int idle_link(void *) { ++polls;return 0; }
static int no_send(void *,const uint8_t *,uint32_t) { ++sends;assert(false);return -2; }
static int no_gauge(void *,uint8_t (*)[2],uint32_t *) { ++gauge_reads;assert(false);return -1; }
static int quiet_adc(void *) { ++adc_quiet;return adc_error; }
static int quiet_board(void *) { ++board_quiet;return board_error; }
static int no_resume(void *) { assert(false);return -1; }
static int no_ovp(void *,uint8_t,uint8_t,uint8_t *,uint8_t *) { assert(false);return -1; }
static void bind(unsigned mode) {
    NpPowerOps ops={};ops.now_ms=now;ops.poll_link=idle_link;ops.send_mca=no_send;ops.read_gauge=no_gauge;
    ops.quiesce=quiet_board;ops.resume=no_resume;ops.control_mode=mode;ops.quiesce_adc=quiet_adc;
    if(mode==4)ops.set_pump_usb_ovp=no_ovp;
    assert(np_power27_bind(&ops)==0);started=1;lifecycle=1;persistentLog=1;
}
static void no_inline_cleanup() {
    assert(!worker_runs&&!polls&&!sends&&!gauge_reads&&!log_stops&&!log_pushes&&!adc_quiet&&!board_quiet);
}
static void terminal_worker(int expected) {
    assert(workerExited&&exit_reports==1&&exit_errno==expected);
    assert(lifecycle==(expected?-1:5));
    assert(log_stops==1&&shutdown_rows==1&&!sends&&!gauge_reads);
    const uint64_t before=clock_ms;const unsigned old_reads=clock_reads,old_sleeps=sleeps;
    assert(nezha_power27_prepare_shutdown()==0);
    assert(clock_ms==before&&clock_reads==old_reads&&sleeps==old_sleeps&&exit_reports==1&&log_stops==1);
    assert(exit_errno==expected); // Terminal IO permission does not erase failure.
}
int main(int argc,char **argv) {
    assert(argc==2);const char *name=argv[1];int returned=0;
    if(!strcmp(name,"inactive")) {
        assert(!started&&!workerExited&&!stopRequested);
        assert(nezha_power27_prepare_shutdown()==0&&!stopRequested&&!clock_reads&&!sleeps);
        no_inline_cleanup();
    } else if(!strcmp(name,"exited-failure")) {
        started=workerExited=1;lifecycle=-1;
        assert(nezha_power27_prepare_shutdown()==0&&lifecycle==-1&&!stopRequested&&!clock_reads&&!sleeps);
        no_inline_cleanup();
    } else if(!strcmp(name,"timeout-mode2")||!strcmp(name,"timeout-mode4")||!strcmp(name,"timeout-mode5")) {
        const unsigned mode=(unsigned)(name[strlen(name)-1]-'0');bind(mode);
        // A lifecycle value (including failure/stopped) is never worker exit.
        lifecycle=mode==4?5:mode==5?-1:1;
        const uint64_t begin=clock_ms,expected=mode==5?14550:mode==4?10550:5050;
        returned=nezha_power27_prepare_shutdown();
        assert(returned==ETIMEDOUT&&clock_ms-begin==expected&&stopRequested&&!workerExited);
        no_inline_cleanup();
    } else if(!strcmp(name,"wait-success")||!strcmp(name,"wait-failure")||!strcmp(name,"pending-stop")) {
        bind(2);terminal_delay=120;worker_due=clock_ms+50;schedule_worker=true;
        if(!strcmp(name,"pending-stop"))stopRequested=1;
        if(!strcmp(name,"wait-failure")){terminal_phase=3;terminal_errno=ENOENT;}
        const uint64_t begin=clock_ms;returned=nezha_power27_prepare_shutdown();
        assert(returned==0&&clock_ms-begin>=170&&worker_runs==1&&stopRequested&&workerExited);
        assert(adc_quiet==1&&board_quiet==1);
        terminal_worker(terminal_errno?ENOENT:0);
        assert(footer_closed==(terminal_errno==0));
    } else {
        bind(2);stopRequested=1;int expected=0;
        if(!strcmp(name,"log-enoent")){terminal_phase=3;terminal_errno=ENOENT;expected=ENOENT;}
        else if(!strcmp(name,"log-failure-no-errno")){terminal_phase=3;expected=EIO;}
        else if(!strcmp(name,"log-terminal-with-errno")){terminal_errno=EACCES;expected=EACCES;}
        else if(!strcmp(name,"log-timeout")){terminal_phase=2;expected=ETIMEDOUT;}
        else if(!strcmp(name,"log-unconfirmed-terminal")){status_confirmed=false;expected=ETIMEDOUT;}
        else if(!strcmp(name,"prior-adc-error")){adc_error=EACCES;terminal_phase=3;terminal_errno=ENOENT;expected=EACCES;}
        else if(!strcmp(name,"prior-drain-error")) {
            control.buck.owned=2;control.buck.state=NP_BUCK_UNCERTAIN;
            terminal_phase=2;expected=EIO;
        } else if(!strcmp(name,"prior-quiesce-error")) {
            // Enter only the worker's early cleanup failure, then test that
            // the new log timeout cannot overwrite that diagnosed errno.
            stopRequested=0;suspendRequested=1;board_error=EACCES;terminal_phase=2;expected=EACCES;
        } else assert(false);
        worker(nullptr,0);terminal_worker(expected);
        assert(adc_quiet==1&&board_quiet==(!strcmp(name,"prior-drain-error")?0u:1u));
        if(terminal_phase==2||!status_confirmed)assert(clock_ms-log_stop_ms>=2500);
    }
    printf("{\"case\":\"%s\",\"passed\":true,\"return\":%d,\"worker_exited\":%u,\"worker_errno\":%d,\"clock_ms\":%llu,\"log_stops\":%u,\"footer_closed\":%s,\"old_tests_run\":false}\n",
        name,returned,workerExited,exit_errno,(unsigned long long)clock_ms,log_stops,footer_closed?"true":"false");
    return 0;
}
