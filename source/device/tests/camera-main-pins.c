/* SPDX-License-Identifier: MIT */
#include "NezhaCameraMainPins.h"
#include "NezhaCameraCanoePins.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { ClockCommand, ClockConfig = 4, ClockGate = 0x18 };
static const uint32_t off = UINT32_C(1) << 31;

_Static_assert(NezhaCameraGpioPhysical == 0x0f159000, "Canoe first camera GPIO window");
_Static_assert(NezhaCameraGpioPhysical - NezhaCameraGpioOffset + 106 * NezhaCameraGpioStride == 0x0f16a000,
               "Canoe sensor reset GPIO106 uses the same pinctrl window as touch");

static unsigned observed;
static bool observe(void *cookie, enum NezhaCameraPinStep step, unsigned item, unsigned value)
{
    (void)cookie;
    assert(step == NezhaCameraPinRead && item == 106 && value == 106 * 4096);
    ++observed;
    return false;
}

struct MainBoard {
    uint32_t tlmm[117][2];
    uint32_t command, config, gate;
    unsigned milliseconds;
    unsigned rails;
    bool vio_error;
    bool gate_disable_stuck;
};

static uint32_t pin_read(void *cookie, uint32_t offset)
{
    struct MainBoard *board = cookie;
    assert(offset / 0x1000 < 117 && (offset % 0x1000 == 0 || offset % 0x1000 == 4));
    return board->tlmm[offset / 0x1000][(offset % 0x1000) / 4];
}

static void pin_write(void *cookie, uint32_t offset, uint32_t value)
{
    struct MainBoard *board = cookie;
    (void)pin_read(cookie, offset);
    const unsigned pin = offset / 0x1000;
    assert(pin == 89 || pin == 106 || pin == 107 || pin == 111 || pin == 112 || pin == 116);
    if (pin == 106 && offset % 0x1000 == 4 && (value & 2u) != 0) {
        assert(board->tlmm[107][1] & 2u);
        assert((board->gate & (off | 1u)) == 1u && board->milliseconds == 19 && board->rails == 7);
    }
    if (pin == 116 && offset % 0x1000 == 4 && (value & 2u) != 0) {
        assert(board->rails == 3 && board->milliseconds == 3);
    }
    board->tlmm[pin][(offset % 0x1000) / 4] = value;
}

static uint32_t clock_read(void *cookie, uint32_t offset)
{
    struct MainBoard *board = cookie;
    switch (offset) {
    case ClockCommand: return board->command;
    case ClockConfig: return board->config;
    case ClockGate: return board->gate;
    default: assert(0); return 0;
    }
}

static void clock_write(void *cookie, uint32_t offset, uint32_t value)
{
    struct MainBoard *board = cookie;
    switch (offset) {
    case ClockCommand: board->command = value & ~1u; break;
    case ClockConfig:
        assert((board->gate & (off | 1u)) == 1);
        board->config = value;
        break;
    case ClockGate:
        assert((board->tlmm[106][1] & 2u) == 0);
        if ((value & 1u) != 0) {
            assert((board->tlmm[116][1] & 2u) != 0 && board->milliseconds == 13);
        }
        board->gate = (value & 1u) != 0 || board->gate_disable_stuck ? value & ~off : value | off;
        break;
    default: assert(0);
    }
}

static void wait_ms(void *cookie, unsigned milliseconds)
{
    struct MainBoard *board = cookie;
    board->milliseconds += milliseconds;
}

static bool acquire_rail(void *cookie, enum NezhaCameraMainRail rail, unsigned millivolts)
{
    struct MainBoard *board = cookie;
    static const unsigned voltages[] = {3296, 3400, 1800};
    static const unsigned start_ms[] = {0, 1, 16};
    const unsigned bit = 1u << rail;
    assert(millivolts == voltages[rail] && board->milliseconds == start_ms[rail]);
    assert(board->rails == bit - 1 && (board->tlmm[106][1] & 2u) == 0);
    if (rail == NezhaCameraMainVio) {
        assert((board->tlmm[107][1] & 2u) != 0 && (board->gate & 1u) != 0);
    }
    board->rails |= bit;
    return !(rail == NezhaCameraMainVio && board->vio_error);
}

static bool release_rail(void *cookie, enum NezhaCameraMainRail rail)
{
    struct MainBoard *board = cookie;
    const unsigned bit = 1u << rail;
    assert(board->rails == bit * 2u - 1u && (board->tlmm[106][1] & 2u) == 0);
    if (rail == NezhaCameraMainVio) {
        assert((board->tlmm[107][1] & 2u) != 0);
    } else {
        assert((board->tlmm[116][1] & 2u) == 0 && (board->gate & (off | 1u)) == off);
    }
    board->rails &= ~bit;
    return true;
}

int main(void)
{
    const struct MainBoard original = {.tlmm = {[106] = {0x801, 0}, [107] = {0x201, 0},
                                     [111] = {5, 1}, [112] = {5, 1}},
                                      .config = 0x10021f, .gate = off};
    struct MainBoard board = original;
    struct NezhaCameraMainPins state = {0};
    const struct NezhaCameraRegisterIo tlmm = {&board, pin_read, pin_write, wait_ms};
    const struct NezhaCameraRegisterIo mclk = {&board, clock_read, clock_write, wait_ms};
    const struct NezhaCameraMainRailIo rails = {&board, acquire_rail, release_rail};

    assert(nezha_camera_main_hold_reset(&tlmm, &state));
    assert(board.tlmm[106][1] == 0 && board.tlmm[107][1] == 0);
    assert(nezha_camera_main_start_pins(&tlmm, &mclk, &rails, &state));
    assert(board.config == 0x100001 && board.gate == 1);
    assert(board.tlmm[106][1] == 2 && board.tlmm[107][1] == 2);
    assert(board.tlmm[111][0] == 4 && board.tlmm[112][0] == 4 && board.milliseconds == 24);

    board.gate_disable_stuck = true;
    assert(!nezha_camera_main_restore_pins(&tlmm, &mclk, &rails, &state));
    assert(board.tlmm[106][1] == 0 && state.mclk_gate_owned && board.rails == 3);
    board.gate_disable_stuck = false;
    board.gate |= off;
    assert(nezha_camera_main_restore_pins(&tlmm, &mclk, &rails, &state));
    assert(!state.reset_held && board.gate == original.gate && board.config == original.config && board.rails == 0);
    assert(memcmp(board.tlmm, original.tlmm, sizeof(board.tlmm)) == 0);

    board = original;
    board.vio_error = true;
    assert(nezha_camera_main_hold_reset(&tlmm, &state));
    assert(!nezha_camera_main_start_pins(&tlmm, &mclk, &rails, &state));
    assert(board.tlmm[106][1] == 0 && !state.running);
    assert(nezha_camera_main_restore_pins(&tlmm, &mclk, &rails, &state));
    assert(board.rails == 0 && memcmp(board.tlmm, original.tlmm, sizeof(board.tlmm)) == 0);

    state.observe = (struct NezhaCameraPinObserver){NULL, observe};
    assert(!nezha_camera_main_hold_reset(&tlmm, &state));
    assert(observed == 1 && !state.reset_held && !state.pins_changed && board.rails == 0);
    puts("Main camera applies the stock rail and MCLK order and restores owned resources after failure");
}
