/*
 * Link cable bridge for the original Elfin Twins PA5 protocol.
 *
 * The GBA transport sends the physical semantics as two events:
 *   1. WAKE immediately when this unit first pulls PA5 low.
 *   2. COUNT when it releases PA5, containing only the transitions after the
 *      initial falling edge.
 *
 * A special COUNT=0 is the held-low answer pulse.
 */
#ifndef ELFIN_LINK_H
#define ELFIN_LINK_H

#include "splb20.h"

#define ELFIN_PA_LINK 0x20

typedef struct {
    uint8_t in_low;
    uint8_t out_low;
    uint8_t wire_low;
    uint8_t pending;
    uint32_t edges_rx, edges_tx;
    uint8_t tx_active;
    uint8_t tx_edges;
    uint8_t tx_wake_ready;
    uint8_t tx_ready;
    uint8_t tx_message;
} elfin_link_t;

#ifdef __cplusplus
extern "C" {
#endif

void elfin_link_reset(elfin_link_t *l);
void elfin_link_before(elfin_link_t *l, splb20_t *c, int pin_low);
int elfin_link_after(elfin_link_t *l, splb20_t *c);

#ifdef __cplusplus
}
#endif

#endif
