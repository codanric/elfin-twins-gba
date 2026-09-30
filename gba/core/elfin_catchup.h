/*
 * Elfin Twins "time away" catch-up.
 *
 * When the GBA was switched off for N seconds, the pet should have lived
 * through those N seconds. Emulating them in full would take far too long, so
 * we exploit how the game keeps time:
 *
 *  - Everything time-related happens in the 2 Hz tick routine at $8C6D,
 *    which the game calls once per half second after waking from sleep.
 *  - On 118 out of 120 ticks that routine only increments two half-second
 *    counters ($82 for the clock, $8A for the pet's life timer). We do that
 *    directly in C.
 *  - At the other minute boundaries the real tick routine runs on the
 *    emulated CPU as a subroutine call (a few hundred instructions).
 *  - Every 10 minutes of pet life and every hour on the clock the game
 *    handles the tick completely: we make the 2 Hz timer fire and emulate
 *    normally until it is back asleep. This lets the main program react
 *    (going to bed at night, waking up, ...).
 *  - Fast-forwarding only happens in the two "steady" modes (awake at home,
 *    asleep at night). Anything else - the daily birthday animation, the
 *    ending, menus - is emulated normally (sleep is skipped, so that is
 *    still quick) until the game is steady again.
 *
 * The result is the same pet state as having left the toy running, minus the
 * cosmetic per-half-second animation work.
 */
#ifndef ELFIN_CATCHUP_H
#define ELFIN_CATCHUP_H

#include "splb20.h"

/* Game constants (Elfin Twins GM-021 ROM) */
#define ELFIN_SLEEP_PC      0x8E83   /* main loop sleep point (after sta $7A) */
#define ELFIN_TICK_ROUTINE  0x8C6D   /* 2 Hz tick: clock + life timers */
#define ELFIN_CLK_HALFSEC   0x82     /* 0..119 */
#define ELFIN_CLK_HOUR      0x83     /* 0..23 */
#define ELFIN_CLK_MINUTE    0x84     /* 0..59 */
#define ELFIN_LIFE_HALFSEC  0x8A     /* 0..119 */
#define ELFIN_LIFE_MINUTE   0x89     /* 0..9 */
#define ELFIN_MODE          0xBD     /* low nibble = game mode */

typedef struct {
    uint32_t ticks_left;     /* half-seconds still to apply */
    uint32_t ticks_total;
    uint32_t fast_ticks;     /* applied by the C fast path */
    uint32_t call_ticks;     /* minute boundaries: tick routine called */
    uint32_t wake_ticks;     /* 10-minute / hour boundaries: full wake-up */
    uint32_t emu_ticks;      /* applied by normal emulation */
    uint32_t tick_mark;      /* t2hz_ticks at the start of normal emulation */
    uint8_t emulating;       /* 1 = running normally until asleep again */
} elfin_catchup_t;

#ifdef __cplusplus
extern "C" {
#endif

void elfin_catchup_begin(elfin_catchup_t *cu, uint32_t seconds);

/* Do some catch-up work, at most about `budget` emulated CPU cycles.
 * Returns 1 while there is more to do, 0 when finished. */
int elfin_catchup_run(elfin_catchup_t *cu, splb20_t *c, int32_t budget);

/* 1 when the game is asleep at its main sleep point. */
int elfin_is_idle(const splb20_t *c);

/* Set the in-game clock (hour 0-23, minute 0-59, second 0-59). */
void elfin_set_clock(splb20_t *c, int hour, int minute, int second);

#ifdef __cplusplus
}
#endif

#endif
