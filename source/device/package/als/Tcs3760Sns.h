// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef TCS3760_SNS_H
#define TCS3760_SNS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Current Android004 libQshQmiIDL SNS_CLIENT_SVC: service 400, IDL 1.7, tool 6.
// Its selected normal request/response/report fields match the reviewed wire
// subset. Jumbo and the additional0x24 operation remain unselected.
// The sdm670 reference checks portable protocol fields only; no hardware
// addresses, DSP session, QRTR endpoint or sensor samples are supplied here.
#define TCS3760_SNS_SERVICE_ID 400u
#define TCS3760_SNS_IDL_MAJOR 1u
#define TCS3760_SNS_IDL_MINOR 7u
#define TCS3760_SNS_IDL_TOOL 6u
#define TCS3760_SNS_PAYLOAD_MAX 1000u
#define TCS3760_SNS_REQUEST_CAPACITY 80u
#define TCS3760_SNS_QMI_REPORT_MAX 1023u
#define TCS3760_SNS_QRTR_REPORT_MAX 1056u
#define TCS3760_SNS_LOOKUP_WORD UINT64_C(0xabababababababab)

enum {
    Tcs3760SnsOkay = 0,
    Tcs3760SnsNoData = 1,
    Tcs3760SnsArgument = -1,
    Tcs3760SnsMalformed = -2,
    Tcs3760SnsUnsupported = -3,
    Tcs3760SnsCorrelation = -4,
    Tcs3760SnsRemote = -5,
    Tcs3760SnsCapacity = -6
};

typedef struct { uint64_t low, high; } Tcs3760SnsUid;
typedef struct {
    Tcs3760SnsUid source;
    uint64_t client_id;
    uint64_t timestamp; // Original SSC fixed64 timestamp; not Darwin uptime.
    uint32_t lux_bits;  // Calibrated SI lux as IEEE-754 binary32, no QLUX shift.
    uint32_t status;    // Original SNS accuracy: 0 unreliable, 1 low, 2 medium, 3 high.
} Tcs3760SnsSample;

typedef struct {
    uint16_t transaction, qmi_result, qmi_error;
    bool client_id_valid, result_valid;
    uint64_t client_id;
    uint32_t result;
} Tcs3760SnsResponse;

typedef struct {
    bool client_id_valid, sensor_uid_valid;
    uint64_t client_id;
    Tcs3760SnsUid sensor_uid;
} Tcs3760SnsMatch;

typedef struct {
    Tcs3760SnsUid source;
    uint64_t client_id;
    bool uid_event, uid_valid, sample_valid, error_valid;
    Tcs3760SnsUid uid;
    Tcs3760SnsSample sample; // Latest real sample in this report, by SSC timestamp.
    uint32_t samples, non_sample_events, error;
} Tcs3760SnsReport;

typedef struct {
    void *context;
    void (*uid)(void *, const Tcs3760SnsUid *);
    void (*sample)(void *, const Tcs3760SnsSample *);
    void (*error)(void *, uint32_t);
} Tcs3760SnsCallbacks;

// Unsent normal QMI request 0x20. Lookup uses the well-known SUID, datatype
// ambient_light, register_updates=true/default_only=true, APSS/wakeup.
// On-change uses message 514 with empty sns_std_request, APSS/non-wakeup.
// Disable uses client message 10. These functions never send or change state.
// On failure, out and *bytes remain unchanged. Transaction zero is permitted.
int tcs3760_sns_encode_lookup(uint16_t transaction, uint8_t *out, size_t capacity, size_t *bytes);
int tcs3760_sns_encode_on_change(Tcs3760SnsUid uid, uint16_t transaction,
                               uint8_t *out, size_t capacity, size_t *bytes);
int tcs3760_sns_encode_disable(Tcs3760SnsUid uid, uint16_t transaction,
                             uint8_t *out, size_t capacity, size_t *bytes);

// Receives QMI bytes after the existing QRTR owner has checked/routed its
// endpoint and removed QRTR padding. Reuses QcomQrtr's QMI parser/TLV access.
// No registration, NEW_SERVER advertisement, FIFO consumer, timer or MMIO.
// Syntactically valid responses populate out even when the remote rejects
// the request. A receipt is not a sensor sample. Other errors leave out intact.
int tcs3760_sns_decode_response(const uint8_t *qmi, size_t bytes, uint16_t transaction,
                              Tcs3760SnsResponse *out);

// Normal indication 0x21 only; jumbo 0x22/extra 0x23 are unsupported.
// Route by source SUID before message ID: 768 means a lookup event for the
// lookup SUID, but physical configuration for the actual sensor SUID.
// A match can omit client_id until a real response/report binds that value.
// Packed/unpacked float fields and bounded unknown protobuf fields are read.
// Calibration event1022 is validated as factory metadata (sns_cal.proto) and
// produces no sample. Its bias/scale/matrix are not applied a second time to
// the physical sensor's already calibrated1025 data.
// NoData still fills source/client_id and uid_event. uid_event distinguishes a
// validated lookup event with an empty default list from an envelope with no
// lookup event; callers can invalidate a default that has been removed.
// Malformed/unmatched input leaves out intact.
int tcs3760_sns_decode_report(const uint8_t *qmi, size_t bytes,
                            const Tcs3760SnsMatch *match, Tcs3760SnsReport *out);
// Callbacks occur only after complete validation, at most once per output kind.
// The caller serializes this with its existing owned SSC client/session.
int tcs3760_sns_receive_report(const uint8_t *qmi, size_t bytes,
                             const Tcs3760SnsMatch *match, const Tcs3760SnsCallbacks *callbacks);

#ifdef __cplusplus
}
#endif
#endif
