/*
 * 8 kHz timer interrupt: samples the four link pins, counts edges, detects
 * pulse bursts and plays back an Elfin Twins style burst.
 * Compiled as ARM code into IWRAM (see Makefile IWRAM_SOURCES).
 */
#include "sampler.h"
#include "linkio.h"

volatile sampler_t smp;

/* Elfin Twins transmit routine ($BE0A): drive low, then write the line 11
 * times alternating high/low starting with high, ~1.26 ms apart, then hold.
 * Open-drain: "high" = release. */
#define BURST_WRITES     11
#define HALF_PERIOD_TICKS 10      /* 10 * 122 us = 1.22 ms */
#define BURST_END_TICKS   82      /* ~10 ms of silence ends a burst */

void sampler_start_burst(int pin) {
    if (smp.tx_active)
        return;
    smp.tx_pin = (uint8_t)pin;
    smp.tx_writes_left = BURST_WRITES;
    smp.tx_timer = HALF_PERIOD_TICKS;
    smp.tx_level_high = 1;           /* first write in the loop is "high" */
    linkio_drive_low(pin);           /* initial low (sta $73 #$DF) */
    smp.tx_active = 1;
}

void sampler_isr(void) {
    smp.ticks++;

    /* transmitter */
    if (smp.tx_active) {
        if (--smp.tx_timer == 0) {
            if (smp.tx_writes_left) {
                if (smp.tx_level_high)
                    linkio_release(smp.tx_pin);
                else
                    linkio_drive_low(smp.tx_pin);
                smp.tx_level_high ^= 1;
                smp.tx_writes_left--;
                smp.tx_timer = HALF_PERIOD_TICKS;
            } else {
                linkio_release(smp.tx_pin);
                smp.tx_active = 0;
                smp.bursts_sent++;
            }
        }
    }

    /* receiver: edge counting on all pins, burst framing on SC/SD */
    uint16_t now = linkio_levels();
    uint16_t changed = now ^ smp.last;
    smp.last = now;
    for (int p = 0; p < 4; p++) {
        if (changed & (1 << p)) {
            smp.edges[p]++;
            if (p <= LINK_SD) {
                smp.cur_burst[p]++;
                smp.quiet[p] = 0;
            }
        }
    }
    for (int p = 0; p <= LINK_SD; p++) {
        if (smp.cur_burst[p]) {
            if (++smp.quiet[p] >= BURST_END_TICKS) {
                smp.last_burst[p] = smp.cur_burst[p];
                smp.last_burst_own[p] = smp.cur_burst_own[p];
                smp.burst_count[p]++;
                smp.cur_burst[p] = 0;
                smp.cur_burst_own[p] = 0;
            } else if (smp.tx_active && smp.tx_pin == p) {
                smp.cur_burst_own[p] = 1;
            }
        }
    }
}
