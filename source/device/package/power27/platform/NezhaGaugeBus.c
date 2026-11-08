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
    if(!r) {
        reg=0x01;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,controls,sizeof(controls));
    }
    /* Stock probe resets the CP after waking NLPM. This driver only clears
     * CHG_EN and verifies switching stopped; it never resets protections or
     * enables a pump using an inherited, unqualified configuration. */
    if(!r&&!b->pump_initialized) {
        b->pump_initial_enable=!!(c[1]&0x80);
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
        uint8_t set16[2]={0x16,(uint8_t)((c[12]&~0x8fu)|0x06u)};
        uint8_t set15[2]={0x15,(uint8_t)((c[11]&~0xc9u)|0x88u)};
        r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,set16,2,0,0);
        if(!r)r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,set15,2,0,0);
        if(!r) {
            b->pause_us(b->context,20000);
            reg=0x15;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,c+11,2);
            if(!r && ((c[11]&0xc9)!=0x88||(c[12]&0x8f)!=0x06))r=EIO;
        }
    }
    if (!r && (c[11] & 0x80)) {
        reg = 0x17;
        r = np_gpi_i2c(&b->gpi, &b->clock, 0x6e, &reg, 1, a, 12);
        if(!r){reg=0x27;r=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,a+16,2);}
    }
    if (!r) {
        r = np_sc8585_decode(out, chip, c, control_valid, a) ? EIO : 0;
        memcpy(out->control,controls,sizeof(controls));out->controls_valid=1;
        out->initial_enable=b->pump_initial_enable;
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
int np_pump_bus_restore(NpGaugeBus *b)
{
    if(!b||!b->pump_registers_owned)return EACCES;
    if(!b->pump_adc_owned)return 0;
    int error=open_bus(b);if(error)return error;
    uint8_t reg=0x0b,enable=0;
    error=np_gpi_i2c(&b->gpi,&b->clock,0x6e,&reg,1,&enable,1);
    if(!error&&(enable&0x80))error=EBUSY;
    if(!error)error=pump_update(b,0x15,0xc9,b->saved_adc15);
    if(!error)error=pump_update(b,0x16,0x8f,b->saved_adc16);
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
