/*
 * GBA Multi-Player SIO transport.
 *
 * Timing is supervised by a dedicated 61.04 us timer rather than the
 * SERIAL IRQ. That makes the transport independent of interrupt-dispatch
 * loss in the surrounding runtime and also gives the slave enough polling
 * resolution to observe a complete 115200-bps 16-bit transfer (~139 us).
 */
#include <string.h>
#include "gba_link.h"

#define POLL_INTERVAL_TICKS 50 /* 50 * 61.035 us ~= 3.052 ms */

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

static void process_completed(gba_link_t *l) {
    if (!l->transfer_active)
        return;
    if (GBA_REG_SIOCNT & GBA_SIO_START)
        return;

    l->transfer_active = 0;
    l->rx_word = l->parent ? GBA_REG_SIOMULTI1 : GBA_REG_SIOMULTI0;
    l->rx_pending = 1;
}

void gba_link_init(gba_link_t *l) {
    uint8_t enabled = l->enabled;

    memset(l, 0, sizeof(*l));

    /*
     * RCNT bit 15=0 selects normal/multiplayer/UART rather than GPIO.
     * SIOCNT bit 13=1 selects 16-bit multiplayer mode.
     */
    GBA_REG_SIOCNT = 0;
    GBA_REG_RCNT = 0;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200;

    l->enabled = enabled;
    l->parent = (GBA_REG_SIOCNT & GBA_SIO_CHILD) == 0;
    l->ready = (GBA_REG_SIOCNT & GBA_SIO_READY) != 0;
    l->tx_word = ELINK_FRAME_IDLE;
    GBA_REG_SIOMLT_SEND = l->tx_word;
}

void gba_link_service(gba_link_t *l) {
    uint16_t cnt;

    if (!l->enabled) {
        l->ready = 0;
        return;
    }

    cnt = GBA_REG_SIOCNT;

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

    /*
     * The parent marks its own transfer active when it raises START. A slave
     * sees that same START/BUSY bit as a read-only level, so latch it here.
     * At 61 us polling this still catches the ~139 us 16-bit wire transfer.
     */
    if (!l->parent && (cnt & GBA_SIO_START))
        l->transfer_active = 1;

    if (l->transfer_active)
        process_completed(l);

    /*
     * A child does not generate clocks. Its send register is written as soon
     * as application data is queued; the next parent start latches it.
     */
    if (!l->parent)
        return;

    /* Parent starts transfers on a fixed ~3.05 ms cadence. */
    if (l->transfer_active)
        return;

    if (l->poll_ticks) {
        --l->poll_ticks;
        return;
    }

    GBA_REG_SIOMLT_SEND = l->tx_pending ? l->tx_word : ELINK_FRAME_IDLE;
    l->tx_pending = 0;
    GBA_REG_SIOCNT = cnt | GBA_SIO_START;
    l->transfer_active = 1;
    l->poll_ticks = POLL_INTERVAL_TICKS;
}

int gba_link_send_edge_count(gba_link_t *l, uint8_t edges) {
    if (!l->enabled || !l->ready)
        return 0;

    if (l->parent) {
        if (l->tx_pending)
            return 0;
        l->tx_word = frame_make(edges);
        l->tx_pending = 1;
    } else {
        /*
         * In multiplayer mode the send register is the slave's next outgoing
         * word. Writing it while a transfer is active is safe because the
         * current word was already clocked; the new value is used by the next
         * parent start.
         */
        l->tx_word = frame_make(edges);
        GBA_REG_SIOMLT_SEND = l->tx_word;
    }

    l->edges_tx++;
    return 1;
}

int gba_link_recv_edge_count(gba_link_t *l, uint8_t *edges) {
    uint16_t word;

    if (!l->rx_pending)
        return 0;

    l->rx_pending = 0;
    word = l->rx_word;
    if (!frame_edges(word, edges))
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
