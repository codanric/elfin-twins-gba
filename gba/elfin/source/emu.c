/*
 * Emulation driver - see emu.h. Compiled as ARM code into IWRAM.
 */
#include <tonc.h>
#include "emu.h"
#include "assets.h"
#include "gba_link.h"
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
static gba_link_t cable_link;

/* Virtual PA5: SIO carries an Elfin edge-count message, then this bridge
 * replays the original falling edge and toggles into the emulated MCU. */
static uint8_t rx_wire_low;
static uint8_t rx_active;
static uint8_t rx_edges_left;
static uint8_t rx_hold;

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

/* Virtual PA5: the SIO transport delivers the original wire semantics.
 *
 * WAKE makes the PA5 input go low immediately. COUNT then supplies the
 * number of post-wake transitions. COUNT=0 is the special held-low answer.
 */
static uint8_t rx_wire_low;
static uint8_t rx_active;
static uint8_t rx_waiting_count;
static uint8_t rx_edges_left;
static uint8_t rx_hold;
static uint16_t rx_timeout;

static inline void link_rx_wake(void) {
    rx_active = 1;
    rx_waiting_count = 1;
    rx_wire_low = 1;
    rx_edges_left = 0;
    rx_hold = 0;
    rx_timeout = (uint16_t)(3 * 2048);
}

static inline void link_rx_count(uint8_t edges) {
    if (!rx_waiting_count)
        return;

    rx_waiting_count = 0;
    rx_timeout = 0;
    rx_edges_left = edges;
    /* The answer pulse is already represented by the WAKE event. */
    rx_hold = edges ? 4 : 16;
}

static inline void link_rx_step(void) {
    if (!rx_active)
        return;

    if (rx_waiting_count) {
        if (rx_timeout)
            --rx_timeout;
        else {
            rx_wire_low = 0;
            rx_active = 0;
            rx_waiting_count = 0;
        }
        return;
    }

    if (rx_hold) {
        --rx_hold;
        return;
    }

    if (rx_edges_left) {
        rx_wire_low ^= 1;
        --rx_edges_left;
        rx_hold = 3;
        return;
    }

    rx_wire_low = 0;
    rx_active = 0;
}

static void link_sync_in(void) {
    uint8_t type, value;

    if (gba_link_recv_event(&cable_link, &type, &value)) {
        if (type == ELINK_RX_WAKE)
            link_rx_wake();
        else if (type == ELINK_RX_COUNT)
            link_rx_count(value);
    }

    link_rx_step();
    elfin_link_before(&link, &cpu, rx_wire_low != 0);
}

static void link_sync_out(void) {
    (void)elfin_link_after(&link, &cpu);

    if (link.tx_wake_ready) {
        if (gba_link_send_wake(&cable_link))
            link.tx_wake_ready = 0;
    }

    if (link.tx_ready) {
        if (gba_link_send_edge_count(&cable_link, link.tx_message))
            link.tx_ready = 0;
    }

    emu_link_edges_rx = cable_link.edges_rx;
    emu_link_edges_tx = cable_link.edges_tx;
    emu_link_ok = gba_link_is_ready(&cable_link);
}

volatile uint32_t emu_isr_count;

/* Link supervisor: do not depend on SERIAL IRQ dispatch. */
void gba_link_timer_isr(void) {
    cable_link.enabled = (emu_link_mode != LINK_OFF);
    gba_link_service(&cable_link);
}

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

    gba_link_init(&cable_link);
    cable_link.enabled = (emu_link_mode != LINK_OFF);
    elfin_link_reset(&link);
    rx_wire_low = rx_active = rx_waiting_count = rx_edges_left = rx_hold = 0;
    rx_timeout = 0;
    emu_link_ok = 0;
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

    /* Link supervisor: 16,384 Hz = one tick every 61.035 us. */
    REG_TM0CNT_H = 0;
    REG_TM0CNT_L = 0xFFFF;
    irq_add(II_TIMER0, gba_link_timer_isr);
    REG_TM0CNT_H = TM_ENABLE | TM_FREQ_1024 | TM_IRQ;

    REG_TM2CNT_H = TM_ENABLE | TM_IRQ;
}

void emu_stop(void) {
    REG_TM2CNT_H = 0;
    REG_TM3CNT_H = 0;
    REG_TM0CNT_H = 0;
    irq_delete(II_TIMER2);
    irq_delete(II_TIMER0);
}

void emu_isr(void) __attribute__((long_call));
void gba_link_timer_isr(void) __attribute__((long_call));
