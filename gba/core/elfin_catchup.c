/*
 * Elfin Twins time-away catch-up. See elfin_catchup.h.
 */
#include "elfin_catchup.h"

#define RAM(c, a) ((c)->ram[(a) - 0x80])

#define SENTINEL_PC     0xFFF0      /* return address used for the call */
#define CALL_MAX_INSTR  60000       /* a tick never takes this long */

int elfin_is_idle(const splb20_t *c) {
    return c->pc == ELFIN_SLEEP_PC && !c->rosc_enbl &&
           (c->sys_ctrl & SPLB20_SYS_32K_ENBL);
}

void elfin_catchup_begin(elfin_catchup_t *cu, uint32_t seconds) {
    cu->ticks_left = seconds * 2;
    cu->ticks_total = cu->ticks_left;
    cu->fast_ticks = cu->call_ticks = cu->wake_ticks = cu->emu_ticks = 0;
    cu->emulating = 0;
    cu->tick_mark = 0;
}

/* Game modes ($BD low nibble) in which the pet just lives on and the tick
 * can be fast-forwarded: 0 = awake at home, A = asleep at night. */
static int steady_mode(const splb20_t *c) {
    uint8_t m = RAM(c, ELFIN_MODE) & 0x0F;
    return m == 0x00 || m == 0x0A;
}

/* Run the tick routine $8C6D as a subroutine on the emulated CPU, the way the
 * wake-up path calls it. Returns 1 if it returned normally (the machine is
 * put back to sleep exactly as before), 0 if it left for the main program
 * (it resets the stack for the daily birthday animation and the ending). */
static int call_tick(splb20_t *c) {
    uint16_t pc = c->pc;
    uint8_t a = c->a, x = c->x, y = c->y, sp = c->sp;
    uint8_t nf = c->nf, vf = c->vf, bf = c->bf, df = c->df, if_ = c->if_,
            zf = c->zf, cf = c->cf;
    uint8_t rosc = c->rosc_enbl, cpu = c->cpu_enbl, sys = c->sys_ctrl;
    uint8_t int_cfg = c->int_cfg;

    /* wake the CPU the way the reset/wake path does ($7A = #$2C clears the
     * stop bits), block interrupts, push the sentinel return address */
    c->rosc_enbl = c->cpu_enbl = 1;
    c->sys_ctrl &= ~(SPLB20_SYS_ROSC_STOP | SPLB20_SYS_CPU_STOP);
    c->int_cfg &= ~SPLB20_INT_NMI_ENBL;
    uint16_t ret = SENTINEL_PC - 1;
    splb20_write(c, c->sp, ret >> 8);
    c->sp--;
    splb20_write(c, c->sp, ret & 0xFF);
    c->sp--;
    c->pc = ELFIN_TICK_ROUTINE;

    for (int n = 0; n < CALL_MAX_INSTR; n++) {
        if (c->pc == SENTINEL_PC) {
            c->pc = pc; c->a = a; c->x = x; c->y = y; c->sp = sp;
            c->nf = nf; c->vf = vf; c->bf = bf; c->df = df; c->if_ = if_;
            c->zf = zf; c->cf = cf;
            c->rosc_enbl = rosc; c->cpu_enbl = cpu;
            c->sys_ctrl = (uint8_t)((c->sys_ctrl & ~0xC0) | (sys & 0xC0));
            c->int_cfg = int_cfg;
            return 1;
        }
        if (splb20_read(c, c->pc) == 0x9A) {   /* txs: stack reset */
            splb20_step(c);
            c->int_cfg = int_cfg;
            return 0;
        }
        splb20_step(c);
    }
    c->int_cfg = int_cfg;
    return 0;
}

int elfin_catchup_run(elfin_catchup_t *cu, splb20_t *c, int32_t budget) {
    while (cu->ticks_left && budget > 0) {
        if (cu->emulating) {
            /* normal emulation (sleep is skipped) until steady and asleep */
            int32_t slice = budget < 20000 ? budget : 20000;
            splb20_run(c, slice << SPLB20_FP);
            budget -= slice;
            uint32_t t = c->t2hz_ticks - cu->tick_mark;
            cu->tick_mark = c->t2hz_ticks;
            if (t > cu->ticks_left)
                t = cu->ticks_left;
            cu->ticks_left -= t;
            cu->emu_ticks += t;
            if (t && elfin_is_idle(c) && steady_mode(c))
                cu->emulating = 0;
            continue;
        }
        if (!elfin_is_idle(c) || !steady_mode(c)) {
            cu->emulating = 1;
            cu->tick_mark = c->t2hz_ticks;
            continue;
        }
        uint8_t hs = RAM(c, ELFIN_CLK_HALFSEC);
        uint8_t ls = RAM(c, ELFIN_LIFE_HALFSEC);
        if (hs + 1 < 0x78 && ls + 1 < 0x78) {
            /* the common case: $8C6D would only do inc $82 / inc $8A */
            RAM(c, ELFIN_CLK_HALFSEC) = hs + 1;
            RAM(c, ELFIN_LIFE_HALFSEC) = ls + 1;
            cu->ticks_left--;
            cu->fast_ticks++;
            budget -= 8;
            continue;
        }
        /* A minute boundary. At 10-minute and hour boundaries let the game
         * handle the tick completely, through its normal wake-up path, so the
         * main program can react (bedtime, waking up, ...): make the 2 Hz
         * timer fire now and emulate until it is asleep again. For the other
         * minutes just run the tick routine. */
        int ten_min = ls + 1 >= 0x78 && RAM(c, ELFIN_LIFE_MINUTE) >= 9;
        int hour = hs + 1 >= 0x78 && RAM(c, ELFIN_CLK_MINUTE) >= 59;
        if (ten_min || hour) {
            c->t2hz_counter = 1;
            cu->wake_ticks++;
            cu->emulating = 1;
            cu->tick_mark = c->t2hz_ticks;
            continue;
        }
        cu->ticks_left--;
        cu->call_ticks++;
        budget -= 600;
        if (!call_tick(c)) {
            cu->emulating = 1;
            cu->tick_mark = c->t2hz_ticks;
        }
    }
    return cu->ticks_left != 0;
}

void elfin_set_clock(splb20_t *c, int hour, int minute, int second) {
    RAM(c, ELFIN_CLK_HOUR) = (uint8_t)hour;
    RAM(c, ELFIN_CLK_MINUTE) = (uint8_t)minute;
    RAM(c, ELFIN_CLK_HALFSEC) = (uint8_t)(second * 2);
}
