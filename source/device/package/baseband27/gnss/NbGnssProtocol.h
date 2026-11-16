/* SPDX-License-Identifier: BSD-2-Clause */
/* Wire definitions used by this board's LOC v2 -> PDS adapter.
 * Sources: Qualcomm location_service_v02.{h,c}@1ed86b1d and libqmi PDS.
 * Apple 24A446's parsedPositionData reader independently requires 105 bytes.
 * Keep floats as IEEE bit patterns: the kernel uses -mgeneral-regs-only.
 */
#ifndef NB_GNSS_PROTOCOL_H
#define NB_GNSS_PROTOCOL_H
#include "../services/NbQmi.h"

#define NB_GNSS_LOC_SERVICE 16u
#define NB_GNSS_PDS_SERVICE 6u
#define NB_GNSS_LOC_REVISION 0x86u
#define NB_GNSS_MAX_SV 176u
#define NB_GNSS_PDS_POSITION_BYTES 105u
#define NB_GNSS_FIX_MAX_DELAY_MS 10000u

enum {
    NB_LOC_REVISION = 0x20, NB_LOC_REGISTER = 0x21,
    NB_LOC_START = 0x22, NB_LOC_STOP = 0x23,
    NB_LOC_POSITION = 0x24, NB_LOC_SATELLITES = 0x25,
    NB_LOC_NMEA = 0x26, NB_LOC_ENGINE = 0x2b, NB_LOC_SESSION = 0x2c,
    NB_LOC_SERVICE_REVISION = 0x32, NB_LOC_SET_MODE = 0x4a
};
enum {
    NB_PDS_RESET = 0, NB_PDS_REPORT = 1, NB_PDS_GET_STATE = 0x20,
    NB_PDS_SET_STATE = 0x21, NB_PDS_GET_TRACKING = 0x29,
    NB_PDS_SET_TRACKING = 0x2a, NB_PDS_GET_AUTO = 0x30,
    NB_PDS_SET_AUTO = 0x31, NB_PDS_FORCE_OFF = 0x49,
    NB_PDS_READY = 0x60
};
enum {
    NB_GNSS_UTC = 1u << 1, NB_GNSS_LEAP = 1u << 2,
    NB_GNSS_TIME_UNCERTAINTY = 1u << 3, NB_GNSS_LAT = 1u << 4,
    NB_GNSS_LON = 1u << 5, NB_GNSS_ALT = 1u << 6,
    NB_GNSS_MSL = 1u << 7, NB_GNSS_SPEED = 1u << 8,
    NB_GNSS_VERTICAL_SPEED = 1u << 9, NB_GNSS_HEADING = 1u << 10,
    NB_GNSS_CIRCULAR = 1u << 11, NB_GNSS_MAJOR = 1u << 12,
    NB_GNSS_MINOR = 1u << 13, NB_GNSS_ORIENTATION = 1u << 14,
    NB_GNSS_VERTICAL_UNCERTAINTY = 1u << 15,
    NB_GNSS_SPEED_UNCERTAINTY = 1u << 16,
    NB_GNSS_HORIZONTAL_CONFIDENCE = 1u << 18,
    NB_GNSS_PDOP = 1u << 19, NB_GNSS_HDOP = 1u << 20,
    NB_GNSS_VDOP = 1u << 21
};
#define NB_GNSS_B1I UINT64_C(0x200)
#define NB_GNSS_B1C UINT64_C(0x400)
#define NB_GNSS_B2AI UINT64_C(0x1000)
#define NB_GNSS_B2AQ UINT64_C(0x80000)
#define NB_GNSS_BDS_SIGNALS (NB_GNSS_B1I | NB_GNSS_B1C | UINT64_C(0x800) | NB_GNSS_B2AI | NB_GNSS_B2AQ)

typedef struct {
    uint16_t id;
    uint8_t system; /* LOC enum: GPS=1, Galileo=2, GLO=5, BDS=6, QZSS=7, NavIC=8 */
    uint64_t signals;
} NbGnssUsedSv;

typedef struct {
    uint8_t session, status, used_present, signals_present, fix_id_present;
    uint32_t technology, fix_id, valid, raw_valid;
    uint64_t utc_ms, received_ms, sequence, system_tick;
    uint8_t system_tick_present;
    uint16_t used_count, gps_used, bds_used, deliveries;
    uint64_t bds_signals;
    uint8_t parsed[NB_GNSS_PDS_POSITION_BYTES];
    NbGnssUsedSv used[NB_GNSS_MAX_SV];
} NbGnssFix;

typedef struct {
    uint32_t valid, system, tracking, elevation, azimuth, cn0;
    uint16_t id;
    uint8_t health, info;
    int8_t glo_frequency;
    uint64_t signals;
} NbGnssSatellite;
typedef struct {
    uint64_t received_ms, sequence;
    uint16_t count, bds_seen;
    uint8_t signals_present;
    NbGnssSatellite sv[NB_GNSS_MAX_SV];
} NbGnssSky;

/* Reject incomplete/duplicate TLVs before a successful state transition. */
static inline int nb_gnss_message(const uint8_t *data, uint32_t length, NbQmiMessage *m)
{
    uint8_t seen[32] = {0};
    if (nb_qmi_parse(data, length, m) || m->length != length - NB_QMI_HEADER) return 0;
    for (uint32_t at = 0; at < m->length;) {
        if (m->length - at < 3u) return 0;
        const uint8_t type = m->tlvs[at];
        const uint32_t n = nb_le16(m->tlvs + at + 1);
        if (n > m->length - at - 3u || (seen[type / 8u] & (1u << (type % 8u)))) return 0;
        seen[type / 8u] |= (uint8_t)(1u << (type % 8u));
        at += 3u + n;
    }
    return 1;
}
static inline const uint8_t *nb_gnss_tlv(const NbQmiMessage *m, uint8_t type, uint32_t length)
{
    uint32_t n = 0;
    const uint8_t *p = nb_qmi_tlv(m, type, &n);
    return p && n == length ? p : 0;
}
static inline uint32_t nb_gnss_result(const NbQmiMessage *m)
{
    const uint8_t *p = nb_gnss_tlv(m, 2, 4);
    if (!p || nb_le16(p) > 1u) return NB_QMI_ERR_MALFORMED_MSG;
    return nb_le16(p) ? (nb_le16(p + 2) ? nb_le16(p + 2) : NB_QMI_ERR_INTERNAL) : 0;
}
/* False means a non-finite or negative uncertainty, not perfect accuracy. */
static inline int nb_gnss_float_valid(uint32_t bits, int nonnegative)
{
    return (bits & 0x7f800000u) != 0x7f800000u &&
        (!nonnegative || !(bits & 0x80000000u) || !(bits & 0x7fffffffu));
}
static inline int nb_gnss_degree_valid(uint64_t bits, uint64_t maximum)
{
    return (bits & UINT64_C(0x7fffffffffffffff)) <= maximum;
}
/* LOC timeUnc is a float in ms; PDS's field is an integer in ms. Round up. */
static inline int nb_gnss_uncertainty(uint32_t bits, uint32_t *out)
{
    if (!nb_gnss_float_valid(bits, 1)) return 0;
    const uint32_t magnitude = bits & 0x7fffffffu;
    if (!magnitude) { *out = 0; return 1; }
    const int exponent = (int)(magnitude >> 23) - 127;
    if (exponent < 0) { *out = 1; return 1; }
    if (exponent > 31) return 0;
    const uint64_t mantissa = (magnitude & 0x7fffffu) | 0x800000u;
    const uint64_t value = exponent >= 23 ? mantissa << (unsigned)(exponent - 23) :
        (mantissa + ((UINT64_C(1) << (unsigned)(23 - exponent)) - 1u)) >> (unsigned)(23 - exponent);
    if (value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}
static inline uint8_t nb_gnss_used_system(uint16_t id)
{
    if (id >= 1u && id <= 32u) return 1;
    if (id >= 65u && id <= 96u) return 5;
    if ((id >= 120u && id <= 158u) || (id >= 183u && id <= 191u)) return 3;
    if (id >= 193u && id <= 197u) return 7;
    if (id >= 201u && id <= 263u) return 6;
    if (id >= 301u && id <= 336u) return 2;
    if (id >= 401u && id <= 414u) return 8;
    return 0;
}

#ifdef __cplusplus
extern "C" {
#endif
int nb_gnss_decode_fix(const NbQmiMessage *, uint64_t received_ms, NbGnssFix *);
int nb_gnss_decode_sky(const NbQmiMessage *, uint64_t received_ms, NbGnssSky *);
int nb_gnss_fix_usable(const NbGnssFix *);
uint32_t nb_gnss_pds_fix(const NbGnssFix *, uint8_t *, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
