/* SPDX-License-Identifier: MIT */
#include "NezhaCameraOvx10500u.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct NezhaCameraOvxControls request={200,3200,256};
    struct NezhaCameraOvxControlPlan plan;
    assert(nezha_camera_ovx10500u_control_plan(&request,&plan));
    assert(plan.exposure_lines==200 && plan.frame_lines==3200 &&
        plan.analog_gain_q8==256 && plan.digital_gain_q10==1024);
    const uint16_t regs[]={0x3208,0x3840,0x380e,0x380f,0x3500,0x3501,0x3502,0x3508,0x3509,0x350a,0x350b,0x350c,0x3208,0x3208};
    const uint8_t expected[]={0,0,12,128,0,0,200,1,0,1,0,0,16,160};
    for (unsigned i=0;i<14;++i) {
        assert(plan.writes[i].reg==regs[i] && plan.writes[i].value==expected[i]);
        assert(!plan.writes[i].burst && !plan.writes[i].burst_bytes && !plan.writes[i].delay_ms);
    }
    request=(struct NezhaCameraOvxControls){3200,3200,545};
    assert(nezha_camera_ovx10500u_control_plan(&request,&plan) && plan.frame_lines==3210 && plan.analog_gain_q8==544);
    assert(plan.writes[3].value==138 && plan.writes[6].value==128);
    request=(struct NezhaCameraOvxControls){0x10000,0x10010,65280};
    assert(nezha_camera_ovx10500u_control_plan(&request,&plan) && plan.writes[1].value==1 &&
        plan.writes[4].value==1 && plan.writes[7].value==255);
    request.exposure_lines=0;
    memset(&plan,0xcd,sizeof(plan));
    assert(!nezha_camera_ovx10500u_control_plan(&request,&plan));
    const struct NezhaCameraOvxControlPlan zero={0};assert(!memcmp(&plan,&zero,sizeof(plan)));
    request.exposure_lines=0xffffffu-9;
    assert(!nezha_camera_ovx10500u_control_plan(&request,&plan));
    request.exposure_lines=1;request.gain_q8=255;
    assert(!nezha_camera_ovx10500u_control_plan(&request,&plan));
    request.gain_q8=65281;
    assert(!nezha_camera_ovx10500u_control_plan(&request,&plan));
    assert(!nezha_camera_ovx10500u_control_plan(NULL,&plan));
    assert(!nezha_camera_ovx10500u_control_plan(&request,NULL));
    puts("OVX mode1 controls: stock group brackets and 11-byte order, analog quantization, frame extension and invalid requests passed");
    return 0;
}
