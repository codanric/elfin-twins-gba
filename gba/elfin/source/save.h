/*
 * Battery-backed SRAM save (32 KB, 8-bit bus at 0x0E000000).
 * Two slots are written alternately so a power cut during a save never
 * loses the pet; the newest valid slot wins.
 */
#ifndef SAVE_H
#define SAVE_H

#include <stdint.h>
#include "splb20.h"

enum { AWAY_REAL = 0, AWAY_MAX_DAY = 1, AWAY_PAUSE = 2 };

typedef struct {
    uint8_t sound;       /* 0/1 */
    uint8_t link_mode;   /* LINK_OFF / LINK_ON_SD / LINK_ON_SC */
    uint8_t away_mode;   /* AWAY_* */
    uint8_t clock_sync;  /* set the pet clock from the RTC at start-up */
    uint8_t blur;        /* LCD fade */
    uint8_t reserved[11];
} settings_t;

#define NO_RTC_TIME 0xFFFFFFFFu

/* Returns 1 and fills cpu/settings/rtc_secs if a valid save exists. The
 * cpu's ROM pointer is preserved. */
int save_load(splb20_t *cpu, settings_t *settings, uint32_t *rtc_secs);

/* Write a snapshot (state copied beforehand with interrupts off). */
void save_write(const splb20_t *state, const settings_t *settings, uint32_t rtc_secs);

void save_erase(void);

#endif
