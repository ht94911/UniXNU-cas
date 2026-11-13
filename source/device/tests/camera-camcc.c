/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCamcc.h"

#include <assert.h>
#include <stdio.h>

enum { PowerDown = 1u << 15, PowerUp = 1u << 16 };
static const uint32_t off_bit = UINT32_C(1) << 31;

struct CamccModel {
    uint32_t gdsc, status, core, cpas, cci;
    uint32_t pll_mode, pll_opmode, pll_l, pll_alpha, pll_user;
    uint32_t pll_cfg[3], pll_user_hi;
    uint32_t cci_config, cci_command;
    unsigned writes, waits;
    bool no_power_ack, no_cci_disable_ack, no_rcg_ack;
    bool sticky_completion;
    bool core_enable_unobservable;
    bool pll_no_lock;
};

struct CaptureModel {
    uint32_t word[0x22000 / 4];
    uint32_t refuse_off;
};

static uint32_t capture_read(void *context, uint32_t offset)
{
    const struct CaptureModel *m = context;
    assert((offset & 3u) == 0 && offset < sizeof(m->word));
    return m->word[offset / 4];
}

static void capture_write(void *context, uint32_t offset, uint32_t value)
{
    struct CaptureModel *m = context;
    assert((offset & 3u) == 0 && offset < sizeof(m->word));
    if (offset == 0x21004) {
        m->word[0x21008 / 4] = (value & 1u) ? PowerDown : PowerUp;
    } else if (offset == 0x20000 || offset == 0x200dc || offset == 0x20100) {
        if ((value & 1u) != 0) {
            assert((value & 2u) != 0 && (m->word[offset / 4] & off_bit) == 0);
        }
        value = (value & ~off_bit & ~1u) | ((value & 2u) != 0 ? 0 : off_bit);
    } else if (offset == 0x20004 || offset == 0x200e0 || offset == 0x20104) {
        assert((m->word[(offset - 4) / 4] & (off_bit | 2u)) == 2u);
    } else {
        assert(offset != 0x21064 && offset != 0x212a4 && offset != 0x21018 && offset != 0x212cc);
        if (value & 1u) {
            value &= ~off_bit;
        } else if (offset != m->refuse_off) {
            value |= off_bit;
        }
    }
    m->word[offset / 4] = value;
}

static void capture_wait(void *context, unsigned milliseconds)
{
    (void)context;
    assert(milliseconds == 1);
}

static uint32_t gcc_read(void *context, uint32_t offset)
{
    uint32_t *registers = context;
    assert(offset == 0x26004 || offset == 0x2603c || offset == 0xa0004 ||
           offset == 0x26014 || offset == 0x26028);
    return registers[offset / 4];
}

static bool gcc_update(void *context, uint32_t offset, bool enabled)
{
    uint32_t *registers = context;
    registers[offset / 4] = (gcc_read(context, offset) & ~1u) | (enabled ? 1u : 0u);
    return true;
}

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
    case 0x1c: return model->pll_user_hi;
    case 0x20: case 0x24: case 0x28: return model->pll_cfg[(offset - 0x20) / 4];
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
    case 0:
        if ((value & 1u) != 0) {
            assert((model->pll_mode & off_bit) != 0 && model->pll_opmode == 1);
        }
        model->pll_mode = (value & ~off_bit) | (model->pll_mode & off_bit);
        break;
    case 4:
        model->pll_opmode = value;
        if (value == 1) {
            assert((model->pll_mode & 4u) != 0 && (model->pll_mode & 1u) == 0);
            assert(model->pll_l == 0x4444003e && model->pll_alpha == 0x8000 && model->pll_user == 0x840f);
            if (!model->pll_no_lock) {
                model->pll_mode |= off_bit;
            }
        } else {
            assert(value == 0 && (model->pll_mode & 1u) == 0);
            model->pll_mode &= ~off_bit;
        }
        break;
    case 0x10: model->pll_l = value; break;
    case 0x14: model->pll_alpha = value; break;
    case 0x18: model->pll_user = value; break;
    case 0x1c: model->pll_user_hi = value; break;
    case 0x20: case 0x24: case 0x28: model->pll_cfg[(offset - 0x20) / 4] = value; break;
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

    struct NezhaCameraPll0Lease pll = {0};
    model = cold;
    assert(nezha_camera_camcc_acquire_pll0(&io, &pll) == NezhaCameraCamccOk);
    assert(pll.ready && pll.owned && (model.pll_mode & (off_bit | 5u)) == (off_bit | 5u));
    assert(nezha_camera_camcc_release_pll0(&io, &pll, false) == NezhaCameraCamccBusy);
    assert((model.pll_mode & 1u) != 0);
    assert(nezha_camera_camcc_release_pll0(&io, &pll, true) == NezhaCameraCamccOk);
    assert(model.pll_mode == 0 && model.pll_l == 0 && model.pll_user == 0 && model.pll_cfg[0] == 0);

    model = cold;
    model.pll_no_lock = true;
    assert(nezha_camera_camcc_acquire_pll0(&io, &pll) == NezhaCameraCamccTimeout);
    assert(pll.owned && !pll.ready && (model.pll_mode & 1u) == 0 && model.waits == 100);
    assert(nezha_camera_camcc_release_pll0(&io, &pll, true) == NezhaCameraCamccOk);
    assert(model.pll_l == 0 && model.pll_mode == 0);

    model = cold;
    model.pll_mode = off_bit | 5;
    model.pll_opmode = 1;
    model.pll_l = 0x4444003e;
    model.pll_alpha = 0x8000;
    model.pll_user = 0x840f;
    assert(nezha_camera_camcc_acquire_pll0(&io, &pll) == NezhaCameraCamccOk);
    assert(pll.ready && !pll.owned && model.writes == 0);
    assert(nezha_camera_camcc_release_pll0(&io, &pll, true) == NezhaCameraCamccOk && model.writes == 0);
    model.pll_l = 0x44440032;
    assert(nezha_camera_camcc_acquire_pll0(&io, &pll) == NezhaCameraCamccBusy && model.writes == 0);

    struct CaptureModel clocks = {0};
    const struct NezhaCameraRegisterIo clock_io = {&clocks, capture_read, capture_write, capture_wait};
    clocks.word[0x21004 / 4] = 0x22f001;
    clocks.word[0x21008 / 4] = PowerDown;
    clocks.word[0x20000 / 4] = off_bit;
    clocks.word[0x200dc / 4] = off_bit;
    clocks.word[0x20100 / 4] = off_bit;
    clocks.word[0x20004 / 4] = 0x100000;
    clocks.word[0x212e4 / 4] = 1; // Inherited live gate; the lease won't turn it off.
    const struct NezhaCameraCamccLease top = {.top_ready = true};
    const struct NezhaCameraPll0Lease ready_pll = {.ready = true};
    struct NezhaCameraCaptureClocks capture = {0};
    assert(nezha_camera_camcc_acquire_capture(&clock_io, &top, &ready_pll, &capture) == NezhaCameraCamccOk);
    assert(capture.ready && clocks.word[0x21008 / 4] == PowerUp);
    assert(clocks.word[0x20004 / 4] == 0x100301 && clocks.word[0x20000 / 4] == off_bit);
    assert(clocks.word[0x200e0 / 4] == 0x100203 && clocks.word[0x20104 / 4] == 0x10020e);
    assert(nezha_camera_camcc_release_capture(&clock_io, &capture, false) == NezhaCameraCamccBusy);
    clocks.refuse_off = 0x21044;
    assert(nezha_camera_camcc_release_capture(&clock_io, &capture, true) == NezhaCameraCamccTimeout);
    assert(clocks.word[0x21008 / 4] == PowerUp && clocks.word[0x20104 / 4] == 0x10020e);
    clocks.refuse_off = 0;
    clocks.word[0x21044 / 4] |= off_bit; // Hardware completes the issued stop.
    assert(nezha_camera_camcc_release_capture(&clock_io, &capture, true) == NezhaCameraCamccOk);
    assert(clocks.word[0x21008 / 4] == PowerDown && clocks.word[0x212e4 / 4] == 1);
    assert(clocks.word[0x20004 / 4] == 0x100000 && clocks.word[0x200e0 / 4] == 0);
    assert(!capture.gates.enabled_by_us && !capture.roots && !capture.tfe_owned);

    static uint32_t gcc_words[0xa0008 / 4];
    const struct NezhaCameraGccIo gcc = {gcc_words, gcc_read, gcc_update};
    gcc_words[0x26004 / 4] = 0x101; // Already enabled.
    gcc_words[0x26014 / 4] = 2;     // Inherited HWCG, independent of enable.
    clocks.word[0x21398 / 4] = 1;
    struct NezhaCameraInterfaces interfaces = {0};
    assert(nezha_camera_interfaces_start(&gcc, &clock_io, &interfaces) == NezhaCameraCamccOk);
    assert(gcc_words[0x26014 / 4] == 3 && interfaces.gcc_owned == 0x1e);
    assert(interfaces.camcc_owned == 0x1d && (clocks.word[0x21390 / 4] & 1u) != 0);
    assert(nezha_camera_interfaces_stop(&gcc, &clock_io, &interfaces, false) == NezhaCameraCamccBusy);
    assert(gcc_words[0x26014 / 4] == 3 && interfaces.ready);
    assert(nezha_camera_interfaces_stop(&gcc, &clock_io, &interfaces, true) == NezhaCameraCamccOk);
    assert(gcc_words[0x26004 / 4] == 0x101 && gcc_words[0x26014 / 4] == 2);
    assert(clocks.word[0x21398 / 4] == 1 && (clocks.word[0x21390 / 4] & 1u) == 0);

    puts("Camera CAMCC owns only C2 gates and software roots, with PLL lock and stop checks");
}
