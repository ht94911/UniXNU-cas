/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8850Rpmh.h"
enum { Invalid = -1, Io = -2, Busy = -3, Timeout = -4, Missing = -5 };
static int rd(Sm8850Rpmh *r, unsigned w, uint32_t o, uint32_t *v)
{
    uint32_t cap = w == Sm8850RpmhRsc ? r->rsc_bytes : r->db_bytes;
    if ((o & 3) || cap < 4 || o > cap - 4) return Invalid;
    if (r->poisoned) return Io;
    if (r->read(r->context, w, o, v)) { r->poisoned = 1; return Io; }
    return 0;
}
static int db16(Sm8850Rpmh *r, uint32_t o, uint32_t *v)
{
    uint32_t n; int e = rd(r, Sm8850RpmhDatabase, o & ~3u, &n);
    if (!e) *v = (n >> (8 * (o & 3))) & 65535;
    return e;
}
static int wr(Sm8850Rpmh *r, uint32_t o, uint32_t v)
{
    if ((o & 3) || r->rsc_bytes < 4 || o > r->rsc_bytes - 4 || r->poisoned) return Invalid;
    if (r->write(r->context, Sm8850RpmhRsc, o, v)) { r->poisoned = 1; return Io; }
    return 0;
}
#define TRY(x) do { int e_ = (x); if (e_) return e_; } while (0)
static int sync(Sm8850Rpmh *r, uint32_t o, uint32_t v)
{
    TRY(wr(r, o, v));
    for (unsigned n = 0; n < 1000; n++) {
        uint32_t got; TRY(rd(r, Sm8850RpmhRsc, o, &got));
        if (got == v) return 0;
        r->sleep_us(r->context, 1);
    }
    r->poisoned = 1; return Timeout;
}
static int lookup(Sm8850Rpmh *r, const char *name, unsigned slave,
                  uint32_t *address, uint32_t *aux, uint32_t *length)
{
    if (!r || !r->read || !r->write || !r->now_us || !r->sleep_us || !name) return Invalid;
    uint32_t magic, key[2] = {0, 0}; unsigned bytes = 0, matches = 0;
    while (bytes < 8 && name[bytes]) { key[bytes / 4] |= (uint32_t)(uint8_t)name[bytes] << (8 * (bytes % 4)); bytes++; }
    if (!bytes || name[bytes]) return Invalid;
    TRY(rd(r, Sm8850RpmhDatabase, 4, &magic)); if (magic != 0x0c0330db) return Invalid;
    for (unsigned i = 0; i < 8; i++) {
        uint32_t h = 8 + i * 16, id, table, data, count;
        TRY(db16(r, h, &id)); if (!id) break;
        TRY(db16(r, h + 2, &table)); TRY(db16(r, h + 4, &data)); TRY(db16(r, h + 6, &count));
        table += 144; data += 144;
        if ((table & 3) || table > r->db_bytes || count > (r->db_bytes - table) / 24 || data > r->db_bytes) return Invalid;
        for (unsigned j = 0; j < count; j++) {
            uint32_t e = table + j * 24, lo, hi;
            TRY(rd(r, Sm8850RpmhDatabase, e, &lo)); TRY(rd(r, Sm8850RpmhDatabase, e + 4, &hi));
            if (lo != key[0] || hi != key[1]) continue;
            uint32_t a, o, n;
            TRY(rd(r, Sm8850RpmhDatabase, e + 16, &a)); TRY(db16(r, e + 20, &n)); TRY(db16(r, e + 22, &o));
            if (id != slave || ((a >> 16) & 7) != slave || (a & 15) ||
                o > r->db_bytes - data || n > r->db_bytes - data - o) return Invalid;
            *address = a; *aux = data + o; *length = n; matches++;
        }
    }
    return matches == 1 ? 0 : Missing;
}
static int vote(Sm8850Rpmh *r, uint32_t address, uint32_t value)
{
    uint32_t id, cfg; TRY(rd(r, 0, 0, &id)); TRY(rd(r, 0, 0x0c, &cfg));
    unsigned major = (id >> 16) & 255, ntcs = (cfg >> 12) & 63, ncpt = cfg >> 27;
    if (major < 3 || ntcs < 3 || ntcs > 16 || ncpt < 1 || ncpt > 16) return Invalid;
    const uint32_t tcs = 0xd00, stride = 0x2a0;
    unsigned selected = 3;
    for (unsigned n = 3; n-- > 0;) {
        uint32_t b = tcs + n * stride, st, ctl, en, wait, pending;
        TRY(rd(r, 0, b + 0x28, &st)); TRY(rd(r, 0, b + 0x24, &ctl));
        TRY(rd(r, 0, b + 0x2c, &en)); TRY(rd(r, 0, b + 0x20, &wait)); TRY(rd(r, 0, tcs + 4, &pending));
        if (st && st != UINT32_MAX && !ctl && !en && !wait && !(pending & (1u << n))) { selected = n; break; }
    }
    if (selected == 3) return Busy;
    uint32_t b = tcs + selected * stride, bit = 1u << selected, irq;
    TRY(rd(r, 0, tcs, &irq)); TRY(sync(r, tcs, irq & ~bit));
    TRY(sync(r, b + 0x34, 8u | 1u << 8 | 1u << 16));
    TRY(sync(r, b + 0x38, address)); TRY(sync(r, b + 0x3c, value));
    TRY(sync(r, b + 0x20, 1)); TRY(sync(r, b + 0x2c, 1));
    TRY(sync(r, b + 0x24, 0)); TRY(sync(r, b + 0x24, 1u << 16));
    TRY(wr(r, b + 0x24, 1u << 16 | 1u << 24));
    uint64_t start = r->now_us(r->context);
    int done = 0;
    do {
        uint32_t cs, st; TRY(rd(r, 0, b + 0x40, &cs)); TRY(rd(r, 0, b + 0x28, &st));
        if (cs != UINT32_MAX && (cs & 0x10100) == 0x10100 && st && st != UINT32_MAX) { done = 1; break; }
        r->sleep_us(r->context, 10);
    } while (r->now_us(r->context) - start < 100000);
    if (!done) { r->poisoned = 1; return Timeout; } /* uncertain TCS is never reused */
    TRY(sync(r, b + 0x24, 1u << 16)); TRY(sync(r, b + 0x24, 0));
    TRY(sync(r, b + 0x2c, 0)); TRY(sync(r, b + 0x20, 0));
    TRY(wr(r, tcs + 8, bit)); TRY(sync(r, tcs, irq));
    return 0;
}
int Sm8850RpmhResource(Sm8850Rpmh *r, const char *name, uint32_t offset, uint32_t value)
{
    uint32_t a, aux, n;
    if (offset != 0 && offset != 4 && offset != 8) return Invalid;
    /* VRM resources: rail/clock names remain explicit board inputs. */
    TRY(lookup(r, name, 4, &a, &aux, &n));
    if (a > UINT32_MAX - offset) return Invalid;
    return vote(r, a + offset, value);
}
int Sm8850RpmhArc(Sm8850Rpmh *r, const char *name, uint32_t virtual_level)
{
    uint32_t a, aux, n; TRY(lookup(r, name, 3, &a, &aux, &n));
    /* cmd-db len is bytes; Qualcomm's ARC capacity is 32 u16 levels.
     * A valid zero-padded 20-level table therefore occupies 40 bytes. */
    if (!n || (n & 1) || n / sizeof(uint16_t) > 32) return Invalid;
    uint32_t previous = 0;
    for (unsigned i = 0; i < n / 2; i++) {
        uint32_t level; TRY(db16(r, aux + i * 2, &level));
        if (level < previous) return Invalid;
        if (level >= virtual_level) return vote(r, a, i);
        previous = level;
    }
    return Invalid; /* never silently vote below the requested level */
}
