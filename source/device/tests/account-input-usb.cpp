/* The actual provider callbacks with clock/boot registration substituted.
 * The architectural register helper is compiled by the real arm64e build. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
static bool PE_parse_boot_argn(const char *,void *,int) {return false;}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "NezhaAA27InputUsb.h"
#pragma GCC diagnostic pop
static uint64_t fake_ticks=100;
extern "C" uint64_t qcom_usb_debug_ticks(void) {return fake_ticks;}
extern "C" int qcom_usb_diag_register(const struct qud_diag_provider *) {return 1;}
int main() {
    aa27InputFrequency=19200000;aa27Input.ready=1;
    char out[512];size_t bytes=0;unsigned trunc=0;
    qud_diag_request request={QUD_SUBMIT,27,1,(100u<<16)|200u,11,0};
    assert(aa27InputSubmit(nullptr,&request,out,127,&bytes,&trunc)==QUD_FAILED);
    assert(aa27Input.phase==AA27_IDLE && !aa27Input.action);
    assert(aa27InputSubmit(nullptr,&request,out,sizeof(out),&bytes,&trunc)==QUD_OK);
    assert(strstr(out,"\"queued\":true") && strstr(out,"\"input_dispatched\":false"));
    assert(aa27Input.phase==AA27_QUEUED && !aa27Input.commits);
    assert(aa27InputSnapshot(nullptr,out,sizeof(out),&bytes,&trunc)==QUD_OK);
    assert(strstr(out,"\"phase\":1") && strstr(out,"\"ui_accepted\":false"));
    aa27_input_state snapshot={};
    assert(aa27InputOwnerState(true,true,&snapshot) && snapshot.phase==AA27_ACTIVE);
    aa27InputLock=1;
    assert(aa27InputSnapshot(nullptr,out,sizeof(out),&bytes,&trunc)==QUD_BUSY);
    assert(!aa27InputOwnerAck(1,0,true));
    aa27InputLock=0;
    while(aa27Input.phase==AA27_ACTIVE) {
        nezha_touch_frame frame={};assert(aa27_input_frame(&aa27Input,&frame));
        assert(aa27InputOwnerAck(frame.sequence,0,aa27Input.step==0 || aa27Input.step==aa27Input.last));
    }
    assert(aa27Input.commits==9 && aa27Input.dispatches==2);
    assert(aa27InputSubmit(nullptr,&request,out,sizeof(out),&bytes,&trunc)==QUD_INVALID);
    request.experiment=12;
    assert(aa27InputSubmit(nullptr,&request,out,sizeof(out),&bytes,&trunc)==QUD_OK);
    assert(aa27InputOwnerState(true,true,&snapshot));
    nezha_touch_frame frame={};assert(aa27_input_frame(&aa27Input,&frame));
    assert(aa27InputOwnerAck(frame.sequence,0xe00002bc,true));
    assert(aa27Input.phase==AA27_FAILED && !aa27Input.ready);
    assert(aa27InputSnapshot(nullptr,out,sizeof(out),&bytes,&trunc)==QUD_OK);
    assert(strstr(out,"\"phase\":4") && strstr(out,"\"ready\":0"));
    std::puts("PASS actual USB callbacks: queue-only receipt, bounded buffer, lock contention, original owner acknowledgement and HID failure");
}
