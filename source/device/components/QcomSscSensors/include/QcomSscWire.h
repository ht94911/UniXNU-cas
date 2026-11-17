// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef QCOM_SSC_WIRE_H
#define QCOM_SSC_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QSSC_SERVICE 400u
#define QSSC_IDL_MAJOR 1u
#define QSSC_IDL_MINOR 7u
#define QSSC_REQUEST_BYTES 192u
#define QSSC_REPORT_BYTES 1023u
#define QSSC_PAYLOAD_BYTES 1000u
#define QSSC_MAX_EVENTS 64u
#define QSSC_MAX_VALUES 6u
#define QSSC_MAX_RATES 32u
#define QSSC_LOOKUP_WORD UINT64_C(0xabababababababab)
enum { QsscOkay=0, QsscNoData=1, QsscAgain=2, QsscArgument=-1,
       QsscWire=-2, QsscUnsupported=-3, QsscIdentity=-4, QsscRemote=-5,
       QsscCapacity=-6, QsscTime=-7, QsscTimeout=-8, QsscConsumer=-9 };
enum { QsscAttrRequest=1, QsscFlushRequest=2, QsscDisableRequest=10,
       QsscAttrEvent=128, QsscFlushEvent=129, QsscErrorEvent=130,
       QsscLookupRequest=512, QsscStreamRequest=513, QsscOnChangeRequest=514,
       QsscLookupEvent=768, QsscProximityEvent=769,
       QsscCalibrationEvent=1022, QsscSampleEvent=1025 };
typedef struct { uint64_t low,high; } QsscUid;
typedef struct { uint32_t node,port,instance; } QsscEndpoint;
typedef struct {
    uint16_t transaction,result,error;
    bool client_id_valid,service_result_valid;
    uint64_t client_id;
    uint32_t service_result;
} QsscResponse;
typedef struct {
    uint32_t id;
    uint64_t ticks;
    const uint8_t *payload; // Borrowed only until the current receive returns.
    size_t bytes;
} QsscEvent;
typedef struct {
    uint64_t client_id;
    QsscUid source;
    QsscEvent events[QSSC_MAX_EVENTS];
    uint32_t count;
} QsscReport;
typedef struct {
    uint32_t bits[QSSC_MAX_VALUES],count,status;
} QsscVector;
typedef struct { uint32_t near,raw_adc,status; } QsscProximity;
typedef struct {
    uint32_t bias[3],scale[3],matrix[9];
    uint32_t bias_count,scale_count,matrix_count,status,cal_id;
    bool cal_id_valid;
} QsscCalibration;
typedef struct {
    char name[64],vendor[64],datatype[64],api[256];
    uint32_t rates[QSSC_MAX_RATES],rate_count,stream_type;
    uint32_t present;
    bool available,name_valid,type_valid,available_valid,stream_valid;
} QsscAttributes;

bool qssc_uid_equal(QsscUid,QsscUid);
bool qssc_float_finite(uint32_t bits);
// Stateless QMI/protobuf subset derived from nezha ALS017/Tcs3760Sns.
// The established QRTR owner provides framing and actual endpoint selection.
int qssc_encode_request(QsscUid,uint32_t id,bool wakeup,uint32_t batch_us,
                        const uint8_t *payload,size_t payload_bytes,
                        uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes);
int qssc_encode_lookup(const char *datatype,uint16_t transaction,
                       uint8_t *out,size_t capacity,size_t *bytes);
int qssc_encode_stream(QsscUid,uint32_t rate_bits,bool wakeup,uint32_t batch_us,
                       uint16_t transaction,uint8_t *out,size_t capacity,size_t *bytes);
int qssc_decode_response(const uint8_t *,size_t,uint16_t,QsscResponse *);
int qssc_decode_report(const uint8_t *,size_t,QsscReport *);
int qssc_decode_lookup(const QsscEvent *,const char *datatype,QsscUid *,bool *present);
int qssc_decode_vector(const QsscEvent *,QsscVector *);
int qssc_decode_proximity(const QsscEvent *,QsscProximity *);
int qssc_decode_calibration(const QsscEvent *,QsscCalibration *);
int qssc_decode_error(const QsscEvent *,uint32_t *);
int qssc_decode_attributes(const QsscEvent *,QsscAttributes *);
int qssc_attributes_merge(QsscAttributes *,const QsscAttributes *);
#ifdef __cplusplus
}
#endif
#endif
