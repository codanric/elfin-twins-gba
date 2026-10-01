/*
 * Elfin Twins GBA native link + RTC hardware test.
 *
 * Put this ROM in BOTH GBAs and connect them with the same GBA Game Link
 * Cable used by ordinary multiplayer games.
 *
 * A = send edge-count 10 (Elfin "hello" result)
 * B = send edge-count 1  (ordinary DATA test)
 * L = send edge-count 4
 * R = send edge-count 7
 * SELECT = reset the native transport session
 * START = set RTC after a power-loss indication
 *
 * The transport itself is native 16-bit GBA multiplayer SIO at 38400 bps;
 * no GPIO/RCNT electrical emulation is involved.
 */
#include <stdio.h>
#include <string.h>
#include <tonc.h>

#include "gba_link.h"
#include "rtc.h"

static gba_link_t link;
static char line[48];
static int rtc_result;
static uint32_t rtc_first_secs;
static uint32_t timer_first;
static volatile uint32_t timer_irqs;
static volatile uint32_t serial_irqs;
static uint32_t app_rx_count;
static uint8_t app_last_count;

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

static void link_timer_isr(void) {
    ++timer_irqs;
    gba_link_service(&link);
}

static void link_serial_isr(void) {
    ++serial_irqs;
    gba_link_on_serial(&link);
}

static void link_reset(void) {
    gba_link_set_enabled(&link, 0);
    gba_link_set_enabled(&link, 1);
}

static void try_send(uint8_t edges) {
    (void)gba_link_send_count(&link, edges);
}

int main(void) {
    REG_WAITCNT = 0x4317;

    irq_init((fnptr)isr_master_nest);
    irq_add(II_VBLANK, NULL);

    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    pal_bg_mem[0] = RGB15(2, 4, 8);

    gba_link_init(&link);
    gba_link_set_enabled(&link, 1);

    /* Parent pacing timer; transfer completion is caught by SERIAL IRQ. */
    REG_TM0CNT_H = 0;
    REG_TM0CNT_L = (u16)(65536 - 50);
    irq_add(II_TIMER0, link_timer_isr);
    irq_set(II_SERIAL, link_serial_isr, ISR_PRIO(0) | ISR_REPLACE);

    /* 50 ticks at 16.78 MHz / 1024 = 3.052 ms. This timer is the
     * multiplayer master clock source; configuring TM0CNT_L alone does not
     * start it. */
    REG_TM0CNT_H = TM_ENABLE | TM_IRQ | TM_FREQ_1024;

    rtc_result = rtc_init();
    rtc_time_t t0;
    if (rtc_present() && rtc_get(&t0) == 0)
        rtc_first_secs = rtc_to_seconds(&t0);
    timer_first = 0;

    for (;;) {
        VBlankIntrWait();
        key_poll();

        if (key_hit(KEY_A)) try_send(10);
        if (key_hit(KEY_B)) try_send(1);
        if (key_hit(KEY_L)) try_send(4);
        if (key_hit(KEY_R)) try_send(7);
        if (key_hit(KEY_SELECT)) link_reset();

        if (key_hit(KEY_START) && rtc_result == RTC_POWER_LOST) {
            rtc_time_t t = {2026, 1, 1, 4, 12, 0, 0};
            rtc_set(&t);
            rtc_result = RTC_OK;
        }

        /* The test app is the consumer. Drain every DATA message so repeated
         * button tests cannot fill the transport queue and manufacture a
         * failure unrelated to the cable. */
        uint8_t received;
        while (gba_link_recv_count(&link, &received)) {
            app_last_count = received;
            ++app_rx_count;
        }

        uint16_t sio = gba_link_status(&link);
        int ready = gba_link_is_ready(&link);

        put(0, "ELFIN TWINS  GBA LINK TEST");
        put(1, "------------------------------");
        put(2, ready ? "CABLE: READY" : "CABLE: NOT READY");
        snprintf(line, sizeof(line), "role:%s  SIOCNT:%04X",
                 gba_link_is_parent(&link) ? "parent" : "child ",
                 sio);
        put(3, line);
        snprintf(line, sizeof(line), "peer:%s  sio_errors:%lu",
                 link.peer_seen ? "yes" : "no ",
                 (unsigned long)link.sio_errors);
        put(4, line);
        snprintf(line, sizeof(line), "last RX:%04X  TX:%04X",
                 link.last_rx_word, link.last_tx_word);
        put(5, line);
        snprintf(line, sizeof(line), "frames RX:%lu TX:%lu",
                 (unsigned long)link.frames_rx,
                 (unsigned long)link.frames_tx);
        put(6, line);
        snprintf(line, sizeof(line), "IRQ S:%lu T:%lu  data:%lu/%u",
                 (unsigned long)serial_irqs,
                 (unsigned long)timer_irqs,
                 (unsigned long)app_rx_count,
                 app_last_count);
        put(7, line);
        put(8, "A:10  B:1  L:4  R:7");
        put(9, "SELECT:reinit  START:set RTC");

        put(11, "------------------------------");
        if (!rtc_present()) {
            put(12, "RTC: NOT FOUND");
            put(13, "enable RTC in flashcart menu");
        } else {
            rtc_time_t t;
            if (rtc_get(&t) == 0) {
                if (!timer_first) timer_first = 1;
                snprintf(line, sizeof(line), "RTC:%04u-%02u-%02u %02u:%02u:%02u",
                         t.year, t.month, t.day, t.hour, t.minute, t.second);
                put(12, line);
                uint32_t elapsed = rtc_first_secs ? rtc_to_seconds(&t) - rtc_first_secs : 0;
                put(13, rtc_result == RTC_POWER_LOST ? "RTC: POWER LOST" : "RTC: OK");
                snprintf(line, sizeof(line), "RTC elapsed:%lu s", (unsigned long)elapsed);
                put(14, line);
            } else {
                put(12, "RTC: INVALID");
                put(13, "");
                put(14, "");
            }
        }

        put(16, "Native 16-bit SIO / 38400 bps");
        put(17, "No GPIO pin mapping is used.");
        put(18, "Connect both GBAs, then use A/B.");
        put(19, "Both units should say READY.");
    }
}
