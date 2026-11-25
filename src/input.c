#include "input.h"

#include <string.h>
#include <psxapi.h>
#include <psxpad.h>

/* 4-byte aligned BIOS controller poll buffers for Ports 1 & 2 */
static uint8_t s_pad_buf[2][34] __attribute__((aligned(4)));

static uint16_t s_held      = 0;
static uint16_t s_prev      = 0;
static uint16_t s_pressed   = 0;
static uint16_t s_released  = 0;
static int      s_connected = 0;

void input_init(void) {
    /* Fill with 0xFF (active-LOW released state) before first BIOS VBlank poll */
    memset(s_pad_buf, 0xFF, sizeof(s_pad_buf));
    s_held      = 0;
    s_prev      = 0;
    s_pressed   = 0;
    s_released  = 0;
    s_connected = 0;

    InitPAD(s_pad_buf[0], 34, s_pad_buf[1], 34);
    StartPAD();
    ChangeClearPAD(0);
}

void input_update(void) {
    const PADTYPE *pad = (const PADTYPE *)s_pad_buf[0];

    s_prev = s_held;

    if (pad->stat == 0 &&
        (pad->type == PAD_ID_DIGITAL ||
         pad->type == PAD_ID_ANALOG ||
         pad->type == PAD_ID_ANALOG_STICK)) {
        uint16_t cur = (uint16_t)(~pad->btn);

        /* Also mirror Left Analog Stick onto D-Pad in Analog/DualShock mode */
        if (pad->type == PAD_ID_ANALOG || pad->type == PAD_ID_ANALOG_STICK) {
            if (pad->ls_x < 64)  cur |= BTN_LEFT;
            if (pad->ls_x > 192) cur |= BTN_RIGHT;
            if (pad->ls_y < 64)  cur |= BTN_UP;
            if (pad->ls_y > 192) cur |= BTN_DOWN;
        }

        s_connected = 1;
        s_held      = cur;
    } else {
        s_connected = 0;
        s_held      = 0;
    }

    s_pressed  = (uint16_t)(s_held & (uint16_t)(~s_prev));
    s_released = (uint16_t)((uint16_t)(~s_held) & s_prev);
}

int input_connected(void) {
    return s_connected;
}

uint16_t input_raw_held(void) {
    return s_held;
}

int input_held(uint16_t btn) {
    return (s_held & btn) != 0;
}

int input_pressed(uint16_t btn) {
    return (s_pressed & btn) != 0;
}

int input_released(uint16_t btn) {
    return (s_released & btn) != 0;
}
