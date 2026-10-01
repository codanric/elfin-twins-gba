/*
 * Link cable bridge for Elfin Twins (portable; used by the GBA port and the
 * host link simulator).
 *
 * The toy's link is one open-drain wire on port A bit 5 (PA5): each unit
 * either pulls it low or lets the pull-up hold it high. Messages are pulse
 * counts (see docs). The bridge maps PA5 to a real wire (the GBA link
 * cable's SD line) once per emulation slice:
 *
 *   before running a slice:  the wire level (as read from the pin) drives
 *                            PA5's external input
 *   after running a slice:   PA5 configured as output-low -> pull the wire
 *
 * The handshake in the ROM only works if a falling edge of the wire that
 * happens while a unit cannot see it (it is pulling the wire low itself, or
 * its key interrupt is disabled) is remembered and delivered as soon as the
 * key interrupt is enabled again - the answering unit starts its "I'm here"
 * low pulse while the caller is still holding the line low after its last
 * pulse. Without that latch, two emulated units never connect; with it the
 * whole session (call, answer, activity choice, tug-of-war) works. We
 * therefore model the port-change interrupt as a latched flag.
 */
#ifndef ELFIN_LINK_H
#define ELFIN_LINK_H

#include "splb20.h"

#define ELFIN_PA_LINK 0x20

typedef struct {
    uint8_t in_low;      /* PA5 external input currently pulled low */
    uint8_t out_low;     /* we are pulling the wire low */
    uint8_t wire_low;    /* last known wire level (0/1 = high/low) */
    uint8_t pending;     /* latched falling edge not yet delivered */
    uint32_t edges_rx, edges_tx;
    uint8_t tx_active;       /* original PA5 has entered output-low */
    uint8_t tx_edges;        /* transitions after the initial falling edge */
    uint8_t tx_ready;        /* tx_message contains a completed burst */
    uint8_t tx_message;
} elfin_link_t;

#ifdef __cplusplus
extern "C" {
#endif

void elfin_link_reset(elfin_link_t *l);

/* pin_low: the wire as read from our pin (1 = low). */
void elfin_link_before(elfin_link_t *l, splb20_t *c, int pin_low);

/* Returns 1 if we must pull the wire low for the next slice. */
int elfin_link_after(elfin_link_t *l, splb20_t *c);

#ifdef __cplusplus
}
#endif

#endif
