/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCamcc.h"

#include <assert.h>
#include <stdio.h>

enum { PowerDown = 1u << 15, PowerUp = 1u << 16 };
static const uint32_t off_bit = UINT32_C(1) << 31;

struct CamccModel {
    uint32_t gdsc, status, core, cpas, cci;
    uint32_t pll_mode, pll_opmode, pll_l, pll_alpha, pll_user;
    uint32_t cci_config, cci_command;
    unsigned writes, waits;
    bool no_power_ack, no_cci_disable_ack, no_rcg_ack;
    bool sticky_completion;
    bool core_enable_unobservable;
};

static uint32_t read32(void *context, uint32_t offset)
{
    const struct CamccModel *model = context;
    switch (offset) {
    case 0x21334: return model->gdsc;
    case 0x21338: return model->status;
    case 0x21348: return model->core;
    case 0x2137c: return model->cpas;
    case 0x21268: return model->cci;
    case 0x21250: return model->cci_command;
    case 0x21254: return model->cci_config;
    case 0x00: return model->pll_mode;
    case 0x04: return model->pll_opmode;
    case 0x10: return model->pll_l;
    case 0x14: return model->pll_alpha;
    case 0x18: return model->pll_user;
    default: assert(0); return 0;
    }
}

static void write32(void *context, uint32_t offset, uint32_t value)
{
    struct CamccModel *model = context;
    ++model->writes;
    switch (offset) {
    case 0x21334:
        assert(((value ^ model->gdsc) & ~((1u << 11) | 1u)) == 0);
        model->gdsc = value;
        if (!model->no_power_ack) {
            model->status = model->sticky_completion ? PowerDown | PowerUp :
                ((value & 1u) != 0 ? PowerDown : PowerUp);
        }
        break;
    case 0x21348:
        model->core = model->core_enable_unobservable ? value & ~1u : value;
        break;
    case 0x2137c:
        model->cpas = (value & 1u) != 0 ? value & ~off_bit : value | off_bit;
        break;
    case 0x21268:
        model->cci = (value & 1u) != 0 || model->no_cci_disable_ack ?
            value & ~off_bit : value | off_bit;
        break;
    case 0x21250:
        assert(value == 1);
        model->cci_command = model->no_rcg_ack ? value : value & ~1u;
        break;
    case 0x21254:
        assert((model->cci & (off_bit | 1u)) == 1);
        model->cci_config = value;
        break;
    default: assert(0);
    }
}

static void wait_ms(void *context, unsigned milliseconds)
{
    struct CamccModel *model = context;
    assert(milliseconds == 1);
    ++model->waits;
}

int main(void)
{
    const struct CamccModel cold = {.gdsc = 0x22f001, .status = PowerDown,
                                    .cpas = off_bit, .cci = off_bit};
    struct CamccModel model = cold;
    struct NezhaCameraCamccLease lease = {0};
    const struct NezhaCameraRegisterIo io = {&model, read32, write32, wait_ms};
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccOk);
    assert(lease.top_ready && model.status == PowerUp && model.cci == off_bit);
    assert(nezha_camera_camcc_enable_cci0(&io, &lease) == NezhaCameraCamccOk);
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.gdsc == cold.gdsc && model.status == PowerDown);
    assert(model.core == cold.core && model.cpas == cold.cpas && model.cci == cold.cci);
    assert(!lease.top_ready && lease.enabled_by_us == 0);

    model = (struct CamccModel){.gdsc = 0x22f800, .status = PowerUp,
                                 .core = 1, .cpas = 1, .cci = 1};
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccOk);
    assert(nezha_camera_camcc_enable_cci0(&io, &lease) == NezhaCameraCamccOk);
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.writes == 0);

    model = cold;
    model.no_power_ack = true;
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccTimeout);
    assert(!lease.top_ready && model.writes == 1 && model.waits == 100);
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.gdsc == cold.gdsc);

    model = cold;
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccOk);
    assert(nezha_camera_camcc_enable_cci0(&io, &lease) == NezhaCameraCamccOk);
    model.no_cci_disable_ack = true;
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccTimeout);
    assert(model.status == PowerUp && model.cpas == 1 && lease.enabled_by_us != 0);
    model.no_cci_disable_ack = false;
    model.cci |= off_bit;
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.status == PowerDown && lease.enabled_by_us == 0);

    model = cold;
    model.cci_config = 0x00100001;
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccOk);
    const unsigned before = model.writes;
    assert(nezha_camera_camcc_select_cci0_rate(&io, &lease, 37500000) == NezhaCameraCamccSource);
    assert(model.writes == before);
    model.pll_mode = 5;
    model.pll_opmode = 1;
    model.pll_l = 0x0044003e;
    model.pll_alpha = 0x8000;
    model.pll_user = 0x840f;
    assert(nezha_camera_camcc_select_cci0_rate(&io, &lease, 37500000) == NezhaCameraCamccOk);
    assert(model.cci_config == 0x0010021f && lease.cci_rate_owned);
    assert(nezha_camera_camcc_enable_cci0(&io, &lease) == NezhaCameraCamccOk);
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.cci_config == 0x00100001 && !lease.cci_rate_owned);

    model = cold;
    model.cci_config = 0x0010021f;
    model.no_rcg_ack = true;
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccOk);
    assert(nezha_camera_camcc_select_cci0_rate(&io, &lease, 19200000) == NezhaCameraCamccTimeout);
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccTimeout);
    assert(model.status == PowerUp && lease.cci_rate_owned);
    model.no_rcg_ack = false;
    model.cci_command = 0;
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.cci_config == 0x0010021f && model.status == PowerDown);

    model = cold;
    model.sticky_completion = true;
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccOk);
    assert(model.status == (PowerUp | PowerDown));
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.gdsc == cold.gdsc && lease.enabled_by_us == 0);

    model = cold;
    model.core_enable_unobservable = true;
    assert(nezha_camera_camcc_acquire_top(&io, &lease) == NezhaCameraCamccOk);
    assert(model.core == 0 && model.cpas == 1);
    assert(nezha_camera_camcc_enable_cci0(&io, &lease) == NezhaCameraCamccOk);
    assert(nezha_camera_camcc_release(&io, &lease) == NezhaCameraCamccOk);
    assert(model.status == PowerDown && lease.enabled_by_us == 0);

    puts("Camera CAMCC restores owned gates and keeps the parent on after an uncertain stop");
}
