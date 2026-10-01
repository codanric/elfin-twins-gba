/*
 * Emulation driver - see emu.h. Compiled as ARM code into IWRAM.
 */
#include <tonc.h>
#include <string.h>
#include "emu.h"
#include "assets.h"
#include "gba_link.h"

splb20_t cpu;

/* The game ROM runs from EWRAM: faster to read than the cartridge. */
EWRAM_BSS static uint8_t rom_copy[ELFIN_ROM_SIZE] __attribute__((aligned(4)));

volatile uint8_t emu_buttons;
volatile uint8_t emu_sound_on = 1;
volatile uint8_t emu_link_mode = LINK_ON_SD;
volatile uint32_t emu_link_edges_rx;
volatile uint32_t emu_link_edges_tx;
volatile uint32_t emu_link_hook_send;
volatile uint32_t emu_link_hook_recv;
volatile uint32_t emu_link_hook_exchange;
volatile uint32_t emu_link_hook_answer;
volatile uint32_t emu_link_sio_errors;
volatile uint16_t emu_link_sio;
volatile uint16_t emu_link_last_rx;
volatile uint16_t emu_link_last_tx;
volatile uint8_t emu_link_parent;
volatile uint8_t emu_link_bus_ready;
volatile uint8_t emu_link_peer_seen;

static uint8_t applied_buttons;
static gba_link_t cable_link;
volatile uint8_t emu_link_ok;      /* native GBA link is synchronized */
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

/*
 * The original ROM link routines are consumed at their entry points. Their
 * logical messages are then transported by native GBA multiplayer SIO.
 */
#define LINK_ROM_SEND      0xBE0D  /* after wait_melody */
#define LINK_ROM_RECV      0xBE41
#define LINK_ROM_EXCHANGE  0xBDB7  /* after wait_melody */
#define LINK_ROM_ANSWER    0xBA99  /* PA5 has just been driven low */
#define LINK_RAM_TMPCOUNT  0xAE
#define LINK_RAM_EDGES     0xB4
#define LINK_RAM_STATE     0xB5
#define LINK_PA_DIR        0x71
#define LINK_PA_DATA       0x73
#define LINK_INT_CFG       0x79
#define LINK_PA5           0x20

static int emu_link_pc_hook(splb20_t *c, uint16_t pc, void *user) {
    gba_link_t *l = (gba_link_t *)user;

    if (pc == LINK_ROM_SEND) {
        uint8_t n = splb20_read(c, LINK_RAM_TMPCOUNT);
        uint8_t edges = n ? (uint8_t)(n - 1) : 0;

        if (!gba_link_send_count(l, edges))
            return SPLB20_HOOK_WAIT;
        ++emu_link_hook_send;

        /* link_send leaves these values after releasing PA5. */
        splb20_write(c, LINK_RAM_TMPCOUNT, 0);
        splb20_write(c, 0xAF, 0xFF);
        splb20_write(c, LINK_PA_DIR, 0x3F);
        splb20_write(c, LINK_PA_DATA, 0xFF);
        splb20_write(c, LINK_INT_CFG, 0x85);
        splb20_return_from_subroutine(c);
        return SPLB20_HOOK_CONSUME;
    }

    if (pc == LINK_ROM_EXCHANGE) {
        /*
         * BDB4 duplicates link_send inline. We enter here at BDB7, after its
         * wait_melody, and then resume at JSR link_recv at BDEC. It is used by
         * the game-selection path, so bypass only the wire
         * send portion and resume at the real receive call. Keeping the
         * caller's JSR frame intact lets the ROM perform its own post-receive
         * validation and RTS.
         */
        uint8_t n = splb20_read(c, LINK_RAM_TMPCOUNT);
        uint8_t edges = n ? (uint8_t)(n - 1) : 0;
        uint8_t flags_b8;

        if (!gba_link_send_count(l, edges))
            return SPLB20_HOOK_WAIT;
        ++emu_link_hook_exchange;

        splb20_write(c, LINK_RAM_TMPCOUNT, 0);
        flags_b8 = splb20_read(c, 0xB8);
        splb20_write(c, 0xB8, (uint8_t)(flags_b8 & 0xF7));
        splb20_write(c, LINK_PA_DIR, 0x3F);
        splb20_write(c, LINK_PA_DATA, 0xFF);
        splb20_write(c, LINK_INT_CFG, 0x00);
        c->pc = 0xBDEC;
        return SPLB20_HOOK_CONSUME;
    }

    if (pc == LINK_ROM_RECV) {
        uint8_t edges;

        /*
         * Match link_recv's entry setup once, then hold the emulated CPU at
         * the subroutine entry until a complete native packet is available.
         */
        if (!l->recv_armed) {
            splb20_write(c, LINK_PA_DIR, 0x3F);
            splb20_write(c, LINK_PA_DATA, 0xFF);
            splb20_write(c, LINK_INT_CFG, 0x00);
            splb20_write(c, LINK_RAM_EDGES, 0);
            splb20_write(c, 0xAF, 0x00);
            splb20_write(c, 0xB0, 0x00);
            l->recv_armed = 1;
        }

        if (!gba_link_recv_count(l, &edges)) {
            /* A lost native session is the synchronous exchange's failure
             * response: return zero edges so the ROM's own S_BDB4 check
             * takes its established abort path instead of waiting forever. */
            if (!gba_link_is_ready(l))
                edges = 0;
            else
                return SPLB20_HOOK_WAIT;
        }
        ++emu_link_hook_recv;

        splb20_write(c, LINK_RAM_EDGES, edges);
        c->in_low &= (uint8_t)~LINK_PA5;
        c->in_high |= LINK_PA5;
        splb20_write(c, LINK_INT_CFG, 0x85);
        l->recv_armed = 0;
        splb20_return_from_subroutine(c);
        return SPLB20_HOOK_CONSUME;
    }

    if (pc == LINK_ROM_ANSWER) {
        /* BA91 is linear code and reaches BA99 once per responder pulse. The
         * caller's wake path does not invoke link_recv for this pulse, so send
         * a wake-only transport event and then let the ROM continue. */
        if (splb20_read(c, LINK_RAM_STATE) == 4) {
            if (!gba_link_send_wake(l))
                return SPLB20_HOOK_WAIT;
            ++emu_link_hook_answer;
        }
        return SPLB20_HOOK_NONE; /* still execute the rest of BA91 */
    }

    return SPLB20_HOOK_NONE;
}

void emu_link_vsync(void) {
    /* Mirrors Emerald's LinkVSync role: offer one transfer from the hardware
     * master per video frame. Elfin messages are one word, so no inter-word
     * pacing timer is needed. */
    gba_link_set_enabled(&cable_link, emu_link_mode != LINK_OFF);
    gba_link_service(&cable_link);
}

static void emu_link_serial_isr(void) {
    gba_link_on_serial(&cable_link);
}

static inline void emu_link_apply_input(void) {
    /* A native DATA frame corresponds to the original falling PA5 wake edge. */
    if (gba_link_line_low(&cable_link)) {
        cpu.in_low |= LINK_PA5;
        cpu.in_high &= (uint8_t)~LINK_PA5;
    } else {
        cpu.in_low &= (uint8_t)~LINK_PA5;
        cpu.in_high |= LINK_PA5;
    }
}

static inline void emu_link_deliver_wake(void) {
    if (gba_link_wake_pending(&cable_link) &&
        (cpu.int_cfg & 0x88) == 0x88) {
        gba_link_mark_wake_delivered(&cable_link);
        splb20_key_irq(&cpu);
    }
}

/*
 * The BA91 responder pulse is used only to wake a caller whose link_state has
 * bit 7 set. Keep virtual PA5 low after raising the interrupt so the ROM's
 * wake handler can actually read it. Once that handler has converted the
 * caller state to state 1, the pulse has served its purpose and can release.
 */
static inline void emu_link_release_wake_only(void) {
    if (gba_link_wake_is_only(&cable_link) &&
        !gba_link_wake_pending(&cable_link) &&
        !(splb20_read(&cpu, LINK_RAM_STATE) & 0x80))
        gba_link_release_wake(&cable_link);
}


volatile uint32_t emu_isr_count;

void __attribute__((section(".iwram"), long_call)) emu_isr(void) {
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

    emu_link_apply_input();
    if (owed_fp > 0)
        owed_fp -= splb20_run(&cpu, owed_fp);
    emu_link_release_wake_only();
    emu_link_deliver_wake();

    emu_link_edges_rx = cable_link.frames_rx;
    emu_link_edges_tx = cable_link.frames_tx;
    emu_link_ok = gba_link_is_ready(&cable_link);
    emu_link_sio_errors = cable_link.sio_errors;
    emu_link_sio = cable_link.last_sio;
    emu_link_last_rx = cable_link.last_rx_word;
    emu_link_last_tx = cable_link.last_tx_word;
    emu_link_parent = cable_link.parent;
    emu_link_bus_ready = cable_link.ready;
    emu_link_peer_seen = cable_link.peer_seen;

    if (cpu.snd_changed)
        emu_sound_update();
}

void emu_init(void) {
    memcpy32(rom_copy, elfin_rom, ELFIN_ROM_SIZE / 4);

    /*
     * DIAGNOSTIC BUILD ONLY.
     *
     * Keep the original cold-start initialization through $989E, then replace
     * the birth/opening sequence beginning at $98A0 with:
     *
     *   lda #1 ; sta tummy
     *   lda #1 ; sta drinks
     *   jmp home_tick ($98E8)
     *
     * This preserves the ROM's real newborn stat initialization and real home
     * setup while making Link hardware iteration immediate.
     */
    static const uint8_t link_diag_boot[] = {
        0xA9, 0x01, 0x85, 0x93,
        0xA9, 0x01, 0x85, 0x94,
        0x4C, 0xE8, 0x98
    };
    memcpy(rom_copy + (0x98A0 - 0x8000),
           link_diag_boot, sizeof(link_diag_boot));

    splb20_init(&cpu, rom_copy, ELFIN_ROM_SIZE, EMU_CLOCK, 0, 0xFF);
    applied_buttons = 0;

    REG_SNDSTAT = SSTAT_ENABLE;
    REG_SNDDMGCNT = SDMG_BUILD_LR(SDMG_SQR1, 7);
    REG_SNDDSCNT = SDS_DMG100;
    REG_SND1SWEEP = SSW_OFF;
    emu_sound_silence();

    gba_link_init(&cable_link);
    gba_link_set_enabled(&cable_link, emu_link_mode != LINK_OFF);
    splb20_set_pc_hook(&cpu, emu_link_pc_hook, &cable_link);
    emu_link_ok = 0;
    emu_link_hook_send = 0;
    emu_link_hook_recv = 0;
    emu_link_hook_exchange = 0;
    emu_link_hook_answer = 0;
    emu_link_sio_errors = 0;
    emu_link_sio = 0;
    emu_link_last_rx = 0;
    emu_link_last_tx = 0;
    emu_link_parent = 0;
    emu_link_bus_ready = 0;
    emu_link_peer_seen = 0;
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

    /* SERIAL IRQ owns transfer completion; VBlank drives master starts. */
    irq_set(II_SERIAL, emu_link_serial_isr, ISR_PRIO(0) | ISR_REPLACE);

    REG_TM2CNT_H = TM_ENABLE | TM_IRQ;
}

void emu_stop(void) {
    REG_TM2CNT_H = 0;
    REG_TM3CNT_H = 0;
    irq_delete(II_TIMER2);
    irq_delete(II_SERIAL);
    gba_link_set_enabled(&cable_link, 0);
}


