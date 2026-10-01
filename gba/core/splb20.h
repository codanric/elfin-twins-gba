/*
 * SPLB20 (Sunplus 6502-subset virtual-pet MCU) core in portable C.
 *
 * This is a line-by-line port of cores/SPLB20.py from BrickEmuPy, written so
 * that it can be compiled for the host (for verification against the Python
 * core) and for the Game Boy Advance (ARM code in IWRAM).
 *
 * All cycle/timer quantities are kept in 24.8 fixed point ("fp8") so that the
 * Python core's fractional sub-clock divider (clock / 32768) is reproduced
 * exactly.
 */
#ifndef SPLB20_H
#define SPLB20_H

#include <stdint.h>

#define SPLB20_SUB_CLOCK        32768
#define SPLB20_FP               8
#define SPLB20_FP_ONE           (1 << SPLB20_FP)

#define SPLB20_CPURAM_SIZE      0x80
#define SPLB20_DATARAM_SIZE     0x800
#define SPLB20_LCDRAM_SIZE      0x40

/* Interrupt configuration register ($79) bits */
#define SPLB20_INT_T2HZ         0x01
#define SPLB20_INT_T128HZ       0x02
#define SPLB20_INT_POWERKEY     0x04
#define SPLB20_INT_NORMALKEY    0x08
#define SPLB20_INT_COUNTER      0x10
#define SPLB20_INT_NMI_ENBL     0x80

/* System control register ($7A) bits */
#define SPLB20_SYS_STATUS       0x03
#define SPLB20_SYS_LCD_ENBL     0x04
#define SPLB20_SYS_LCD_ON       0x08
#define SPLB20_SYS_TIMER_ENBL   0x10
#define SPLB20_SYS_32K_ENBL     0x20
#define SPLB20_SYS_CPU_STOP     0x40
#define SPLB20_SYS_ROSC_STOP    0x80

typedef struct splb20 splb20_t;

/* Optional host-side hooks for hardware-backed subroutines. */
typedef int (*splb20_pc_hook_t)(struct splb20 *c, uint16_t pc, void *user);

/* Hook return values. OBSERVE means execute the original instruction normally. */
enum {
    SPLB20_HOOK_NONE = 0,
    SPLB20_HOOK_CONSUME = 1,
    SPLB20_HOOK_WAIT = 2,
    SPLB20_HOOK_OBSERVE = 3
};

struct splb20 {
    /* registers */
    uint16_t pc;
    uint8_t a, x, y, sp;
    uint8_t nf, vf, bf, df, if_, zf, cf;

    /* memories */
    uint8_t ram[SPLB20_CPURAM_SIZE];
    uint8_t dram[SPLB20_DATARAM_SIZE];
    uint8_t lcd[SPLB20_LCDRAM_SIZE];

    /* port A */
    uint8_t pdir;        /* $71: 1 = input, 0 = output            */
    uint8_t pcfg;        /* $72: port output configuration (b6-7 = buzzer) */
    uint8_t pullup;      /* internal pull-up (never written by the core) */
    uint8_t platch;      /* $73 write latch                        */
    uint8_t pullup_ext;  /* mask option: external pull-ups          */
    uint8_t in_low;      /* external devices pulling pins low       */
    uint8_t in_high;     /* external devices pulling pins high      */

    /* SFRs */
    uint8_t lcd_cfg, lcd_bias, sys_ctrl, int_cfg, ireq;
    uint8_t tc_preset, prescalar, keyscan_ctrl;
    int32_t tc;

    uint8_t rosc_enbl, cpu_enbl;
    uint8_t non_crystal;

    /* timers, fp8 cycles */
    int32_t timer_counter;
    int32_t t2hz_counter;
    int32_t t128hz_counter;
    int32_t sub_clock_div_fp;   /* clock / 32768 in fp8 */

    uint32_t clock_hz;
    uint32_t instr_counter;
    uint32_t t2hz_ticks;        /* number of 2 Hz timer events so far */

    /* buzzer */
    uint32_t snd_tc_div;
    uint32_t snd_clock_div;
    uint8_t snd_enable;
    uint8_t snd_changed;        /* set whenever the tone may have changed */

    /* Set when an unimplemented opcode is executed (debug aid). */
    uint8_t illegal;
    uint16_t illegal_pc;

    /* Cycles executed but not yet applied to the timers (fast path in
     * splb20_run, always 0 between calls). */
    int32_t pending_fp;
    uint8_t io_written;

    splb20_pc_hook_t pc_hook;
    void *pc_hook_user;

    /* Optional hook: time multiplier for the 2 Hz / 128 Hz / counter timers.
     * 1 = real time. Used for RTC catch-up (fast-forward of pet time). */
    int32_t timer_warp;

    const uint8_t *rom;
    uint32_t rom_mask;
    uint32_t rom_offset;
};

/* On the GBA the core lives in IWRAM; callers in ROM need long calls. */
#if defined(__GBA__)
#define SPLB20_API __attribute__((long_call))
#else
#define SPLB20_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

SPLB20_API void splb20_init(splb20_t *c, const uint8_t *rom, uint32_t rom_size,
                 uint32_t clock_hz, int non_crystal, uint8_t pullup_ext);
SPLB20_API void splb20_reset(splb20_t *c);
SPLB20_API void splb20_set_pc_hook(splb20_t *c, splb20_pc_hook_t hook, void *user);

/* Complete a JSR as if the intercepted subroutine executed RTS. */
SPLB20_API void splb20_return_from_subroutine(splb20_t *c);

/* Execute one Python-core "clock()" call. Returns elapsed cycles in fp8. */
SPLB20_API int32_t splb20_step(splb20_t *c);

/* Run until at least `budget_fp` (fp8) cycles have elapsed. Returns the
 * number of fp8 cycles actually executed. */
SPLB20_API int32_t splb20_run(splb20_t *c, int32_t budget_fp);

/* Equivalent of SPLB20.port_handler("PA", mask, level):
 * level 0  = pull pins low, level 1 = pull pins high, level -1 = release. */
SPLB20_API void splb20_port(splb20_t *c, uint8_t mask, int level);

SPLB20_API uint8_t splb20_port_read(const splb20_t *c);

/* Raise the port A key interrupt (if enabled). */
SPLB20_API void splb20_key_irq(splb20_t *c);

/* Pins configured as outputs that are currently driven low / high. */
static inline uint8_t splb20_drive_low(const splb20_t *c) {
    return (uint8_t)(~c->pdir & ~c->platch);
}
static inline uint8_t splb20_drive_high(const splb20_t *c) {
    return (uint8_t)(~c->pdir & c->platch);
}

/* 1 when the LCD controller is enabled (Python get_VRAM() non-empty). */
static inline int splb20_lcd_enabled(const splb20_t *c) {
    return (c->sys_ctrl & SPLB20_SYS_LCD_ENBL) != 0;
}

/* Current buzzer frequency in Hz, 0 when silent. */
SPLB20_API uint32_t splb20_sound_freq(const splb20_t *c);
/* Buzzer period in CPU clocks (clock_div * tc_div), 0 when silent. The tone is
 * clock_hz / period Hz; use this for exact pitch instead of the rounded Hz. */
SPLB20_API uint32_t splb20_sound_period(const splb20_t *c);

SPLB20_API uint8_t splb20_read(splb20_t *c, uint16_t addr);
SPLB20_API void splb20_write(splb20_t *c, uint16_t addr, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
