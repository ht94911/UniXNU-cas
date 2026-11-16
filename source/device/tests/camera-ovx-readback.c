/* SPDX-License-Identifier: MIT */
#include "NezhaCameraOvx10500u.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls,fail_at;
static enum NezhaCameraCciStatus failure;
enum NezhaCameraCciStatus nezha_camera_cci_read(const struct NezhaCameraRegisterIo *io,
    unsigned master,uint8_t slave,uint16_t reg,uint8_t *bytes,unsigned count,
    struct NezhaCameraCciObservation *transfer)
{
    static const uint16_t address[5]={0x3840,0x380e,0x3500,0x3508,0x350a};
    static const unsigned sizes[5]={1,2,3,2,3},offset[5]={0,1,3,6,8};
    static const uint8_t raw[11]={0,0x0f,0xa0,0,1,0x40,2,0x20,1,0,0};
    assert(io && master==1 && slave==0x10 && calls<5);
    const unsigned i=calls++;
    assert(reg==address[i] && count==sizes[i]);
    if (i==fail_at) {transfer->needs_recovery=true;transfer->phase=NezhaCciData;return failure;}
    memcpy(bytes,raw+offset[i],count);return NezhaCciOk;
}
enum NezhaCameraCciStatus nezha_camera_cci_read_byte(const struct NezhaCameraRegisterIo *io,
    unsigned master,uint8_t slave,uint16_t reg,uint8_t *bytes,struct NezhaCameraCciObservation *transfer)
{return nezha_camera_cci_read(io,master,slave,reg,bytes,1,transfer);}
enum NezhaCameraCciStatus nezha_camera_cci_write_byte(const struct NezhaCameraRegisterIo *io,
    unsigned master,uint8_t slave,uint16_t reg,uint8_t byte,struct NezhaCameraCciObservation *transfer)
{(void)io;(void)master;(void)slave;(void)reg;(void)byte;(void)transfer;assert(false);return NezhaCciInvalid;}
enum NezhaCameraCciStatus nezha_camera_cci_write_burst(const struct NezhaCameraRegisterIo *io,
    unsigned master,uint8_t slave,uint16_t reg,const uint8_t *bytes,size_t count,
    unsigned queue,struct NezhaCameraCciObservation *transfer)
{(void)io;(void)master;(void)slave;(void)reg;(void)bytes;(void)count;(void)queue;(void)transfer;assert(false);return NezhaCciInvalid;}
int main(void)
{
    const struct NezhaCameraRegisterIo io={0};
    struct NezhaCameraOvxControlReadback result;
    fail_at=5;
    assert(nezha_camera_ovx10500u_read_controls(&io,&result)==NezhaCciOk && calls==5);
    assert(result.bytes_read==11 && result.frame_lines==4000 && result.exposure_lines==320 &&
        result.analog_gain_q8==544 && result.digital_gain_q10==1024);
    const unsigned complete_before[5]={0,1,3,6,8};
    for (unsigned error=0;error<2;++error) for (fail_at=0;fail_at<5;++fail_at) {
        calls=0;failure=error ? NezhaCciTimeout:NezhaCciNack;
        assert(nezha_camera_ovx10500u_read_controls(&io,&result)==failure);
        assert(calls==fail_at+1 && result.bytes_read==complete_before[fail_at] &&
            result.transfer.needs_recovery && !result.exposure_lines && !result.frame_lines &&
            !result.analog_gain_q8 && !result.digital_gain_q10);
    }
    assert(nezha_camera_ovx10500u_read_controls(&io,NULL)==NezhaCciInvalid);
    puts("OVX original control readback: eleven register bytes, first NACK/timeout stop, no writes and no incomplete settings passed");
}
