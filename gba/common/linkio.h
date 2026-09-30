/*
 * GBA link port in "General Purpose" (GPIO) mode.
 *
 * RCNT (0x04000134): bit 15..14 = 10b selects GPIO mode,
 *   bits 0..3 = SC, SD, SI, SO data, bits 4..7 = direction (1 = output).
 *
 * The Elfin Twins link is a single open-drain wire with pull-ups. We emulate
 * that on the shared SD line of the GBA link cable (SD is wired to every
 * console, unlike SO/SI which are daisy-chained): "drive low" configures the
 * pin as an output at 0, "release" switches it back to an input so that the
 * cable's pull-up (or the other console) decides the level. We never drive a
 * pin high, so two consoles can never fight each other.
 */
#ifndef LINKIO_H
#define LINKIO_H

#include <stdint.h>

#define REG_RCNT_IO   (*(volatile uint16_t *)0x04000134)
#define REG_SIOCNT_IO (*(volatile uint16_t *)0x04000128)

enum { LINK_SC = 0, LINK_SD = 1, LINK_SI = 2, LINK_SO = 3 };

extern volatile uint16_t linkio_dir;   /* shadow of the direction bits */

static inline void linkio_init(void) {
    REG_SIOCNT_IO = 0;
    linkio_dir = 0;
    REG_RCNT_IO = 0x8000;              /* GPIO mode, all pins inputs */
}

static inline void linkio_drive_low(int pin) {
    linkio_dir |= (uint16_t)(1 << (pin + 4));
    REG_RCNT_IO = 0x8000 | linkio_dir;  /* data bits 0 -> driven low */
}

static inline void linkio_release(int pin) {
    linkio_dir &= (uint16_t)~(1 << (pin + 4));
    REG_RCNT_IO = 0x8000 | linkio_dir;
}

static inline int linkio_driving(int pin) {
    return (linkio_dir >> (pin + 4)) & 1;
}

/* Current level of all four pins (bit n = pin n). */
static inline uint16_t linkio_levels(void) {
    return REG_RCNT_IO & 0xF;
}

static inline int linkio_level(int pin) {
    return (REG_RCNT_IO >> pin) & 1;
}

#endif
