// SPDX-License-Identifier: BSD-2-Clause-Patent
// Portable wire facts cross-checked against SNS source 20bb8ae36c93fc16bbadda0e0a83f930c0c8a271
// and current Android004 libQshQmiIDL (IDL1.7/tool6). The earlier libsnsapi
// IDL1.4 comparison is historical; the selected normal wire fields are equal.
// No sdm670 controller code, hardware addresses, generated readings or link owner.
#include "Tcs3760Sns.h"
#include "QcomQrtr.h"
#include <string.h>

enum { SnsLookupRequest=512, SnsLookupEvent=768, SnsOnChange=514, SnsCalibrationEvent=1022,
       SnsStreamConfig=513, SnsSampleEvent=1025, SnsDisable=10, SnsErrorEvent=130 };
static const char datatype[]="ambient_light";
static const Tcs3760SnsUid lookup_uid={TCS3760_SNS_LOOKUP_WORD,TCS3760_SNS_LOOKUP_WORD};
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0]|(uint16_t)p[1]<<8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p)|(uint32_t)rd16(p+2)<<16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p)|(uint64_t)rd32(p+4)<<32; }
static void wr16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t *p,uint32_t v) { wr16(p,(uint16_t)v);wr16(p+2,(uint16_t)(v>>16)); }
static void wr64(uint8_t *p,uint64_t v) { wr32(p,(uint32_t)v);wr32(p+4,(uint32_t)(v>>32)); }
static bool uid_equal(Tcs3760SnsUid a,Tcs3760SnsUid b) { return a.low==b.low&&a.high==b.high; }

// This bounded cursor is private to the TCS3760 protobuf subset. QMI/QRTR
// framing remains the existing QcomQrtr interface, not another transport.
typedef struct { const uint8_t *p;size_t left; } Pb;
typedef struct { uint32_t number,wire;uint64_t value;const uint8_t *data;size_t bytes; } Field;
static int varint(Pb *p,uint64_t *value) {
    uint64_t result=0;
    for (unsigned i=0;i<10;++i) {
        if (!p->left) return Tcs3760SnsMalformed;
        uint8_t b=*p->p++;--p->left;
        if (i==9 && b>1) return Tcs3760SnsMalformed;
        result|=(uint64_t)(b&127u)<<(7*i);
        if (!(b&128u)) {*value=result;return Tcs3760SnsOkay;}
    }
    return Tcs3760SnsMalformed;
}
static int field(Pb *p,Field *f) {
    uint64_t key=0,n=0;int r=varint(p,&key);if (r) return r;
    if (!(key>>3) || (key>>3)>UINT32_C(0x1fffffff)) return Tcs3760SnsMalformed;
    *f=(Field){(uint32_t)(key>>3),(uint32_t)(key&7u),0,0,0};
    switch (f->wire) {
    case 0:return varint(p,&f->value);
    case 1:n=8;break;
    case 2:r=varint(p,&n);if (r) return r;break;
    case 5:n=4;break;
    default:return Tcs3760SnsUnsupported; // No groups in the proved SNS subset.
    }
    if (n>p->left) return Tcs3760SnsMalformed;
    f->data=p->p;f->bytes=(size_t)n;p->p+=n;p->left-=n;
    return Tcs3760SnsOkay;
}
static int uid_decode(const uint8_t *data,size_t bytes,Tcs3760SnsUid *out) {
    Pb p={data,bytes};Tcs3760SnsUid u={0,0};unsigned seen=0;
    while (p.left) {
        Field f;int r=field(&p,&f);if (r) return r;
        if (f.number==1 || f.number==2) {
            if (f.wire!=1) return Tcs3760SnsMalformed;
            if (f.number==1) {u.low=rd64(f.data);seen|=1;} else {u.high=rd64(f.data);seen|=2;}
        }
    }
    if (seen!=3) return Tcs3760SnsMalformed;
    *out=u;return Tcs3760SnsOkay;
}
typedef struct { uint32_t id;uint64_t timestamp;const uint8_t *payload;size_t bytes; } Event;
static int event_decode(const uint8_t *data,size_t bytes,Event *out) {
    Pb p={data,bytes};Event e={0,0,0,0};unsigned seen=0;
    while (p.left) {
        Field f;int r=field(&p,&f);if (r) return r;
        if (f.number==1) {if (f.wire!=5) return Tcs3760SnsMalformed;e.id=rd32(f.data);seen|=1;}
        else if (f.number==2) {if (f.wire!=1) return Tcs3760SnsMalformed;e.timestamp=rd64(f.data);seen|=2;}
        else if (f.number==3) {if (f.wire!=2) return Tcs3760SnsMalformed;e.payload=f.data;e.bytes=f.bytes;seen|=4;}
    }
    if (seen!=7) return Tcs3760SnsMalformed;
    *out=e;return Tcs3760SnsOkay;
}
static int suid_event(const uint8_t *data,size_t bytes,Tcs3760SnsUid *uid,bool *valid) {
    Pb p={data,bytes};bool name_seen=false,named=false,found=false;Tcs3760SnsUid selected={0,0};
    while (p.left) {
        Field f;int r=field(&p,&f);if (r) return r;
        if (f.number==1) {
            if (f.wire!=2) return Tcs3760SnsMalformed;
            name_seen=true;
            named=f.bytes==sizeof(datatype)-1 && !memcmp(f.data,datatype,sizeof(datatype)-1);
        } else if (f.number==2) {
            Tcs3760SnsUid u;if (f.wire!=2) return Tcs3760SnsMalformed;
            r=uid_decode(f.data,f.bytes,&u);if (r) return r;
            // default_only=true asks for one default sensor, not an arbitrary
            // choice among distinct sensors. Repeated identical UIDs are safe.
            if (found && !uid_equal(selected,u)) return Tcs3760SnsUnsupported;
            selected=u;found=true;
        }
    }
    if (!name_seen) return Tcs3760SnsMalformed;
    if (!named) return Tcs3760SnsCorrelation;
    *valid=found;if (found) *uid=selected;
    return Tcs3760SnsOkay;
}
static int sensor_event(const uint8_t *data,size_t bytes,uint32_t *lux,uint32_t *status,bool *valid) {
    Pb p={data,bytes};bool got_data=false,got_status=false;uint32_t first=0,accuracy=0;
    while (p.left) {
        Field f;int r=field(&p,&f);if (r) return r;
        if (f.number==1) {
            if (f.wire!=5 && f.wire!=2) return Tcs3760SnsMalformed;
            if (f.bytes%4) return Tcs3760SnsMalformed;
            if (f.bytes && !got_data) {first=rd32(f.data);got_data=true;}
        } else if (f.number==2) {
            if (f.wire!=0) return Tcs3760SnsMalformed;
            if (f.value>3) return Tcs3760SnsUnsupported;
            accuracy=(uint32_t)f.value;got_status=true;
        }
    }
    if (!got_status) return Tcs3760SnsMalformed;
    if (got_data) {
        // Integer bit checks avoid floating-point instructions in the kernel.
        // Preserve real zero, including negative zero; never turn a bad word
        // into zero lux or apply a fabricated fixed-point scaling.
        if ((first&UINT32_C(0x7f800000))==UINT32_C(0x7f800000) ||
            ((first&UINT32_C(0x80000000)) && (first&UINT32_C(0x7fffffff)))) return Tcs3760SnsMalformed;
        *lux=first;*status=accuracy;
    }
    *valid=got_data;return Tcs3760SnsOkay;
}
static int error_event(const uint8_t *data,size_t bytes,uint32_t *error) {
    Pb p={data,bytes};bool seen=false;uint32_t code=0;
    while (p.left) {
        Field f;int r=field(&p,&f);if (r) return r;
        if (f.number==1) {
            if (f.wire!=0 || f.value>UINT32_MAX) return Tcs3760SnsMalformed;
            code=(uint32_t)f.value;seen=true;
        }
    }
    if (!seen) return Tcs3760SnsMalformed;
    *error=code;return Tcs3760SnsOkay;
}
static int calibration_event(const uint8_t *data,size_t bytes) {
    // Qualcomm sns_cal.proto:1022 conveys the factory calibration already
    // applied by this physical sensor. Its scale_factor is never a lux sample.
    Pb p={data,bytes};bool status_seen=false;
    while (p.left) {
        Field f;int r=field(&p,&f);if (r) return r;
        if (f.number>=1 && f.number<=3) {
            if ((f.wire!=5 && f.wire!=2) || f.bytes%4) return Tcs3760SnsMalformed;
        } else if (f.number==4) {
            if (f.wire!=0) return Tcs3760SnsMalformed;
            if (f.value>3) return Tcs3760SnsUnsupported;
            status_seen=true;
        } else if (f.number==5 && f.wire!=5) return Tcs3760SnsMalformed;
    }
    return status_seen ? Tcs3760SnsOkay:Tcs3760SnsMalformed;
}
static int qmi_view(const uint8_t *qmi,size_t bytes,uint8_t type,uint16_t id,uint32_t limit,QcomQmiMessage *m) {
    if (!qmi || bytes>UINT32_MAX) return Tcs3760SnsArgument;
    if (qcom_qmi_parse(qmi,(uint32_t)bytes,m)) return Tcs3760SnsMalformed;
    if (m->type!=type || m->message!=id) return Tcs3760SnsUnsupported;
    if (m->length>limit || bytes!=QCOM_QMI_HEADER+m->length) return Tcs3760SnsMalformed;
    return Tcs3760SnsOkay;
}
// Validate the whole profile body before retrieving fields with qcom_qmi_tlv.
// The existing helper returns a view early; a malformed trailing TLV must not
// cause a partially decoded report to emit a sample.
static int profile_tlvs(const QcomQmiMessage *m,uint32_t known) {
    uint32_t at=0,seen=0;
    while (at<m->length) {
        if (m->length-at<3) return Tcs3760SnsMalformed;
        uint8_t id=m->tlvs[at];uint32_t n=rd16(m->tlvs+at+1);
        if (n>m->length-at-3) return Tcs3760SnsMalformed;
        uint32_t bit=id<32 ? UINT32_C(1)<<id:0;
        if (known&bit) {if (seen&bit) return Tcs3760SnsMalformed;seen|=bit;}
        at+=3+n;
    }
    return Tcs3760SnsOkay;
}
int tcs3760_sns_decode_response(const uint8_t *qmi,size_t bytes,uint16_t transaction,Tcs3760SnsResponse *out) {
    if (!out) return Tcs3760SnsArgument;
    QcomQmiMessage m;int r=qmi_view(qmi,bytes,QCOM_QMI_RESPONSE,0x20,25,&m);if (r) return r;
    if (m.transaction!=transaction) return Tcs3760SnsCorrelation;
    r=profile_tlvs(&m,(UINT32_C(1)<<2)|(UINT32_C(1)<<16)|(UINT32_C(1)<<17));if (r) return r;
    Tcs3760SnsResponse result={0};result.transaction=m.transaction;uint32_t n=0;
    const uint8_t *v=qcom_qmi_tlv(&m,2,&n);
    if (!v || n!=4) return Tcs3760SnsMalformed;
    result.qmi_result=rd16(v);result.qmi_error=rd16(v+2);
    v=qcom_qmi_tlv(&m,0x10,&n);if (v) {if (n!=8) return Tcs3760SnsMalformed;result.client_id_valid=true;result.client_id=rd64(v);}
    v=qcom_qmi_tlv(&m,0x11,&n);if (v) {if (n!=4) return Tcs3760SnsMalformed;result.result_valid=true;result.result=rd32(v);}
    *out=result;
    return result.qmi_result || result.qmi_error || (result.result_valid&&result.result) ? Tcs3760SnsRemote:Tcs3760SnsOkay;
}
int tcs3760_sns_decode_report(const uint8_t *qmi,size_t bytes,const Tcs3760SnsMatch *match,Tcs3760SnsReport *out) {
    if (!out) return Tcs3760SnsArgument;
    QcomQmiMessage m;int r=qmi_view(qmi,bytes,QCOM_QMI_INDICATION,0x21,1016,&m);if (r) return r;
    r=profile_tlvs(&m,(UINT32_C(1)<<1)|(UINT32_C(1)<<2));if (r) return r;
    Tcs3760SnsReport result={0};uint32_t n=0;
    const uint8_t *v=qcom_qmi_tlv(&m,1,&n);if (!v || n!=8) return Tcs3760SnsMalformed;
    result.client_id=rd64(v);
    if (match && match->client_id_valid && match->client_id!=result.client_id) return Tcs3760SnsCorrelation;
    v=qcom_qmi_tlv(&m,2,&n);if (!v || n<2 || n-2>TCS3760_SNS_PAYLOAD_MAX || rd16(v)!=n-2) return Tcs3760SnsMalformed;
    Pb envelope={v+2,n-2};bool source_seen=false;
    // First pass establishes the final singular source UID before routing IDs.
    while (envelope.left) {
        Field f;r=field(&envelope,&f);if (r) return r;
        if (f.number==1) {if (f.wire!=2) return Tcs3760SnsMalformed;r=uid_decode(f.data,f.bytes,&result.source);if (r) return r;source_seen=true;}
        else if (f.number==2 && f.wire!=2) return Tcs3760SnsMalformed;
    }
    if (!source_seen) return Tcs3760SnsMalformed;
    const bool lookup=uid_equal(result.source,lookup_uid);
    if (!lookup && (!match || !match->sensor_uid_valid || !uid_equal(result.source,match->sensor_uid))) return Tcs3760SnsCorrelation;
    envelope=(Pb){v+2,n-2};bool recognized=false;unsigned unsupported=0;
    while (envelope.left) {
        Field f;r=field(&envelope,&f);if (r) return r;
        if (f.number!=2) continue;
        Event e;r=event_decode(f.data,f.bytes,&e);if (r) return r;
        if (e.id==SnsErrorEvent) {
            uint32_t error=0;r=error_event(e.payload,e.bytes,&error);if (r) return r;
            if (!result.error_valid || error) result.error=error;
            result.error_valid=true;
            recognized=true;
        } else if (lookup && e.id==SnsLookupEvent) {
            r=suid_event(e.payload,e.bytes,&result.uid,&result.uid_valid);if (r) return r;
            result.uid_event=true;
            recognized=true;
        } else if (!lookup && e.id==SnsCalibrationEvent) {
            r=calibration_event(e.payload,e.bytes);if (r) return r;
            ++result.non_sample_events;
            recognized=true;
        } else if (!lookup && e.id==SnsSampleEvent) {
            uint32_t lux=0,status=0;bool valid=false;
            r=sensor_event(e.payload,e.bytes,&lux,&status,&valid);if (r) return r;
            recognized=true;
            if (valid) {
                ++result.samples;
                if (!result.sample_valid || e.timestamp>=result.sample.timestamp)
                    result.sample=(Tcs3760SnsSample){result.source,result.client_id,e.timestamp,lux,status};
                result.sample_valid=true;
            }
        } else if (!lookup && (e.id==SnsLookupEvent || e.id==SnsOnChange || e.id==SnsStreamConfig)) {
            ++result.non_sample_events; // Configuration, never a lookup UID or lux.
            recognized=true;
        } else {++result.non_sample_events;++unsupported;}
    }
    if (unsupported && !recognized) return Tcs3760SnsUnsupported;
    *out=result;
    if (result.error_valid && result.error) return Tcs3760SnsRemote;
    return result.uid_valid || result.sample_valid ? Tcs3760SnsOkay:Tcs3760SnsNoData;
}
int tcs3760_sns_receive_report(const uint8_t *qmi,size_t bytes,const Tcs3760SnsMatch *match,const Tcs3760SnsCallbacks *callbacks) {
    if (!callbacks) return Tcs3760SnsArgument;
    Tcs3760SnsReport report;int r=tcs3760_sns_decode_report(qmi,bytes,match,&report);
    if (r==Tcs3760SnsRemote) {if (callbacks->error) callbacks->error(callbacks->context,report.error);return r;}
    if (r!=Tcs3760SnsOkay) return r;
    if (report.uid_valid && callbacks->uid) callbacks->uid(callbacks->context,&report.uid);
    if (report.sample_valid && callbacks->sample) callbacks->sample(callbacks->context,&report.sample);
    return r;
}
static int encode(Tcs3760SnsUid uid,uint32_t id,uint8_t delivery,const uint8_t *payload,size_t payload_bytes,
                  uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes) {
    if (!out || !bytes) return Tcs3760SnsArgument;
    uint8_t pb[64],qmi[TCS3760_SNS_REQUEST_CAPACITY];size_t at=0;
    pb[at++]=0x0a;pb[at++]=18;pb[at++]=0x09;wr64(pb+at,uid.low);at+=8;
    pb[at++]=0x11;wr64(pb+at,uid.high);at+=8;
    pb[at++]=0x15;wr32(pb+at,id);at+=4; // fixed32, not a protobuf varint.
    pb[at++]=0x1a;pb[at++]=4;pb[at++]=8;pb[at++]=1;pb[at++]=16;pb[at++]=delivery;
    pb[at++]=0x22;pb[at++]=(uint8_t)(payload_bytes ? payload_bytes+2:0);
    if (payload_bytes) {pb[at++]=0x12;pb[at++]=(uint8_t)payload_bytes;memcpy(pb+at,payload,payload_bytes);at+=payload_bytes;}
    const size_t total=QCOM_QMI_HEADER+5+at;
    if (total>capacity) return Tcs3760SnsCapacity;
    qmi[0]=QCOM_QMI_REQUEST;wr16(qmi+1,transaction);wr16(qmi+3,0x20);wr16(qmi+5,(uint16_t)(5+at));
    qmi[7]=1;wr16(qmi+8,(uint16_t)(2+at));wr16(qmi+10,(uint16_t)at);memcpy(qmi+12,pb,at);
    // use_jumbo_report is omitted: its proved default is normal reports.
    memcpy(out,qmi,total);*bytes=total;return Tcs3760SnsOkay;
}
int tcs3760_sns_encode_lookup(uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes) {
    uint8_t payload[19]={0x0a,13};memcpy(payload+2,datatype,13);
    payload[15]=0x10;payload[16]=1;payload[17]=0x18;payload[18]=1;
    return encode(lookup_uid,SnsLookupRequest,0,payload,sizeof payload,transaction,out,capacity,bytes);
}
int tcs3760_sns_encode_on_change(Tcs3760SnsUid uid,uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes) {
    return encode(uid,SnsOnChange,1,0,0,transaction,out,capacity,bytes);
}
int tcs3760_sns_encode_disable(Tcs3760SnsUid uid,uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes) {
    return encode(uid,SnsDisable,1,0,0,transaction,out,capacity,bytes);
}
