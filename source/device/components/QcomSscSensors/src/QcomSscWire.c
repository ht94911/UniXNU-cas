// SPDX-License-Identifier: BSD-2-Clause-Patent
// Common normal SNS_CLIENT IDL1.7 wire subset, from nezha ALS017 (374f5b6).
// Cross-checked with stock libQshQmiIDL and SNS protobufs. No remote owner here.
#include "QcomSscWire.h"
#include "QcomQrtr.h"
#include <string.h>
typedef struct { const uint8_t *p;size_t left; } Pb;
typedef struct { uint32_t number,wire;uint64_t value;const uint8_t *data;size_t bytes; } Field;
static uint16_t rd16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t rd32(const uint8_t *p){return (uint32_t)rd16(p)|(uint32_t)rd16(p+2)<<16;}
static uint64_t rd64(const uint8_t *p){return (uint64_t)rd32(p)|(uint64_t)rd32(p+4)<<32;}
static void wr16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void wr32(uint8_t *p,uint32_t v){wr16(p,(uint16_t)v);wr16(p+2,(uint16_t)(v>>16));}
static void wr64(uint8_t *p,uint64_t v){wr32(p,(uint32_t)v);wr32(p+4,(uint32_t)(v>>32));}
bool qssc_uid_equal(QsscUid a,QsscUid b){return a.low==b.low&&a.high==b.high;}
bool qssc_float_finite(uint32_t bits){return (bits&0x7f800000u)!=0x7f800000u;}
static int varint(Pb *p,uint64_t *v){
    uint64_t n=0;
    for(unsigned i=0;i<10;i++){
        if(!p->left)return QsscWire;
        uint8_t b=*p->p++;p->left--;
        if(i==9&&b>1)return QsscWire;
        n|=(uint64_t)(b&127)<<(7*i);
        if(!(b&128)){*v=n;return QsscOkay;}
    }return QsscWire;
}
static int field(Pb *p,Field *f){
    uint64_t key,n=0;int r=varint(p,&key);if(r)return r;
    if(!(key>>3)||(key>>3)>0x1fffffffu)return QsscWire;
    *f=(Field){(uint32_t)(key>>3),(uint32_t)(key&7),0,0,0};
    switch(f->wire){
    case 0:return varint(p,&f->value);
    case 1:n=8;break;
    case 2:r=varint(p,&n);if(r)return r;break;
    case 5:n=4;break;
    default:return QsscUnsupported;
    }
    if(n>p->left)return QsscWire;
    f->data=p->p;f->bytes=(size_t)n;p->p+=n;p->left-=n;return QsscOkay;
}
static int uid(const uint8_t *data,size_t bytes,QsscUid *out){
    Pb p={data,bytes};QsscUid u={0};unsigned seen=0;
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number==1||f.number==2){
            unsigned bit=1u<<(f.number-1);if(f.wire!=1||(seen&bit))return QsscWire;
            if(f.number==1)u.low=rd64(f.data);else u.high=rd64(f.data);seen|=bit;
        }
    }if(seen!=3)return QsscWire;*out=u;return QsscOkay;
}
static int event(const uint8_t *data,size_t bytes,QsscEvent *out){
    Pb p={data,bytes};QsscEvent e={0};unsigned seen=0;
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number>=1&&f.number<=3){unsigned bit=1u<<(f.number-1);if(seen&bit)return QsscWire;
            if(f.number==1){if(f.wire!=5)return QsscWire;e.id=rd32(f.data);}
            if(f.number==2){if(f.wire!=1)return QsscWire;e.ticks=rd64(f.data);}
            if(f.number==3){if(f.wire!=2)return QsscWire;e.payload=f.data;e.bytes=f.bytes;}
            seen|=bit;
        }
    }if(seen!=7)return QsscWire;*out=e;return QsscOkay;
}
static int view(const uint8_t *qmi,size_t bytes,uint8_t type,uint16_t id,QcomQmiMessage *m){
    if(!qmi||bytes>QSSC_REPORT_BYTES||bytes<QCOM_QMI_HEADER)return QsscArgument;
    if(qcom_qmi_parse(qmi,(uint32_t)bytes,m)||bytes!=QCOM_QMI_HEADER+m->length)return QsscWire;
    if(m->type!=type||m->message!=id)return QsscUnsupported;
    uint32_t at=0,seen=0;
    while(at<m->length){
        if(m->length-at<3)return QsscWire;
        unsigned tag=m->tlvs[at];uint32_t n=rd16(m->tlvs+at+1);if(n>m->length-at-3)return QsscWire;
        uint32_t bit=tag<32?1u<<tag:0;
        if(bit&&(seen&bit))return QsscWire;
        seen|=bit;at+=3+n;
    }return QsscOkay;
}
int qssc_decode_response(const uint8_t *qmi,size_t bytes,uint16_t transaction,QsscResponse *out){
    if(!out)return QsscArgument;QcomQmiMessage m;int r=view(qmi,bytes,QCOM_QMI_RESPONSE,0x20,&m);if(r)return r;
    if(m.transaction!=transaction)return QsscIdentity;
    QsscResponse a={0};a.transaction=m.transaction;uint32_t n;
    const uint8_t *v=qcom_qmi_tlv(&m,2,&n);if(!v||n!=4)return QsscWire;
    a.result=rd16(v);a.error=rd16(v+2);
    v=qcom_qmi_tlv(&m,16,&n);if(v){if(n!=8)return QsscWire;a.client_id_valid=true;a.client_id=rd64(v);}
    v=qcom_qmi_tlv(&m,17,&n);if(v){if(n!=4)return QsscWire;a.service_result_valid=true;a.service_result=rd32(v);}
    *out=a;return a.result||a.error||(a.service_result_valid&&a.service_result)?QsscRemote:QsscOkay;
}
int qssc_decode_report(const uint8_t *qmi,size_t bytes,QsscReport *out){
    if(!out)return QsscArgument;QcomQmiMessage m;int r=view(qmi,bytes,QCOM_QMI_INDICATION,0x21,&m);if(r)return r;
    uint32_t n;const uint8_t *v=qcom_qmi_tlv(&m,1,&n);if(!v||n!=8)return QsscWire;
    QsscReport a={0};a.client_id=rd64(v);
    v=qcom_qmi_tlv(&m,2,&n);if(!v||n<2||n-2>QSSC_PAYLOAD_BYTES||rd16(v)!=n-2)return QsscWire;
    Pb p={v+2,n-2};bool source=false;
    while(p.left){Field f;r=field(&p,&f);if(r)return r;
        if(f.number==1){if(source||f.wire!=2)return QsscWire;r=uid(f.data,f.bytes,&a.source);if(r)return r;source=true;}
        else if(f.number==2){if(f.wire!=2)return QsscWire;if(a.count==QSSC_MAX_EVENTS)return QsscCapacity;
            r=event(f.data,f.bytes,&a.events[a.count]);if(r)return r;a.count++;}
    }if(!source)return QsscWire;*out=a;return a.count?QsscOkay:QsscNoData;
}
int qssc_decode_lookup(const QsscEvent *e,const char *datatype,QsscUid *out,bool *present){
    if(!e||!datatype||!out||!present||e->id!=QsscLookupEvent)return QsscArgument;
    Pb p={e->payload,e->bytes};bool named=false,found=false;QsscUid selected={0};
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number==1){if(named||f.wire!=2)return QsscWire;
            if(f.bytes!=strlen(datatype)||memcmp(f.data,datatype,f.bytes))return QsscIdentity;named=true;}
        else if(f.number==2){QsscUid u;if(f.wire!=2)return QsscWire;r=uid(f.data,f.bytes,&u);if(r)return r;
            if(found&&!qssc_uid_equal(selected,u))return QsscIdentity;selected=u;found=true;}
    }if(!named)return QsscWire;*present=found;if(found)*out=selected;return QsscOkay;
}
static int floats(const Field *f,uint32_t *out,unsigned capacity,uint32_t *count){
    if((f->wire!=5&&f->wire!=2)||f->bytes%4)return QsscWire;
    size_t n=f->bytes/4;if(n>capacity-*count)return QsscCapacity;
    for(size_t i=0;i<n;i++){uint32_t v=rd32(f->data+4*i);if(!qssc_float_finite(v))return QsscWire;out[(*count)++]=v;}
    return QsscOkay;
}
int qssc_decode_vector(const QsscEvent *e,QsscVector *out){
    if(!e||!out||e->id!=QsscSampleEvent)return QsscArgument;
    Pb p={e->payload,e->bytes};QsscVector a={0};bool status=false;
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number==1){r=floats(&f,a.bits,QSSC_MAX_VALUES,&a.count);if(r)return r;}
        else if(f.number==2){if(status||f.wire!=0||f.value>3)return QsscWire;a.status=(uint32_t)f.value;status=true;}
    }if(!status)return QsscWire;*out=a;return a.count?QsscOkay:QsscNoData;
}
int qssc_decode_proximity(const QsscEvent *e,QsscProximity *out){
    if(!e||!out||e->id!=QsscProximityEvent)return QsscArgument;
    Pb p={e->payload,e->bytes};QsscProximity a={0};unsigned seen=0;
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number>=1&&f.number<=3){unsigned bit=1u<<(f.number-1);
            if(seen&bit||f.wire!=0||f.value>UINT32_MAX)return QsscWire;
            if(f.number==1){if(f.value>1)return QsscWire;a.near=(uint32_t)f.value;}
            if(f.number==2)a.raw_adc=(uint32_t)f.value;
            if(f.number==3){if(f.value>3)return QsscWire;a.status=(uint32_t)f.value;}seen|=bit;
        }
    }if(seen!=7)return QsscWire;*out=a;return QsscOkay;
}
int qssc_decode_calibration(const QsscEvent *e,QsscCalibration *out){
    if(!e||!out||e->id!=QsscCalibrationEvent)return QsscArgument;
    Pb p={e->payload,e->bytes};QsscCalibration a={0};bool status=false;
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number==1)r=floats(&f,a.bias,3,&a.bias_count);
        else if(f.number==2)r=floats(&f,a.scale,3,&a.scale_count);
        else if(f.number==3)r=floats(&f,a.matrix,9,&a.matrix_count);
        else if(f.number==4){if(status||f.wire!=0||f.value>3)return QsscWire;a.status=(uint32_t)f.value;status=true;}
        else if(f.number==5){if(a.cal_id_valid||f.wire!=5)return QsscWire;a.cal_id_valid=true;a.cal_id=rd32(f.data);}
        if(r)return r;
    }
    // Physical scalar devices such as TCS3760 publish1-axis factory metadata;
    // the independent IMU cal clients later require their own3-axis bias.
    if(!status||(a.bias_count&&a.bias_count!=1&&a.bias_count!=3)||
       (a.scale_count&&a.scale_count!=1&&a.scale_count!=3)||
       (a.matrix_count&&a.matrix_count!=1&&a.matrix_count!=9))return QsscWire;
    *out=a;return QsscOkay;
}
int qssc_decode_error(const QsscEvent *e,uint32_t *out){
    if(!e||!out||e->id!=QsscErrorEvent)return QsscArgument;
    Pb p={e->payload,e->bytes};bool seen=false;uint32_t a=0;
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number==1){if(seen||f.wire!=0||f.value>UINT32_MAX)return QsscWire;a=(uint32_t)f.value;seen=true;}}
    if(!seen)return QsscWire;*out=a;return QsscOkay;
}
static size_t put_varint(uint8_t *p,uint64_t v){size_t n=0;do{p[n]=(uint8_t)(v&127);v>>=7;if(v)p[n]|=128;n++;}while(v);return n;}
int qssc_encode_request(QsscUid u,uint32_t id,bool wakeup,uint32_t batch_us,const uint8_t *payload,
 size_t payload_bytes,uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes){
    if(!out||!bytes||(!payload&&payload_bytes)||payload_bytes>64)return QsscArgument;
    uint8_t pb[160],request[100],qmi[QSSC_REQUEST_BYTES];size_t at=0,rn=0;
    // Standard request batching; zero latency is explicitly no batching.
    uint8_t batch[8];batch[0]=8;size_t bn=1+put_varint(batch+1,batch_us);
    request[rn++]=0x0a;request[rn++]=(uint8_t)bn;memcpy(request+rn,batch,bn);rn+=bn;
    if(payload_bytes){request[rn++]=0x12;request[rn++]=(uint8_t)payload_bytes;memcpy(request+rn,payload,payload_bytes);rn+=payload_bytes;}
    pb[at++]=0x0a;pb[at++]=18;pb[at++]=9;wr64(pb+at,u.low);at+=8;pb[at++]=17;wr64(pb+at,u.high);at+=8;
    pb[at++]=0x15;wr32(pb+at,id);at+=4;
    pb[at++]=0x1a;pb[at++]=4;pb[at++]=8;pb[at++]=1;pb[at++]=16;pb[at++]=wakeup?0:1;
    pb[at++]=0x22;pb[at++]=(uint8_t)rn;memcpy(pb+at,request,rn);at+=rn;
    size_t total=12+at;if(total>capacity||total>sizeof qmi)return QsscCapacity;
    qmi[0]=QCOM_QMI_REQUEST;wr16(qmi+1,transaction);wr16(qmi+3,32);wr16(qmi+5,(uint16_t)(total-7));
    qmi[7]=1;wr16(qmi+8,(uint16_t)(at+2));wr16(qmi+10,(uint16_t)at);memcpy(qmi+12,pb,at);
    // use_jumbo_report omitted: the actual IDL defaults to normal reports.
    memcpy(out,qmi,total);*bytes=total;return QsscOkay;
}
int qssc_encode_lookup(const char *datatype,uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes){
    if(!datatype)return QsscArgument;size_t n=strlen(datatype);if(!n||n>48)return QsscArgument;
    uint8_t payload[54]={10,(uint8_t)n};memcpy(payload+2,datatype,n);
    payload[n+2]=16;payload[n+3]=1;payload[n+4]=24;payload[n+5]=1;
    QsscUid u={QSSC_LOOKUP_WORD,QSSC_LOOKUP_WORD};return qssc_encode_request(u,QsscLookupRequest,true,0,payload,n+6,transaction,out,capacity,bytes);
}
int qssc_encode_stream(QsscUid u,uint32_t rate,bool wakeup,uint32_t batch,uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes){
    if(!qssc_float_finite(rate)||(rate>>31)||!(rate&0x7fffffffu))return QsscArgument;
    uint8_t p[5]={13};wr32(p+1,rate);return qssc_encode_request(u,QsscStreamRequest,wakeup,batch,p,5,transaction,out,capacity,bytes);
}
static int string_value(const uint8_t *p,size_t n,char *out,size_t capacity){
    if(n&&p[n-1]==0)n--; // Stock strings include a single terminal NUL.
    if(n>=capacity)return QsscCapacity;
    for(size_t i=0;i<n;i++)if(!p[i])return QsscWire;
    memcpy(out,p,n);out[n]=0;return QsscOkay;
}
static int attribute_value(uint32_t id,const uint8_t *data,size_t bytes,QsscAttributes *a,unsigned depth){
    if(depth>4)return QsscWire;Pb p={data,bytes};
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;
        if(f.number!=1)continue;if(f.wire!=2)return QsscWire;
        Pb value={f.data,f.bytes};unsigned one=0;
        while(value.left){Field v;r=field(&value,&v);if(r)return r;if(v.number>5)continue;
            if(++one>1)return QsscWire;
            if(v.number==1){if(v.wire!=2)return QsscWire;r=attribute_value(id,v.data,v.bytes,a,depth+1);if(r)return r;}
            else if(v.number==2){if(v.wire!=2)return QsscWire;
                if(id==0){r=string_value(v.data,v.bytes,a->name,sizeof a->name);a->name_valid=!r;}
                else if(id==1)r=string_value(v.data,v.bytes,a->vendor,sizeof a->vendor);
                else if(id==2){r=string_value(v.data,v.bytes,a->datatype,sizeof a->datatype);a->type_valid=!r;}
                else if(id==5){size_t at=strlen(a->api);if(at)at++;if(at>=sizeof a->api)return QsscCapacity;
                    r=string_value(v.data,v.bytes,a->api+at,sizeof a->api-at);if(!r&&at)a->api[at-1]=';';}
                if(r)return r;
            }else if(v.number==3){if(v.wire!=5)return QsscWire;
                if(id==6){uint32_t rate=rd32(v.data);if(!qssc_float_finite(rate)||rate>>31||!rate)return QsscWire;
                    if(a->rate_count==QSSC_MAX_RATES)return QsscCapacity;a->rates[a->rate_count++]=rate;}}
            else if(v.number==4){if(v.wire!=1)return QsscWire;
                if(id==16){uint64_t stream=rd64(v.data);if(stream>2)return QsscWire;a->stream_type=(uint32_t)stream;a->stream_valid=true;}}
            else if(v.number==5){if(v.wire!=0||v.value>1)return QsscWire;
                if(id==3){a->available=v.value!=0;a->available_valid=true;}}
        }
        if(!one)return QsscWire;
    }return QsscOkay;
}
int qssc_decode_attributes(const QsscEvent *e,QsscAttributes *out){
    if(!e||!out||e->id!=QsscAttrEvent)return QsscArgument;
    Pb p={e->payload,e->bytes};QsscAttributes a={0};
    while(p.left){Field f;int r=field(&p,&f);if(r)return r;if(f.number!=1)continue;if(f.wire!=2)return QsscWire;
        Pb attr={f.data,f.bytes};uint32_t id=0;bool id_seen=false,value_seen=false;const uint8_t *value=0;size_t n=0;
        while(attr.left){Field v;r=field(&attr,&v);if(r)return r;
            if(v.number==1){if(id_seen||v.wire!=0||v.value>UINT32_MAX)return QsscWire;id=(uint32_t)v.value;id_seen=true;}
            else if(v.number==2){if(value_seen||v.wire!=2)return QsscWire;value=v.data;n=v.bytes;value_seen=true;}}
        if(!id_seen||!value_seen)return QsscWire;r=attribute_value(id,value,n,&a,0);if(r)return r;
        if(id<32)a.present|=1u<<id;
    }
    *out=a;return QsscOkay;
}
int qssc_attributes_merge(QsscAttributes *a,const QsscAttributes *b){
    if(!a||!b)return QsscArgument;
    if(b->present&1u){memcpy(a->name,b->name,sizeof a->name);a->name_valid=b->name_valid;}
    if(b->present&2u)memcpy(a->vendor,b->vendor,sizeof a->vendor);
    if(b->present&4u){memcpy(a->datatype,b->datatype,sizeof a->datatype);a->type_valid=b->type_valid;}
    if(b->present&8u){a->available=b->available;a->available_valid=b->available_valid;}
    if(b->present&(1u<<5))memcpy(a->api,b->api,sizeof a->api);
    if(b->present&(1u<<6)){memcpy(a->rates,b->rates,sizeof a->rates);a->rate_count=b->rate_count;}
    if(b->present&(1u<<16)){a->stream_type=b->stream_type;a->stream_valid=b->stream_valid;}
    a->present|=b->present;
    return a->name_valid&&a->type_valid&&a->available_valid&&a->stream_valid&&
        (a->present&(1u<<5))&&(a->stream_type!=0||a->rate_count)?QsscOkay:QsscNoData;
}
