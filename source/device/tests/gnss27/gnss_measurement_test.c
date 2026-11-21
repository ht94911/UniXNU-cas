/* SPDX-License-Identifier: BSD-2-Clause */
/* Protocol fixtures contain generated measurements, never device positions. */
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/gnss/NbGnssProtocol.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %u %s\n",__LINE__,#x); } } while (0)

static uint32_t message(uint8_t *out, uint8_t system, unsigned count, int optional)
{
    NbQmiWriter w;
    nb_qmi_begin(&w,out,4096,NB_QMI_INDICATION,0,NB_LOC_MEASUREMENT);
    nb_qmi_u8(&w,1,1);nb_qmi_u8(&w,2,2);nb_qmi_u32(&w,3,system);
    if(optional) {
        uint8_t clock[17]={0};nb_put32(clock,120000);clock[4]=1;
        nb_put64(clock+5,1700000000000ull);nb_put32(clock+13,1);
        nb_qmi_put(&w,0x1a,clock,sizeof clock);
        uint8_t time[18]={0};nb_put32(time,system);nb_put16(time+4,2000);
        nb_put32(time+6,120000);nb_put32(time+10,0x3e800000u);nb_put32(time+14,0x3b03126fu);
        nb_qmi_put(&w,0x18,time,sizeof time);
        if (system==5u) {
            uint8_t glo[15]={0};glo[0]=8;nb_put16(glo+1,1000);
            nb_put32(glo+3,120000);nb_put32(glo+7,0x3e800000u);
            nb_put32(glo+11,0x3b03126fu);
            nb_qmi_put(&w,0x19,glo,sizeof glo);
        }
        uint8_t freq[12]={0};nb_put32(freq,0x3dcccccd);nb_put32(freq+4,0x3c23d70a);nb_put32(freq+8,2);
        nb_qmi_put(&w,0x10,freq,sizeof freq);
        nb_qmi_u64(&w,0x1e,system==1?1u:NB_GNSS_B1I);
        nb_qmi_u32(&w,0x1c,5);
    }
    uint8_t list[1+16*NB_GNSS_LOC_MEASUREMENT_BYTES]={0};list[0]=(uint8_t)count;
    for(unsigned i=0;i<count&&i<16u;++i) {
        uint8_t *s=list+1+i*NB_GNSS_LOC_MEASUREMENT_BYTES;
        const unsigned first=system==1?1u:system==2?301u:system==3?120u:
            system==5?65u:system==6?201u:system==7?193u:401u;
        nb_put16(s,first+i);nb_put32(s+3,3);
        nb_put16(s+7,0xff);s[9]=1;s[10]=3;
        nb_put64(s+11,0x0800003full);nb_put64(s+19,0x0800003full);
        nb_put16(s+27,380);nb_put16(s+29,12);nb_put32(s+31,7);
        nb_put32(s+35,119925);nb_put32(s+39,0x3f000000);nb_put32(s+43,0x3b03126f);
        nb_put32(s+47,0xc1200000);nb_put32(s+51,0x3e4ccccd);
        s[55]=1;nb_put32(s+56,0x3dcccccd);s[60]=0;
        nb_put32(s+61,0x3f800000);nb_put32(s+65,0xc1200000);nb_put32(s+69,0x3dcccccd);
        nb_put64(s+73,0x412e848000000000ull);s[81]=9;
        nb_put32(s+82,0x3fc00000);nb_put32(s+86,0x3f000000);
    }
    nb_qmi_put(&w,0x1b,list,1u+count*NB_GNSS_LOC_MEASUREMENT_BYTES);
    return nb_qmi_end(&w);
}

static void epoch(const char *path, uint32_t tick, uint8_t empty_system)
{
    /* Real receiver shape: a zero-band empty constellation block occurs
     * between otherwise convertible numbered reports. It must reach the
     * original PDS whole-epoch validator without renumbering any block. */
    const uint8_t systems[]={1,1,2,5,6,6,6,7,7,8};
    const uint8_t band_index[]={1,3,7,0,10,11,13,14,16,19};
    uint8_t in[4096],out[4096];NbQmiMessage loc,pds;NbGnssMeasurement info;
    FILE *f=path?fopen(path,"wb"):0;
    if(path)CHECK(f!=0);
    for(unsigned i=0;i<sizeof systems;++i){
        const uint8_t system=i==3u?empty_system:systems[i];
        uint32_t n=message(in,system,i==3u?0u:1u,1);
        CHECK(nb_gnss_message(in,n,&loc));
        ((uint8_t*)nb_gnss_tlv(&loc,1,1))[0]=(uint8_t)(i+1u);
        ((uint8_t*)nb_gnss_tlv(&loc,2,1))[0]=(uint8_t)sizeof systems;
        nb_put64((uint8_t*)nb_gnss_tlv(&loc,0x1e,8),
                 band_index[i]?UINT64_C(1)<<(band_index[i]-1u):0);
        nb_put32((uint8_t*)nb_gnss_tlv(&loc,0x1a,17),tick);
        uint32_t bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
        CHECK(bytes&&nb_gnss_message(out,bytes,&pds));
        CHECK(info.sequence==i+1u&&info.maximum==sizeof systems&&info.tick==tick);
        CHECK(*nb_gnss_tlv(&pds,1,1)==i+1u&&*nb_gnss_tlv(&pds,2,1)==sizeof systems);
        CHECK(info.count==(i==3u?0u:1u));
        if(f){uint8_t length[4];nb_put32(length,bytes);CHECK(fwrite(length,1,4,f)==4);
              CHECK(fwrite(out,1,bytes,f)==bytes);}
    }
    if(f)CHECK(!fclose(f));
}

int main(int argc,char **argv)
{
    uint8_t in[4096],out[4096];NbQmiMessage loc,pds;NbGnssMeasurement info;
    uint32_t n=message(in,6,2,1);CHECK(nb_gnss_message(in,n,&loc));
    uint32_t bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
    CHECK(bytes&&nb_gnss_message(out,bytes,&pds));CHECK(pds.message==NB_PDS_MEASUREMENT&&pds.type==NB_QMI_INDICATION);
    CHECK(info.sequence==1&&info.maximum==2&&info.system==6&&info.count==2);
    CHECK(info.tick_present&&info.tick==120000&&info.resets_present&&info.clock_resets==5);
    CHECK(info.signals==NB_GNSS_B1I&&info.signal==10);
    CHECK(*nb_gnss_tlv(&pds,3,1)==4); /* PDS BDS enum differs from LOC. */
    CHECK(nb_le32(nb_gnss_tlv(&pds,0x15,4))==5);
    const uint8_t *clock=nb_gnss_tlv(&pds,0x10,14);CHECK(clock&&clock[4]==1&&clock[13]==1&&nb_le64(clock+5)==1700000000000ull);
    const uint8_t *time=nb_gnss_tlv(&pds,0x11,14);CHECK(time&&nb_le16(time)==2000&&nb_le32(time+2)==120000);
    CHECK(nb_le32(nb_gnss_tlv(&pds,0x1e,4))==10);
    const uint8_t *list=nb_gnss_tlv(&pds,0x1d,203);CHECK(list&&list[0]==2);
    if(!list)return 1;
    const uint8_t *sv=list+1;CHECK(nb_le16(sv)==201&&nb_le16(sv+101)==202);
    CHECK(nb_le16(sv+17)==380&&nb_le16(sv+19)==7);
    CHECK(nb_le32(sv+24)==119925&&nb_le32(sv+28)==0x3f000000&&nb_le32(sv+32)==0x3b03126f);
    CHECK(nb_le64(sv+44)==0x0800003full);
    CHECK(nb_le64(sv+2)==0x40cu&&sv[84]==9&&nb_le32(sv+76)==0xc1200000);
    /* Do not copy LOC fields into unrelated PDS fields. */
    CHECK(!sv[11]&&!sv[12]&&!sv[13]&&!nb_le16(sv+14)&&!sv[16]&&!sv[21]&&!nb_le16(sv+22));
    CHECK(!nb_le32(sv+52)&&!nb_le32(sv+56)&&!nb_le32(sv+60)&&!nb_le32(sv+72));
    /* Detection enums are absent: azimuth/elevation IEEE bits cannot be
     * relabeled as PDS detection methods or acquire bit9 validity. */
    CHECK(!(nb_le64(sv+2)&0x200u)&&!nb_le32(sv+85)&&!nb_le32(sv+89));
    CHECK(nb_gnss_pds_measurement(&loc,out,bytes-1,&info)==0);
    if(argc>=2) {
        bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
        FILE *f=fopen(argv[1],"wb");CHECK(f!=0);if(f){CHECK(fwrite(out,1,bytes,f)==bytes);CHECK(!fclose(f));}
    }
    /* No clock, band, or list may acquire valid fields from old output. */
    n=message(in,1,0,0);CHECK(nb_gnss_message(in,n,&loc));
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);CHECK(bytes&&nb_gnss_message(out,bytes,&pds));
    CHECK(!info.tick_present&&!info.signals&&!info.count&&!nb_qmi_tlv(&pds,0x10,0)&&!nb_qmi_tlv(&pds,0x1e,0));
    /* Every byte truncation fails the complete-chain parser. */
    n=message(in,6,2,1);
    for(unsigned i=0;i<n;++i)CHECK(!nb_gnss_message(in,i,&loc));
    CHECK(nb_gnss_message(in,n,&loc));
    uint8_t *s=(uint8_t*)nb_gnss_tlv(&loc,0x1b,181)+1;
    /* Unknown status must not become fresh; DONT_USE remains visible. */
    nb_put64(s+11,0);nb_put64(s+19,UINT64_C(0x800000000800003f));
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);CHECK(bytes&&nb_gnss_message(out,bytes,&pds));
    list=nb_gnss_tlv(&pds,0x1d,203);CHECK(list&&nb_le64(list+1+44)==UINT64_C(0x8000000000000000));
    nb_put16(s+7,0);bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);CHECK(bytes&&nb_gnss_message(out,bytes,&pds));
    list=nb_gnss_tlv(&pds,0x1d,203);CHECK(list&&nb_le64(list+1+2)==0x400u&&!list[1+84]);
    nb_put32(s+31,32768);CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    nb_put32(s+31,(uint32_t)-32769);CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    /* Invalid lengths and missing mandatory fields do not partially emit. */
    n=message(in,6,2,1);CHECK(nb_gnss_message(in,n,&loc));
    s=(uint8_t*)nb_gnss_tlv(&loc,0x1b,181);s[0]=3;CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    s[0]=2;((uint8_t*)nb_gnss_tlv(&loc,1,1))[0]=0;CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    ((uint8_t*)nb_gnss_tlv(&loc,1,1))[0]=3;CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    n=message(in,6,2,1);CHECK(nb_gnss_message(in,n,&loc));
    s=(uint8_t*)nb_gnss_tlv(&loc,0x1e,8);nb_put64(s,NB_GNSS_B1I|NB_GNSS_B1C);
    CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    n=message(in,6,2,1);CHECK(nb_gnss_message(in,n,&loc));
    s=(uint8_t*)nb_gnss_tlv(&loc,0x1b,181)+1;nb_put16(s,1);CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    n=message(in,6,16,1);CHECK(nb_gnss_message(in,n,&loc));
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);CHECK(bytes&&info.count==16);
    /* Verify all real wire constellations and bands, including the LOC/PDS
     * B2aQ enum insertion. Unsupported bands never acquire a default. */
    const uint8_t systems[]={1,2,3,5,6,7,8},pds_systems[]={1,2,3,5,4,6,7};
    const uint8_t band_index[]={1,7,18,5,20,14,19},pds_bands[]={1,7,19,5,14,15,20};
    for(unsigned i=0;i<sizeof systems;++i){
        n=message(in,systems[i],1,1);CHECK(nb_gnss_message(in,n,&loc));
        nb_put64((uint8_t*)nb_gnss_tlv(&loc,0x1e,8),UINT64_C(1)<<(band_index[i]-1));
        bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
        CHECK(bytes&&nb_gnss_message(out,bytes,&pds));
        CHECK(*nb_gnss_tlv(&pds,3,1)==pds_systems[i]&&info.signal==pds_bands[i]);
    }
    /* Empty constellation slots preserve the receiver's whole-epoch
     * numbering. Unknown COMPASS never supplies BDS or a fabricated band. */
    n=message(in,4,0,1);CHECK(nb_gnss_message(in,n,&loc));
    nb_put64((uint8_t*)nb_gnss_tlv(&loc,0x1e,8),0);
    ((uint8_t*)nb_gnss_tlv(&loc,1,1))[0]=4;
    ((uint8_t*)nb_gnss_tlv(&loc,2,1))[0]=10;
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
    CHECK(bytes&&nb_gnss_message(out,bytes,&pds));
    CHECK(info.sequence==4&&info.maximum==10&&info.system==4&&!info.signals&&!info.count);
    CHECK(*nb_gnss_tlv(&pds,3,1)==0&&!nb_qmi_tlv(&pds,0x1e,0)&&!nb_qmi_tlv(&pds,0x11,0));
    CHECK(info.tick_present&&info.tick==120000&&info.resets_present&&info.clock_resets==5);
    /* Empty zero-band blocks of known systems also keep their real clocks;
     * a nonempty or multi-band block still cannot invent a signal. */
    ((uint8_t*)nb_gnss_tlv(&loc,3,4))[0]=6;
    ((uint8_t*)nb_gnss_tlv(&loc,0x18,18))[0]=6;
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
    CHECK(bytes&&nb_gnss_message(out,bytes,&pds)&&*nb_gnss_tlv(&pds,3,1)==4&&!info.signal);
    n=message(in,6,1,1);CHECK(nb_gnss_message(in,n,&loc));
    nb_put64((uint8_t*)nb_gnss_tlv(&loc,0x1e,8),0);CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    ((uint8_t*)nb_gnss_tlv(&loc,3,4))[0]=4;CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    n=message(in,5,1,1);CHECK(nb_gnss_message(in,n,&loc));
    s=(uint8_t*)nb_gnss_tlv(&loc,0x1b,91)+1;nb_put16(s,255);s[2]=8;
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
    CHECK(bytes&&nb_gnss_message(out,bytes,&pds));
    list=nb_gnss_tlv(&pds,0x1d,102);CHECK(list&&nb_le16(list+1)==255&&list[11]==8);
    /* The minor177 factory descriptor encodes gloTime as 15 packed bytes.
     * Its empty constellation slot must complete the receiver epoch too. */
    n=message(in,5,0,1);CHECK(nb_gnss_message(in,n,&loc));
    nb_put64((uint8_t*)nb_gnss_tlv(&loc,0x1e,8),0);
    CHECK(nb_gnss_tlv(&loc,0x19,15)!=0);
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
    CHECK(bytes&&info.system==5&&!info.count&&!info.signal&&info.tick==120000);
    CHECK(nb_gnss_message(out,bytes,&pds)&&!nb_qmi_tlv(&pds,0x12,0));
    uint8_t *glo=(uint8_t*)nb_gnss_tlv(&loc,0x19,15);
    CHECK(glo!=0);
    if(glo){
        /* Reject truncated/padded fields before any consumer output. */
        const unsigned glo_at=(unsigned)(glo-in)-3u;
        uint8_t malformed[4096];
        memcpy(malformed,in,n);nb_put16(malformed+glo_at+1u,14);
        memmove(malformed+glo_at+3u+14,malformed+glo_at+3u+15,n-glo_at-18u);
        nb_put16(malformed+5,n-1u-NB_QMI_HEADER);
        CHECK(nb_gnss_message(malformed,n-1u,&loc));
        CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
        for(unsigned extra_bytes=1;extra_bytes<=4;++extra_bytes){
            memcpy(malformed,in,glo_at+18u);
            memset(malformed+glo_at+18u,0,extra_bytes);
            memcpy(malformed+glo_at+18u+extra_bytes,in+glo_at+18u,n-glo_at-18u);
            nb_put16(malformed+glo_at+1u,15u+extra_bytes);
            nb_put16(malformed+5,n+extra_bytes-NB_QMI_HEADER);
            CHECK(nb_gnss_message(malformed,n+extra_bytes,&loc));
            CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
        }
    }
    /* Real optional extended measurements keep their order and original
     * per-SV fields. Never label a partial sub-sequence as complete. */
    n=message(in,6,16,1);CHECK(nb_gnss_message(in,n,&loc));
    uint8_t extra[1+8*NB_GNSS_LOC_MEASUREMENT_BYTES]={8};
    const uint8_t *original=nb_gnss_tlv(&loc,0x1b,1441)+1;
    for(unsigned i=0;i<8;++i){memcpy(extra+1+i*90,original,90);nb_put16(extra+1+i*90,217u+i);}
    in[n++]=0x2e;nb_put16(in+n,sizeof extra);n+=2;memcpy(in+n,extra,sizeof extra);n+=sizeof extra;
    nb_put16(in+5,n-NB_QMI_HEADER);CHECK(nb_gnss_message(in,n,&loc));
    bytes=nb_gnss_pds_measurement(&loc,out,sizeof out,&info);
    CHECK(bytes&&nb_gnss_message(out,bytes,&pds)&&info.count==24);
    list=nb_gnss_tlv(&pds,0x1d,2425);CHECK(list&&list[0]==24&&nb_le16(list+1+23*101)==224);
    in[n++]=0x36;nb_put16(in+n,1);n+=2;in[n++]=1;
    in[n++]=0x37;nb_put16(in+n,1);n+=2;in[n++]=2;
    nb_put16(in+5,n-NB_QMI_HEADER);CHECK(nb_gnss_message(in,n,&loc));
    CHECK(!nb_gnss_pds_measurement(&loc,out,sizeof out,&info));
    in[n-1]=1;CHECK(nb_gnss_message(in,n,&loc));
    CHECK(nb_gnss_pds_measurement(&loc,out,sizeof out,&info)&&info.count==24);
    epoch(0,120000,5);epoch(0,121000,4);
    if(argc==3)epoch(argv[2],120000,5);
    printf("GNSS measurement %u checks, %u failures\n",checks,failures);return failures?1:0;
}
