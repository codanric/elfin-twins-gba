/*
 * GBA Multi-Player SIO transport.
 *
 * The application protocol is deliberately not timing-sensitive at the
 * physical level. PA5 transitions are converted to WAKE + COUNT events and
 * moved through standard GBA multiplayer transfers.
 */
#include <string.h>
#include "gba_link.h"

#define POLL_INTERVAL_TICKS 50 /* 50 * 61.035 us ~= 3.052 ms */

static int tx_push(gba_link_t *l, uint16_t word) {
    if (l->tx_count >= ELINK_TX_QUEUE_SIZE)
        return 0;
    l->tx_queue[(l->tx_head + l->tx_count) % ELINK_TX_QUEUE_SIZE] = word;
    ++l->tx_count;
    return 1;
}

static uint16_t tx_front(const gba_link_t *l) {
    return l->tx_queue[l->tx_head];
}

static void tx_pop(gba_link_t *l) {
    if (!l->tx_count)
        return;
    l->tx_head = (uint8_t)((l->tx_head + 1) % ELINK_TX_QUEUE_SIZE);
    --l->tx_count;
}

static int frame_decode(uint16_t word, uint8_t *type, uint8_t *value) {
    if ((word & ELINK_FRAME_MASK) != ELINK_FRAME_MAGIC)
        return 0;

    uint16_t kind = word & 0x0F00;
    if (kind == ELINK_FRAME_WAKE) {
        *type = ELINK_RX_WAKE;
        *value = 0;
        return 1;
    }
    if (kind == ELINK_FRAME_COUNT) {
        *type = ELINK_RX_COUNT;
        *value = (uint8_t)(word & 0x00FF);
        return 1;
    }
    return 0;
}

static void process_completed(gba_link_t *l) {
    if (!l->transfer_active)
        return;
    if (GBA_REG_SIOCNT & GBA_SIO_START)
        return;

    l->transfer_active = 0;

    /*
     * In multiplayer mode every node gets the complete set of received
     * words. With two nodes, parent receives the child in slot 1 and child
     * receives the parent in slot 0.
     */
    l->rx_word = l->parent ? GBA_REG_SIOMULTI1 : GBA_REG_SIOMULTI0;
    l->rx_pending = 1;

    if (l->tx_inflight) {
        tx_pop(l);
        l->tx_inflight = 0;
    }

    /*
     * A child has no clock, so publish the next queued word immediately.
     * That word will be sampled by the next parent transfer.
     */
    if (!l->parent) {
        GBA_REG_SIOMLT_SEND = l->tx_count ? tx_front(l) : 0;
        l->tx_inflight = l->tx_count != 0;
    }
}

void gba_link_init(gba_link_t *l) {
    uint8_t enabled = l->enabled;

    memset(l, 0, sizeof(*l));

    /* Stop any previous mode, then select 16-bit multiplayer mode. */
    GBA_REG_SIOCNT = 0;
    GBA_REG_RCNT = 0;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200;

    l->enabled = enabled;
    l->parent = (GBA_REG_SIOCNT & GBA_SIO_CHILD) == 0;
    l->ready = (GBA_REG_SIOCNT & GBA_SIO_READY) != 0;
    GBA_REG_SIOMLT_SEND = 0;
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
     * A slave sees the master's START bit even though it is read-only.
     * Latch that transfer before polling for its completion.
     */
    if (!l->parent && (cnt & GBA_SIO_START))
        l->transfer_active = 1;

    process_completed(l);

    if (!l->parent) {
        /*
         * A slave has no clock. Its next word must already be in
         * SIOMLT_SEND before the parent starts the next transfer.
         */
        if (!l->transfer_active && l->tx_count && !l->tx_inflight) {
            GBA_REG_SIOMLT_SEND = tx_front(l);
            l->tx_inflight = 1;
        }
        return;
    }

    if (l->transfer_active)
        return;

    if (l->poll_ticks) {
        --l->poll_ticks;
        return;
    }

    /*
     * Parent clocks every transfer. If there is no queued application event,
     * send zero as an idle word; zero is not a valid Elfin frame.
     */
    if (l->tx_count) {
        GBA_REG_SIOMLT_SEND = tx_front(l);
        l->tx_inflight = 1;
    } else {
        GBA_REG_SIOMLT_SEND = 0;
        l->tx_inflight = 0;
    }

    GBA_REG_SIOCNT = cnt | GBA_SIO_START;
    l->transfer_active = 1;
    l->poll_ticks = POLL_INTERVAL_TICKS;
}

int gba_link_send_wake(gba_link_t *l) {
    if (!l->enabled)
        return 0;
    if (!tx_push(l, ELINK_FRAME_MAGIC | ELINK_FRAME_WAKE))
        return 0;
    ++l->edges_tx;
    return 1;
}

int gba_link_send_edge_count(gba_link_t *l, uint8_t edges) {
    if (!l->enabled)
        return 0;
    if (!tx_push(l, ELINK_FRAME_MAGIC | ELINK_FRAME_COUNT | edges))
        return 0;
    /*
     * Keep the debug counter as "completed messages emitted by the bridge",
     * rather than physical SIO words.
     */
    ++l->edges_tx;
    return 1;
}

int gba_link_recv_event(gba_link_t *l, uint8_t *type, uint8_t *value) {
    uint8_t t, v;

    if (!l->rx_pending)
        return 0;

    l->rx_pending = 0;
    if (!frame_decode(l->rx_word, &t, &v))
        return 0;

    *type = t;
    *value = v;
    ++l->edges_rx;
    return 1;
}

int gba_link_is_ready(const gba_link_t *l) {
    return l->enabled && l->ready;
}

int gba_link_is_parent(const gba_link_t *l) {
    return l->parent != 0;
}
