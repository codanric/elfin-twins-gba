/*
 * Emulation driver - see emu.h. Compiled as ARM code into IWRAM.
 */
#include <tonc.h>
#include "emu.h"
#include "assets.h"
#include "linkio.h"
#include "elfin_link.h"

splb20_t cpu;

/* The game ROM runs from EWRAM: faster to read than the cartridge. */
EWRAM_BSS static uint8_t rom_copy[ELFIN_ROM_SIZE] __attribute__((aligned(4)));

volatile uint8_t emu_buttons;
volatile uint8_t emu_sound_on = 1;
volatile uint8_t emu_link_mode = LINK_ON_SD;
volatile uint32_t emu_link_edges_rx;
volatile uint32_t emu_link_edges_tx;

static uint8_t applied_buttons;
static elfin_link_t link;
static uint8_t link_pin_active = LINK_SD;

/* A wire that has never been seen high (no cable pull-up, nothing plugged
 * in on some setups) or that stays low far longer than the protocol ever
 * holds it (~3 s) is ignored until it reads high again. */
#define LINK_STUCK_SLICES (10 * EMU_IRQ_HZ)
static uint8_t link_stuck = 1;
static uint32_t link_low_run;
volatile uint8_t emu_link_ok;      /* wire seen high: cable usable */
/* Pacing: Timer 3 runs freely at 16.78 MHz / 64 = 262144 Hz. Each interrupt
 * runs the emulated cycles for the real time that has passed since the
 * previous one (560000 / 262144 = 4375/2048 cycles = 4375/8 fp8 per tick),
 * so a late or merged interrupt is made up for instead of lost. */
static uint16_t last_t3;
static uint32_t time_frac;
static int32_t owed_fp;
#define OWED_MAX_FP ((EMU_CLOCK / 8) << SPLB20_FP)   /* never catch up more than 1/8 s */
static uint32_t last_period = 0xFFFFFFFF;

static inline int link_pin(void) {
    return emu_link_mode == LINK_ON_SC ? LINK_SC : LINK_SD;
}

void emu_sound_silence(void) {
    REG_SND1CNT = SSQR_ENV_BUILD(0, 0, 0) | SSQR_DUTY1_2;
    REG_SND1FREQ = SFREQ_RESET;
    last_period = 0;
}

void emu_sound_update(void) {
    /* The buzzer plays clock_hz / period Hz. DMG square channel 1 plays
     * 131072 / (2048 - rate) Hz, so rate = 2048 - 131072 * period / clock_hz,
     * rounded to the nearest step (truncating made every note sharp, by a
     * different amount per note: up to +27 cents). */
    uint32_t period = emu_sound_on ? splb20_sound_period(&cpu) : 0;
    cpu.snd_changed = 0;
    if (period == last_period)
        return;
    uint32_t clock = cpu.clock_hz;
    /* audible range 64 Hz .. 65536 Hz; also keeps 131072 * period in 32 bits */
    if (period == 0 || period > clock / 64 || period * 65536u < clock) {
        emu_sound_silence();
        return;
    }
    uint32_t steps = (131072u * period + clock / 2) / clock;   /* 2048 - rate */
    if (steps < 1)
        steps = 1;
    uint32_t rate = 2048 - steps;
    if (last_period == 0) {
        REG_SND1CNT = SSQR_ENV_BUILD(9, 0, 0) | SSQR_DUTY1_2;
        REG_SND1FREQ = SFREQ_RESET | rate;
    } else {
        REG_SND1FREQ = rate;
    }
    last_period = period;
}

static void link_sync_in(void) {
    int pin = link_pin();
    if (pin != link_pin_active) {
        /* the user switched wires: let go of the old one */
        linkio_release(link_pin_active);
        link_pin_active = (uint8_t)pin;
    }
    int raw_low = emu_link_mode != LINK_OFF && !linkio_level(link_pin_active);
    if (!raw_low) {
        link_stuck = 0;
        link_low_run = 0;
        emu_link_ok = 1;
    } else if (!link.out_low && ++link_low_run > LINK_STUCK_SLICES) {
        link_stuck = 1;
    }
    elfin_link_before(&link, &cpu, raw_low && !link_stuck);
}

static void link_sync_out(void) {
    int drive = elfin_link_after(&link, &cpu) && emu_link_mode != LINK_OFF;
    if (drive)
        linkio_drive_low(link_pin_active);
    else
        linkio_release(link_pin_active);
    emu_link_edges_rx = link.edges_rx;
    emu_link_edges_tx = link.edges_tx;
}

volatile uint32_t emu_isr_count;

void emu_isr(void) {
    emu_isr_count++;
    /* buttons: press = pull low (level 0), release = let go (-1) */
    uint8_t want = emu_buttons;
    uint8_t pressed = want & ~applied_buttons;
    uint8_t released = applied_buttons & ~want;
    if (released)
        splb20_port(&cpu, released, -1);
    if (pressed)
        splb20_port(&cpu, pressed, 0);
    applied_buttons = want;

    uint16_t now = REG_TM3CNT_L;
    uint32_t delta = (uint16_t)(now - last_t3);
    last_t3 = now;
    time_frac += delta * 4375;
    owed_fp += (int32_t)(time_frac >> 3);
    time_frac &= 7;
    if (owed_fp > OWED_MAX_FP)
        owed_fp = OWED_MAX_FP;

    link_sync_in();
    if (owed_fp > 0)
        owed_fp -= splb20_run(&cpu, owed_fp);
    link_sync_out();

    if (cpu.snd_changed)
        emu_sound_update();
}

void emu_init(void) {
    memcpy32(rom_copy, elfin_rom, ELFIN_ROM_SIZE / 4);
    splb20_init(&cpu, rom_copy, ELFIN_ROM_SIZE, EMU_CLOCK, 0, 0xFF);
    applied_buttons = 0;

    REG_SNDSTAT = SSTAT_ENABLE;
    REG_SNDDMGCNT = SDMG_BUILD_LR(SDMG_SQR1, 7);
    REG_SNDDSCNT = SDS_DMG100;
    REG_SND1SWEEP = SSW_OFF;
    emu_sound_silence();

    linkio_init();
    elfin_link_reset(&link);
    link_stuck = 1;
    link_low_run = 0;
    emu_link_ok = 0;
    link_pin_active = (uint8_t)link_pin();
}

void emu_start(void) {
    applied_buttons = 0;
    REG_TM3CNT_H = 0;
    REG_TM3CNT_L = 0;
    REG_TM3CNT_H = TM_ENABLE | TM_FREQ_64;
    last_t3 = REG_TM3CNT_L;
    time_frac = 0;
    owed_fp = 0;
    REG_TM2CNT_H = 0;
    REG_TM2CNT_L = (u16)(65536 - (16777216 / EMU_IRQ_HZ));
    irq_add(II_TIMER2, emu_isr);
    REG_TM2CNT_H = TM_ENABLE | TM_IRQ;
}

void emu_stop(void) {
    REG_TM2CNT_H = 0;
    REG_TM3CNT_H = 0;
    irq_delete(II_TIMER2);
}
