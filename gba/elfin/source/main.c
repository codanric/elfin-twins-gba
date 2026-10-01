/*
 * Elfin Twins GM-021 for the Game Boy Advance.
 *
 * The original toy's program runs unmodified on an emulated Sunplus SPLB20
 * (BrickEmuPy's core, ported to C). This file is the GBA front end:
 * start-up, "time away" catch-up from the cartridge RTC, controls, saving
 * and the settings menu.
 *
 * Controls
 *   A = ENTER      B = ESC      SELECT / L / R = CLOCK
 *   D-pad = the toy's four arrow buttons
 *   START = settings menu (the pet keeps living while it is open)
 */
#include <stdio.h>
#include <string.h>
#include <tonc.h>

#include "assets.h"
#include "elfin_catchup.h"
#include "emu.h"
#include "lcd.h"
#include "rtc.h"
#include "save.h"

#define VERSION "1.0"

/* ------------------------------------------------------------ colours */
#define C_BOX     RGB15(5, 5, 14)
#define C_BORDER  RGB15(22, 23, 31)
#define C_TEXT    RGB15(31, 31, 31)
#define C_DIM     RGB15(18, 19, 25)
#define C_HI      RGB15(30, 30, 17)

static settings_t settings;
static int rtc_state = RTC_NOT_FOUND;

/* ------------------------------------------------------------ helpers */

static void ui_box(int x0, int y0, int x1, int y1) {
    m3_rect(x0, y0, x1, y1, C_BOX);
    m3_frame(x0, y0, x1, y1, C_BORDER);
}

static void ui_text(int x, int y, u16 color, const char *s) {
    tte_set_ink(color);
    tte_set_pos(x, y);
    tte_write(s);
}

static void ui_center(int y, u16 color, const char *s) {
    POINT16 sz = tte_get_text_size(s);
    ui_text(120 - sz.x / 2, y, color, s);
}

static uint32_t rtc_now(void) {
    rtc_time_t t;
    if (!rtc_present() || rtc_get(&t) != 0)
        return NO_RTC_TIME;
    return rtc_to_seconds(&t);
}

static void apply_settings(void) {
    emu_sound_on = settings.sound;
    emu_link_mode = settings.link_mode;
    lcd_set_motion_blur(settings.blur);
}

static void default_settings(void) {
    memset(&settings, 0, sizeof(settings));
    settings.sound = 1;
    settings.link_mode = LINK_ON_SD;
    settings.away_mode = AWAY_REAL;
    settings.clock_sync = 1;
    settings.blur = 0;           /* LCD fade is opt-in: the ROM draws plain on/off dots */
}

/* Copy the machine state with the emulation interrupt masked, then write. */
EWRAM_BSS static splb20_t snap;
static void save_now(void) {
    u16 ime = REG_IME;
    REG_IME = 0;
    memcpy(&snap, &cpu, sizeof(cpu));
    REG_IME = ime;
    save_write(&snap, &settings, rtc_now());
}

static void sync_clock_from_rtc(void) {
    rtc_time_t t;
    if (!rtc_present() || rtc_get(&t) != 0)
        return;
    u16 ime = REG_IME;
    REG_IME = 0;
    elfin_set_clock(&cpu, t.hour, t.minute, t.second);
    REG_IME = ime;
}

/* ------------------------------------------------------------ catch-up */

static void fmt_duration(char *out, uint32_t secs) {
    uint32_t d = secs / 86400, h = (secs / 3600) % 24, m = (secs / 60) % 60;
    if (d)
        sprintf(out, "%lu day%s %lu h %lu min", (unsigned long)d, d == 1 ? "" : "s",
                (unsigned long)h, (unsigned long)m);
    else if (h)
        sprintf(out, "%lu h %lu min", (unsigned long)h, (unsigned long)m);
    else
        sprintf(out, "%lu min %lu s", (unsigned long)m, (unsigned long)(secs % 60));
}

static void catch_up(uint32_t seconds) {
    static elfin_catchup_t cu;
    char line[48], dur[32];
    elfin_catchup_begin(&cu, seconds);
    fmt_duration(dur, seconds);

    lcd_force_redraw();
    ui_box(20, 44, 219, 115);
    ui_center(50, C_HI, "While you were away...");
    sprintf(line, "%s passed", dur);
    ui_center(66, C_TEXT, line);
    m3_frame(39, 86, 201, 96, C_BORDER);
    ui_center(100, C_DIM, "your Elfin Twins lived on");
    int shown = -1;
    while (elfin_catchup_run(&cu, &cpu, 150000)) {
        uint32_t done = cu.ticks_total - cu.ticks_left;
        int w = cu.ticks_total ? (int)((uint64_t)done * 160 / cu.ticks_total) : 160;
        if (w != shown) {
            m3_rect(40, 87, 40 + w, 95, C_HI);
            shown = w;
        }
    }
    /* keep the message up for a moment so it can be read */
    m3_rect(40, 87, 200, 95, C_HI);
    for (int i = 0; i < 90; i++)
        VBlankIntrWait();
    lcd_force_redraw();
}

/* ------------------------------------------------------------ RTC setup */

static void edit_rtc(void) {
    rtc_time_t t;
    if (rtc_get(&t) != 0) {
        t.year = 2026; t.month = 1; t.day = 1; t.hour = 12; t.minute = 0; t.second = 0;
    }
    int v[5] = {t.year, t.month, t.day, t.hour, t.minute};
    const int lo[5] = {2000, 1, 1, 0, 0}, hi[5] = {2099, 12, 31, 23, 59};
    int field = 0;
    int dirty = 1;
    char line[40];
    for (;;) {
        VBlankIntrWait();
        key_poll();
        if (key_hit(KEY_LEFT)) { field = (field + 4) % 5; dirty = 1; }
        if (key_hit(KEY_RIGHT)) { field = (field + 1) % 5; dirty = 1; }
        if (key_repeat(KEY_UP)) { v[field] = v[field] >= hi[field] ? lo[field] : v[field] + 1; dirty = 1; }
        if (key_repeat(KEY_DOWN)) { v[field] = v[field] <= lo[field] ? hi[field] : v[field] - 1; dirty = 1; }
        if (key_hit(KEY_B)) return;
        if (key_hit(KEY_A)) {
            rtc_time_t n = {(uint16_t)v[0], (uint8_t)v[1], (uint8_t)v[2], 0,
                            (uint8_t)v[3], (uint8_t)v[4], 0};
            rtc_time_t w;
            rtc_from_seconds(rtc_to_seconds(&n), &w);   /* normalise + weekday */
            rtc_set(&w);
            rtc_state = RTC_OK;
            return;
        }
        if (!dirty)
            continue;
        dirty = 0;
        ui_box(30, 50, 209, 110);
        ui_center(55, C_HI, "Set cartridge clock");
        sprintf(line, "%04d-%02d-%02d  %02d:%02d", v[0], v[1], v[2], v[3], v[4]);
        ui_center(73, C_TEXT, line);
        static const int fx[5] = {0, 5, 8, 12, 15};
        static const int fl[5] = {4, 2, 2, 2, 2};
        POINT16 sz = tte_get_text_size(line);
        int x0 = 120 - sz.x / 2;
        int cw = sz.x / 17;
        m3_rect(31, 86, 208, 88, C_BOX);
        m3_rect(x0 + fx[field] * cw, 86, x0 + (fx[field] + fl[field]) * cw, 87, C_HI);
        ui_center(94, C_DIM, "D-pad: change   A: set   B: cancel");
    }
}

/* ------------------------------------------------------------ menu */

enum { M_RESUME, M_SOUND, M_LINK, M_AWAY, M_SYNC, M_BLUR, M_SETRTC, M_SAVE, M_RESET, M_COUNT };

static const char *link_names[] = {"off", "GBA cable", "GBA cable"};
static const char *away_names[] = {"real time", "max 1 day", "paused"};

static int confirm(const char *q) {
    ui_box(30, 60, 209, 100);
    ui_center(66, C_HI, q);
    ui_center(82, C_TEXT, "A: yes     B: no");
    for (;;) {
        VBlankIntrWait();
        key_poll();
        if (key_hit(KEY_A)) return 1;
        if (key_hit(KEY_B)) return 0;
    }
}

static void menu(void) {
    int sel = 0;
    char line[48];
    int dirty = 1;
    int link_diag = 0;
    uint32_t tick = 0;
    save_now();
    for (;;) {
        VBlankIntrWait();
        key_poll();
        tick++;
        if (key_hit(KEY_UP)) { sel = (sel + M_COUNT - 1) % M_COUNT; dirty = 1; }
        if (key_hit(KEY_DOWN)) { sel = (sel + 1) % M_COUNT; dirty = 1; }
        if (key_hit(KEY_SELECT)) { link_diag ^= 1; dirty = 1; }
        int dir = key_hit(KEY_RIGHT) ? 1 : key_hit(KEY_LEFT) ? -1 : 0;
        int act = key_hit(KEY_A);
        if (key_hit(KEY_B) || key_hit(KEY_START))
            break;
        if (act || dir) {
            int d = dir ? dir : 1;
            switch (sel) {
            case M_RESUME: if (act) goto done; break;
            case M_SOUND: settings.sound ^= 1; break;
            case M_LINK: settings.link_mode = (uint8_t)((settings.link_mode + 3 + d) % 3); break;
            case M_AWAY: settings.away_mode = (uint8_t)((settings.away_mode + 3 + d) % 3); break;
            case M_SYNC:
                if (act && rtc_present()) sync_clock_from_rtc();
                else if (dir) settings.clock_sync ^= 1;
                break;
            case M_BLUR: settings.blur ^= 1; break;
            case M_SETRTC: if (act && rtc_present()) edit_rtc(); break;
            case M_SAVE: if (act) save_now(); break;
            case M_RESET:
                if (act && confirm("Start again with a new pet?")) {
                    emu_stop();
                    splb20_reset(&cpu);
                    emu_start();
                    save_now();
                    goto done;
                }
                break;
            }
            apply_settings();
            dirty = 2;       /* full redraw (sub-screens may have drawn over us) */
        }

        if (dirty == 2 || (dirty && tick < 3)) {
            ui_box(14, 8, 225, 151);
            ui_center(12, C_HI, "ELFIN TWINS GM-021  for GBA  v" VERSION);
        }
        if (dirty) {
            const char *labels[M_COUNT];
            char vals[M_COUNT][24];
            labels[M_RESUME] = "Back to the pet"; strcpy(vals[M_RESUME], "");
            labels[M_SOUND] = "Sound"; strcpy(vals[M_SOUND], settings.sound ? "on" : "off");
            labels[M_LINK] = "Link cable"; strcpy(vals[M_LINK], link_names[settings.link_mode % 3]);
            labels[M_AWAY] = "Time while off"; strcpy(vals[M_AWAY], away_names[settings.away_mode % 3]);
            labels[M_SYNC] = "Pet clock = RTC"; strcpy(vals[M_SYNC], settings.clock_sync ? "auto" : "manual");
            labels[M_BLUR] = "LCD fade"; strcpy(vals[M_BLUR], settings.blur ? "on" : "off");
            labels[M_SETRTC] = "Set cartridge clock"; strcpy(vals[M_SETRTC], rtc_present() ? "" : "no RTC");
            labels[M_SAVE] = "Save now"; strcpy(vals[M_SAVE], "");
            labels[M_RESET] = "New pet (reset)"; strcpy(vals[M_RESET], "");
            for (int i = 0; i < M_COUNT; i++) {
                int y = 28 + i * 11;
                m3_rect(18, y - 1, 221, y + 10, i == sel ? RGB15(10, 9, 22) : C_BOX);
                ui_text(24, y, i == sel ? C_HI : C_TEXT, labels[i]);
                ui_text(150, y, C_DIM, vals[i]);
            }
            dirty = 0;
            tick = 30;       /* refresh the status lines now */
        }
        if (tick % 30)
            continue;
        m3_rect(18, 128, 221, 138, C_BOX);
        m3_rect(18, 139, 221, 149, C_BOX);
        if (!link_diag) {
            rtc_time_t t;
            if (rtc_present() && rtc_get(&t) == 0)
                sprintf(line, "RTC %04u-%02u-%02u %02u:%02u:%02u   pet %02u:%02u",
                        t.year, t.month, t.day, t.hour, t.minute, t.second,
                        cpu.ram[ELFIN_CLK_HOUR - 0x80], cpu.ram[ELFIN_CLK_MINUTE - 0x80]);
            else
                sprintf(line, "no cartridge RTC   pet %02u:%02u",
                        cpu.ram[ELFIN_CLK_HOUR - 0x80], cpu.ram[ELFIN_CLK_MINUTE - 0x80]);
            ui_center(128, C_DIM, line);
            sprintf(line, "link %s  rx %lu tx %lu",
                    settings.link_mode == LINK_OFF ? "off" :
                    emu_link_ok ? "ready" : "not connected",
                    (unsigned long)emu_link_edges_rx,
                    (unsigned long)emu_link_edges_tx);
            ui_center(139, C_DIM, line);
        } else {
            sprintf(line, "SIO %04X %c bus:%u peer:%u err:%lu",
                    emu_link_sio, emu_link_parent ? 'P' : 'C',
                    emu_link_bus_ready, emu_link_peer_seen,
                    (unsigned long)emu_link_sio_errors);
            ui_center(128, C_DIM, line);
            sprintf(line, "H %lu/%lu/%lu  W %04X/%04X",
                    (unsigned long)emu_link_hook_send,
                    (unsigned long)emu_link_hook_recv,
                    (unsigned long)emu_link_hook_answer,
                    emu_link_last_rx, emu_link_last_tx);
            ui_center(139, C_DIM, line);
        }
    }
done:
    save_now();
    lcd_force_redraw();
}

/* ------------------------------------------------------------ main */

int main(void) {
    REG_WAITCNT = 0x4317;   /* ROM 3/1 wait states + prefetch */

    irq_init((fnptr)isr_master_nest);
    irq_add(II_VBLANK, NULL);
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;
    tte_init_bmp(3, &verdana9Font, NULL);
    key_repeat_limits(12, 4);

    lcd_init();
    default_settings();
    rtc_state = rtc_init();
    emu_init();

    uint32_t saved_secs = NO_RTC_TIME;
    int have_save = save_load(&cpu, &settings, &saved_secs);
    apply_settings();

    int sync_at_frame = -1;
    if (have_save) {
        uint32_t now = rtc_now();
        if (settings.away_mode != AWAY_PAUSE && now != NO_RTC_TIME &&
            saved_secs != NO_RTC_TIME && now > saved_secs) {
            uint32_t away = now - saved_secs;
            if (settings.away_mode == AWAY_MAX_DAY && away > 86400)
                away = 86400;
            if (away >= 2)
                catch_up(away);
        }
        if (settings.clock_sync)
            sync_clock_from_rtc();
    } else {
        /* A brand new pet: the game sets its clock to 12:00 while booting,
         * so sync with the RTC shortly after. */
        sync_at_frame = 90;
    }
    save_now();

    emu_start();

    uint32_t frame = 0, last_input = 0, last_save = 0;
    int dirty = 0;
    for (;;) {
        VBlankIntrWait();
        frame++;
        key_poll();

        uint8_t b = 0;
        if (key_is_down(KEY_A)) b |= PA_ENTER;
        if (key_is_down(KEY_B)) b |= PA_ESC;
        if (key_is_down(KEY_SELECT | KEY_L | KEY_R)) b |= PA_CLOCK;
        if (key_is_down(KEY_LEFT)) b |= PA_LEFT;
        if (key_is_down(KEY_RIGHT)) b |= PA_RIGHT;
        if (key_is_down(KEY_UP)) b |= PA_UP;
        if (key_is_down(KEY_DOWN)) b |= PA_DOWN;
        emu_buttons = b;
        if (b) {
            last_input = frame;
            dirty = 1;
        }

        lcd_update(cpu.lcd, splb20_lcd_enabled(&cpu));

        if (sync_at_frame >= 0 && frame == (uint32_t)sync_at_frame) {
            if (settings.clock_sync)
                sync_clock_from_rtc();
            sync_at_frame = -1;
        }

        if (key_hit(KEY_START)) {
            emu_buttons = 0;
            menu();
            last_save = frame;
            dirty = 0;
            continue;
        }

        /* save 3 s after the last button press, and every minute anyway */
        if ((dirty && frame - last_input > 180) || frame - last_save > 3600) {
            save_now();
            last_save = frame;
            dirty = 0;
        }
    }
}
