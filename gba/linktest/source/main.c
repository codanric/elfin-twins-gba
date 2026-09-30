/*
 * Elfin Twins GBA port - link cable + RTC hardware test.
 *
 * Run this on BOTH GBAs connected with a GBA link cable. It checks:
 *   1. that the SD (and SC) wires reach the other console,
 *   2. that a released line idles HIGH (pull-up present), which the Elfin
 *      Twins protocol needs,
 *   3. that an Elfin-style pulse burst (1 falling edge + 11 toggles, 12 edges)
 *      arrives intact on the other side,
 *   4. that the flashcart RTC is detected and ticking.
 *
 * Controls:
 *   A       drive SD low / release SD
 *   B       drive SC low / release SC
 *   L       send an Elfin pulse burst on SD
 *   R       send an Elfin pulse burst on SC
 *   SELECT  clear counters
 *   START   (RTC lost power only) set the RTC to 2026-01-01 12:00:00
 */
#include <stdio.h>
#include <string.h>
#include <tonc.h>

#include "linkio.h"
#include "rtc.h"
#include "sampler.h"

static const char *pin_names[4] = {"SC", "SD", "SI", "SO"};
static const char *wday_names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static char line[40];

static void put(int row, const char *s) {
    char buf[32];
    int n = (int)strlen(s);
    if (n > 30) n = 30;
    memcpy(buf, s, n);
    memset(buf + n, ' ', 30 - n);
    buf[30] = 0;
    tte_set_pos(0, row * 8);
    tte_write(buf);
}

static uint16_t idle_levels;
static int rtc_result;
static uint32_t rtc_first_secs;
static uint32_t rtc_first_tick;

int main(void) {
    REG_WAITCNT = 0x4317;   /* standard fast ROM wait states + prefetch */

    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    pal_bg_mem[0] = RGB15(2, 4, 8);

    linkio_init();
    /* Let the lines settle, then record the idle levels (no one driving). */
    for (int i = 0; i < 10; i++)
        VBlankIntrWait();
    idle_levels = linkio_levels();

    memset((void *)&smp, 0, sizeof(smp));
    smp.last = linkio_levels();

    /* 8 kHz sampling timer */
    REG_TM0CNT_H = 0;
    REG_TM0CNT_L = (u16)(65536 - (16777216 / SAMPLE_HZ));
    irq_add(II_TIMER0, sampler_isr);
    REG_TM0CNT_H = TM_ENABLE | TM_IRQ;

    rtc_result = rtc_init();
    rtc_time_t t0;
    if (rtc_present() && rtc_get(&t0) == 0) {
        rtc_first_secs = rtc_to_seconds(&t0);
        rtc_first_tick = smp.ticks;
    }

    put(0, "ELFIN TWINS GBA  LINK+RTC TEST");
    put(1, "------------------------------");

    for (;;) {
        VBlankIntrWait();
        key_poll();

        if (key_hit(KEY_A)) {
            if (linkio_driving(LINK_SD)) linkio_release(LINK_SD);
            else linkio_drive_low(LINK_SD);
        }
        if (key_hit(KEY_B)) {
            if (linkio_driving(LINK_SC)) linkio_release(LINK_SC);
            else linkio_drive_low(LINK_SC);
        }
        if (key_hit(KEY_L)) sampler_start_burst(LINK_SD);
        if (key_hit(KEY_R)) sampler_start_burst(LINK_SC);
        if (key_hit(KEY_SELECT)) {
            u16 ime = REG_IME;
            REG_IME = 0;
            for (int p = 0; p < 4; p++) smp.edges[p] = 0;
            for (int p = 0; p < 2; p++) {
                smp.last_burst[p] = 0;
                smp.burst_count[p] = 0;
            }
            smp.bursts_sent = 0;
            REG_IME = ime;
        }
        if (key_hit(KEY_START) && rtc_result == RTC_POWER_LOST) {
            rtc_time_t t = {2026, 1, 1, 4, 12, 0, 0};
            rtc_set(&t);
            rtc_result = RTC_OK;
        }

        /* --- link section --- */
        put(2, "LINK PORT (GPIO MODE)");
        put(3, " PIN DRIVE LEVEL IDLE  EDGES");
        uint16_t lv = linkio_levels();
        for (int p = 0; p < 4; p++) {
            snprintf(line, sizeof(line), " %s  %-4s  %d     %-4s %5lu",
                     pin_names[p], linkio_driving(p) ? "LOW" : "rel",
                     (lv >> p) & 1, ((idle_levels >> p) & 1) ? "HIGH" : "low",
                     (unsigned long)smp.edges[p]);
            put(4 + p, line);
        }
        for (int p = 0; p < 2; p++) {
            snprintf(line, sizeof(line), " %s burst: %2lu edges %s (#%lu)",
                     pin_names[p], (unsigned long)smp.last_burst[p],
                     smp.last_burst_own[p] ? "own" : "rx ",
                     (unsigned long)smp.burst_count[p]);
            put(8 + p, line);
        }
        int sd_ok = (idle_levels >> LINK_SD) & 1;
        put(10, sd_ok ? " SD idles HIGH: pull-up OK" : " SD idles LOW: no pull-up!");
        snprintf(line, sizeof(line), " expect 12 edges/burst  tx:%lu",
                 (unsigned long)smp.bursts_sent);
        put(11, line);

        /* --- RTC section --- */
        put(12, "------------------------------");
        rtc_time_t t;
        if (!rtc_present()) {
            put(13, "RTC: NOT FOUND");
            put(14, " enable RTC for this ROM in");
            put(15, " the flashcart menu");
            put(16, "");
        } else if (rtc_get(&t) == 0) {
            snprintf(line, sizeof(line), "RTC: %s  status %02X",
                     rtc_result == RTC_POWER_LOST ? "POWER LOST" : "OK", rtc_status());
            put(13, line);
            snprintf(line, sizeof(line), " %04u-%02u-%02u %s %02u:%02u:%02u",
                     t.year, t.month, t.day, wday_names[t.wday % 7],
                     t.hour, t.minute, t.second);
            put(14, line);
            uint32_t rtc_el = rtc_to_seconds(&t) - rtc_first_secs;
            uint32_t tim_el = (smp.ticks - rtc_first_tick) / SAMPLE_HZ;
            snprintf(line, sizeof(line), " elapsed rtc %lus timer %lus",
                     (unsigned long)rtc_el, (unsigned long)tim_el);
            put(15, line);
            put(16, rtc_result == RTC_POWER_LOST ? " START: set 2026-01-01 12:00" : "");
        } else {
            put(13, "RTC: present, invalid data");
            put(14, "");
            put(15, "");
            put(16, "");
        }

        put(17, "------------------------------");
        put(18, "A/B:drive SD/SC L/R:burst");
        put(19, "SELECT: clear counters");
    }
}
