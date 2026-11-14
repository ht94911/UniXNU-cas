/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerBoard.h"
#include <string.h>
#include <sys/errno.h>
#include <sys/systm.h>

static int valid(NpPowerBoard *b) {
    return !b->quarantine && b->resources.lease_valid(b->resources.context, b->resources.lease);
}
static uint64_t now(void *v) {
    NpPowerBoard *b = v; return b->resources.now_ms(b->resources.context);
}
static int read32(NpPowerBoard *b, uint64_t pa, uint32_t *value) {
    if (!valid(b)) return EACCES;
    return b->resources.read32(b->resources.context, b->resources.lease, pa, value);
}
static int dma(void *v, uint64_t pa, uint32_t bytes) {
    NpPowerBoard *b = v;
    if (!valid(b)) return EACCES;
    return b->resources.dma_identity(b->resources.context, b->resources.lease, 0xb6, pa, bytes);
}
static void release_bus(void *v, int stopped) {
    NpPowerBoard *b = v;
    if (!stopped || !valid(b)) { b->quarantine = 1; return; }
    if (b->vote && b->resources.gauge_vote(b->resources.context, b->resources.lease, 0)) {
        b->quarantine = 1; return;
    }
    b->vote = 0;
}
static int acquire_bus(void *v) {
    NpPowerBoard *b = v;
    if (!valid(b) || b->suspend || b->vote) return EBUSY;
    if (b->resources.gauge_vote(b->resources.context, b->resources.lease, 1)) return EIO;
    b->vote = 1;
    const uint32_t branches[] = {0x2315c, 0x23148, 0x23140, 0x23144, 0x18508};
    for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); ++i) {
        unsigned tries; uint32_t value = 0;
        for (tries = 0; tries < 100; ++tries) {
            if (read32(b, 0x100000 + branches[i], &value)) goto fail;
            if (!(value & 0x80000000u)) break;
            b->resources.pause_us(b->resources.context, 100);
        }
        if (tries == 100) goto fail;
    }
    uint32_t firmware = 0, fifo_disabled = 0;
    if (read32(b, 0xa94068, &firmware) || read32(b, 0xa94064, &fifo_disabled) ||
        ((firmware >> 8) & 255) != 3 || !(fifo_disabled & 1)) goto fail;
    /* Shared pinmux/root clock already belongs to the platform. Leave it
     * configured. The GPI LOCK arbitrates SE execution with SOCCP. */
    return 0;
fail:
    release_bus(b, 1); return EIO;
}
static int gauge(void *v, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *fields) {
    NpPowerBoard *b = v; return np_gauge_bus_read(&b->bus, out, fields);
}
int np_board_read_original_temperature(void *v,NpBoardTemperatureReading *out) {
    if(!out)return EINVAL;
    memset(out,0,sizeof(*out));
    NpPowerBoard *b=v;
    if(!b||!valid(b)||b->suspend){out->error=EACCES;return EACCES;}
    int error=np_gauge_bus_original_temperature(&b->bus,out);
    /* release_bus can quarantine a failed vote/lease release after the
     * actual transfer. Keep its evidence but withdraw sample validity. */
    if(!error&&!valid(b)){error=EIO;out->error=EIO;out->valid=0;}
    return error;
}
static void pause_bus(void *v,uint32_t us) {
    NpPowerBoard *b=v;b->resources.pause_us(b->resources.context,us);
}
static void wait_auth(void *v,uint32_t ms) {
    NpPowerBoard *b=v;b->resources.auth_wait_ms(b->resources.context,ms);
}
static int battery_digest(void *v,const uint8_t nonce[32],uint8_t digest[32]) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_auth)return EACCES;
    return np_gauge_bus_auth_digest(&b->bus,nonce,digest);
}
static int identity(void *v,NpGaugeIdentity *out) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend)return EACCES;
    return np_gauge_bus_identity(&b->bus,out);
}
static int pump(void *v,NpPump *out) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump)return EACCES;
    int error=np_pump_bus_read(&b->bus,out);
    if(!error&&out->chip==0x85&&out->controls_valid)b->pump_seen=1;
    return error;
}
static int authorize(void *v,const uint8_t *p,uint32_t bytes) {
    NpPowerBoard *b=v;if(!valid(b)||b->suspend)return 0;
    return np_power27_write_allowed(p,bytes);
}
static int usb_ovp(void *v,uint8_t expected,uint8_t value,uint8_t *observed,uint8_t *attempted) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump) {
        if(attempted)*attempted=0;
        return EACCES;
    }
    return np_pump_bus_usb_ovp(&b->bus,expected,value,observed,attempted);
}
static int watchdog(void *v,unsigned timeout_ms,uint8_t *observed,uint8_t *attempted) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump){
        if(attempted)*attempted=0;
        return EACCES;
    }
    return np_pump_bus_watchdog(&b->bus,timeout_ms,observed,attempted);
}
static int watchdog_feed(void *v,uint8_t *observed,uint8_t *attempted) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump){
        if(attempted)*attempted=0;
        return EACCES;
    }
    return np_pump_bus_watchdog_feed(&b->bus,observed,attempted);
}
static int mode1(void *v,NpPumpModeReading *out) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump){
        if(out)memset(out,0,sizeof(*out));
        return EACCES;
    }
    return np_pump_bus_mode1(&b->bus,out);
}
static int enable_pump(void *v,uint32_t expected_vbus_mv,NpPump *out) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump||!b->resources.auth_wait_ms){
        if(out)memset(out,0,sizeof(*out));
        return EACCES;
    }
    return np_pump_bus_enable(&b->bus,expected_vbus_mv,out);
}
static int disable_pump(void *v) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump)return EACCES;
    return np_pump_bus_disable(&b->bus);
}
static int prepare_pump_exit(void *v) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump)return EACCES;
    return np_pump_bus_prepare_exit(&b->bus);
}
static int restore_pump_mode(void *v) {
    NpPowerBoard *b=v;
    if(!valid(b)||b->suspend||!b->resources.enable_pump)return EACCES;
    return np_pump_bus_mode_restore(&b->bus);
}
static int lock(void *v) {
    NpPowerBoard *b = v;
    return valid(b) && b->resources.smem_lock(b->resources.context);
}
static void unlock(void *v) {
    NpPowerBoard *b = v; b->resources.smem_unlock(b->resources.context);
}
static void kick(void *v, uint32_t client, uint32_t signal) {
    NpPowerBoard *b = v;
    if (valid(b)) b->resources.doorbell(b->resources.context, client, signal);
    else b->quarantine = 1;
}
static void link(void *v, int up, uint64_t epoch) {
    (void)v; np_power27_link(up, epoch);
}
static void receive(void *v, uint64_t epoch, const uint8_t *p, uint32_t n) {
    (void)v; np_power27_receive(epoch, p, n);
}
static int poll(void *v) {
    NpPowerBoard *b = v;
    if (!valid(b) || b->suspend) return -1;
    if (!b->resources.enable_soccp) return 0;
    if (!b->attach_attempted) {
        uint64_t time = now(b);
        if (time < b->attach_retry_at) return 0;
        if (!b->attach_deadline) {
            if (time > UINT64_MAX - 30000) return -1;
            b->attach_deadline = time + 30000;
        }
        NpSoccpIo io = {b, lock, unlock, kick, now, link, receive,authorize};
        int result = np_soccp_attach(&b->soccp, b->resources.smem, 0x200000, &io);
        if (result == NP_SOCCP_WAIT_REMOTE && time < b->attach_deadline) {
            b->attach_retry_at = time + 1000;
            return 0;
        }
        b->attach_attempted = 1; /* never replay a partial outbound setup */
        printf("NEZHA POWER SOCCP: board attach result=%d at=%llu deadline=%llu\n",
            result,(unsigned long long)time,(unsigned long long)b->attach_deadline);
        if (result) return -1;
    }
    return np_soccp_poll(&b->soccp);
}
static int send(void *v, const uint8_t *p, uint32_t n) {
    NpPowerBoard *b = v;
    if (!valid(b) || b->suspend || !b->resources.enable_soccp) return -2;
    return np_soccp_send(&b->soccp, p, n);
}
static int quiesce(void *v) {
    NpPowerBoard *b = v;
    if(b->resources.enable_pump&&b->pump_seen&&np_pump_bus_disable(&b->bus))return EIO;
    if(b->resources.enable_pump&&b->bus.pump_watchdog_owned&&np_pump_bus_watchdog_restore(&b->bus))return EIO;
    if(b->resources.enable_pump&&b->bus.pump_mode_owned&&np_pump_bus_mode_restore(&b->bus))return EIO;
    if(b->resources.enable_pump&&b->bus.pump_adc_owned&&np_pump_bus_restore(&b->bus))return EIO;
    b->suspend = 1;
    if (b->resources.enable_soccp) np_soccp_quiesce(&b->soccp);
    if (b->bus.gpi.owned || b->vote || b->bus.gpi.poisoned || b->quarantine) return EIO;
    if(b->resources.enable_pump&&b->resources.pump_wake(b->resources.context,b->resources.lease,0))return EIO;
    return 0;
}
static int resume(void *v) {
    NpPowerBoard *b = v;
    if (!valid(b)) return EACCES;
    b->suspend = 0;
    if(b->resources.enable_pump&&b->resources.pump_wake(b->resources.context,b->resources.lease,1))return EIO;
    if (b->soccp.attached) return np_soccp_resume(&b->soccp);
    return 0;
}
int np_board_prepare(NpPowerBoard *b, const NpBoardResources *r, NpPowerOps *ops) {
    if (!b || !r || !ops || !r->lease || r->gpii >= 5 || !r->lease_valid || !r->read32 ||
        !r->gauge_vote || !r->dma_identity || !r->now_ms || !r->pause_us ||
        !r->stock_clock.clk_div || !r->stock_clock.t_cycle ||
        (r->enable_soccp && (!r->smem || !r->smem_lock || !r->smem_unlock || !r->doorbell)) ||
        (r->enable_pump&&!r->pump_wake)||(r->enable_auth&&!r->auth_wait_ms)) return EINVAL;
    memset(b, 0, sizeof(*b)); b->resources = *r;
    b->bus.context = b; b->bus.acquire = acquire_bus; b->bus.release = release_bus;
    b->bus.gpii = r->gpii; b->bus.clock = r->stock_clock;
    b->bus.pump_registers_owned=!!r->enable_pump;b->bus.pause_us=pause_bus;
    b->bus.auth_qualified=!!r->enable_auth;b->bus.auth_wait_ms=r->enable_auth?wait_auth:0;
    b->bus.now_ms=now;
    b->bus.gpi.dma_context = b; b->bus.gpi.authorize_dma = dma;
    ops->context = b; ops->now_ms = now; ops->poll_link = poll; ops->send_mca = send;
    ops->read_gauge = gauge; ops->quiesce = quiesce; ops->resume = resume;
    ops->read_pump=pump;ops->set_pump_usb_ovp=usb_ovp;
    ops->set_pump_watchdog=watchdog;
    ops->feed_pump_watchdog=watchdog_feed;
    ops->set_pump_mode1=mode1;
    ops->enable_pump=enable_pump;
    ops->disable_pump=disable_pump;
    ops->prepare_pump_exit=prepare_pump_exit;
    ops->restore_pump_mode=restore_pump_mode;
    ops->auth.context=b;ops->auth.battery_digest=battery_digest;
    ops->read_identity=identity;
    if (!r->enable_soccp) ops->schema_verified = 0;
    return 0;
}
