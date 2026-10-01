/*
 * Emulation driver - see emu.h. Compiled as ARM code into IWRAM.
 */
#include <tonc.h>
#include "emu.h"
#include "gba_link.h"
#include "assets.h"
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

/* Virtual PA5 wire. Incoming edge-count frames are replayed as the original
 * falling edge plus ~1.26 ms-spaced transitions. */
static uint8_t rx_wire_low;
static uint8_t rx_active;
static uint8_t rx_edges_left;
static uint8_t rx_hold;

volatile uint8_t emu_link_ok;      /* GBA SIO cable reports ready */
/* Pacing: Timer 3 runs freely at 16.78 MHz / 64 = 262144 Hz. Each interrupt
 * runs the emulated cycles for the real time that has passed since the
 * previous one (560000 / 262144 = 4375/2048 cycles = 4375/8 fp8 per tick),
 * so a late or merged interrupt is made up for instead of lost. */
static uint16_t last_t3;
static uint32_t time_frac;
static int32_t owed_fp;
#define OWED_MAX_FP ((EMU_CLOCK / 8) << SPLB20_FP)   /* never catch up more than 1/8 s */
static uint32_t last_period = 0xFFFFFFFF;

static inline void link_rx_start(uint8_t edges) {
    if (rx_active)
        return;
    rx_active = 1;
    rx_wire_low = 1;
    rx_edges_left = edges;
    rx_hold = 4; /* ~2 ms initial low */
}

static inline void link_rx_step(void) {
    if (!rx_active)
        return;
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
     * Keep the final level quiet long enough for link_recv's 10 x ~0.3 ms
     * detector, then release the virtual wire to idle high.
     */
    if (rx_wire_low)
        rx_wire_low = 0;
    else
        rx_active = 0;
    rx_hold = 8;
}

static void link_sync_in(void) {
    uint8_t edges;
    if (gba_link_recv_edge_count(&cable_link, &edges))
        link_rx_start(edges);
    link_rx_step();

    /* elfin_link supplies the original PA5 input semantics and latches a
     * falling edge if the emulated MCU is itself holding PA5 low. */
    elfin_link_before(&link, &cpu, rx_wire_low != 0);
}

static void link_sync_out(void) {
    (void)elfin_link_after(&link, &cpu);

    if (link.tx_ready) {
        if (gba_link_send_edge_count(&cable_link, link.tx_message))
            link.tx_ready = 0;
    }

    emu_link_edges_rx = cable_link.edges_rx;
    emu_link_edges_tx = cable_link.edges_tx;
    emu_link_ok = gba_link_is_ready(&cable_link);
}

