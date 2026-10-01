#include "gba_link.h"
#include <string.h>

#define GBA_REG_IME (*(volatile uint16_t *)0x04000208)

static uint16_t link_irq_lock(void) {
    uint16_t ime = GBA_REG_IME;
    GBA_REG_IME = 0;
    return ime;
}

static void link_irq_unlock(uint16_t ime) {
    GBA_REG_IME = ime;
}

static int queue_push(uint8_t *q, uint8_t *head, uint8_t *len, uint8_t v) {
    if (*len >= ELINK_QUEUE_SIZE)
        return 0;
    q[(uint8_t)((*head + *len) & (ELINK_QUEUE_SIZE - 1))] = v;
    ++*len;
    return 1;
}

static int queue_pop(uint8_t *q, uint8_t *head, uint8_t *len, uint8_t *v) {
    if (!*len)
        return 0;
    *v = q[*head];
    *head = (uint8_t)((*head + 1) & (ELINK_QUEUE_SIZE - 1));
    --*len;
    return 1;
}

static uint8_t sio_id(uint16_t cnt) {
    return (uint8_t)((cnt & GBA_SIO_ID_MASK) >> 4);
}

/*
 * Pokemon Emerald determines master from the physical SD/SI terminal state
 * and local multiplayer ID, not from one bit interpreted as "parent".
 */
static uint8_t sio_is_master(uint16_t cnt) {
    return (uint8_t)(((cnt & (GBA_SIO_MULTI_SD | GBA_SIO_MULTI_SI))
                      == GBA_SIO_MULTI_SD) &&
                     sio_id(cnt) == 0);
}

static uint16_t event_word(uint8_t event) {
    if (event & ELINK_TX_WAKE)
        return ELINK_WAKE;
    return (uint16_t)(ELINK_DATA | (event & ELINK_VALUE_MASK));
}

static int is_payload(uint16_t w) {
    uint16_t kind = (uint16_t)(w & ELINK_KIND_MASK);
    return kind == ELINK_DATA || kind == ELINK_WAKE;
}

static void reset_session(gba_link_t *l) {
    l->ready = 0;
    l->parent = 0;
    l->peer_seen = 0;
    l->local_id = 0;

    l->tx_head = l->tx_len = 0;
    l->rx_head = l->rx_len = 0;

    l->wake_pending = 0;
    l->wake_delivered = 0;
    l->wake_only = 0;
    l->answer_armed = 0;
    l->recv_armed = 0;

    l->tx_word = ELINK_HELLO;
    l->last_rx_word = 0;
    l->last_tx_word = 0;
    l->last_sio = 0;
}

static void sample_role(gba_link_t *l, uint16_t cnt) {
    l->last_sio = cnt;
    l->local_id = sio_id(cnt);
    l->parent = sio_is_master(cnt);
}

/* Load exactly one word for the *next* hardware transfer. */
static void preload_next(gba_link_t *l) {
    uint8_t event;
    uint16_t word;

    if (queue_pop(l->tx_queue, &l->tx_head, &l->tx_len, &event))
        word = event_word(event);
    else if (!l->peer_seen)
        word = ELINK_HELLO;
    else
        word = ELINK_IDLE;

    l->tx_word = word;
    GBA_REG_SIOMLT_SEND = word;
}

static void hw_start(gba_link_t *l) {
    /*
     * This is the retail-game initialization sequence used by Emerald:
     * RCNT=0, multiplayer mode, 115200 bps, SERIAL interrupt enabled.
     */
    GBA_REG_RCNT = 0;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 | GBA_SIO_IRQ;

    reset_session(l);
    l->hw_enabled = 1;
    GBA_REG_SIOMLT_SEND = l->tx_word;
}

static void handle_peer_word(gba_link_t *l, uint16_t word) {
    uint16_t kind = (uint16_t)(word & ELINK_KIND_MASK);

    if (word == ELINK_HELLO) {
        l->peer_seen = 1;
        l->ready = 1;
        l->last_rx_word = word;
        return;
    }

    if (kind == ELINK_DATA) {
        uint8_t value = (uint8_t)(word & ELINK_VALUE_MASK);
        if (!queue_push(l->rx_queue, &l->rx_head, &l->rx_len, value))
            return;
        l->peer_seen = 1;
        l->ready = 1;
        l->last_rx_word = word;
        l->wake_pending = 1;
        l->wake_delivered = 0;
        l->wake_only = 0;
        ++l->frames_rx;
        return;
    }

    if (kind == ELINK_WAKE) {
        l->peer_seen = 1;
        l->ready = 1;
        l->last_rx_word = word;
        l->wake_pending = 1;
        l->wake_delivered = 0;
        l->wake_only = 1;
        ++l->frames_rx;
    }
}

static uint16_t multi_word(unsigned i) {
    switch (i) {
    case 0: return GBA_REG_SIOMULTI0;
    case 1: return GBA_REG_SIOMULTI1;
    case 2: return GBA_REG_SIOMULTI2;
    default: return GBA_REG_SIOMULTI3;
    }
}

void gba_link_init(gba_link_t *l) {
    memset(l, 0, sizeof(*l));
}

void gba_link_set_enabled(gba_link_t *l, int enabled) {
    enabled = enabled != 0;
    if (enabled == l->enabled)
        return;

    l->enabled = (uint8_t)enabled;
    if (!enabled) {
        GBA_REG_SIOCNT = GBA_SIO_MULTI;
        GBA_REG_RCNT = 0;
        GBA_REG_SIOMLT_SEND = 0;
        l->hw_enabled = 0;
        reset_session(l);
        return;
    }

    hw_start(l);
}

void gba_link_service(gba_link_t *l) {
    uint16_t cnt;

    if (!l->enabled || !l->hw_enabled)
        return;

    cnt = GBA_REG_SIOCNT;
    sample_role(l, cnt);

    /*
     * Emerald starts transfers only from the physical multiplayer master.
     * If a transfer is already in progress, the timer tick is simply ignored.
     */
    if (l->parent && !(cnt & GBA_SIO_START))
        GBA_REG_SIOCNT = (uint16_t)(GBA_SIO_MULTI |
                                    GBA_SIO_BAUD_115200 |
                                    GBA_SIO_IRQ |
                                    GBA_SIO_START);
}

void gba_link_on_serial(gba_link_t *l) {
    uint16_t cnt;
    uint16_t sent;
    unsigned i;

    if (!l->enabled || !l->hw_enabled)
        return;

    cnt = GBA_REG_SIOCNT;
    sample_role(l, cnt);

    if (cnt & GBA_SIO_ERROR) {
        ++l->sio_errors;
        l->peer_seen = 0;
        l->ready = 0;
    }

    /*
     * The word that was preloaded before this IRQ is the word just sent.
     * A DATA/WAKE event is therefore complete exactly once here.
     */
    sent = l->tx_word;
    l->last_tx_word = sent;
    if (is_payload(sent))
        ++l->frames_tx;

    /*
     * Multiplayer receive registers contain one synchronized word for each
     * participant. Scan every slot except our own and accept only Elfin words;
     * absent slots are normally 0xFFFF and idle slots are zero.
     */
    for (i = 0; i < 4; ++i) {
        uint16_t word;
        if (i == l->local_id)
            continue;
        word = multi_word(i);
        if (word == ELINK_HELLO ||
            (word & ELINK_KIND_MASK) == ELINK_DATA ||
            (word & ELINK_KIND_MASK) == ELINK_WAKE) {
            handle_peer_word(l, word);
            break;
        }
    }

    /*
     * Like Emerald's DoSend(), preload the next word immediately in SERIAL
     * completion so a child is ready before the master starts again.
     */
    preload_next(l);
}

int gba_link_send_count(gba_link_t *l, uint8_t count) {
    uint16_t ime;
    int ok;

    if (!l->enabled)
        return 0;

    ime = link_irq_lock();
    ok = queue_push(l->tx_queue, &l->tx_head, &l->tx_len,
                    (uint8_t)(count & ELINK_VALUE_MASK));
    link_irq_unlock(ime);
    return ok;
}

int gba_link_send_wake(gba_link_t *l) {
    uint16_t ime;
    int ok;

    if (!l->enabled)
        return 0;

    ime = link_irq_lock();
    ok = queue_push(l->tx_queue, &l->tx_head, &l->tx_len, ELINK_TX_WAKE);
    link_irq_unlock(ime);
    return ok;
}

int gba_link_recv_count(gba_link_t *l, uint8_t *count) {
    uint16_t ime;
    uint8_t value;
    int ok = 0;

    ime = link_irq_lock();
    if (queue_pop(l->rx_queue, &l->rx_head, &l->rx_len, &value)) {
        *count = value;
        if (!l->rx_len) {
            l->wake_pending = 0;
            l->wake_delivered = 0;
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
    return l->enabled && l->hw_enabled && l->peer_seen;
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
