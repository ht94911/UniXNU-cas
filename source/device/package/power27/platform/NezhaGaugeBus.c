/* SPDX-License-Identifier: APSL-2.0 */
/* Board coordinates from nezha's stock FDT, operations from pandora GPI. This
 * peripheral layer never allocates/reprograms someone else's SMMU context or
 * copies the entire GCC vote register back over another consumer's votes. */
#include "NezhaGaugeBus.h"
#include <sys/errno.h>
#include <string.h>

static int open_bus(NpGaugeBus *b)
{
    if (!b || !b->acquire || !b->release || b->gpii >= 5 || !b->clock.clk_div || !b->clock.t_cycle)
        return EINVAL;
    int r = b->acquire(b->context);
    if (r) return r;
    b->gpi.quiet = true;
    r = np_gpi_open(&b->gpi, 0xa00000, 0x60000, 0x10000, b->gpii, 5, 3);
    if (r) {
        const int stop = np_gpi_close(&b->gpi);
        b->release(b->context, stop == 0 && !b->gpi.poisoned);
    }
    return r;
}
static int close_bus(NpGaugeBus *b, int result)
{
    const int stop = np_gpi_close(&b->gpi);
    b->release(b->context, stop == 0 && !b->gpi.poisoned);
    return stop ? stop : result;
}
int np_gauge_bus_read(void *context, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *valid)
{
    NpGaugeBus *b = context;
    if (!out || !valid) return EINVAL;
    *valid = 0; memset(out, 0, NP_GAUGE_WORDS * 2);
    int r = open_bus(b);
    if (r) return r;
    for (unsigned i = 0; i < NP_GAUGE_WORDS; ++i) {
        const uint8_t command = np_gauge_commands[i];
        r = np_gpi_i2c(&b->gpi, &b->clock, 0x55, &command, 1, out[i], 2);
        if (r) break; /* no further transfers after a bus/ownership error */
        *valid |= 1u << i;
    }
    return close_bus(b, r);
}
int np_gauge_bus_original_temperature(NpGaugeBus *b,NpBoardTemperatureReading *out)
{
    if(!out)return EINVAL;
    memset(out,0,sizeof(*out));
    if(!b||!b->now_ms){out->error=EACCES;return EACCES;}
    out->begin_ms=b->now_ms(b->context);
    int error=open_bus(b);
    if(!error){
        /* N0support-pnc selects MASTER original_temp. This one direct word
         * read uses the same shared GPI bus lease as ordinary gauge samples. */
        const uint8_t command=0x1e;
        uint8_t bytes[2]={0};
        error=np_gpi_i2c(&b->gpi,&b->clock,0x55,&command,1,bytes,2);
        if(!error){
            out->raw=(uint16_t)(bytes[0]|(uint16_t)bytes[1]<<8);
            error=np_bq_original_temperature_milli_c(out->raw,&out->milli_c);
        }
        error=close_bus(b,error);
    }
    out->end_ms=b->now_ms(b->context);
    if(!error&&out->end_ms<out->begin_ms)error=EIO;
    out->error=error<0?-error:error;
    out->valid=!error;
    return out->error;
}
int np_gauge_bus_auth_digest(NpGaugeBus *b,const uint8_t nonce[32],uint8_t digest[32])
{
    if(!nonce||!digest)return EINVAL;
    memset(digest,0,32);
    if(!b||!b->auth_qualified||!b->auth_wait_ms)return EACCES;
    int error=open_bus(b);
    if(error)return error;
    /* Actual OS4 fg_set_verify_digest: volatile command0, challenge32,
     * ones-complement checksum and length36 at MAC3e; wait400ms/read MAC40.
     * Its register table indices19/20 differ from newer public enum indices. */
    uint8_t bytes[37]={0x3e,0,0};unsigned sum=0;
    memcpy(bytes+3,nonce,32);
    for(unsigned i=0;i<32;++i)sum+=nonce[i];
    bytes[35]=(uint8_t)~sum;bytes[36]=0x24;
    error=np_gpi_i2c(&b->gpi,&b->clock,0x55,bytes,sizeof(bytes),0,0);
    if(!error){
        b->auth_wait_ms(b->context,400);
        uint8_t reg=0x40;
        error=np_gpi_i2c(&b->gpi,&b->clock,0x55,&reg,1,digest,32);
    }
    volatile uint8_t *secret=bytes;
    for(unsigned i=0;i<sizeof(bytes);++i)secret[i]=0;
    if(error)memset(digest,0,32);
    return close_bus(b,error);
}
int np_gauge_bus_identity(NpGaugeBus *b,NpGaugeIdentity *out)
{
    if(!out)return EINVAL;
    memset(out,0,sizeof(*out));
    if(!b||!b->pause_us)return EACCES;
    int error=open_bus(b);if(error)return error;
    uint8_t name[16]={0},chemistry[4]={0},reply[36]={0};
    const uint16_t commands[]={0x004a,0x004b};
    for(unsigned i=0;!error&&i<2;++i) {
        /* OS4 fg_mac_read_block: selector only, wait4000us, read36 at3e,
         * match echoed command, length and ones-complement checksum. */
        uint8_t select[3]={0x3e,(uint8_t)commands[i],0},reg=0x3e;
        error=np_gpi_i2c(&b->gpi,&b->clock,0x55,select,sizeof(select),0,0);
        if(!error){
            b->pause_us(b->context,4000);
            error=np_gpi_i2c(&b->gpi,&b->clock,0x55,&reg,1,reply,sizeof(reply));
        }
        if(!error&&np_mac_info_decode(commands[i],reply,i?chemistry:name,i?4:16))error=EIO;
    }
    if(!error&&np_gauge_identity_decode(out,name,chemistry))error=EIO;
    error=close_bus(b,error);
    if(error){memset(out,0,sizeof(*out));out->error=(uint32_t)error;}
    return error;
}
static int pump_update(NpGaugeBus *,uint8_t,uint8_t,uint8_t);
int np_pump_bus_read(NpGaugeBus *b, NpPump *out)
{
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (!b || !b->pump_registers_owned) return EACCES;
    if(b->pump_init_failed)return EIO;
    int r = open_bus(b);
    if (r) return r;
    uint8_t reg = 0x6e, chip = 0, c[13] = {0}, a[18] = {0},controls[22]={0};
    r = np_gpi_i2c(&b->gpi, &b->clock, 0x6e, &reg, 1, &chip, 1);
    if (!r && chip != 0x85) r = ENODEV;
    /* Exclusive event ownership is required: 15h includes ADC_DONE_FLAG and
     * its read side effects are not qualified. Never run alongside the stock
     * SC8585 event handler. Do not turn on the ADC merely to measure it. */
    static const uint8_t registers[] = {0x0a,0x0b,0x0e,0x15,0x16};
    uint16_t control_valid = 0;
    for (unsigned i = 0; i < sizeof(registers) && !r; ++i) {
        reg = registers[i];
        r = np_gpi_i2c(&b->gpi, &b->clock, 0x6e, &reg, 1, &c[reg - 0x0a], 1);
        if (!r) control_valid |= 1u << (reg - 0x0a);
    }
    const uint8_t first_state=c[0];
    const uint8_t first_enable=c[1]&0x80;
    if(!r) {
        reg=0x01;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,controls,sizeof(controls));
        /* Guard initialization with the later complete register read. Keep
         * the first status flags separately if a subsequent read consumes them. */
        if(!r)for(unsigned i=0;i<sizeof(registers);++i)
            c[registers[i]-0x0a]=controls[registers[i]-1];
    }
    /* Stock probe resets the CP after waking NLPM. This driver only clears
     * CHG_EN and verifies switching stopped; it never resets protections or
     * enables a pump using an inherited, unqualified configuration. */
    if(!r&&!b->pump_initialized) {
        b->pump_initial_enable=!!(first_enable|(c[1]&0x80));
        b->pump_init_failed=1; /* no replay after an uncertain handoff write */
        if((c[1]&0x80)||(c[0]&2)) {
            r=pump_update(b,0x0b,0x80,0);
            for(unsigned i=0;!r&&i<50;++i) {
                reg=0x0a;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,c,2);
                if(r||!(c[0]&2))break;
                if(!b->pause_us){r=EACCES;break;}
                b->pause_us(b->context,1000);
            }
            if(!r&&((c[0]&2)||(c[1]&0x80)))r=ETIMEDOUT;
        }
        if(!r){b->pump_initialized=1;b->pump_init_failed=0;}
    }
    /* Only the selected sole SC8585 owner may enable measurement. Never
     * change scan controls underneath an inherited running pump. */
    if(!r && !(c[1]&0x80) && !(c[0]&2) && (c[4]&7)<=2 && b->pause_us && !b->pump_adc_owned) {
        b->saved_adc15=c[11];b->saved_adc16=c[12];
        b->pump_adc_owned=1; /* before either write; uncertain IO is retained */
        /* Stock SC8585 skips IBAT/TSBAT scan controls (16h bits2/1).
         * Only own VBUS/VBAT/TDIE; preserve the other scan fields. */
        uint8_t set16[2]={0x16,(uint8_t)(c[12]&~0x89u)};
        uint8_t set15[2]={0x15,(uint8_t)((c[11]&~0xc9u)|0x88u)};
        r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,set16,2,0,0);
        if(!r)r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,set15,2,0,0);
        if(!r) {
            b->pause_us(b->context,20000);
            reg=0x15;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,c+11,2);
        }
    }
    /* Ownership records a possibly submitted write, not its confirmation.
     * Every sample must observe the same owned configuration before decoding. */
    if(!r && b->pump_adc_owned &&
       ((c[11]&0xc9)!=0x88||(c[12]&0x89)!=0))r=EIO;
    if (!r && (c[11] & 0x80)) {
        reg = 0x17;
        r = np_gpi_i2c(&b->gpi, &b->clock, 0x6e, &reg, 1, a, 12);
        if(!r){reg=0x27;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,a+16,2);}
    }
    if (!r) {
        r = np_sc8585_decode(out, chip, c, control_valid, a) ? EIO : 0;
        memcpy(out->control,controls,sizeof(controls));out->controls_valid=1;
        out->initial_enable=b->pump_initial_enable;
        (void)np_sc8585_protection_decode(&out->protection,chip,controls,first_state);
    }
    return close_bus(b, r);
}
static int pump_update(NpGaugeBus *b,uint8_t reg,uint8_t mask,uint8_t value)
{
    uint8_t old=0,readback=0,bytes[2]={reg,0};
    int error=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&old,1);
    if(error)return error;
    bytes[1]=(old&~mask)|(value&mask);
    error=np_gpi_i2c(&b->gpi,&b->clock,0x6e,bytes,2,0,0);
    if(!error)error=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&readback,1);
    return error?error:((readback&mask)==(value&mask)?0:EIO);
}
int np_pump_bus_usb_ovp(NpGaugeBus *b,uint8_t expected,uint8_t value,
                       uint8_t *observed,uint8_t *attempted)
{
    if(!observed||!attempted)return EINVAL;
    *observed=255;*attempted=0;
    if(!b||!b->pump_registers_owned||!b->pump_initialized||b->pump_init_failed||
       b->pump_usb_ovp_failed||!b->pump_adc_owned)return EACCES;
    if(!((expected==15&&value==0&&!b->pump_usb_ovp_owned)||
         (expected==0&&value==15&&b->pump_usb_ovp_owned&&b->saved_usb_ovp==15)))return EACCES;
    int r=open_bus(b);if(r)return r;
    uint8_t reg=0x6e,chip=0,state[2]={0},adc=0,bus[2]={0},old=0,after=0;
    r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&chip,1);
    if(!r&&chip!=0x85)r=ENODEV;
    if(!r){reg=0x0a;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,state,2);}
    if(!r&&((state[0]&2)||(state[1]&0x80)))r=EBUSY;
    if(!r){reg=0x15;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&adc,1);}
    if(!r&&(adc&0xc9)!=0x88)r=EACCES;
    if(!r){reg=0x19;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,bus,2);}
    uint32_t raw=(uint32_t)bus[0]<<8|bus[1],mv=raw*625u/100u;
    /* Both changing to11V and restoring7.5V require measured low input.
     * Never lower a protection threshold while9V is still present. */
    if(!r&&(raw>0xfff||mv>5500||(value==0&&mv<4500)))r=EBUSY;
    if(!r){reg=0x03;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&old,1);*observed=old&15;}
    if(!r&&(old&15)!=expected)r=EAGAIN;
    if(!r) {
        if(value==0){b->saved_usb_ovp=expected;b->pump_usb_ovp_owned=1;}
        *attempted=1;uint8_t bytes[2]={0x03,(uint8_t)((old&0xf0)|value)};
        r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,bytes,2,0,0);
        if(!r){reg=0x03;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&after,1);*observed=after&15;}
        if(!r&&(after&15)!=value)r=EIO;
        if(r)b->pump_usb_ovp_failed=1;
        else if(value==15)b->pump_usb_ovp_owned=0;
    }
    return close_bus(b,r);
}
int np_pump_bus_restore(NpGaugeBus *b)
{
    if(!b||!b->pump_registers_owned)return EACCES;
    if(b->pump_usb_ovp_owned||b->pump_usb_ovp_failed)return EBUSY;
    if(!b->pump_adc_owned)return 0;
    int error=open_bus(b);if(error)return error;
    uint8_t reg=0x0b,enable=0;
    error=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&enable,1);
    if(!error&&(enable&0x80))error=EBUSY;
    if(!error)error=pump_update(b,0x15,0xc9,b->saved_adc15);
    if(!error)error=pump_update(b,0x16,0x89,b->saved_adc16);
    if(!error)b->pump_adc_owned=0;
    return close_bus(b,error);
}
int np_pump_bus_disable(NpGaugeBus *b)
{
    if(!b||!b->pump_registers_owned||!b->pause_us)return EACCES;
    int error=open_bus(b);if(error)return error;
    error=pump_update(b,0x0b,0x80,0);
    uint8_t state=2,reg=0x0a;
    for(unsigned i=0;!error&&i<50;++i) {
        error=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&state,1);
        if(error||!(state&2))break;
        b->pause_us(b->context,1000);
    }
    if(!error&&(state&2))error=ETIMEDOUT;
    return close_bus(b,error);
}
