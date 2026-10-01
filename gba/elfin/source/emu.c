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

/* Virtual PA5: the SIO transport delivers the original wire semantics.
 *
 * WAKE makes the PA5 input go low immediately. COUNT then supplies the
 * number of post-wake transitions. A COUNT of zero is the special held-low
 * answer pulse.
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
    rx_timeout = (uint16_t)(3 * 2048); /* ~3 s: longer than any normal send */
}

static inline void link_rx_count(uint8_t edges) {
    if (!rx_waiting_count)
        return;

    rx_waiting_count = 0;
    rx_timeout = 0;
    rx_edges_left = edges;
    /*
     * COUNT=0 is the answer pulse. The receiving MCU already saw its wake
     * edge, so hold low for > the 3.4 ms quiet detector then release.
     */
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
        rx_hold = 3; /* ~1.46 ms between transitions */
        return;
    }

    /*
     * Normal message: the last transition leaves the line high. Answer:
     * COUNT=0 leaves it low until this release point.
     */
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

    /*
     * Feed the reconstructed line into PA5. elfin_link also models the
     * direction-change wake when a peer keeps the line low through release.
     */
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

/*
 * Poll GBA SIO from a dedicated 61.04 us timer. We intentionally do not use
 * the SERIAL IRQ here: the link transport must remain correct even if the
 * surrounding interrupt dispatcher drops a serial interrupt.
 */
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
    irq_delete(II_TIMER2);
    irq_delete(II_SERIAL);
}

void emu_isr(void) __attribute__((long_call));
void gba_link_irq_handler(void) __attribute__((long_call));
