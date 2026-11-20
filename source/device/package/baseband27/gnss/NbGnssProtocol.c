/* SPDX-License-Identifier: BSD-2-Clause */
#include "NbGnssProtocol.h"

struct nb_gnss_float_field { uint8_t loc, offset; uint32_t bit; uint8_t nonnegative; };
static const struct nb_gnss_float_field nb_gnss_float_fields[] = {
    {0x1a,43,NB_GNSS_ALT,0}, {0x1b,47,NB_GNSS_MSL,0},
    {0x18,51,NB_GNSS_SPEED,1}, {0x1f,55,NB_GNSS_VERTICAL_SPEED,0},
    {0x20,59,NB_GNSS_HEADING,1}, {0x12,63,NB_GNSS_CIRCULAR,1},
    {0x14,67,NB_GNSS_MAJOR,1}, {0x13,71,NB_GNSS_MINOR,1},
    {0x15,75,NB_GNSS_ORIENTATION,1}, {0x1c,79,NB_GNSS_VERTICAL_UNCERTAINTY,1},
    {0x19,83,NB_GNSS_SPEED_UNCERTAINTY,1}
};

int nb_gnss_decode_fix(const NbQmiMessage *m, uint64_t received_ms, NbGnssFix *f)
{
    const uint8_t *status = nb_gnss_tlv(m, 1, 4), *session = nb_gnss_tlv(m, 2, 1);
    if (!status || !session || nb_le32(status) > 7u) return 0;
    __builtin_memset(f, 0, sizeof *f);
    f->status = (uint8_t)nb_le32(status); f->session = *session;
    f->received_ms = received_ms;
    /* The original PDS reader copies altitude with lat/lon even if bit 6 is
     * clear. Keep missing altitude as unknown instead of a normal zero. */
    nb_put32(f->parsed + 43, 0x7fc00000u); nb_put32(f->parsed + 47, 0x7fc00000u);
    const uint8_t *p = nb_gnss_tlv(m, 0x10, 8);
    if (p && nb_gnss_degree_valid(nb_le64(p), UINT64_C(0x4056800000000000))) {
        __builtin_memcpy(f->parsed + 27, p, 8); f->valid |= NB_GNSS_LAT;
    }
    p = nb_gnss_tlv(m, 0x11, 8);
    if (p && nb_gnss_degree_valid(nb_le64(p), UINT64_C(0x4066800000000000))) {
        __builtin_memcpy(f->parsed + 35, p, 8); f->valid |= NB_GNSS_LON;
    }
    p = nb_gnss_tlv(m, 0x25, 8);
    if (p && nb_le64(p) && nb_le64(p) <= UINT64_C(253402300799999)) {
        f->utc_ms = nb_le64(p); __builtin_memcpy(f->parsed + 15, p, 8);
        f->valid |= NB_GNSS_UTC;
    }
    for (unsigned i = 0; i < sizeof nb_gnss_float_fields / sizeof nb_gnss_float_fields[0]; ++i) {
        const struct nb_gnss_float_field *field = &nb_gnss_float_fields[i];
        p = nb_gnss_tlv(m, field->loc, 4);
        if (!p || !nb_gnss_float_valid(nb_le32(p), field->nonnegative)) continue;
        if (field->bit == NB_GNSS_HEADING && (nb_le32(p) & 0x7fffffffu) > 0x43b40000u) continue;
        if (field->bit == NB_GNSS_ORIENTATION && (nb_le32(p) & 0x7fffffffu) > 0x43340000u) continue;
        __builtin_memcpy(f->parsed + field->offset, p, 4); f->valid |= field->bit;
    }
    p = nb_gnss_tlv(m, 0x26, 1);
    if (p) { f->parsed[14] = *p; f->valid |= NB_GNSS_LEAP; }
    p = nb_gnss_tlv(m, 0x28, 4);
    uint32_t uncertainty;
    if (p && nb_gnss_uncertainty(nb_le32(p), &uncertainty)) {
        nb_put32(f->parsed + 23, uncertainty); f->valid |= NB_GNSS_TIME_UNCERTAINTY;
    }
    p = nb_gnss_tlv(m, 0x16, 1);
    if (p && *p <= 100u) { f->parsed[91] = *p; f->valid |= NB_GNSS_HORIZONTAL_CONFIDENCE; }
    p = nb_gnss_tlv(m, 0x24, 12);
    if (p) for (unsigned i = 0; i < 3; ++i) {
        if (!nb_gnss_float_valid(nb_le32(p + 4u * i), 1)) continue;
        __builtin_memcpy(f->parsed + 92u + 4u * i, p + 4u * i, 4);
        f->valid |= NB_GNSS_PDOP << i;
    }
    f->raw_valid = f->valid;
    /* The native reader's speed path consumes uncertainty unconditionally. */
    if (!(f->valid & NB_GNSS_SPEED_UNCERTAINTY)) f->valid &= ~NB_GNSS_SPEED;
    /* Native PDS uses the LOC ellipse at its original95% confidence. With a
     * circular radius preserve that radius and omit incompatible ellipse bits. */
    if ((f->valid & (NB_GNSS_MAJOR | NB_GNSS_MINOR)) &&
        (!(f->valid & NB_GNSS_HORIZONTAL_CONFIDENCE) || f->parsed[91] != 95u))
        f->valid &= ~(NB_GNSS_MAJOR | NB_GNSS_MINOR | NB_GNSS_ORIENTATION);
    p = nb_gnss_tlv(m, 0x23, 4); if (p) f->technology = nb_le32(p);
    p = nb_gnss_tlv(m, 0x2b, 4);
    if (p) { f->fix_id = nb_le32(p); f->fix_id_present = 1; }
    p = nb_gnss_tlv(m, 0x3f, 8);
    if (p) { f->system_tick = nb_le64(p); f->system_tick_present = 1; }

    /* The count is one byte in this IDL (the maxima are below 256). The
     * expanded list replaces the legacy list; signal masks align only with
     * the expanded list. Never infer BDS usage from a preceding sky report. */
    uint32_t bytes = 0, signal_bytes = 0;
    const uint8_t *used = nb_qmi_tlv(m, 0x36, &bytes);
    const uint8_t *signals = nb_qmi_tlv(m, 0x37, &signal_bytes);
    const int expanded = used != 0;
    if (!used) used = nb_qmi_tlv(m, 0x2c, &bytes);
    if (used) {
        const uint32_t maximum = expanded ? NB_GNSS_MAX_SV : 80u;
        if (!bytes || used[0] > maximum || bytes != 1u + 2u * used[0]) return 0;
        f->used_present = 1; f->used_count = used[0];
        if (signals && (!expanded || !signal_bytes || signals[0] != used[0] ||
                        signal_bytes != 1u + 8u * signals[0])) return 0;
        f->signals_present = signals != 0;
        for (unsigned i = 0; i < f->used_count; ++i) {
            NbGnssUsedSv *sv = &f->used[i]; sv->id = nb_le16(used + 1u + 2u * i);
            sv->system = nb_gnss_used_system(sv->id);
            sv->signals = signals ? nb_le64(signals + 1u + 8u * i) : 0;
            /* A multiband report may have more than one entry for an SV.
             * Preserve those signal records, count each satellite once. */
            unsigned first = 1;
            for (unsigned j = 0; j < i; ++j) if (f->used[j].id == sv->id) first = 0;
            if (sv->system == 1 && first) ++f->gps_used;
            if (sv->system == 6) {
                if (first) ++f->bds_used;
                f->bds_signals |= sv->signals & NB_GNSS_BDS_SIGNALS;
            }
        }
    } else if (signals) return 0;
    nb_put32(f->parsed, f->valid);
    return 1;
}

uint32_t nb_gnss_reference_position(const NbQmiMessage *m, uint8_t *out, uint32_t room, uint32_t *length)
{
    *length = 0;
    const uint8_t *utc = nb_gnss_tlv(m, 0x10, 8);
    const uint8_t *lat = nb_gnss_tlv(m, 0x11, 8), *lon = nb_gnss_tlv(m, 0x12, 8);
    const uint8_t *horizontal = nb_gnss_tlv(m, 0x13, 4), *vertical = nb_gnss_tlv(m, 0x16, 4);
    const uint8_t *hconf = nb_gnss_tlv(m, 0x17, 1), *vconf = nb_gnss_tlv(m, 0x18, 1);
    const uint8_t *source = nb_gnss_tlv(m, 0x19, 1), *context = nb_gnss_tlv(m, 0x1a, 1);
    const uint8_t *reliability = nb_gnss_tlv(m, 0x1b, 2), *altinfo = nb_gnss_tlv(m, 0x1c, 3);
    uint32_t altitude_bytes = 0;
    const uint8_t *altitude = nb_qmi_tlv(m, 0x15, &altitude_bytes);
    if (!utc || !nb_le64(utc) || nb_le64(utc) > UINT64_C(253402300799999) || !lat || !lon || !horizontal || !vertical ||
        !hconf || !vconf || !source || !context || !reliability || !altinfo ||
        (altitude && altitude_bytes != 4u)) return NB_QMI_ERR_INVALID_ARG;
    /* The original24A446 builder stores its messenger context's low byte
     * in TLV0x1a, independently of the timestamp argument in TLV0x10.
     * Its cached-reference producer converts CF absolute time to UTC ms
     * using kCFAbsoluteTimeIntervalSince1970 and1000. Do not interpret the
     * context byte as a timebase or reject the valid supplied timestamp. */
    if (!nb_gnss_degree_valid(nb_le64(lat), UINT64_C(0x4056800000000000)) ||
        !nb_gnss_degree_valid(nb_le64(lon), UINT64_C(0x4066800000000000)) ||
        !nb_gnss_float_valid(nb_le32(horizontal), 1) || !(nb_le32(horizontal) & 0x7fffffffu) ||
        !nb_gnss_float_valid(nb_le32(vertical), 1) ||
        !*hconf || *hconf > 100u || !*vconf || *vconf > 100u ||
        (altitude && !nb_gnss_float_valid(nb_le32(altitude), 0))) return NB_QMI_ERR_INVALID_ARG;
    /* The measured cached-reference caller has source1, unknown horizontal
     * reliability and other/dependent/full altitude metadata. Keep other
     * PDS position-source conventions unsupported until their units are known. */
    if (*source != 1u || reliability[0] || reliability[1] > 4u ||
        altinfo[0] != 9u || altinfo[1] != 2u || altinfo[2] != 2u) return NB_QMI_ERR_NOT_SUPPORTED;
    for (uint32_t at = 0; at < m->length; at += 3u + nb_le16(m->tlvs + at + 1)) {
        const uint8_t tag = m->tlvs[at];
        if (tag < 0x10u || tag > 0x1cu || tag == 0x14u) return NB_QMI_ERR_NOT_SUPPORTED;
    }
    NbQmiWriter w;
    if (nb_qmi_begin(&w, out, room, NB_QMI_REQUEST, 0, NB_LOC_INJECT_POSITION)) return NB_QMI_ERR_NO_MEMORY;
    nb_qmi_put(&w, 0x10, lat, 8); nb_qmi_put(&w, 0x11, lon, 8);
    nb_qmi_put(&w, 0x12, horizontal, 4); nb_qmi_u8(&w, 0x13, *hconf == 100u ? 99u : *hconf);
    if (altitude) nb_qmi_put(&w, 0x15, altitude, 4);
    nb_qmi_put(&w, 0x17, vertical, 4); nb_qmi_u8(&w, 0x18, *vconf == 100u ? 99u : *vconf);
    nb_qmi_u32(&w, 0x19, reliability[1]);
    if (altitude) {
        uint8_t info[12]; nb_put32(info, 9); nb_put32(info + 4, 2); nb_put32(info + 8, 2);
        nb_qmi_put(&w, 0x1a, info, sizeof info);
    }
    nb_qmi_put(&w, 0x1b, utc, 8);
    nb_qmi_u32(&w, 0x1d, 6); /* LOC OTHER: the reference is supplied by the OS, not a satellite fix. */
    *length = nb_qmi_end(&w);
    return *length ? 0u : NB_QMI_ERR_NO_MEMORY;
}

int nb_gnss_decode_sky(const NbQmiMessage *m, uint64_t received_ms, NbGnssSky *sky)
{
    uint32_t bytes = 0, signal_bytes = 0;
    const uint8_t *altitude = nb_gnss_tlv(m, 1, 1);
    const uint8_t *p = nb_qmi_tlv(m, 0x11, &bytes);
    const unsigned stride = p ? 29u : 28u;
    const unsigned maximum = p ? NB_GNSS_MAX_SV : 80u;
    if (!p) p = nb_qmi_tlv(m, 0x10, &bytes);
    const uint8_t *signals = nb_qmi_tlv(m, 0x12, &signal_bytes);
    if (!p) {
        /* LOC permits its mandatory altitudeAssumed field without either
         * optional satellite list. Retain that fresh status as list absent,
         * keeping unknown satellites distinct from an explicit empty list. */
        if (!altitude || *altitude > 1u || signals) return 0;
        __builtin_memset(sky, 0, sizeof *sky);
        sky->received_ms = received_ms;
        sky->altitude_present = 1; sky->altitude_assumed = *altitude;
        return 1;
    }
    if (!bytes || p[0] > maximum || bytes != 1u + stride * p[0] || (altitude && *altitude > 1u)) return 0;
    if (signals && (!signal_bytes || signals[0] != p[0] || signal_bytes != 1u + 8u * signals[0])) return 0;
    __builtin_memset(sky, 0, sizeof *sky);
    sky->received_ms = received_ms; sky->count = p[0]; sky->signals_present = signals != 0;
    sky->list_present = 1; sky->altitude_present = altitude != 0;
    if (altitude) sky->altitude_assumed = *altitude;
    for (unsigned i = 0; i < sky->count; ++i) {
        const uint8_t *raw = p + 1u + stride * i;
        NbGnssSatellite *sv = &sky->sv[i];
        sv->valid = nb_le32(raw); sv->system = nb_le32(raw + 4); sv->id = nb_le16(raw + 8);
        sv->health = raw[10]; sv->tracking = nb_le32(raw + 11); sv->info = raw[15];
        sv->elevation = nb_le32(raw + 16); sv->azimuth = nb_le32(raw + 20); sv->cn0 = nb_le32(raw + 24);
        if (stride == 29u) sv->glo_frequency = (int8_t)raw[28];
        if (signals) sv->signals = nb_le64(signals + 1u + 8u * i);
        if ((sv->valid & 0x8bu) == 0x8bu && sv->system == 6u && sv->id >= 201u && sv->id <= 263u &&
            sv->tracking == 3u && nb_gnss_float_valid(sv->cn0, 1) && (sv->cn0 & 0x7fffffffu)) ++sky->bds_seen;
    }
    return 1;
}

int nb_gnss_fix_usable(const NbGnssFix *f)
{
    const uint32_t required = NB_GNSS_LAT | NB_GNSS_LON | NB_GNSS_UTC;
    if (f->status || (f->valid & required) != required ||
        (!(f->valid & NB_GNSS_CIRCULAR) &&
            ((f->valid & (NB_GNSS_MAJOR | NB_GNSS_MINOR | NB_GNSS_HORIZONTAL_CONFIDENCE)) !=
                (NB_GNSS_MAJOR | NB_GNSS_MINOR | NB_GNSS_HORIZONTAL_CONFIDENCE) || f->parsed[91] != 95u)) ||
        (!(f->technology & 1u) && !f->used_count)) return 0;
    return 1;
}
uint32_t nb_gnss_pds_fix(const NbGnssFix *f, uint8_t *out, uint32_t room)
{
    if (!nb_gnss_fix_usable(f)) return 0;
    NbQmiWriter w;
    if (nb_qmi_begin(&w, out, room, NB_QMI_INDICATION, 0, NB_PDS_REPORT)) return 0;
    nb_qmi_u8(&w, 0x12, f->status);
    nb_qmi_put(&w, 0x13, f->parsed, sizeof f->parsed);
    return nb_qmi_end(&w);
}
