/*
 * Link cable bridge - see elfin_link.h.
 */
#include "elfin_link.h"

void elfin_link_reset(elfin_link_t *l) {
    l->in_low = l->out_low = l->wire_low = l->pending = 0;
    l->edges_rx = l->edges_tx = 0;
    l->tx_active = l->tx_wake_ready = l->tx_ready = l->tx_edges = l->tx_message = 0;
}

void elfin_link_before(elfin_link_t *l, splb20_t *c, int pin_low) {
    uint8_t ext_low = (uint8_t)(!l->out_low && pin_low);
    uint8_t wire = (uint8_t)(l->out_low || pin_low);

    if (wire && !l->wire_low)
        l->pending = 1;

    l->wire_low = wire;

    if (ext_low != l->in_low) {
        l->in_low = ext_low;
        splb20_port(c, ELFIN_PA_LINK, ext_low ? 0 : -1);
        if (ext_low)
            ++l->edges_rx;
    }
}

int elfin_link_after(elfin_link_t *l, splb20_t *c, int pin_low) {
    uint8_t drive = (splb20_drive_low(c) & ELFIN_PA_LINK) != 0;

    if (drive && !l->out_low) {
        l->edges_tx++;
        l->tx_active = 1;
        l->tx_edges = 0;
        l->tx_wake_ready = 1;
        l->wire_low = 1;
    } else if (!drive && l->out_low && l->tx_active) {
        /*
         * Direction change while the other side still holds PA5 low exposes
         * a low input to the caller. The real toy uses the resulting key
         * interrupt to wake. Preserve that edge in the emulation bridge.
         */
        /*
         * Only the peer's level can produce this wake. l->wire_low is not
         * sufficient here because it was necessarily low while our own
         * transmitter was driving the pin low.
         */
        if (pin_low)
            l->pending = 1;

        l->tx_message = l->tx_edges;
        l->tx_ready = 1;
        l->tx_active = 0;
    } else if (drive != l->out_low && l->tx_active) {
        if (l->tx_edges != 0xFF)
            ++l->tx_edges;
    }

    l->out_low = drive;
    l->wire_low = (uint8_t)(drive || pin_low);

    if (l->pending &&
        (c->int_cfg & (SPLB20_INT_NMI_ENBL | SPLB20_INT_NORMALKEY)) ==
            (SPLB20_INT_NMI_ENBL | SPLB20_INT_NORMALKEY)) {
        l->pending = 0;
        if (l->wire_low)
            splb20_key_irq(c);
    }

    return drive;
}
