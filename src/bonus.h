#ifndef TSINGTAO_BONUS_H
#define TSINGTAO_BONUS_H

#include <stdint.h>
#include "roguelike.h"

#define BONUS_NUM_CANS 4

typedef enum {
    BONUS_PHASE_INTRO   = 0,
    BONUS_PHASE_SHAKE   = 1,
    BONUS_PHASE_SHUFFLE = 2,
    BONUS_PHASE_PICK    = 3,
    BONUS_PHASE_REVEAL  = 4,
    BONUS_PHASE_REWARD  = 5,
    BONUS_PHASE_DONE    = 6
} BonusPhase;

typedef struct {
    int32_t  x_fp;         /* Current X position (24.8 fixed-point) */
    int32_t  y_fp;         /* Current Y position (24.8 fixed-point) */
    uint8_t  slot;         /* Logical slot 0..3 */
    uint8_t  is_cold;      /* 1 = the unshaken cold can, 0 = shaken */
} BonusCan;

typedef struct {
    BonusPhase phase;
    uint16_t   night;
    uint16_t   phase_timer;
    uint32_t   rng_state;

    BonusCan   cans[BONUS_NUM_CANS];
    int8_t     cold_slot;    /* Current logical slot 0..3 of the cold can */

    /* Active parabolic arc swap */
    int8_t     swap_a;       /* Can index A (0..3) */
    int8_t     swap_b;       /* Can index B (0..3) */
    int8_t     prev_swap_a;  /* Last swapped can index A (prevent back-to-back repeat) */
    int8_t     prev_swap_b;  /* Last swapped can index B */
    int16_t    swap_start_ax;
    int16_t    swap_start_bx;
    int16_t    swap_tick;
    int16_t    swap_dur;     /* Frames per swap */
    int16_t    swaps_left;
    int16_t    swap_pause;   /* Short pause between swaps */

    /* Player pick & result */
    int8_t     cursor_slot;  /* Highlighted slot 0..3 */
    int8_t     picked_can;   /* Index into cans[] */
    uint8_t    won;          /* 1 = picked cold unshaken can */
    int16_t    pick_timeout; /* Safety countdown during pick phase */

    /* 3D Reward Card Picker on WIN */
    int8_t     reward_cursor;  /* 0..2 */
    uint8_t    reward_coaster; /* Rolled Rare Coaster ID for slot 0 */
    int16_t    reward_flip;    /* Card flip-in timer */
    int16_t    reward_timeout; /* Safety countdown during reward pick */
} BonusState;

/* Initialize "SPOT THE COLD ONE!" Boss Bonus Round */
void bonus_init(BonusState *bs, RunState *run);

/* Step 1 frame at 50 Hz. Returns 1 when bonus round (and reward pick) is finished. */
int bonus_update(BonusState *bs, RunState *run);

/* Draw the 2.5D hero bar, 4 real Tsingtao cans, shake/swap VFX, or 3D reward picker */
void bonus_draw(const BonusState *bs, const RunState *run);

#endif /* TSINGTAO_BONUS_H */
