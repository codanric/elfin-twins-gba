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

static int queue_push_front(uint8_t *q, uint8_t *head, uint8_t *len, uint8_t v) {
    if (*len >= ELINK_QUEUE_SIZE)
        return 0;
    *head = (uint8_t)((*head - 1) & (ELINK_QUEUE_SIZE - 1));
    q[*head] = v;
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

/* Exact Emerald CheckMasterOrSlave condition: SD=1, SI=0, local ID 0. */
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

static int is_handshake(uint16_t w) {
    return w == ELINK_MASTER_HANDSHAKE ||
           (uint16_t)(w & (uint16_t)~3u) == ELINK_SLAVE_HANDSHAKE;
}

static uint8_t event_from_word(uint16_t w) {
    if ((w & ELINK_KIND_MASK) == ELINK_WAKE)
        return ELINK_TX_WAKE;
    return (uint8_t)(w & ELINK_VALUE_MASK);
}

static uint16_t multi_word(unsigned i) {
    switch (i) {
    case 0: return GBA_REG_SIOMULTI0;
    case 1: return GBA_REG_SIOMULTI1;
    case 2: return GBA_REG_SIOMULTI2;
    default: return GBA_REG_SIOMULTI3;
    }
}

static void preload_word(gba_link_t *l, uint16_t word) {
    l->tx_word = word;
    GBA_REG_SIOMLT_SEND = word;
}

/* Load exactly one logical word for the next completed transfer. */
static void preload_next(gba_link_t *l) {
    uint8_t event;
    if (queue_pop(l->tx_queue, &l->tx_head, &l->tx_len, &event))
        preload_word(l, event_word(event));
    else
        preload_word(l, ELINK_IDLE);
}

static void clear_receive_side(gba_link_t *l) {
    l->rx_head = l->rx_len = 0;
    l->wake_pending = 0;
    l->wake_delivered = 0;
    l->wake_only = 0;
    l->recv_armed = 0;
}

static void reset_session(gba_link_t *l) {
    l->ready = 0;
    l->parent = 0;
    l->peer_seen = 0;
    l->local_id = 0;
    l->bus_state = ELINK_BUS_HANDSHAKE;
    l->handshake_count = 0;
    l->handshake_stable = 0;
    l->handshake_master = 0;

    l->tx_head = l->tx_len = 0;
    clear_receive_side(l);

    l->hook_tx_active = 0;
    l->hook_tx_pc = 0;
    l->hook_tx_goal = 0;

    l->last_rx_word = 0;
    l->last_tx_word = 0;
    l->last_sio = 0;
    preload_word(l, ELINK_SLAVE_HANDSHAKE);
}

/* Re-enter only the native bus handshake. Keep unsent Elfin events and an
 * active ROM-hook completion target so a transient cable error cannot make a
 * hooked link_send return before its event actually crosses the bus. */
static void restart_handshake(gba_link_t *l) {
    l->ready = 0;
    l->parent = 0;
    l->peer_seen = 0;
    l->bus_state = ELINK_BUS_HANDSHAKE;
    l->handshake_count = 0;
    l->handshake_stable = 0;
    l->handshake_master = 0;
    clear_receive_side(l);
    preload_word(l, ELINK_SLAVE_HANDSHAKE);
}

static void requeue_inflight(gba_link_t *l) {
    if (is_payload(l->tx_word))
        (void)queue_push_front(l->tx_queue, &l->tx_head, &l->tx_len,
                               event_from_word(l->tx_word));
}

static void hw_start(gba_link_t *l) {
    GBA_REG_RCNT = 0;
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 | GBA_SIO_IRQ;

    reset_session(l);
    l->hw_enabled = 1;
}

static uint8_t handshake_player_count(void) {
    uint8_t count = 0;
    unsigned i;

    for (i = 0; i < 4; ++i) {
        uint16_t w = multi_word(i);
        if (is_handshake(w)) {
            ++count;
            continue;
        }
        if (w != 0xFFFF)
            count = 0;
        break;
    }
    return count;
}

/* Emerald requires the same nontrivial player count twice and a master marker
 * in slot 0 before declaring the cable session established. */
static void handle_handshake(gba_link_t *l) {
    uint8_t count = handshake_player_count();
    uint8_t stable = (uint8_t)(count > 1 && count == l->handshake_count);
    uint16_t first = multi_word(0);

    l->handshake_stable = stable;
    l->handshake_count = count;

    if (count > 1 && l->parent)
        l->handshake_master = 1;

    if (stable && first == ELINK_MASTER_HANDSHAKE) {
        l->bus_state = ELINK_BUS_READY;
        l->ready = 1;
        l->peer_seen = 1;
        l->last_rx_word = first;
        preload_next(l);
        return;
    }

    preload_word(l, (l->parent && l->handshake_master)
                      ? ELINK_MASTER_HANDSHAKE
                      : ELINK_SLAVE_HANDSHAKE);
}

static void handle_peer_word(gba_link_t *l, uint16_t word) {
    uint16_t kind = (uint16_t)(word & ELINK_KIND_MASK);

    if (kind == ELINK_DATA) {
        uint8_t value = (uint8_t)(word & ELINK_VALUE_MASK);
        if (!queue_push(l->rx_queue, &l->rx_head, &l->rx_len, value))
            return;
        l->last_rx_word = word;
        l->wake_pending = 1;
        l->wake_delivered = 0;
        l->wake_only = 0;
        ++l->frames_rx;
        return;
    }

    if (kind == ELINK_WAKE) {
        l->last_rx_word = word;
        l->wake_pending = 1;
        l->wake_delivered = 0;
        l->wake_only = 1;
        ++l->frames_rx;
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
        l->hw_enabled = 0;
        l->bus_state = ELINK_BUS_OFF;
        reset_session(l);
        l->bus_state = ELINK_BUS_OFF;
        GBA_REG_SIOMLT_SEND = 0;
        return;
    }

    hw_start(l);
}

void gba_link_service(gba_link_t *l) {
    uint16_t cnt;

    if (!l->enabled || !l->hw_enabled)
        return;

    cnt = GBA_REG_SIOCNT;
    l->last_sio = cnt;
    l->local_id = sio_id(cnt);

    if (l->bus_state == ELINK_BUS_HANDSHAKE)
        l->parent = sio_is_master(cnt);

    /* During handshake the exact Emerald terminal test elects the master.
     * Once established, keep that role stable and let the master offer one
     * transfer per VBlank just as LinkVSync does. */
    if (l->parent && !(cnt & GBA_SIO_START))
        GBA_REG_SIOCNT |= GBA_SIO_START;
}

void gba_link_on_serial(gba_link_t *l) {
    uint16_t cnt;
    uint16_t sent;
    unsigned i;

    if (!l->enabled || !l->hw_enabled)
        return;

    cnt = GBA_REG_SIOCNT;
    l->last_sio = cnt;
    l->local_id = sio_id(cnt);

    if (cnt & GBA_SIO_ERROR) {
        ++l->sio_errors;
        requeue_inflight(l);
        restart_handshake(l);
        return;
    }

    if (l->bus_state == ELINK_BUS_HANDSHAKE) {
        handle_handshake(l);
        return;
    }

    sent = l->tx_word;

    /* If a peer reset and starts advertising the hardware handshake again,
     * do not count the application word as delivered. Requeue it and resync. */
    for (i = 0; i < 4; ++i) {
        uint16_t word;
        if (i == l->local_id)
            continue;
        word = multi_word(i);
        if (is_handshake(word)) {
            requeue_inflight(l);
            restart_handshake(l);
            return;
        }
    }

    l->last_tx_word = sent;
    if (is_payload(sent))
        ++l->frames_tx;

    for (i = 0; i < 4; ++i) {
        uint16_t word;
        if (i == l->local_id)
            continue;
        word = multi_word(i);
        if ((word & ELINK_KIND_MASK) == ELINK_DATA ||
            (word & ELINK_KIND_MASK) == ELINK_WAKE) {
            handle_peer_word(l, word);
            break;
        }
    }

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
    return l->enabled && l->hw_enabled &&
           l->bus_state == ELINK_BUS_READY && l->peer_seen;
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
