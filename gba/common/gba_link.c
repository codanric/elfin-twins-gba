#include "gba_link.h"
#include <string.h>

#define TX_WAKE_MARK 0x80
#define GBA_REG_IME (*(volatile uint16_t *)0x04000208)

static uint16_t link_irq_lock(void) {
    uint16_t ime = GBA_REG_IME;
    GBA_REG_IME = 0;
    return ime;
}

static void link_irq_unlock(uint16_t ime) {
    GBA_REG_IME = ime;
}

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
    l->wake_only = 0;
    l->transfer_ticks = 0;
    l->transfer_active = 0;
    l->transfer_started_seen = 0;
    l->slave_word_loaded = 0;
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

    if (l->tx_inflight) {
        uint16_t type = (l->tx_wait_value & TX_WAKE_MARK) ? ELINK_WAKE : ELINK_DATA;
        return make_frame(type, l->tx_wait_seq, l->tx_wait_value);
    }

    if (!l->peer_seen)
        return make_frame(ELINK_HELLO, 0, 1);

    if (l->tx_len) {
        l->tx_wait_seq = l->tx_seq;
        l->tx_wait_value = queue_peek(l->tx_queue, l->tx_head);
        l->tx_inflight = 1;
        queue_pop(&l->tx_head, &l->tx_len);
        return make_frame((l->tx_wait_value & TX_WAKE_MARK) ? ELINK_WAKE : ELINK_DATA,
                          l->tx_wait_seq, l->tx_wait_value);
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
    case ELINK_WAKE:
        l->peer_seen = 1;

        /*
         * DATA and WAKE share one reliable sequence stream per direction.
         * WAKE models the long BA91 responder pulse: it wakes the original
         * ROM but deliberately does not enqueue a link_recv count.
         */
        if (seq == l->rx_expected) {
            if (type == ELINK_DATA &&
                !queue_push(l->rx_queue, &l->rx_head, &l->rx_len, value))
                break;

            l->rx_expected = (uint8_t)((seq + 1) & 0x0F);
            l->wake_pending = 1;
            l->wake_delivered = 0;
            l->wake_only = (uint8_t)(type == ELINK_WAKE);
        } else if (seq == (uint8_t)((l->rx_expected - 1) & 0x0F)) {
            /* Duplicate: ACK again without re-triggering the emulated wake. */
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

static void slave_preload(gba_link_t *l) {
    if (l->parent || l->slave_word_loaded)
        return;

    uint16_t next = make_next_tx(l);
    GBA_REG_SIOMLT_SEND = next;
    l->last_tx_word = next;
    l->slave_word_loaded = 1;
}

static void hw_start(gba_link_t *l) {
    /* No GPIO mode. RCNT bit 15 must remain zero. */
    GBA_REG_RCNT = 0;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_38400 | GBA_SIO_IRQ;
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
     * Both roles may observe the hardware BUSY bit. The pacing interrupt is
     * intentionally much slower than one 38.4-kbps transfer, so SERIAL IRQ is
     * authoritative on a child. A console that did observe BUSY may still use
     * the state below as a narrow lost-IRQ fallback.
     */
    if (cnt & GBA_SIO_START) {
        l->transfer_started_seen = 1;
        l->transfer_active = 1;
        if (!l->parent)
            l->slave_word_loaded = 0;
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
        slave_preload(l);
    }

    /*
     * A missed SERIAL IRQ must not permanently wedge the session. If the
     * hardware is no longer BUSY, recover the completed transfer here.
     */
    if (l->transfer_active && !(cnt & GBA_SIO_START))
        gba_link_on_serial(l);

    if (!l->parent) {
        /*
         * Do not treat a stable SIOMULTI0 value as a fresh transfer on every
         * timer tick. SERIAL completion is authoritative for a child; if we
         * happened to observe BUSY above, the transfer_active fallback already
         * handled a missed IRQ. Otherwise keep the already-loaded response.
         */
        slave_preload(l);
        return;
    }

    if (l->transfer_active)
        return;

    /*
     * Nintendo's own programmer documentation uses a guard interval between
     * multiplayer transfers. 3.05 ms matches the interval used by established
     * real-hardware GBA link implementations at 38.4 kbps.
     */
    uint16_t word = make_next_tx(l);
    GBA_REG_SIOMLT_SEND = word;
    l->last_tx_word = word;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_38400 | GBA_SIO_IRQ | GBA_SIO_START;
    l->transfer_active = 1;
    l->transfer_started_seen = 1;
}

void gba_link_on_serial(gba_link_t *l) {
    uint16_t cnt, rx;

    if (!l->enabled || !l->hw_enabled)
        return;

    cnt = GBA_REG_SIOCNT;
    l->last_sio = cnt;
    l->ready = (uint8_t)((cnt & GBA_SIO_READY) != 0);
    l->parent = (uint8_t)((cnt & GBA_SIO_CHILD) == 0);

    if (cnt & GBA_SIO_ERROR) {
        ++l->sio_errors;
        hw_start(l);
        return;
    }

    if (!(cnt & GBA_SIO_READY))
        return;

    /*
     * SERIAL is asserted when the current multiplayer transfer completes.
     * This is the critical path on the child: timer polling alone can miss a
     * sub-millisecond transfer while the emulator timer ISR is executing.
     */
    rx = l->parent ? GBA_REG_SIOMULTI1 : GBA_REG_SIOMULTI0;
    l->transfer_active = 0;
    l->transfer_started_seen = 0;
    handle_rx(l, rx);

    if (!l->parent) {
        /* Child has no clock; preload exactly one response for the next
         * parent start and do not let timer service overwrite it. */
        uint16_t next = make_next_tx(l);
        GBA_REG_SIOMLT_SEND = next;
        l->last_tx_word = next;
        l->slave_word_loaded = 1;
    }
}

int gba_link_send_count(gba_link_t *l, uint8_t count) {
    uint16_t ime;
    int ok;

    if (!l->enabled)
        return 0;

    /*
     * SERIAL may preempt the emulator timer ISR. Protect the producer update
     * so make_next_tx() can never observe a half-updated ring-buffer state.
     */
    ime = link_irq_lock();
    ok = queue_push(l->tx_queue, &l->tx_head, &l->tx_len,
                    (uint8_t)(count & 0x0F));
    link_irq_unlock(ime);
    return ok;
}

int gba_link_send_wake(gba_link_t *l) {
    uint16_t ime;
    int ok;

    if (!l->enabled)
        return 0;

    ime = link_irq_lock();
    ok = queue_push(l->tx_queue, &l->tx_head, &l->tx_len, TX_WAKE_MARK);
    link_irq_unlock(ime);
    return ok;
}

int gba_link_recv_count(gba_link_t *l, uint8_t *count) {
    uint16_t ime;
    int ok = 0;

    ime = link_irq_lock();
    if (l->rx_len) {
        *count = queue_peek(l->rx_queue, l->rx_head);
        queue_pop(&l->rx_head, &l->rx_len);

        if (!l->rx_len) {
            l->wake_pending = 0;
            l->wake_only = 0;
        } else {
            l->wake_pending = 1;
            l->wake_delivered = 0;
        }
        ok = 1;
    }
    link_irq_unlock(ime);
    return ok;
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

int gba_link_line_low(const gba_link_t *l) {
    return l->wake_pending != 0;
}

int gba_link_wake_is_only(const gba_link_t *l) {
    return l->wake_pending && l->wake_only;
}

void gba_link_mark_wake_delivered(gba_link_t *l) {
    l->wake_delivered = 1;
}

void gba_link_release_wake(gba_link_t *l) {
    l->wake_pending = 0;
    l->wake_delivered = 0;
    l->wake_only = 0;
}
