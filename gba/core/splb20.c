/*
 * SPLB20 core - see splb20.h. Port of cores/SPLB20.py (BrickEmuPy).
 */
#include "splb20.h"
#include <string.h>

#if defined(__GBA__)
#define SPLB20_FAST __attribute__((section(".iwram"), long_call, target("arm")))
#else
#define SPLB20_FAST
#endif

#define LCDRAM_OFFSET   0x00
#define SFR_OFFSET      0x40
#define SFR_SIZE        0x40
#define CPU_RAM_OFFSET  0x80
#define DATARAM_OFFSET  0x200

#define VADDR_NMI       0xFFFA
#define VADDR_RESET     0xFFFC
#define VADDR_IRQ       0xFFFE

#define MCLOCK_DIV_FP   (1 << SPLB20_FP)

static inline __attribute__((always_inline)) uint8_t rom_byte(const splb20_t *c, uint32_t addr) {
    return c->rom[addr & c->rom_mask];
}

static inline uint16_t rom_word(const splb20_t *c, uint32_t addr) {
    return (uint16_t)(rom_byte(c, addr) | (rom_byte(c, addr + 1) << 8));
}

/* ------------------------------------------------------------------ sound */

static void sound_tone(splb20_t *c) {
    c->snd_changed = 1;
}

static void sound_set_enable(splb20_t *c, int en) {
    c->snd_enable = (uint8_t)(en != 0);
    sound_tone(c);
}

uint32_t splb20_sound_freq(const splb20_t *c) {
    if (c->snd_tc_div > 1 && c->snd_enable)
        return c->clock_hz / c->snd_clock_div / c->snd_tc_div;
    return 0;
}

uint32_t splb20_sound_period(const splb20_t *c) {
    if (c->snd_tc_div <= 1 || !c->snd_enable)
        return 0;
    if (c->snd_clock_div > 0xFFFFu)
        return 0xFFFFFFFFu;               /* far below audible */
    return c->snd_clock_div * c->snd_tc_div;
}

/* -------------------------------------------------------------- port A */

uint8_t splb20_port_read(const splb20_t *c) {
    return (uint8_t)((~c->pdir & c->platch) |
                     (c->pdir & (~c->in_low & (c->in_high | c->pullup | c->pullup_ext))));
}

static void push(splb20_t *c, uint8_t v);

static void go_vector(splb20_t *c, uint16_t addr) {
    c->pc = rom_word(c, addr);
}

static uint8_t get_ps(const splb20_t *c) {
    return (uint8_t)((c->nf << 7) | (c->vf << 6) | (c->bf << 4) | (c->df << 3) |
                     (c->if_ << 2) | (c->zf << 1) | c->cf);
}

static void set_ps(splb20_t *c, uint8_t ps) {
    c->nf = ps >> 7;
    c->vf = (ps & 0x40) != 0;
    c->bf = (ps & 0x10) != 0;
    c->df = (ps & 0x08) != 0;
    c->if_ = (ps & 0x04) != 0;
    c->zf = (ps & 0x02) != 0;
    c->cf = ps & 1;
}

static void nmi(splb20_t *c) {
    if (c->int_cfg & SPLB20_INT_NMI_ENBL) {
        if (c->rosc_enbl && c->cpu_enbl) {
            push(c, c->pc >> 8);
            push(c, c->pc & 0xFF);
            push(c, get_ps(c));
            go_vector(c, VADDR_NMI);
            c->sys_ctrl &= ~0x3;
        } else {
            if (c->sys_ctrl & SPLB20_SYS_32K_ENBL)
                c->sys_ctrl |= 0x1;
            else
                c->sys_ctrl |= 0x3;
            c->cpu_enbl = c->rosc_enbl = 1;
            c->int_cfg &= ~SPLB20_INT_NMI_ENBL;
            go_vector(c, VADDR_RESET);
        }
    }
}

static void irq(splb20_t *c) {
    push(c, c->pc >> 8);
    push(c, c->pc & 0xFF);
    push(c, get_ps(c));
    c->if_ = 1;
    go_vector(c, VADDR_IRQ);
}

void splb20_port(splb20_t *c, uint8_t mask, int level) {
    uint8_t prev = splb20_port_read(c);
    c->in_low &= ~mask;
    c->in_high &= ~mask;
    if (level == 0)
        c->in_low |= mask;
    else if (level > 0)
        c->in_high |= mask;
    if (prev != splb20_port_read(c) && level == 0) {
        if (mask & 0xFE) {
            if (c->int_cfg & SPLB20_INT_NORMALKEY) {
                c->ireq |= SPLB20_INT_NORMALKEY;
                nmi(c);
            }
        }
        if (mask & 0x01) {
            if (c->int_cfg & SPLB20_INT_POWERKEY) {
                c->ireq |= SPLB20_INT_POWERKEY;
                nmi(c);
            }
        }
    }
}

static void timers_clock(splb20_t *c, int32_t exec);

/* The fast path in splb20_run() applies elapsed cycles to the timers in
 * batches. Writes that change how the timers count ($7A timer enable / stop
 * bits, $7C prescaler) flush the batch first, so they see the same timer
 * state as with per-instruction updates, and end the batch. (No timer event
 * can be pending in a batch: the fast loop stops as soon as one is due, and
 * events are then processed with the registers as they are at that moment,
 * exactly like the per-instruction path.) */
static inline void flush_pending(splb20_t *c) {
    if (c->pending_fp) {
        int32_t p = c->pending_fp;
        c->pending_fp = 0;
        timers_clock(c, p);
    }
    c->io_written = 1;
}

/* Raise the "normal key" interrupt as if a port A pin had fallen. Used by
 * the link bridge to deliver an edge the pin itself could not see (see
 * emu.c / docs: the ROM's link handshake relies on a latched edge). */
void splb20_key_irq(splb20_t *c) {
    if (c->int_cfg & SPLB20_INT_NORMALKEY) {
        c->ireq |= SPLB20_INT_NORMALKEY;
        nmi(c);
    }
}

/* ---------------------------------------------------------------- SFRs */

static uint8_t io_read(splb20_t *c, uint16_t addr) {
    switch (addr) {
    case 0x70: return c->lcd_cfg;
    case 0x71: return c->pdir;
    case 0x72: return c->pcfg;
    case 0x73: return splb20_port_read(c);
    case 0x74: return 0;
    case 0x75: return 0;
    case 0x76: return c->lcd_bias;
    case 0x77: return 0;
    case 0x78: return 0;
    case 0x79: {
        uint8_t buf = c->ireq | (c->int_cfg & 0x80);
        c->ireq = 0;
        return buf;
    }
    case 0x7A: return c->sys_ctrl;
    case 0x7B: return c->tc_preset;
    case 0x7C: return c->prescalar;
    case 0x7E: return c->keyscan_ctrl;
    case 0x7F: return 0;
    default:   return 0;
    }
}

static void io_write(splb20_t *c, uint16_t addr, uint8_t value) {
    if (addr == 0x7A || addr == 0x7C)
        flush_pending(c);
    switch (addr) {
    case 0x70: c->lcd_cfg = value; break;
    case 0x71: c->pdir = value; break;
    case 0x72:
        c->pcfg = value;
        sound_set_enable(c, (value & 0xC0) == 0xC0);
        break;
    case 0x73: c->platch = value; break;
    case 0x74: break;
    case 0x75: break;
    case 0x76: c->lcd_bias = value; break;
    case 0x77: break;
    case 0x78: break;
    case 0x79: c->int_cfg = value; break;
    case 0x7A: {
        uint8_t inv = (uint8_t)~value;
        c->rosc_enbl = ((c->sys_ctrl | inv) & SPLB20_SYS_ROSC_STOP) != 0;
        c->cpu_enbl = ((c->sys_ctrl | inv) & SPLB20_SYS_CPU_STOP) != 0;
        c->sys_ctrl = value;
        if ((!c->rosc_enbl && c->non_crystal) || !(value & SPLB20_SYS_TIMER_ENBL))
            sound_set_enable(c, 0);
        else
            sound_set_enable(c, (c->pcfg & 0xC0) == 0xC0);
        break;
    }
    case 0x7B:
        c->tc_preset = value;
        c->snd_tc_div = (uint32_t)value + 1;
        sound_tone(c);
        break;
    case 0x7C:
        c->prescalar = value;
        c->snd_clock_div = 1u << (value & 31);
        sound_tone(c);
        break;
    case 0x7E: c->keyscan_ctrl = value; break;
    case 0x7F: break; /* watchdog clear */
    default: break;
    }
}

/* -------------------------------------------------------------- memory */

void splb20_write(splb20_t *c, uint16_t addr, uint8_t value) {
    if (addr < LCDRAM_OFFSET + SPLB20_LCDRAM_SIZE) {
        c->lcd[addr - LCDRAM_OFFSET] = value;
    } else if (addr >= CPU_RAM_OFFSET && addr < CPU_RAM_OFFSET + SPLB20_CPURAM_SIZE) {
        c->ram[addr - CPU_RAM_OFFSET] = value;
    } else if (addr >= DATARAM_OFFSET && (addr & 0x8FFF) < SPLB20_DATARAM_SIZE) {
        c->dram[addr & 0x7FF] = value;
    } else {
        io_write(c, addr, value);
    }
}

uint8_t splb20_read(splb20_t *c, uint16_t addr) {
    if (addr < LCDRAM_OFFSET + SPLB20_LCDRAM_SIZE)
        return c->lcd[addr - LCDRAM_OFFSET];
    if (addr >= CPU_RAM_OFFSET && addr < CPU_RAM_OFFSET + SPLB20_CPURAM_SIZE)
        return c->ram[addr - CPU_RAM_OFFSET];
    if (addr >= DATARAM_OFFSET && (addr & 0x8FFF) < SPLB20_DATARAM_SIZE)
        return c->dram[addr & 0x7FF];
    if (addr >= SFR_OFFSET && addr < SFR_OFFSET + SFR_SIZE)
        return io_read(c, addr);
    if (addr >= c->rom_offset)
        return rom_byte(c, addr - c->rom_offset);
    return 0;
}

/* Zero page accesses (stack and zp operands) go through the same decoder as
 * in the Python core: $00-$3F LCD RAM, $40-$7F SFRs, $80-$FF CPU RAM. */
static inline __attribute__((always_inline)) uint8_t zp_read(splb20_t *c, uint8_t addr) {
    if (addr >= CPU_RAM_OFFSET)
        return c->ram[addr - CPU_RAM_OFFSET];
    if (addr < SFR_OFFSET)
        return c->lcd[addr];
    return io_read(c, addr);
}

static inline __attribute__((always_inline)) void zp_write(splb20_t *c, uint8_t addr, uint8_t value) {
    if (addr >= CPU_RAM_OFFSET)
        c->ram[addr - CPU_RAM_OFFSET] = value;
    else if (addr < SFR_OFFSET)
        c->lcd[addr] = value;
    else
        io_write(c, addr, value);
}

static void push(splb20_t *c, uint8_t v) {
    zp_write(c, c->sp, v);
    c->sp = (uint8_t)(c->sp - 1);
}

static inline uint8_t pull(splb20_t *c) {
    c->sp = (uint8_t)(c->sp + 1);
    return zp_read(c, c->sp);
}

/* ------------------------------------------------------ loop skipping */

/* Addresses where one of two side-effect-free idle loops starts:
 *   delay:  85 7F  CA  D0 FB      sta $7F / dex / bne *-3   (8 cycles/iter)
 *   wait:   A5 aa  C5 bb  F0 FA   lda aa / cmp bb / beq *-4 (9 cycles/iter)
 * (the wait form only when aa and bb are RAM, i.e. only an interrupt can
 * end it). The run loop jumps over whole iterations of these loops up to
 * the next timer event, which gives exactly the same result as executing
 * them. One bit per address. */
#if defined(__GBA__)
__attribute__((section(".sbss")))
#endif
static uint8_t loop_hint[0x10000 / 8];

static void scan_loops(splb20_t *c) {
    memset(loop_hint, 0, sizeof(loop_hint));
    for (uint32_t a = c->rom_offset; a + 6 <= 0x10000; a++) {
        const uint8_t *p = &c->rom[(a - c->rom_offset) & c->rom_mask];
        int hit = 0;
        if (p[0] == 0x85 && p[1] == 0x7F && p[2] == 0xCA && p[3] == 0xD0 && p[4] == 0xFB)
            hit = 1;
        if (p[0] == 0xA5 && p[2] == 0xC5 && p[4] == 0xF0 && p[5] == 0xFA &&
            (p[1] >= 0x80 || p[1] < 0x40) && (p[3] >= 0x80 || p[3] < 0x40))
            hit = 1;
        if (hit)
            loop_hint[a >> 3] |= (uint8_t)(1 << (a & 7));
    }
}

/* Try to skip iterations of the idle loop starting at c->pc, spending at
 * most `room` fp8 cycles. Returns the fp8 cycles skipped (0 = none). */
static int32_t skip_loop(splb20_t *c, int32_t room) {
    uint8_t op = rom_byte(c, c->pc);
    if (op == 0x85) {
        /* delay loop: keep the final iteration for the normal path */
        int x = c->x ? c->x : 256;
        int32_t k = room / (8 << SPLB20_FP);
        if (k > x - 1)
            k = x - 1;
        if (k <= 0)
            return 0;
        c->x = (uint8_t)(c->x - k);
        c->nf = c->x >> 7;
        c->zf = c->x == 0;
        c->instr_counter += 3 * k;
        return k * (8 << SPLB20_FP);
    }
    /* wait loop: only while it would keep looping */
    uint8_t aa = rom_byte(c, c->pc + 1), bb = rom_byte(c, c->pc + 3);
    uint8_t va = zp_read(c, aa);
    if (va != zp_read(c, bb))
        return 0;
    int32_t k = room / (9 << SPLB20_FP);
    if (k <= 0)
        return 0;
    c->a = va;
    c->nf = 0;
    c->zf = 1;
    c->cf = 1;
    c->instr_counter += 3 * k;
    return k * (9 << SPLB20_FP);
}

/* ---------------------------------------------------------------- init */

void splb20_init(splb20_t *c, const uint8_t *rom, uint32_t rom_size,
                 uint32_t clock_hz, int non_crystal, uint8_t pullup_ext) {
    memset(c, 0, sizeof(*c));
    c->rom = rom;
    c->rom_mask = rom_size - 1;       /* ROM size must be a power of two */
    c->rom_offset = 0x10000 - rom_size;
    c->clock_hz = clock_hz;
    c->non_crystal = (uint8_t)non_crystal;
    c->pullup_ext = pullup_ext;
    /* clock / 32768 in fp8 (exact for 560000 Hz: 17 + 23/256) */
    c->sub_clock_div_fp = (int32_t)(((uint64_t)clock_hz << SPLB20_FP) / SPLB20_SUB_CLOCK);
    c->snd_tc_div = 1;
    c->snd_clock_div = 1;
    c->timer_warp = 1;
    c->pc_hook = 0;
    c->pc_hook_user = 0;
    scan_loops(c);
    splb20_reset(c);
}

void splb20_set_pc_hook(splb20_t *c, splb20_pc_hook_t hook, void *user) {
    c->pc_hook = hook;
    c->pc_hook_user = user;
}

void splb20_return_from_subroutine(splb20_t *c) {
    uint16_t ret = pull(c);
    ret |= (uint16_t)pull(c) << 8;
    c->pc = (uint16_t)(ret + 1);
}

void splb20_reset(splb20_t *c) {
    c->timer_counter = 0;
    c->t2hz_counter = 0;
    c->t128hz_counter = 0;
    c->pc = 0;
    c->sp = 0;
    c->a = c->x = c->y = 0;
    set_ps(c, 0x04);
    c->rosc_enbl = 1;
    c->cpu_enbl = 1;
    memset(c->ram, 0, sizeof(c->ram));
    memset(c->dram, 0, sizeof(c->dram));
    memset(c->lcd, 0, sizeof(c->lcd));
    c->pcfg = 0;
    c->pdir = 0;
    c->pullup = 0;
    c->platch = 0;
    c->lcd_cfg = 0;
    c->lcd_bias = 0;
    c->sys_ctrl = 0;
    c->int_cfg = 0;
    c->ireq = 0;
    c->tc = 0;
    c->tc_preset = 0;
    c->prescalar = 0;
    c->keyscan_ctrl = 0;
    sound_set_enable(c, 0);
    go_vector(c, VADDR_RESET);
}

/* -------------------------------------------------------------- timers */

static void SPLB20_FAST timers_clock(splb20_t *c, int32_t exec) {
    if (c->sys_ctrl & SPLB20_SYS_TIMER_ENBL) {
        c->timer_counter -= exec;
        while (c->timer_counter <= 0) {
            c->timer_counter += (1 << c->prescalar) << SPLB20_FP;
            c->tc -= 1;
            if (c->tc < 0) {
                c->tc = c->tc_preset;
                if (c->int_cfg & SPLB20_INT_COUNTER) {
                    c->ireq |= SPLB20_INT_COUNTER;
                    nmi(c);
                }
            }
        }
    }

    int32_t warped = exec * c->timer_warp;

    c->t2hz_counter -= warped;
    while (c->t2hz_counter <= 0) {
        if (c->non_crystal)
            c->t2hz_counter += ((1 << c->prescalar) * (SPLB20_SUB_CLOCK / 2)) << SPLB20_FP;
        else
            c->t2hz_counter += c->sub_clock_div_fp * (SPLB20_SUB_CLOCK / 2);
        c->t2hz_ticks++;
        if (c->int_cfg & SPLB20_INT_T2HZ) {
            c->ireq |= SPLB20_INT_T2HZ;
            nmi(c);
        }
    }

    c->t128hz_counter -= warped;
    while (c->t128hz_counter <= 0) {
        if (c->non_crystal)
            c->t128hz_counter += ((1 << c->prescalar) * (SPLB20_SUB_CLOCK / 128)) << SPLB20_FP;
        else
            c->t128hz_counter += c->sub_clock_div_fp * (SPLB20_SUB_CLOCK / 128);
        if (c->int_cfg & SPLB20_INT_T128HZ) {
            c->ireq |= SPLB20_INT_T128HZ;
            nmi(c);
        }
    }
}

/* ------------------------------------------------------------- execute */

#define SET_NZ(v)   do { uint8_t _v = (v); c->nf = _v >> 7; c->zf = (_v == 0); } while (0)
#define IMM()       (rom_byte(c, c->pc))
#define NEXT(n)     (c->pc = (uint16_t)(c->pc + (n)))

#define BRANCH(cond)                                                       \
    do {                                                                   \
        if (cond) {                                                        \
            uint8_t _o = IMM();                                            \
            NEXT(1);                                                       \
            uint16_t _prev = c->pc;                                        \
            c->pc = (uint16_t)(c->pc + _o - ((_o & 0x80) << 1));           \
            return 3 + ((c->pc ^ _prev) > 255);                            \
        }                                                                  \
        NEXT(1);                                                           \
        return 2;                                                          \
    } while (0)

static inline void op_adc(splb20_t *c, uint8_t operand) {
    int a = c->a;
    int nv = a + operand + c->cf;
    if (c->df && ((a & 0x0F) + (operand & 0x0F) + c->cf > 9))
        nv += 6;
    c->vf = ((~(a ^ operand) & (a ^ nv)) >> 7) & 1;
    c->nf = (nv >> 7) & 1;
    if (c->df && nv > 0x99)
        nv += 0x60;
    c->zf = (nv & 0xFF) == 0;
    c->cf = nv > 255;
    c->a = (uint8_t)nv;
}

static inline void op_sbc(splb20_t *c, uint8_t operand) {
    int a = c->a;
    int nc = !c->cf;
    int nv = a - operand - nc;
    if (c->df) {
        if ((a & 0x0F) - (operand & 0x0F) - nc < 0)
            nv -= 6;
        if (nv < 0)
            nv -= 0x60;
    }
    c->vf = (((a ^ operand) & (a ^ nv)) >> 7) & 1;
    c->nf = (nv >> 7) & 1;
    c->zf = (nv & 0xFF) == 0;
    c->cf = nv >= 0;
    c->a = (uint8_t)nv;
}

static inline void op_cmp(splb20_t *c, int reg, uint8_t m) {
    int t = reg - m;
    c->nf = (t >> 7) & 1;
    c->zf = (t == 0);
    c->cf = t >= 0;
}

static inline __attribute__((always_inline)) int execute(splb20_t *c, uint8_t opcode) {
    uint8_t zp, m;
    uint16_t addr;
    int nv;

    switch (opcode) {
    case 0x00: /* brk */
        c->bf = 1;
        NEXT(1);
        irq(c);
        return 7;
    case 0x05: /* ora zp */
        c->a |= zp_read(c, IMM()); NEXT(1); SET_NZ(c->a); return 3;
    case 0x08: /* php */
        push(c, get_ps(c)); return 3;
    case 0x09: /* ora imm */
        c->a |= IMM(); NEXT(1); SET_NZ(c->a); return 2;
    case 0x10: BRANCH(!c->nf);
    case 0x18: c->cf = 0; return 2;
    case 0x20: { /* jsr abs */
        uint16_t ret = (uint16_t)(c->pc + 1);
        push(c, ret >> 8);
        push(c, ret & 0xFF);
        c->pc = rom_word(c, c->pc);
        return 6;
    }
    case 0x24: /* bit zp */
        m = zp_read(c, IMM()); NEXT(1);
        c->nf = m >> 7; c->vf = (m & 0x40) != 0; c->zf = (c->a & m) == 0;
        return 3;
    case 0x25: c->a &= zp_read(c, IMM()); NEXT(1); SET_NZ(c->a); return 3;
    case 0x26: /* rol zp */
        zp = IMM(); NEXT(1);
        nv = (zp_read(c, zp) << 1) | c->cf;
        zp_write(c, zp, (uint8_t)nv);
        c->nf = (nv & 0x80) != 0; c->zf = (nv & 0xFF) == 0; c->cf = nv > 0xFF;
        return 5;
    case 0x28: set_ps(c, pull(c)); return 4;
    case 0x29: c->a &= IMM(); NEXT(1); SET_NZ(c->a); return 2;
    case 0x2A: /* rol a */
        nv = (c->a << 1) | c->cf;
        c->a = (uint8_t)nv;
        c->nf = (nv & 0x80) != 0; c->zf = (nv & 0xFF) == 0; c->cf = nv > 0xFF;
        return 2;
    case 0x2C: /* bit abs */
        m = splb20_read(c, rom_word(c, c->pc)); NEXT(2);
        c->nf = m >> 7; c->vf = (m & 0x40) != 0; c->zf = (c->a & m) == 0;
        return 4;
    case 0x30: BRANCH(c->nf);
    case 0x38: c->cf = 1; return 2;
    case 0x40: /* rti */
        set_ps(c, pull(c));
        c->pc = pull(c);
        c->pc |= pull(c) << 8;
        return 6;
    case 0x45: c->a ^= zp_read(c, IMM()); NEXT(1); SET_NZ(c->a); return 3;
    case 0x48: push(c, c->a); return 3;
    case 0x49: c->a ^= IMM(); NEXT(1); SET_NZ(c->a); return 2;
    case 0x4C: c->pc = rom_word(c, c->pc); return 3;
    case 0x50: BRANCH(!c->vf);
    case 0x55: c->a ^= zp_read(c, (uint8_t)(IMM() + c->x)); NEXT(1); SET_NZ(c->a); return 4;
    case 0x58: c->if_ = 0; return 2;
    case 0x60: /* rts */
        c->pc = pull(c);
        c->pc |= pull(c) << 8;
        c->pc = (uint16_t)(c->pc + 1);
        return 6;
    case 0x65: op_adc(c, zp_read(c, IMM())); NEXT(1); return 3;
    case 0x66: { /* ror zp */
        zp = IMM(); NEXT(1);
        uint8_t prev = zp_read(c, zp);
        zp_write(c, zp, (uint8_t)((prev >> 1) | (c->cf << 7)));
        c->nf = c->cf;
        c->zf = ((prev & 0xFE) | c->cf) == 0;
        c->cf = prev & 1;
        return 5;
    }
    case 0x68: c->a = pull(c); SET_NZ(c->a); return 4;
    case 0x69: op_adc(c, IMM()); NEXT(1); return 2;
    case 0x6A: { /* ror a */
        uint8_t prev = c->a;
        c->a = (uint8_t)((prev >> 1) | (c->cf << 7));
        c->nf = c->cf;
        c->zf = ((prev & 0xFE) | c->cf) == 0;
        c->cf = prev & 1;
        return 2;
    }
    case 0x6C: /* jmp (ind) */
        addr = rom_word(c, c->pc);
        c->pc = splb20_read(c, addr);
        c->pc |= splb20_read(c, (uint16_t)(addr + 1)) << 8;
        return 6;
    case 0x70: BRANCH(c->vf);
    case 0x78: c->if_ = 1; return 2;
    case 0x81: /* sta (zp,x) */
        zp = IMM(); NEXT(1);
        addr = (uint16_t)(zp_read(c, (uint8_t)(zp + c->x)) |
                          (zp_read(c, (uint8_t)(zp + c->x + 1)) << 8));
        splb20_write(c, addr, c->a);
        return 6;
    case 0x85: zp_write(c, IMM(), c->a); NEXT(1); return 3;
    case 0x86: zp_write(c, IMM(), c->x); NEXT(1); return 3;
    case 0x8A: c->a = c->x; SET_NZ(c->a); return 2;
    case 0x8E: splb20_write(c, rom_word(c, c->pc), c->x); NEXT(2); return 4;
    case 0x90: BRANCH(!c->cf);
    case 0x95: zp_write(c, (uint8_t)(IMM() + c->x), c->a); NEXT(1); return 4;
    case 0x9A: c->sp = c->x; return 2;
    case 0xA1: { /* lda (zp,x) */
        uint8_t v = (uint8_t)(IMM() + c->x);
        NEXT(1);
        addr = (uint16_t)(zp_read(c, v) | (zp_read(c, (uint8_t)(v + 1)) << 8));
        c->a = splb20_read(c, addr);
        SET_NZ(c->a);
        return 6;
    }
    case 0xA2: c->x = IMM(); NEXT(1); SET_NZ(c->x); return 2;
    case 0xA5: c->a = zp_read(c, IMM()); NEXT(1); SET_NZ(c->a); return 3;
    case 0xA6: c->x = zp_read(c, IMM()); NEXT(1); SET_NZ(c->x); return 3;
    case 0xA9: c->a = IMM(); NEXT(1); SET_NZ(c->a); return 2;
    case 0xAA: c->x = c->a; SET_NZ(c->x); return 2;
    case 0xAD: c->a = splb20_read(c, rom_word(c, c->pc)); NEXT(2); SET_NZ(c->a); return 4;
    case 0xAE: c->x = splb20_read(c, rom_word(c, c->pc)); NEXT(2); SET_NZ(c->x); return 4;
    case 0xB0: BRANCH(c->cf);
    case 0xB5: c->a = zp_read(c, (uint8_t)(IMM() + c->x)); NEXT(1); SET_NZ(c->a); return 4;
    case 0xB8: c->vf = 0; return 2;
    case 0xBA: c->x = c->sp; SET_NZ(c->x); return 2;
    case 0xBD: { /* lda abs,x */
        uint16_t base = rom_word(c, c->pc);
        NEXT(2);
        addr = (uint16_t)(base + c->x);
        c->a = splb20_read(c, addr);
        SET_NZ(c->a);
        return 4 + ((base ^ addr) > 255);
    }
    case 0xC5: op_cmp(c, c->a, zp_read(c, IMM())); NEXT(1); return 3;
    case 0xC6: /* dec zp */
        zp = IMM(); NEXT(1);
        m = (uint8_t)(zp_read(c, zp) - 1);
        zp_write(c, zp, m);
        SET_NZ(m);
        return 5;
    case 0xC9: op_cmp(c, c->a, IMM()); NEXT(1); return 2;
    case 0xCA: c->x--; SET_NZ(c->x); return 2;
    case 0xD0: BRANCH(!c->zf);
    case 0xD5: op_cmp(c, c->a, zp_read(c, (uint8_t)(IMM() + c->x))); NEXT(1); return 4;
    case 0xD6: /* dec zp,x */
        zp = (uint8_t)(IMM() + c->x); NEXT(1);
        m = (uint8_t)(zp_read(c, zp) - 1);
        zp_write(c, zp, m);
        SET_NZ(m);
        return 6;
    case 0xE0: op_cmp(c, c->x, IMM()); NEXT(1); return 2;
    case 0xE4: op_cmp(c, c->x, zp_read(c, IMM())); NEXT(1); return 3;
    case 0xE5: op_sbc(c, zp_read(c, IMM())); NEXT(1); return 3;
    case 0xE6: /* inc zp */
        zp = IMM(); NEXT(1);
        m = (uint8_t)(zp_read(c, zp) + 1);
        zp_write(c, zp, m);
        SET_NZ(m);
        return 5;
    case 0xE8: c->x++; SET_NZ(c->x); return 2;
    case 0xE9: op_sbc(c, IMM()); NEXT(1); return 2;
    case 0xEA: return 2;
    case 0xF0: BRANCH(c->zf);
    case 0xF8: c->df = 1; return 2;
    default:
        /* The Python core prints "illegal instruction" and continues. */
        if (!c->illegal) {
            c->illegal = 1;
            c->illegal_pc = c->pc;
        }
        return 2;
    }
}

int32_t SPLB20_FAST splb20_step(splb20_t *c) {
    int32_t exec = MCLOCK_DIV_FP;

    /*
     * A hardware-backed subroutine can be consumed without executing the
     * original timing-sensitive implementation. This is used only for the
     * Elfin PA5 link routines on GBA; the host core leaves the hook unset.
     */
    if (c->rosc_enbl && c->cpu_enbl && c->pc_hook) {
        int action = c->pc_hook(c, c->pc, c->pc_hook_user);
        if (action == SPLB20_HOOK_CONSUME || action == SPLB20_HOOK_WAIT) {
            int cycles = action == SPLB20_HOOK_WAIT ? 8 : 6;
            exec = cycles << SPLB20_FP;
            timers_clock(c, exec);
            c->io_written = 0;
            return exec;
        }
    }
    if (c->rosc_enbl) {
        if (c->cpu_enbl) {
            uint8_t opcode = rom_byte(c, c->pc);
            c->pc = (uint16_t)(c->pc + 1);
            exec = execute(c, opcode) << SPLB20_FP;
            c->instr_counter++;
        }
        timers_clock(c, exec);
    } else if (c->sys_ctrl & SPLB20_SYS_32K_ENBL) {
        exec = c->sub_clock_div_fp;
        timers_clock(c, exec);
    }
    c->io_written = 0;
    return exec;
}

/*
 * While the main oscillator is stopped (SYS_CTRL bit 7, the game's "sleep")
 * with the 32 kHz clock running, every step just advances the timers by
 * clock/32768 cycles. Skip straight to the step on which a timer event would
 * occur. The skipped steps have no side effects, so the result is identical
 * to stepping one by one.
 */
static int32_t sleep_skip(splb20_t *c, int32_t budget_fp) {
    if (c->rosc_enbl || !(c->sys_ctrl & SPLB20_SYS_32K_ENBL) ||
        (c->sys_ctrl & SPLB20_SYS_TIMER_ENBL))
        return 0;
    int32_t step = c->sub_clock_div_fp * c->timer_warp;
    int32_t lim = c->t2hz_counter < c->t128hz_counter ? c->t2hz_counter : c->t128hz_counter;
    /* number of whole steps that leave both counters > 0 */
    int32_t n = (lim - 1) / step;
    int32_t nb = budget_fp / c->sub_clock_div_fp;
    if (n > nb)
        n = nb;
    if (n <= 0)
        return 0;
    c->t2hz_counter -= n * step;
    c->t128hz_counter -= n * step;
    return n * c->sub_clock_div_fp;
}

/* Cycles (fp8) until the next timer event that would raise an interrupt,
 * or until a counter would wrap. Only valid with timer_warp == 1. */
static int32_t next_event(const splb20_t *c) {
    int32_t n = c->t2hz_counter < c->t128hz_counter ? c->t2hz_counter : c->t128hz_counter;
    if (c->sys_ctrl & SPLB20_SYS_TIMER_ENBL) {
        /* the down counter underflows after timer_counter + tc prescaler periods */
        int64_t t = (int64_t)c->timer_counter +
                    (int64_t)c->tc * ((1 << c->prescalar) << SPLB20_FP);
        if (t < n)
            n = (int32_t)t;
    }
    return n;
}

/*
 * The PC hook is currently used by the GBA front end for the Elfin
 * link entry points. splb20_run() normally executes awake code through its
 * batched fast path, bypassing splb20_step(); stop at these PCs so the hook
 * is guaranteed to run before the intercepted ROM instruction executes.
 *
 * Keep this test tiny: it runs once per fast-path instruction and avoids
 * disabling batching for the whole game merely because a hook is installed.
 */
static inline __attribute__((always_inline)) int pc_hook_trap(const splb20_t *c) {
    if (!c->pc_hook)
        return 0;
    return c->pc == 0xBE0D || c->pc == 0xBE41 ||
           c->pc == 0xBDB7 || c->pc == 0xBA99;
}

int32_t SPLB20_FAST splb20_run(splb20_t *c, int32_t budget_fp) {
    int32_t done = 0;
    while (done < budget_fp) {
        if (!c->rosc_enbl || !c->cpu_enbl || c->timer_warp != 1 ||
            c->prescalar > 20 || pc_hook_trap(c)) {
            int32_t skipped = sleep_skip(c, budget_fp - done);
            if (skipped) {
                done += skipped;
                continue;
            }
            done += splb20_step(c);
            continue;
        }
        /* Awake: execute instructions and apply the timers in one batch,
         * stopping at the instruction on which a timer event is due, after
         * an SFR write, when the CPU stops, or at the end of the budget. */
        int32_t until = next_event(c);
        int32_t limit = budget_fp - done;
        c->pending_fp = 0;
        c->io_written = 0;
        for (;;) {
            /*
             * A branch/JSR in this batch may have just reached a hardware
             * hook. Flush the cycles already accumulated in this batch, then
             * let the next outer iteration dispatch it through splb20_step().
             */
            if (pc_hook_trap(c))
                break;

            uint8_t opcode = rom_byte(c, c->pc);
            if ((opcode == 0x85 || opcode == 0xA5) &&
                (loop_hint[c->pc >> 3] >> (c->pc & 7) & 1)) {
                int32_t end = until < limit ? until : limit;
                int32_t sk = skip_loop(c, end - c->pending_fp - 1);
                if (sk) {
                    done += sk;
                    c->pending_fp += sk;
                    continue;
                }
            }
            c->pc = (uint16_t)(c->pc + 1);
            int32_t cyc = execute(c, opcode) << SPLB20_FP;
            c->instr_counter++;
            done += cyc;
            c->pending_fp += cyc;
            if (c->io_written) {
                /* the batch before the write was flushed by io_write();
                 * what remains is this instruction's own cycles */
                break;
            }
            if (c->pending_fp >= until || c->pending_fp >= limit)
                break;
        }
        int32_t p = c->pending_fp;
        c->pending_fp = 0;
        timers_clock(c, p);
        c->io_written = 0;
    }
    return done;
}
