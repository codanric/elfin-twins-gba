/*
 * Link cable bridge - see elfin_link.h.
 */
#include "elfin_link.h"

void elfin_link_reset(elfin_link_t *l) {
    l->in_low = l->out_low = l->wire_low = l->pending = 0;
    l->edges_rx = l->edges_tx = 0;
    l->tx_active = l->tx_ready = l->tx_edges = l->tx_message = 0;
}

void elfin_link_before(elfin_link_t *l, splb20_t *c, int pin_low) {
    /* While we pull the wire low our pin reads low regardless of the other
     * unit, so only use the pin as the other unit's signal when we don't. */
    uint8_t ext_low = (uint8_t)(!l->out_low && pin_low);
    uint8_t wire = (uint8_t)(l->out_low || pin_low);
    if (wire && !l->wire_low)
        l->pending = 1;
    l->wire_low = wire;
    if (ext_low != l->in_low) {
        l->in_low = ext_low;
        splb20_port(c, ELFIN_PA_LINK, ext_low ? 0 : -1);
        if (ext_low)
            l->edges_rx++;
    }
}

int elfin_link_after(elfin_link_t *l, splb20_t *c) {
    uint8_t drive = (splb20_drive_low(c) & ELFIN_PA_LINK) != 0;

    if (drive && !l->out_low) {
        l->edges_tx++;
        l->tx_active = 1;
        l->tx_edges = 0;
        if (!l->wire_low)
            l->pending = 1;
        l->wire_low = 1;
    } else if (!drive && l->out_low && l->tx_active) {
        /*
         * link_send holds its final low level long enough for link_recv to
         * finish before release, so the release is normally not part of the
         * message. A bare answer pulse, however, has no toggles; represent
         * that as one falling-edge event so the peer can wake.
         */
        l->tx_message = l->tx_edges ? l->tx_edges : 1;
        l->tx_ready = 1;
        l->tx_active = 0;
    } else if (drive != l->out_low && l->tx_active) {
        if (l->tx_edges != 0xFF)
            l->tx_edges++;
    }

    l->out_low = drive;
    if (l->pending &&
        (c->int_cfg & (SPLB20_INT_NMI_ENBL | SPLB20_INT_NORMALKEY)) ==
            (SPLB20_INT_NMI_ENBL | SPLB20_INT_NORMALKEY)) {
        l->pending = 0;
        if (l->wire_low)
            splb20_key_irq(c);
    }
    return drive;
}
