/*
 * Real-time emulation driver: runs the SPLB20 core from a 2048 Hz timer
 * interrupt (273 emulated cycles per interrupt), applies button presses,
 * carries the original link messages over the GBA's native multiplayer SIO
 * bus and drives the GBA sound hardware.
 */
#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include "splb20.h"

#define EMU_CLOCK      560000
#define EMU_IRQ_HZ     2048
#define EMU_SLICE_FP   ((EMU_CLOCK << SPLB20_FP) / EMU_IRQ_HZ)   /* 70000 */

/* Elfin Twins port A: bits 0-4 buttons (active low), bit 5 is kept as a
 * virtual input by the emulator, bits 6-7 buzzer. */
#define PA_ESC    0x01
#define PA_LEFT   0x02
#define PA_RIGHT  0x04
#define PA_CLOCK  0x08
#define PA_ENTER  0x10
#define PA_UP     (PA_LEFT | PA_RIGHT)
#define PA_DOWN   (PA_CLOCK | PA_ENTER)   /* the ROM decodes 0x07 as DOWN */
#define PA_LINK   0x20

enum { LINK_OFF = 0, LINK_ON_SD = 1, LINK_ON_SC = 2 };
/* LINK_ON_SD and LINK_ON_SC remain as save-file-compatible values; both now
 * mean "use the native GBA Game Link Cable". */

extern splb20_t cpu;

/* Written by the main loop, applied by the interrupt. */
extern volatile uint8_t emu_buttons;     /* PA bits to pull low */
extern volatile uint8_t emu_sound_on;
extern volatile uint8_t emu_link_mode;

/* Link activity counters (for the status indicator). */
extern volatile uint32_t emu_link_edges_rx;
extern volatile uint32_t emu_link_edges_tx;
extern volatile uint32_t emu_link_hook_send;
extern volatile uint32_t emu_link_hook_recv;
extern volatile uint32_t emu_link_hook_exchange;
extern volatile uint32_t emu_link_hook_answer;
extern volatile uint32_t emu_link_sio_errors;
extern volatile uint16_t emu_link_sio;
extern volatile uint16_t emu_link_last_rx;
extern volatile uint16_t emu_link_last_tx;
extern volatile uint8_t emu_link_parent;
extern volatile uint8_t emu_link_bus_ready;
extern volatile uint8_t emu_link_peer_seen;
extern volatile uint8_t emu_link_ok;

void emu_init(void);
void emu_start(void);         /* start the 2048 Hz interrupt */
void emu_stop(void);          /* stop it (for save / catch-up / reset) */
void emu_sound_update(void);  /* push the buzzer state to the sound chip */
void emu_sound_silence(void);

void emu_isr(void) __attribute__((long_call));

#endif
