#include "gba_link.h"
#include <string.h>

#define LINK_SERVICE_TICKS      128  /* 7.8125 ms at 16,384 Hz */
#define LINK_HANDSHAKE_PERIOD   16

static uint16_t make_frame(uint16_t type, uint8_t seq, uint8_t value) {
    return (uint16_t)(ELINK_MAGIC | type |
                      ((uint16_t)(seq & 0x0F) << 4) |
                      (value & 0x0F));
}

static int decode_frame(uint16_t w, uint16_t *type, uint8_t *seq, uint8_t *value) {
    if ((w & ELINK_MAGIC_MASK) != ELINK_MAGIC)
        return 0;

    *type = (uint16_t)(w & ELINK_TYPE_MASK);
    *seq = (uint8_t)((w >> 4) & 0x0F);
    *value = (uint8_t)(w & 0x0F);
    return 1;
}

static void reset_session(gba_link_t *l) {
    l->peer_seen = 0;
    l->answer_armed = 0;
    l->recv_armed = 0;
    l->tx_seq = 0;
    l->tx_inflight = 0;
    l->tx_wait_seq = 0;
    l->tx_wait_value = 0;
    l->tx_head = l->tx_len = 0;
    l->rx_expected = 0;
    l->rx_head = l->rx_len = 0;
    l->ack_pending = 0;
    l->ack_seq = 0;
    l->wake_pending = 0;
    l->wake_delivered = 0;
    l->transfer_ticks = 0;
    l->transfer_active = 0;
    l->transfer_started_seen = 0;
    l->last_rx_word = 0;
    l->last_tx_word = 0;
    l->last_sio = 0;
}

static int queue_push(uint8_t *q, uint8_t *head, uint8_t *len, uint8_t v) {
    if (*len >= ELINK_QUEUE_SIZE)
        return 0;
    q[(uint8_t)((*head + *len) & (ELINK_QUEUE_SIZE - 1))] = v;
    ++*len;
    return 1;
}

static uint8_t queue_peek(const uint8_t *q, uint8_t head) {
    return q[head];
}

static void queue_pop(uint8_t *head, uint8_t *len) {
    if (*len) {
        *head = (uint8_t)((*head + 1) & (ELINK_QUEUE_SIZE - 1));
        --*len;
    }
}

static uint16_t make_next_tx(gba_link_t *l) {
    if (l->ack_pending) {
        uint16_t w = make_frame(ELINK_ACK, l->ack_seq, 0);
        /*
         * ACKs are retransmitted when the peer retransmits DATA, so one
         * completed transfer is enough to clear the pending flag.
         */
        l->ack_pending = 0;
        return w;
    }

    if (l->tx_inflight)
        return make_frame(ELINK_DATA, l->tx_wait_seq, l->tx_wait_value);

    if (!l->peer_seen)
        return make_frame(ELINK_HELLO, 0, 1);

    if (l->tx_len) {
        l->tx_wait_seq = l->tx_seq;
        l->tx_wait_value = queue_peek(l->tx_queue, l->tx_head);
        l->tx_inflight = 1;
        queue_pop(&l->tx_head, &l->tx_len);
        return make_frame(ELINK_DATA, l->tx_wait_seq, l->tx_wait_value);
    }

    return make_frame(ELINK_PING, 0, 0);
}

static void handle_rx(gba_link_t *l, uint16_t word) {
    uint16_t type;
    uint8_t seq, value;

    l->last_rx_word = word;
    if (!decode_frame(word, &type, &seq, &value))
        return;

    switch (type) {
    case ELINK_HELLO:
        if (value != 1) {
            l->ack_pending = 1;
            l->ack_seq = 0x0F;
            return;
        }
        l->peer_seen = 1;
        break;

    case ELINK_DATA:
        l->peer_seen = 1;

        /*
         * There is one in-flight DATA per direction, so only the expected
         * sequence is new. An old sequence is a retransmission; ACK it again.
         * A future sequence is ignored rather than silently dropping an
         * earlier packet.
         */
        if (seq == l->rx_expected) {
            if (!queue_push(l->rx_queue, &l->rx_head, &l->rx_len, value))
                break;

            l->rx_expected = (uint8_t)((seq + 1) & 0x0F);
            l->wake_pending = 1;
            l->wake_delivered = 0;
        } else if (seq == (uint8_t)((l->rx_expected - 1) & 0x0F)) {
            /* Duplicate: the original ACK may have been lost. */
        } else {
            break;
        }

        l->ack_pending = 1;
        l->ack_seq = seq;
        ++l->frames_rx;
        break;

    case ELINK_ACK:
        if (l->tx_inflight && seq == l->tx_wait_seq) {
            l->tx_inflight = 0;
            l->tx_seq = (uint8_t)((seq + 1) & 0x0F);
            ++l->frames_tx;
        }
        break;

    case ELINK_RESET:
        reset_session(l);
        break;

    case ELINK_PING:
        l->peer_seen = 1;
        break;

    default:
        break;
    }
}

static void hw_start(gba_link_t *l) {
    /* No GPIO mode. RCNT bit 15 must remain zero. */
    GBA_REG_RCNT = 0;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_38400;
    GBA_REG_SIOMLT_SEND = make_frame(ELINK_HELLO, 0, 1);

    l->hw_enabled = 1;
    l->ready = 0;
    l->parent = 0;
    reset_session(l);
}

void gba_link_init(gba_link_t *l) {
    memset(l, 0, sizeof(*l));
    l->enabled = 0;
    l->hw_enabled = 0;
}

void gba_link_set_enabled(gba_link_t *l, int enabled) {
    enabled = enabled != 0;

    if (enabled == l->enabled)
        return;

    l->enabled = (uint8_t)enabled;

    if (!enabled) {
        GBA_REG_SIOCNT = 0;
        GBA_REG_RCNT = 0;
        l->hw_enabled = 0;
        l->ready = 0;
        reset_session(l);
        return;
    }

    hw_start(l);
}

void gba_link_service(gba_link_t *l) {
    uint16_t cnt, rx;

    if (!l->enabled || !l->hw_enabled)
        return;

    cnt = GBA_REG_SIOCNT;
    l->last_sio = cnt;

    if (cnt & GBA_SIO_ERROR) {
        ++l->sio_errors;
        hw_start(l);
        return;
    }

    l->ready = (uint8_t)((cnt & GBA_SIO_READY) != 0);
    l->parent = (uint8_t)((cnt & GBA_SIO_CHILD) == 0);

    if (!l->ready) {
        l->transfer_active = 0;
        l->transfer_started_seen = 0;
        return;
    }

    /*
     * Both roles poll the hardware BUSY bit. On a slave, the START bit is
     * observable for roughly the entire 38400-bps transfer (~417 us), while
     * this service runs every ~61 us. That gives several observations even if
     * one timer interrupt is delayed.
     */
    if (cnt & GBA_SIO_START) {
        l->transfer_started_seen = 1;
        l->transfer_active = 1;
        return;
    }

    if (l->transfer_active && l->transfer_started_seen) {
        l->transfer_active = 0;
        l->transfer_started_seen = 0;

        rx = l->parent ? GBA_REG_SIOMULTI1 : GBA_REG_SIOMULTI0;
        handle_rx(l, rx);

        /*
         * The slave has no clock. Its next outgoing word must be in
         * SIOMLT_SEND before the master starts again.
         */
        if (!l->parent) {
            uint16_t next = make_next_tx(l);
            GBA_REG_SIOMLT_SEND = next;
            l->last_tx_word = next;
        }
    }

    if (!l->parent)
        return;

    if (l->transfer_active)
        return;

    if (l->transfer_ticks) {
        --l->transfer_ticks;
        return;
    }

    /*
     * Nintendo's own programmer documentation requires a guard interval
     * between multiplayer transfers; 7.8 ms is deliberately much longer than
     * the minimum while still making the game feel immediate.
     */
    uint16_t word = make_next_tx(l);
    GBA_REG_SIOMLT_SEND = word;
    l->last_tx_word = word;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_38400 | GBA_SIO_START;
    l->transfer_active = 1;
    l->transfer_started_seen = 1;
    l->transfer_ticks = LINK_SERVICE_TICKS;
}

int gba_link_send_count(gba_link_t *l, uint8_t count) {
    if (!l->enabled)
        return 0;

    /*
     * Queue before the cable reaches READY. This is important because the
     * original Elfin caller emits its wake/message before the peer has had a
     * chance to finish joining the GBA bus.
     */
    return queue_push(l->tx_queue, &l->tx_head, &l->tx_len,
                      (uint8_t)(count & 0x0F));
}

int gba_link_recv_count(gba_link_t *l, uint8_t *count) {
    if (!l->rx_len)
        return 0;

    *count = queue_peek(l->rx_queue, l->rx_head);
    queue_pop(&l->rx_head, &l->rx_len);

    if (!l->rx_len) {
        l->wake_pending = 0;
    } else {
        l->wake_pending = 1;
        l->wake_delivered = 0;
    }

    return 1;
}

int gba_link_is_ready(const gba_link_t *l) {
    return l->enabled && l->ready && l->peer_seen;
}

int gba_link_is_parent(const gba_link_t *l) {
    return l->parent != 0;
}

uint16_t gba_link_status(const gba_link_t *l) {
    return l->last_sio;
}

int gba_link_wake_pending(const gba_link_t *l) {
    return l->wake_pending && !l->wake_delivered;
}

void gba_link_mark_wake_delivered(gba_link_t *l) {
    l->wake_delivered = 1;
}
