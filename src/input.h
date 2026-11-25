#ifndef TSINGTAO_INPUT_H
#define TSINGTAO_INPUT_H

#include <stdint.h>

/* PlayStation 1 Port 1 controller button bitmasks (active-HIGH after input_update) */
#define BTN_SELECT   (1u << 0)
#define BTN_L3       (1u << 1)
#define BTN_R3       (1u << 2)
#define BTN_START    (1u << 3)
#define BTN_UP       (1u << 4)
#define BTN_RIGHT    (1u << 5)
#define BTN_DOWN     (1u << 6)
#define BTN_LEFT     (1u << 7)
#define BTN_L2       (1u << 8)
#define BTN_R2       (1u << 9)
#define BTN_L1       (1u << 10)
#define BTN_R1       (1u << 11)
#define BTN_TRIANGLE (1u << 12)
#define BTN_CIRCLE   (1u << 13)
#define BTN_CROSS    (1u << 14)
#define BTN_SQUARE   (1u << 15)

/* Initialize PS1 controller BIOS polling (InitPAD / StartPAD) */
void input_init(void);

/* Sample Port 1 pad once per frame and compute pressed/released edges */
void input_update(void);

/* Non-zero if a valid Digital or Analog/DualShock pad is connected on Port 1 */
int input_connected(void);

/* Bitmask of all currently held buttons */
uint16_t input_raw_held(void);

/* Edge and state queries (accepts single BTN_* or bitwise OR of multiple) */
int input_held(uint16_t btn);
int input_pressed(uint16_t btn);
int input_released(uint16_t btn);

#endif /* TSINGTAO_INPUT_H */
