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

static uint32_t reference(NbQmiWriter *w, uint8_t *out, uint8_t source, uint8_t time_type)
{
    nb_qmi_begin(w,out,4096,NB_QMI_REQUEST,9,NB_PDS_INJECT_POSITION);
    nb_qmi_u64(w,0x10,1700000000123ull);nb_qmi_u64(w,0x11,0);nb_qmi_u64(w,0x12,0);
    nb_qmi_u32(w,0x13,0x41a00000); /* 20m, directly supplied reference uncertainty. */
    nb_qmi_u32(w,0x15,0x42b40000);nb_qmi_u32(w,0x16,0x41f00000);
    nb_qmi_u8(w,0x17,68);nb_qmi_u8(w,0x18,68);
    nb_qmi_u8(w,0x19,source);nb_qmi_u8(w,0x1a,time_type);
    const uint8_t reliability[]={0,2},altinfo[]={9,2,2};
    nb_qmi_put(w,0x1b,reliability,2);nb_qmi_put(w,0x1c,altinfo,3);
    return nb_qmi_end(w);
}

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
    CHECK(sky.list_present&&!sky.altitude_present);
    /* The real11-byte LOC report can contain only its mandatory status.
     * This is fresh unknown-list metadata, not malformed data or BDS proof. */
    nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,NB_LOC_SATELLITES);
    nb_qmi_u8(&w,1,1);n=nb_qmi_end(&w);CHECK(n==11);
    CHECK(nb_gnss_message(raw,n,&m)&&nb_gnss_decode_sky(&m,80,&sky));
    CHECK(!sky.list_present&&sky.altitude_present&&sky.altitude_assumed&&
          !sky.count&&!sky.bds_seen&&sky.received_ms==80&&!sky.signals_present);
    const uint8_t no_satellites=0;
    nb_qmi_put(&w,0x10,&no_satellites,1);n=nb_qmi_end(&w);
    CHECK(nb_gnss_message(raw,n,&m)&&nb_gnss_decode_sky(&m,81,&sky));
    CHECK(sky.list_present&&sky.altitude_assumed&&!sky.count&&!sky.bds_seen);
    nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,NB_LOC_SATELLITES);
    n=nb_qmi_end(&w);CHECK(nb_gnss_message(raw,n,&m)&&!nb_gnss_decode_sky(&m,82,&sky));
    nb_qmi_u8(&w,1,2);n=nb_qmi_end(&w);
    CHECK(nb_gnss_message(raw,n,&m)&&!nb_gnss_decode_sky(&m,83,&sky));
    /* Reference assistance uses the real supplied fields and UTC timestamp.
     * Its translated packet must never populate the receiver's fix record. */
    n=reference(&w,raw,1,0);CHECK(n==88);
    CHECK(nb_gnss_message(raw,n,&m));uint32_t encoded=0;
    CHECK(!nb_gnss_reference_position(&m,pds,sizeof pds,&encoded));
    NbQmiMessage loc;CHECK(nb_gnss_message(pds,encoded,&loc)&&loc.message==NB_LOC_INJECT_POSITION);
    CHECK(nb_le64(nb_gnss_tlv(&loc,0x10,8))==0&&nb_le64(nb_gnss_tlv(&loc,0x11,8))==0);
    CHECK(nb_le32(nb_gnss_tlv(&loc,0x12,4))==0x41a00000&&*nb_gnss_tlv(&loc,0x13,1)==68);
    CHECK(nb_le32(nb_gnss_tlv(&loc,0x15,4))==0x42b40000&&nb_le32(nb_gnss_tlv(&loc,0x17,4))==0x41f00000);
    CHECK(nb_le32(nb_gnss_tlv(&loc,0x19,4))==2);
    CHECK(nb_le64(nb_gnss_tlv(&loc,0x1b,8))==1700000000123ull&&nb_le32(nb_gnss_tlv(&loc,0x1d,4))==6);
    const uint8_t *alt=nb_gnss_tlv(&loc,0x1a,12);
    CHECK(alt&&nb_le32(alt)==9&&nb_le32(alt+4)==2&&nb_le32(alt+8)==2);
    const uint32_t required=encoded;
    CHECK(nb_gnss_reference_position(&m,pds,required-1,&encoded)==NB_QMI_ERR_NO_MEMORY&&!encoded);
    n=reference(&w,raw,2,0);CHECK(nb_gnss_message(raw,n,&m));
    CHECK(nb_gnss_reference_position(&m,pds,sizeof pds,&encoded)==NB_QMI_ERR_NOT_SUPPORTED&&!encoded);
    n=reference(&w,raw,1,1);CHECK(nb_gnss_message(raw,n,&m));
    CHECK(nb_gnss_reference_position(&m,pds,sizeof pds,&encoded)==NB_QMI_ERR_NOT_SUPPORTED);
    n=reference(&w,raw,1,0);CHECK(nb_gnss_message(raw,n,&m));
    nb_put64((uint8_t *)nb_gnss_tlv(&m,0x11,8),UINT64_C(0x7ff8000000000000));
    CHECK(nb_gnss_reference_position(&m,pds,sizeof pds,&encoded)==NB_QMI_ERR_INVALID_ARG);
    n=reference(&w,raw,1,0);CHECK(nb_gnss_message(raw,n,&m));
    *(uint8_t *)nb_gnss_tlv(&m,0x17,1)=100;
    CHECK(!nb_gnss_reference_position(&m,pds,sizeof pds,&encoded));CHECK(nb_gnss_message(pds,encoded,&loc));
    CHECK(*nb_gnss_tlv(&loc,0x13,1)==99);
    n=reference(&w,raw,1,0);CHECK(nb_gnss_message(raw,n,&m));
    nb_put64((uint8_t *)nb_gnss_tlv(&m,0x10,8),UINT64_MAX);
    CHECK(nb_gnss_reference_position(&m,pds,sizeof pds,&encoded)==NB_QMI_ERR_INVALID_ARG&&!encoded);
    printf("GNSS protocol %u checks, %u failures\n",checks,failures);return failures?1:0;
}
