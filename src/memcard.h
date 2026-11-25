#ifndef TSINGTAO_MEMCARD_H
#define TSINGTAO_MEMCARD_H

#include <stdint.h>

#define MC_MAX_SCORES   5
#define MC_SAVE_VERSION 2u

typedef struct {
    char     initials[4]; /* 3 uppercase ASCII chars + NUL */
    uint32_t score;
    uint16_t wave;        /* Night reached (1..8+) */
    uint8_t  shift;       /* Shift within night (0..2) */
    uint8_t  won;         /* 1 if Night 8 Boss cleared, 0 otherwise */
} HighScoreEntry;

typedef enum {
    MC_OK = 0,
    MC_NO_CARD = 1,
    MC_UNFORMATTED = 2,
    MC_FULL = 3,
    MC_NO_FILE = 4,
    MC_ERROR = 5
} MemCardStatus;

/* Initialize Slot 1 PS1 BIOS Memory Card subsystem (InitCARD / StartCARD / _bu_init) */
void memcard_init(void);

/* Populate table with built-in 1996 Qingdao arcade default high scores */
void memcard_load_defaults(HighScoreEntry scores[MC_MAX_SCORES]);

/*
 * Load Top 5 high scores from bu00:BESLES-09600TSING.
 * Never blocks or crashes if no card is inserted or unformatted; falls back
 * to built-in defaults on any non-MC_OK result.
 */
MemCardStatus memcard_load_scores(HighScoreEntry scores[MC_MAX_SCORES]);

/*
 * Save Top 5 high scores as a valid 1-block (8192-byte) PS1 save file with
 * Shift-JIS title and 16x16 4-bit Tsingtao Can icon for the PS1 BIOS Manager.
 */
MemCardStatus memcard_save_scores(const HighScoreEntry scores[MC_MAX_SCORES]);

/* Returns 1 if `score` beats the lowest entry in `scores` (and score > 0) */
int memcard_qualifies(const HighScoreEntry scores[MC_MAX_SCORES], uint32_t score);

/* Insert score in descending order with Night/Shift/Won metadata; returns 0..4 rank or -1 */
int memcard_insert_score_ex(HighScoreEntry scores[MC_MAX_SCORES],
                            const char initials[4], uint32_t score,
                            uint16_t night, uint8_t shift, uint8_t won);

/* Legacy v1 wrapper: insert score in descending order; returns 0..4 rank if placed, or -1 */
int memcard_insert_score(HighScoreEntry scores[MC_MAX_SCORES],
                         const char initials[4], uint32_t score, uint16_t wave);

/* Short uppercase status string for HUD toast feedback */
const char *memcard_status_str(MemCardStatus st);

#endif /* TSINGTAO_MEMCARD_H */
