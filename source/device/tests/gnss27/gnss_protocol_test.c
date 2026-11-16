/* SPDX-License-Identifier: BSD-2-Clause */
/* Synthetic LOC v2 frames, not device fixes. Compile/run only after the user
 * selects a test package. Verify the actual source's wire layouts and validity. */
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/gnss/NbGnssProtocol.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %u %s\n",__LINE__,#x); } } while (0)
static NbGnssFix fix;
static NbGnssSky sky;

static void position(NbQmiWriter *w,uint8_t *out,uint8_t status,uint64_t utc,int usage)
{
    nb_qmi_begin(w,out,4096,NB_QMI_INDICATION,0,NB_LOC_POSITION);
    nb_qmi_u32(w,1,status);nb_qmi_u8(w,2,3);
    nb_qmi_u64(w,0x10,0);nb_qmi_u64(w,0x11,0); /* valid WGS84 origin */
    nb_qmi_u64(w,0x25,utc);nb_qmi_u32(w,0x12,0x40a00000); /* 5 m */
    nb_qmi_u32(w,0x23,1);nb_qmi_u32(w,0x28,0x3fc00000); /* 1.5 ms */
    if(usage){
        const uint8_t ids[]={3,1,0,201,0,7,1}; /* LE16 SV263 */
        uint8_t signals[25]={3};nb_put64(signals+1,1);nb_put64(signals+9,NB_GNSS_B1I|NB_GNSS_B1C);
        nb_put64(signals+17,NB_GNSS_B2AI|NB_GNSS_B2AQ);
        nb_qmi_put(w,0x36,ids,sizeof ids);nb_qmi_put(w,0x37,signals,sizeof signals);
    }
}
int main(void)
{
    uint8_t raw[4096],pds[128];NbQmiWriter w;NbQmiMessage m;
    position(&w,raw,0,1700000000123ull,1);unsigned n=nb_qmi_end(&w);
    CHECK(nb_gnss_message(raw,n,&m));CHECK(nb_gnss_decode_fix(&m,50,&fix));
    CHECK(fix.bds_used==2&&fix.gps_used==1&&fix.used_count==3);
    CHECK(fix.bds_signals==(NB_GNSS_B1I|NB_GNSS_B1C|NB_GNSS_B2AI|NB_GNSS_B2AQ));
    CHECK(nb_le32(fix.parsed+23)==2);CHECK(fix.utc_ms==1700000000123ull);
    CHECK(nb_le64(fix.parsed+27)==0&&nb_le64(fix.parsed+35)==0);
    CHECK(!(fix.valid&NB_GNSS_ALT)&&nb_le32(fix.parsed+43)==0x7fc00000u);
    n=nb_gnss_pds_fix(&fix,pds,sizeof pds);CHECK(n==119);
    CHECK(nb_gnss_message(pds,n,&m));CHECK(m.message==NB_PDS_REPORT);
    CHECK(nb_gnss_tlv(&m,0x13,105)!=0&&*nb_gnss_tlv(&m,0x12,1)==0);
    CHECK(nb_le64(nb_gnss_tlv(&m,0x13,105)+15)==fix.utc_ms);
    CHECK(nb_gnss_pds_fix(&fix,pds,118)==0);
    /* An SV can appear at several frequencies in one expanded usage report. */
    position(&w,raw,0,1700000000123ull,0);
    const uint8_t repeat_ids[]={2,201,0,201,0};uint8_t repeat_signals[17]={2};
    nb_put64(repeat_signals+1,NB_GNSS_B1I);nb_put64(repeat_signals+9,NB_GNSS_B2AI);
    nb_qmi_put(&w,0x36,repeat_ids,sizeof repeat_ids);nb_qmi_put(&w,0x37,repeat_signals,sizeof repeat_signals);
    n=nb_qmi_end(&w);CHECK(nb_gnss_message(raw,n,&m)&&nb_gnss_decode_fix(&m,55,&fix));
    CHECK(fix.used_count==2&&fix.bds_used==1&&fix.bds_signals==(NB_GNSS_B1I|NB_GNSS_B2AI));
    position(&w,raw,0,1700000000123ull,0);
    nb_qmi_u32(&w,0x14,0x40a00000);nb_qmi_u32(&w,0x13,0x40800000);nb_qmi_u8(&w,0x16,68);
    n=nb_qmi_end(&w);CHECK(nb_gnss_message(raw,n,&m)&&nb_gnss_decode_fix(&m,55,&fix));
    CHECK(nb_gnss_fix_usable(&fix)&&!(fix.valid&NB_GNSS_MAJOR)&&(fix.raw_valid&NB_GNSS_MAJOR));
    /* In progress/non-satellite/missing precision must never become a fix. */
    position(&w,raw,1,1700000001123ull,0);n=nb_qmi_end(&w);
    CHECK(nb_gnss_message(raw,n,&m)&&nb_gnss_decode_fix(&m,60,&fix));CHECK(!nb_gnss_fix_usable(&fix));
    position(&w,raw,0,1700000001123ull,0);n=nb_qmi_end(&w);
    CHECK(nb_gnss_message(raw,n,&m)&&nb_gnss_decode_fix(&m,60,&fix));CHECK(fix.bds_used==0&&!fix.used_present);
    fix.technology=0;CHECK(!nb_gnss_fix_usable(&fix));fix.technology=1;
    fix.valid&=~NB_GNSS_CIRCULAR;CHECK(!nb_gnss_fix_usable(&fix));
    /* Truncation and a duplicate TLV are rejected before decoding. */
    position(&w,raw,0,1700000001123ull,0);nb_qmi_u32(&w,0x12,0);n=nb_qmi_end(&w);
    CHECK(!nb_gnss_message(raw,n,&m));CHECK(!nb_gnss_message(raw,n-1,&m));
    uint32_t rounded;
    CHECK(nb_gnss_uncertainty(0,&rounded)&&rounded==0);
    CHECK(nb_gnss_uncertainty(0x3f000000,&rounded)&&rounded==1);
    CHECK(!nb_gnss_uncertainty(0x7f800000,&rounded));CHECK(!nb_gnss_uncertainty(0xbf800000,&rounded));
    CHECK(!nb_gnss_degree_valid(0x7ff8000000000000ull,0x4056800000000000ull));
    /* Sky metadata describes tracking, independently of used-in-fix lists. */
    nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,NB_LOC_SATELLITES);
    uint8_t satellites[30]={1};nb_put32(satellites+1,0xff);nb_put32(satellites+5,6);
    nb_put16(satellites+9,201);satellites[11]=1;nb_put32(satellites+12,3);
    nb_put32(satellites+25,0x42100000); /* 36 dB-Hz, actual tracking field validity */
    nb_qmi_put(&w,0x11,satellites,sizeof satellites);n=nb_qmi_end(&w);
    CHECK(nb_gnss_message(raw,n,&m)&&nb_gnss_decode_sky(&m,70,&sky));
    CHECK(sky.bds_seen==1&&sky.sv[0].tracking==3&&!sky.signals_present);
    printf("GNSS protocol %u checks, %u failures\n",checks,failures);return failures?1:0;
}
