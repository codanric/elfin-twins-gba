/*
 * Thin C API around the SPLB20 core for use from Python via ctypes
 * (see verify_core.py). Built as a shared library on the host.
 */
#include <stdlib.h>
#include <string.h>
#include "../core/splb20.h"
#include "../core/elfin_catchup.h"
#include "../core/elfin_link.h"

typedef struct {
    splb20_t cpu;
    uint8_t rom[0x10000];
    elfin_link_t link;
} harness_t;

harness_t *h_create(const uint8_t *rom, uint32_t size, uint32_t clock_hz,
                    int non_crystal, int pullup_ext) {
    harness_t *h = calloc(1, sizeof(*h));
    memcpy(h->rom, rom, size);
    splb20_init(&h->cpu, h->rom, size, clock_hz, non_crystal, (uint8_t)pullup_ext);
    return h;
}

void h_destroy(harness_t *h) { free(h); }
int32_t h_step(harness_t *h) { return splb20_step(&h->cpu); }
int32_t h_run(harness_t *h, int32_t budget_fp) { return splb20_run(&h->cpu, budget_fp); }

/*
 * Regression hook for test_pc_hook_fastpath.py. It deliberately consumes the
 * trapped instruction without changing PC; a one-instruction-sized run budget
 * is enough to prove that splb20_run() dispatched the hook instead of silently
 * executing through its optimized awake loop.
 */
static int h_test_hook_hits;
static int h_test_pc_hook(splb20_t *c, uint16_t pc, void *user) {
    (void)c;
    (void)pc;
    (void)user;
    ++h_test_hook_hits;
    return SPLB20_HOOK_CONSUME;
}
void h_test_hook_arm(harness_t *h, int pc) {
    h_test_hook_hits = 0;
    h->cpu.pc = (uint16_t)pc;
    splb20_set_pc_hook(&h->cpu, h_test_pc_hook, NULL);
}
int h_test_hook_hits_get(harness_t *h) {
    (void)h;
    return h_test_hook_hits;
}
void h_test_hook_clear(harness_t *h) {
    splb20_set_pc_hook(&h->cpu, NULL, NULL);
}
void h_port(harness_t *h, int mask, int level) { splb20_port(&h->cpu, (uint8_t)mask, level); }
void h_reset(harness_t *h) { splb20_reset(&h->cpu); }
void h_set_warp(harness_t *h, int warp) { h->cpu.timer_warp = warp; }
int h_illegal(harness_t *h) { return h->cpu.illegal ? h->cpu.illegal_pc : -1; }
uint32_t h_instr(harness_t *h) { return h->cpu.instr_counter; }
uint32_t h_sound_freq(harness_t *h) { return splb20_sound_freq(&h->cpu); }
int h_read(harness_t *h, int addr) {
    /* side-effect free peek for RAM areas */
    splb20_t *c = &h->cpu;
    if (addr < 0x40) return c->lcd[addr];
    if (addr >= 0x80 && addr < 0x100) return c->ram[addr - 0x80];
    if (addr >= 0x200 && addr < 0xA00) return c->dram[addr & 0x7FF];
    return 0;
}
void h_poke(harness_t *h, int addr, int value) {
    splb20_t *c = &h->cpu;
    if (addr < 0x40) c->lcd[addr] = (uint8_t)value;
    else if (addr >= 0x80 && addr < 0x100) c->ram[addr - 0x80] = (uint8_t)value;
    else if (addr >= 0x200 && addr < 0xA00) c->dram[addr & 0x7FF] = (uint8_t)value;
}

/* regs: pc, a, x, y, sp, ps, pdir, pcfg, platch, int_cfg, sys_ctrl, ireq,
 *       tc, tc_preset, prescalar, rosc, cpu, portread,
 *       timer_counter, t2hz, t128hz (fp8) */
void h_regs(harness_t *h, int32_t *out) {
    splb20_t *c = &h->cpu;
    out[0] = c->pc; out[1] = c->a; out[2] = c->x; out[3] = c->y; out[4] = c->sp;
    out[5] = (c->nf << 7) | (c->vf << 6) | (c->bf << 4) | (c->df << 3) |
             (c->if_ << 2) | (c->zf << 1) | c->cf;
    out[6] = c->pdir; out[7] = c->pcfg; out[8] = c->platch; out[9] = c->int_cfg;
    out[10] = c->sys_ctrl; out[11] = c->ireq; out[12] = c->tc; out[13] = c->tc_preset;
    out[14] = c->prescalar; out[15] = c->rosc_enbl; out[16] = c->cpu_enbl;
    out[17] = splb20_port_read(c);
    out[18] = c->timer_counter; out[19] = c->t2hz_counter; out[20] = c->t128hz_counter;
    out[21] = c->lcd_cfg; out[22] = c->lcd_bias; out[23] = c->snd_enable;
}

void h_mem(harness_t *h, uint8_t *out) {
    memcpy(out, h->cpu.lcd, 0x40);
    memcpy(out + 0x40, h->cpu.ram, 0x80);
    memcpy(out + 0xC0, h->cpu.dram, 0x800);
}

/* Whole-machine snapshot (used for save states in the workbench). */
int h_state_size(void) { return (int)sizeof(splb20_t); }
void h_state_get(harness_t *h, uint8_t *out) {
    splb20_t tmp = h->cpu;
    tmp.rom = NULL;           /* host pointer: not part of the machine state */
    memcpy(out, &tmp, sizeof(splb20_t));
}
void h_state_set(harness_t *h, const uint8_t *in) {
    const uint8_t *rom = h->cpu.rom;
    memcpy(&h->cpu, in, sizeof(splb20_t));
    h->cpu.rom = rom;
}

/* PC histogram over `cycles` emulated cycles (analysis helper). */
void h_profile(harness_t *h, int32_t cycles, uint32_t *hist) {
    int64_t budget = (int64_t)cycles << SPLB20_FP;
    while (budget > 0) {
        hist[h->cpu.pc]++;
        budget -= splb20_step(&h->cpu);
    }
}

/* Same as splb20_run() but without the sleep fast path (for verification). */
int32_t h_run_noskip(harness_t *h, int32_t budget_fp) {
    int32_t done = 0;
    while (done < budget_fp)
        done += splb20_step(&h->cpu);
    return done;
}

/* catch-up (see core/elfin_catchup.h) */
static elfin_catchup_t g_cu;
void h_catchup_begin(harness_t *h, uint32_t seconds) { (void)h; elfin_catchup_begin(&g_cu, seconds); }
int h_catchup_run(harness_t *h, int32_t budget) { return elfin_catchup_run(&g_cu, &h->cpu, budget); }
void h_catchup_stats(uint32_t *out) {
    out[0] = g_cu.ticks_left; out[1] = g_cu.fast_ticks; out[2] = g_cu.call_ticks;
    out[3] = g_cu.emu_ticks; out[4] = g_cu.wake_ticks; out[5] = g_cu.emulating;
}
int h_is_idle(harness_t *h) { return elfin_is_idle(&h->cpu); }

/* Run while logging whenever PC hits a trap address (analysis helper).
 * Each record: pc, mode ($BD), $CA, $BA. Returns number of records. */
static uint8_t trap_map[0x10000];
void h_trap_set(int addr, int on) { trap_map[addr & 0xFFFF] = (uint8_t)on; }
int h_run_traced(harness_t *h, int32_t cycles, uint16_t *out, int max) {
    int n = 0;
    int64_t budget = (int64_t)cycles << SPLB20_FP;
    while (budget > 0) {
        splb20_t *c = &h->cpu;
        if (trap_map[c->pc] && c->rosc_enbl && c->cpu_enbl && n < max) {
            out[n * 4 + 0] = c->pc;
            out[n * 4 + 1] = c->ram[0xBD - 0x80];
            out[n * 4 + 2] = c->ram[0xCA - 0x80];
            out[n * 4 + 3] = c->ram[0xBA - 0x80];
            n++;
        }
        budget -= splb20_step(c);
    }
    return n;
}

/* Link-cable simulation: pins driven low by this core (PA bit 5 = link). */
int h_drive_low(harness_t *h) { return splb20_drive_low(&h->cpu); }
void h_key_irq(harness_t *h) { splb20_key_irq(&h->cpu); }
int h_int_cfg(harness_t *h) { return h->cpu.int_cfg; }

/* link bridge (core/elfin_link.c), same code as the GBA port */
void h_link_reset(harness_t *h) { elfin_link_reset(&h->link); }
void h_link_before(harness_t *h, int pin_low) { elfin_link_before(&h->link, &h->cpu, pin_low); }
int h_link_after(harness_t *h) { return elfin_link_after(&h->link, &h->cpu); }
