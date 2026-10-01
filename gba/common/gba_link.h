/*
 * Elfin Twins link transport using the GBA multiplayer SIO hardware.
 *
 * The hardware sequencing follows the proven cable model used by retail GBA
 * titles such as Pokemon Emerald:
 *   - RCNT = 0, multiplayer mode, 115200 bps, SERIAL IRQ enabled
 *   - master is elected from SD/SI terminal state + multiplayer ID
 *   - an explicit multi-transfer handshake stabilizes participant count/roles
 *   - only the hardware master starts transfers
 *   - SERIAL IRQ is the authoritative transfer-completion path
 *   - each transfer carries one already-loaded 16-bit word per GBA
 *
 * Above that bus handshake, Elfin keeps its own tiny protocol. The original
 * toy's pulse-count messages are carried as DATA words and the responder's
 * special BA91 low pulse is carried as a WAKE word.
 */
#ifndef GBA_LINK_H
#define GBA_LINK_H

#include <stdint.h>

#define GBA_REG_SIOMULTI0    (*(volatile uint16_t *)0x04000120)
#define GBA_REG_SIOMULTI1    (*(volatile uint16_t *)0x04000122)
#define GBA_REG_SIOMULTI2    (*(volatile uint16_t *)0x04000124)
#define GBA_REG_SIOMULTI3    (*(volatile uint16_t *)0x04000126)
#define GBA_REG_SIOCNT       (*(volatile uint16_t *)0x04000128)
#define GBA_REG_SIOMLT_SEND  (*(volatile uint16_t *)0x0400012A)
#define GBA_REG_RCNT         (*(volatile uint16_t *)0x04000134)

#define GBA_SIO_BAUD_115200  0x0003
#define GBA_SIO_MULTI_SI     0x0004
#define GBA_SIO_MULTI_SD     0x0008
#define GBA_SIO_ID_MASK      0x0030
#define GBA_SIO_ERROR        0x0040
#define GBA_SIO_START        0x0080
#define GBA_SIO_MULTI        0x2000
#define GBA_SIO_IRQ          0x4000

/* Emerald's proven multiplayer-cable handshake values. */
#define ELINK_MASTER_HANDSHAKE 0x8FFF
#define ELINK_SLAVE_HANDSHAKE  0xB9A0

/* One 16-bit logical Elfin event per completed multiplayer transfer. */
#define ELINK_IDLE           0x0000
#define ELINK_HELLO          0xE100 /* legacy internal value; no longer used */
#define ELINK_DATA           0xE200
#define ELINK_WAKE           0xE300
#define ELINK_KIND_MASK      0xFF00
#define ELINK_VALUE_MASK     0x000F

#define ELINK_QUEUE_SIZE     8
#define ELINK_TX_WAKE        0x80

enum {
    ELINK_BUS_OFF = 0,
    ELINK_BUS_HANDSHAKE,
    ELINK_BUS_READY
};

typedef struct {
    uint8_t enabled;
    uint8_t hw_enabled;
    uint8_t ready;          /* native multiplayer bus handshake completed */
    uint8_t parent;         /* locked hardware multiplayer master role */
    uint8_t peer_seen;
    uint8_t local_id;

    uint8_t bus_state;
    uint8_t handshake_count;
    uint8_t handshake_stable;
    uint8_t handshake_master;

    uint8_t tx_queue[ELINK_QUEUE_SIZE];
    uint8_t tx_head;
    uint8_t tx_len;
    uint8_t rx_queue[ELINK_QUEUE_SIZE];
    uint8_t rx_head;
    uint8_t rx_len;

    uint8_t wake_pending;
    uint8_t wake_delivered;
    uint8_t wake_only;
    uint8_t recv_armed;

    /* ROM-hook send completion tracking. A hook may yield until SERIAL IRQ
     * confirms that the native word carrying the Elfin event actually ran. */
    uint8_t hook_tx_active;
    uint16_t hook_tx_pc;
    uint32_t hook_tx_goal;

    /* Word currently preloaded in SIOMLT_SEND and diagnostics. */
    uint16_t tx_word;
    uint16_t last_rx_word;
    uint16_t last_tx_word;
    uint16_t last_sio;

    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t sio_errors;
} gba_link_t;

void gba_link_init(gba_link_t *l);
void gba_link_set_enabled(gba_link_t *l, int enabled);

/* Called once per VBlank. During handshake this elects the Emerald-style
 * master; after handshake the locked master offers one transfer per frame. */
void gba_link_service(gba_link_t *l);

/* Called directly from the GBA SERIAL IRQ after a multiplayer transfer. */
void gba_link_on_serial(gba_link_t *l);

int gba_link_send_count(gba_link_t *l, uint8_t count);
int gba_link_send_wake(gba_link_t *l);
int gba_link_recv_count(gba_link_t *l, uint8_t *count);

int gba_link_is_ready(const gba_link_t *l);
int gba_link_is_parent(const gba_link_t *l);
uint16_t gba_link_status(const gba_link_t *l);

int gba_link_wake_pending(const gba_link_t *l);
int gba_link_line_low(const gba_link_t *l);
int gba_link_wake_is_only(const gba_link_t *l);
void gba_link_mark_wake_delivered(gba_link_t *l);
void gba_link_release_wake(gba_link_t *l);

#endif
