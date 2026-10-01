/*
 * GBA Multi-Player SIO transport.
 *
 * Hardware facts used here:
 *   - RCNT=0 selects normal serial/multiplayer/UART modes.
 *   - SIOCNT bit 13 selects Multi-Player, bits 0-1 select baud.
 *   - The cable elects one parent; only the parent starts transfers.
 *   - Each transfer puts the local SIOMLT_SEND word in the matching
 *     SIOMULTI slot at the other end.
 *
 * The Elfin emulation layer turns an original link_send pulse train into
 * one edge-count frame. This transport only moves that semantic message.
 */
#include <string.h>
#include "gba_link.h"

#define POLL_INTERVAL_TICKS 6 /* 6 * 488 us ~= 2.93 ms */
#define GBA_SIO_ERROR 0x0040

static uint16_t frame_make(uint8_t edges) {
    return (uint16_t)(ELINK_FRAME_MAGIC | ELINK_FRAME_EDGE | edges);
}

static int frame_edges(uint16_t word, uint8_t *edges) {
    if ((word & ELINK_FRAME_MASK) != ELINK_FRAME_MAGIC)
        return 0;
    if ((word & 0x0F00) != ELINK_FRAME_EDGE)
        return 0;
    *edges = (uint8_t)(word & 0x00FF);
    return 1;
}

void gba_link_init(gba_link_t *l) {
    memset(l, 0, sizeof(*l));

    /* Stop any previous mode before selecting multiplayer. */
    GBA_REG_SIOCNT = 0;
    GBA_REG_RCNT = 0;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 | GBA_SIO_IRQ;

    /*
     * The cable hardware determines parent/child. The terminal bit is valid
     * before the first transfer on real hardware; readiness is additionally
     * gated on SD-terminal (bit 3).
     */
    l->parent = (GBA_REG_SIOCNT & GBA_SIO_CHILD) == 0;
    l->ready = (GBA_REG_SIOCNT & GBA_SIO_READY) != 0;
    l->tx_word = ELINK_FRAME_IDLE;
    GBA_REG_SIOMLT_SEND = l->tx_word;
}

void gba_link_irq(gba_link_t *l) {
    uint16_t cnt = GBA_REG_SIOCNT;
    if (cnt & GBA_SIO_START)
        return; /* not a completion */

    /*
     * An interrupt can occur for a stale/initialised transfer. Ignore it
     * unless the cable reports a valid multiplayer link.
     */
    l->ready = (cnt & GBA_SIO_READY) != 0;
    if (!l->ready)
        return;

    if (l->parent)
        l->rx_word = GBA_REG_SIOMULTI1;
    else
        l->rx_word = GBA_REG_SIOMULTI0;

    l->rx_pending = 1;
    l->transfer_done = 1;

    /* A slave's transmit word must be loaded before the next parent start. */
    if (!l->parent) {
        if (l->tx_pending) {
            GBA_REG_SIOMLT_SEND = l->tx_word;
            l->tx_pending = 0;
        } else {
            GBA_REG_SIOMLT_SEND = ELINK_FRAME_IDLE;
        }
    }
}

void gba_link_service(gba_link_t *l) {
    uint16_t cnt = GBA_REG_SIOCNT;

    if (!l->enabled) {
        l->ready = 0;
        return;
    }

    if (cnt & GBA_SIO_ERROR) {
        uint8_t enabled = l->enabled;
        gba_link_init(l);
        l->enabled = enabled;
        return;
    }

    l->ready = (cnt & GBA_SIO_READY) != 0;
    if (!l->ready)
        return;

    l->parent = (cnt & GBA_SIO_CHILD) == 0;

    if (!l->parent)
        return;

    /* Parent must not restart until the previous transfer has completed. */
    if (cnt & GBA_SIO_START)
        return;

    if (l->poll_ticks)
        --l->poll_ticks;
    if (l->poll_ticks)
        return;

    /*
     * Keep polling even with no application data. This is important because
     * a child can queue its response after processing the preceding poll.
     */
    GBA_REG_SIOMLT_SEND = l->tx_pending ? l->tx_word : ELINK_FRAME_IDLE;
    l->tx_pending = 0;
    GBA_REG_SIOCNT = cnt | GBA_SIO_START;
    l->poll_ticks = POLL_INTERVAL_TICKS;
}

int gba_link_send_edge_count(gba_link_t *l, uint8_t edges) {
    if (!l->enabled || !l->ready)
        return 0;
    if (l->tx_pending)
        return 0;

    l->tx_word = frame_make(edges);
    if (l->parent || (GBA_REG_SIOCNT & GBA_SIO_START)) {
        l->tx_pending = 1;
    } else {
        /*
         * Slaves must have their next word loaded before the parent clocks
         * the next transfer. If the parent is not currently clocking, it is
         * safe to publish it immediately; otherwise the serial IRQ publishes
         * it when the current transfer completes.
         */
        GBA_REG_SIOMLT_SEND = l->tx_word;
        l->tx_pending = 0;
    }
    l->edges_tx++;
    return 1;
}

int gba_link_recv_edge_count(gba_link_t *l, uint8_t *edges) {
    if (!l->rx_pending)
        return 0;
    l->rx_pending = 0;
    if (!frame_edges(l->rx_word, edges))
        return 0;
    l->edges_rx++;
    return 1;
}

int gba_link_is_ready(const gba_link_t *l) {
    return l->enabled && l->ready;
}

int gba_link_is_parent(const gba_link_t *l) {
    return l->parent != 0;
}
