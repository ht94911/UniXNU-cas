/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Derived from sheng package/QcomQrtr.c qcom_servloc_answer; see NbServloc.h. */
#include "NbServloc.h"
#include "NbQmi.h"

#define NAME_MAX_ 64u
#define LIST_MAX 32u

static const char *const modem_root[] = {"tms/servreg", "gps/gps_service", 0};
static const char *const modem_oem[] = {"tms/servreg", "oempd/srv1", 0};
static const char *const servreg[] = {"tms/servreg", 0};
static const char *const with_audio[] = {"tms/servreg", "avs/audio", 0};

const NbPdDomain nb_nezha_domains[] = {
    {"msm/modem/root_pd", 180, modem_root},
    {"msm/modem/oem_pd", 180, modem_oem},
    {"msm/adsp/root_pd", 74, servreg},
    {"msm/adsp/sensor_pd", 74, servreg},
    {"msm/adsp/audio_pd", 74, with_audio},
    {"msm/cdsp/root_pd", 76, with_audio},
};
const unsigned nb_nezha_domain_count = sizeof nb_nezha_domains / sizeof nb_nezha_domains[0];

static int same(const char *a, const uint8_t *b, uint32_t n)
{
    uint32_t i = 0;
    for (; i < n && a[i]; ++i) if ((uint8_t)a[i] != b[i]) return 0;
    return i == n && !a[i];
}

static int provides(const NbPdDomain *d, const uint8_t *name, uint32_t n)
{
    for (unsigned i = 0; d->services && d->services[i]; ++i) if (same(d->services[i], name, n)) return 1;
    return 0;
}

uint32_t nb_servloc_answer(const NbPdDomain *domains, unsigned count, const uint8_t *request, uint32_t length, uint8_t *out,
                           uint32_t capacity)
{
    NbQmiMessage m;
    NbQmiWriter w;
    uint32_t name_size = 0, skip_size = 0;
    if (!domains || nb_qmi_parse(request, length, &m) || m.type != NB_QMI_REQUEST || m.message != NB_SERVLOC_GET_DOMAIN_LIST)
        return 0;
    const uint8_t *name = nb_qmi_tlv(&m, 0x01, &name_size);
    const uint8_t *skip = nb_qmi_tlv(&m, 0x10, &skip_size);
    if (!name || name_size > NAME_MAX_ || (skip && skip_size != 4)) return 0;
    while (name_size && !name[name_size - 1]) --name_size;  /* a sender may count the terminating zero */
    const uint32_t first = skip ? nb_le32(skip) : 0;
    if (nb_qmi_begin(&w, out, capacity, NB_QMI_RESPONSE, m.transaction, m.message)) return 0;
    nb_qmi_result(&w, 0, 0);
    uint32_t total = 0;
    for (unsigned i = 0; i < count; ++i) total += provides(&domains[i], name, name_size);
    uint8_t b[2];
    nb_put16(b, total); nb_qmi_put(&w, 0x10, b, 2);
    nb_put16(b, 1); nb_qmi_put(&w, 0x11, b, 2);
    /* The list: measure, then write in place. */
    uint32_t bytes = 1, listed = 0, index = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!provides(&domains[i], name, name_size)) continue;
        if (index++ < first || listed == LIST_MAX) continue;
        uint32_t n = 0;
        while (domains[i].domain[n] && n < NAME_MAX_) ++n;
        bytes += 1 + n + 9;
        listed++;
    }
    if (listed) {
        uint8_t *p = nb_qmi_put(&w, 0x12, 0, bytes);
        if (!p) return 0;
        *p++ = (uint8_t)listed;
        index = 0;
        uint32_t written = 0;
        for (unsigned i = 0; i < count && written < listed; ++i) {
            if (!provides(&domains[i], name, name_size) || index++ < first) continue;
            uint32_t n = 0;
            while (domains[i].domain[n] && n < NAME_MAX_) ++n;
            *p++ = (uint8_t)n;
            for (uint32_t c = 0; c < n; ++c) *p++ = (uint8_t)domains[i].domain[c];
            nb_put32(p, domains[i].instance);  /* then service data valid (no) and service data */
            p[4] = 0;
            nb_put32(p + 5, 0);
            p += 9;
            written++;
        }
    }
    return nb_qmi_end(&w);
}
